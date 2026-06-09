#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
build_dir="$repo_root/build"
agent="$build_dir/gdb-agent"
target="$build_dir/type_sanitizer_fixture"

if [[ "${GDB_AGENT_FORCE_LIVE_TEST:-0}" != "1" && "$(uname -s)" != "Linux" ]]; then
    echo "skip: type sanitizer smoke requires Linux + GDB"
    exit 0
fi

if ! command -v gdb >/dev/null 2>&1; then
    echo "skip: gdb is not installed"
    exit 0
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "skip: python3 is not installed"
    exit 0
fi

if [[ ! -x "$agent" || ! -x "$target" ]]; then
    echo "missing build artifacts; run: cmake -S . -B build && cmake --build build" >&2
    exit 1
fi

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/gdb-agent-type-sanitizer-smoke.XXXXXX")"
socket_path="$work_dir/gdb-agent.sock"
daemon_log="$work_dir/daemon.log"
daemon_pid=""

cleanup() {
    if [[ -n "$daemon_pid" ]] && kill -0 "$daemon_pid" >/dev/null 2>&1; then
        "$agent" shutdown --socket "$socket_path" >/dev/null 2>&1 || true
        wait "$daemon_pid" >/dev/null 2>&1 || true
    fi
}

on_error() {
    local line="$1"
    local code="$2"
    echo "type sanitizer smoke failed at line $line with exit code $code" >&2
    echo "work dir: $work_dir" >&2
    echo "daemon log:" >&2
    cat "$daemon_log" >&2 || true
    exit "$code"
}

trap cleanup EXIT
trap 'on_error "$LINENO" "$?"' ERR

require_contains() {
    local text="$1"
    local expected="$2"
    if [[ "$text" != *"$expected"* ]]; then
        echo "expected response to contain: $expected" >&2
        echo "$text" >&2
        exit 1
    fi
}

require_file_contains() {
    local path="$1"
    local expected="$2"
    if ! grep -F "$expected" "$path" >/dev/null; then
        echo "expected $path to contain: $expected" >&2
        cat "$path" >&2
        exit 1
    fi
}

require_file_not_contains() {
    local path="$1"
    local unexpected="$2"
    if grep -F "$unexpected" "$path" >/dev/null; then
        echo "expected $path not to contain: $unexpected" >&2
        cat "$path" >&2
        exit 1
    fi
}

action_seq=0
run_action() {
    local session="$1"
    local payload="$2"
    action_seq=$((action_seq + 1))
    local action_file="$work_dir/action-$action_seq.json"
    printf '%s\n' "$payload" >"$action_file"
    "$agent" action "$session" "$action_file" --socket "$socket_path"
}

write_task() {
    local path="$1"
    cat >"$path" <<TASK
### problem

Type sanitizer fixture.

### executable

$target

### working directory

$repo_root

### args


### run timeout

30000

### core dump

TASK
}

summary_for_response() {
    local assets="$1"
    local response="$2"
    python3 - "$assets" "$response" <<'PY'
import json
import pathlib
import sys

assets = pathlib.Path(sys.argv[1])
response = json.loads(sys.argv[2])
evidence_id = response["evidence"]
index = json.loads((assets / "evidence" / "index.json").read_text())
for item in index["evidence"]:
    if item["id"] == evidence_id:
        print(item["summary_file"])
        break
else:
    raise SystemExit(f"missing evidence id {evidence_id}")
PY
}

artifact_check() {
    local assets="$1"
    local report="$2"
    python3 - "$assets" "$report" <<'PY'
import json
import pathlib
import re
import sys

assets = pathlib.Path(sys.argv[1])
report = pathlib.Path(sys.argv[2])
index = json.loads((assets / "evidence" / "index.json").read_text())
summary = json.loads((assets / "session_summary.json").read_text())
evidence = index.get("evidence", [])
ids = {item["id"] for item in evidence}
if summary.get("evidence_count") != len(evidence):
    raise SystemExit(f"evidence_count mismatch: {summary.get('evidence_count')} vs {len(evidence)}")
for item in evidence:
    for key in ("view_file", "raw_file", "summary_file"):
        if not pathlib.Path(item[key]).exists():
            raise SystemExit(f"{key} missing for {item['id']}: {item[key]}")
report_ids = set(re.findall(r"\bE\d{4}\b", report.read_text()))
missing = report_ids - ids
if missing:
    raise SystemExit(f"report references missing evidence ids: {sorted(missing)}")
print("type sanitizer artifact consistency ok")
PY
}

"$agent" daemon --socket "$socket_path" >"$daemon_log" 2>&1 &
daemon_pid="$!"

for _ in {1..50}; do
    if [[ -S "$socket_path" ]]; then
        break
    fi
    sleep 0.1
done

if [[ ! -S "$socket_path" ]]; then
    echo "daemon did not create socket" >&2
    cat "$daemon_log" >&2 || true
    exit 1
fi

task="$work_dir/type-sanitizer.task.md"
report="$work_dir/type-sanitizer.report.md"
assets="$work_dir/type-sanitizer.report.assets"
write_task "$task"

create_response="$("$agent" create "$task" --socket "$socket_path" --session TS1 --out "$report" --assets "$assets")"
require_contains "$create_response" '"ok":true'
require_contains "$create_response" '"state":"stopped"'
require_contains "$create_response" '"signal":"SIGTRAP"'

default_response="$(run_action TS1 '{"action":"raw_mi","command":"-interpreter-exec console \"ptype type_sanitizer::DefaultTypes\"","risk":"advanced"}')"
require_contains "$default_response" '"ok":true'
default_summary="$(summary_for_response "$assets" "$default_response")"

require_file_contains "$default_summary" "std::vector<type_sanitizer::Foo> default_vector"
require_file_contains "$default_summary" "std::set<type_sanitizer::Foo> default_set"
require_file_contains "$default_summary" "std::map<std::string_view, std::chrono::time_point"
require_file_contains "$default_summary" "std::unordered_map<std::string_view, std::array<int, 2>> default_unordered"
require_file_contains "$default_summary" "std::unique_ptr<type_sanitizer::Foo> default_unique"
require_file_contains "$default_summary" "std::array<std::string_view, 2> labels"
require_file_contains "$default_summary" "std::function<int(std::string_view)> callback"
require_file_contains "$default_summary" "std::chrono::duration<long, std::ratio<1, 1000>> timeout"
require_file_not_contains "$default_summary" "std::allocator<type_sanitizer::Foo>"
require_file_not_contains "$default_summary" "std::default_delete<type_sanitizer::Foo>"
require_file_not_contains "$default_summary" "std::less<std::string_view>"
require_file_not_contains "$default_summary" "std::hash<std::string_view>"
require_file_not_contains "$default_summary" "std::equal_to<std::string_view>"
require_file_not_contains "$default_summary" "std::allocator<std::pair<std::string_view"

custom_response="$(run_action TS1 '{"action":"raw_mi","command":"-interpreter-exec console \"ptype type_sanitizer::CustomTypes\"","risk":"advanced"}')"
require_contains "$custom_response" '"ok":true'
custom_summary="$(summary_for_response "$assets" "$custom_response")"

require_file_contains "$custom_summary" "type_sanitizer::ArenaAllocator<type_sanitizer::Foo>"
require_file_contains "$custom_summary" "type_sanitizer::TransparentLess"
require_file_contains "$custom_summary" "type_sanitizer::ViewLess"
require_file_contains "$custom_summary" "type_sanitizer::ViewHash"
require_file_contains "$custom_summary" "type_sanitizer::ViewEqual"
require_file_contains "$custom_summary" "std::unique_ptr<type_sanitizer::Foo, type_sanitizer::FdCloser> custom_unique"

finish_response="$("$agent" finish TS1 --socket "$socket_path" --out "$report")"
require_contains "$finish_response" '"ok":true'
artifact_check "$assets" "$report"

shutdown_response="$("$agent" shutdown --socket "$socket_path")"
require_contains "$shutdown_response" '"ok":true'
wait "$daemon_pid" >/dev/null 2>&1 || true
daemon_pid=""

echo "smoke ok: type sanitizer flow passed"
