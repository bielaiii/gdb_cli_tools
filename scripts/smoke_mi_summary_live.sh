#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
build_dir="$repo_root/build"
agent="$build_dir/gdb-agent"
target="$build_dir/mi_summary_fixture"

if [[ "${GDB_AGENT_FORCE_LIVE_TEST:-0}" != "1" && "$(uname -s)" != "Linux" ]]; then
    echo "skip: MI summary live smoke requires Linux + GDB"
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

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/gdb-agent-mi-summary-live.XXXXXX")"
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
    echo "MI summary live smoke failed at line $line with exit code $code" >&2
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

require_file() {
    local path="$1"
    if [[ ! -f "$path" ]]; then
        echo "missing expected file: $path" >&2
        exit 1
    fi
}

require_file_contains() {
    local path="$1"
    local expected="$2"
    if ! grep -F -- "$expected" "$path" >/dev/null; then
        echo "expected $path to contain: $expected" >&2
        cat "$path" >&2
        exit 1
    fi
}

require_file_not_contains() {
    local path="$1"
    local unexpected="$2"
    if grep -F -- "$unexpected" "$path" >/dev/null; then
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

MI summary live fixture.

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

evidence_file_for_response() {
    local assets="$1"
    local response="$2"
    local key="$3"
    python3 - "$assets" "$response" "$key" <<'PY'
import json
import pathlib
import sys

assets = pathlib.Path(sys.argv[1])
response = json.loads(sys.argv[2])
key = sys.argv[3]
evidence_id = response["evidence"]
index = json.loads((assets / "evidence" / "index.json").read_text())
for item in index["evidence"]:
    if item["id"] == evidence_id:
        print(item[key])
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
if not evidence:
    raise SystemExit("evidence index is empty")
if summary.get("evidence_count") != len(evidence):
    raise SystemExit(f"evidence_count mismatch: {summary.get('evidence_count')} vs {len(evidence)}")
for item in evidence:
    for key in ("view_file", "raw_file", "summary_file"):
        path = pathlib.Path(item[key])
        if not path.exists():
            raise SystemExit(f"{key} missing for {item['id']}: {path}")
report_ids = set(re.findall(r"\bE\d{4}\b", report.read_text()))
missing = report_ids - ids
if missing:
    raise SystemExit(f"report references missing evidence ids: {sorted(missing)}")
print("MI summary artifact consistency ok")
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

task="$work_dir/mi-summary.task.md"
report="$work_dir/mi-summary.report.md"
assets="$work_dir/mi-summary.report.assets"
write_task "$task"

create_response="$("$agent" create "$task" --socket "$socket_path" --session MS1 --out "$report" --assets "$assets")"
require_contains "$create_response" '"ok":true'
require_contains "$create_response" '"state":"stopped"'
require_contains "$create_response" '"signal":"SIGTRAP"'

break_response="$(run_action MS1 '{"action":"breakpoint_set","location":"mi_summary_fixture::mi_summary_observe_here","comment":"MI summary stable stop","purpose":"live summary fixture"}')"
require_contains "$break_response" '"ok":true'
require_contains "$break_response" '"breakpoint":"'

continue_response="$(run_action MS1 '{"action":"continue","deadline_ms":30000}')"
require_contains "$continue_response" '"ok":true'
require_contains "$continue_response" '"stop_reason":"breakpoint-hit"'
continue_view="$(evidence_file_for_response "$assets" "$continue_response" "view_file")"
require_file "$continue_view"
require_file_contains "$continue_view" "Raw MI Audit"
require_file_contains "$continue_view" '`async`'

backtrace_response="$(run_action MS1 '{"action":"backtrace"}')"
require_contains "$backtrace_response" '"ok":true'
backtrace_summary="$(evidence_file_for_response "$assets" "$backtrace_response" "summary_file")"
backtrace_view="$(evidence_file_for_response "$assets" "$backtrace_response" "view_file")"
require_file "$backtrace_summary"
require_file "$backtrace_view"
require_file_contains "$backtrace_summary" "mi_summary_observe_here"
require_file_contains "$backtrace_summary" "mi_summary_leaf"
require_file_contains "$backtrace_summary" "mi_summary_middle"
require_file_contains "$backtrace_summary" "examples/mi_summary_fixture.cpp"
require_file_not_contains "$backtrace_summary" "$repo_root/"
require_file_contains "$backtrace_view" "Raw MI Audit"
require_file_contains "$backtrace_view" '`stream`'
require_file_contains "$backtrace_view" '`result`'

threads_response="$(run_action MS1 '{"action":"threads"}')"
require_contains "$threads_response" '"ok":true'
threads_summary="$(evidence_file_for_response "$assets" "$threads_response" "summary_file")"
threads_view="$(evidence_file_for_response "$assets" "$threads_response" "view_file")"
require_file "$threads_summary"
require_file "$threads_view"
require_file_contains "$threads_summary" "* "
require_file_contains "$threads_summary" "- "
require_file_contains "$threads_summary" "LWP"
require_file_contains "$threads_summary" "mi_summary_observe_here"
require_file_contains "$threads_view" "Raw MI Audit"

locals_response="$(run_action MS1 '{"action":"locals"}')"
require_contains "$locals_response" '"ok":true'
locals_summary="$(evidence_file_for_response "$assets" "$locals_response" "summary_file")"
require_file "$locals_summary"
require_file_contains "$locals_summary" "local_guard"
require_file_not_contains "$locals_summary" "std::__cxx11::basic_string<char, std::char_traits<char>"
require_file_not_contains "$locals_summary" "> >"

frame_response="$(run_action MS1 '{"action":"frame_select","frame":1}')"
require_contains "$frame_response" '"ok":true'

raw_value_response="$(run_action MS1 '{"action":"raw_mi","command":"-data-evaluate-expression guard","risk":"advanced"}')"
require_contains "$raw_value_response" '"ok":true'
raw_value_summary="$(evidence_file_for_response "$assets" "$raw_value_response" "summary_file")"
raw_value_view="$(evidence_file_for_response "$assets" "$raw_value_response" "view_file")"
require_file "$raw_value_summary"
require_file "$raw_value_view"
require_file_contains "$raw_value_summary" "result:done"
require_file_contains "$raw_value_summary" "value="
require_file_contains "$raw_value_view" "Raw MI Audit"
require_file_contains "$raw_value_view" '`result`'

type_response="$(run_action MS1 '{"action":"raw_mi","command":"-interpreter-exec console \"ptype mi_summary_fixture::SummaryPayload\"","risk":"advanced"}')"
require_contains "$type_response" '"ok":true'
type_summary="$(evidence_file_for_response "$assets" "$type_response" "summary_file")"
require_file "$type_summary"
require_file_contains "$type_summary" "std::vector<std::string> names"
require_file_contains "$type_summary" "std::map<std::string, int> counters"
require_file_not_contains "$type_summary" "std::__cxx11::basic_string<char, std::char_traits<char>"
require_file_not_contains "$type_summary" "> >"

finish_response="$("$agent" finish MS1 --socket "$socket_path" --out "$report")"
require_contains "$finish_response" '"ok":true'
require_file "$assets/session_summary.json"
artifact_check "$assets" "$report"

shutdown_response="$("$agent" shutdown --socket "$socket_path")"
require_contains "$shutdown_response" '"ok":true'
wait "$daemon_pid" >/dev/null 2>&1 || true
daemon_pid=""

echo "smoke ok: MI summary live flow passed"
