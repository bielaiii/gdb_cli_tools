#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
build_dir="$repo_root/build"
agent="$build_dir/gdb-agent"
target="$build_dir/capability_fixture"

if [[ "${GDB_AGENT_FORCE_LIVE_TEST:-0}" != "1" && "$(uname -s)" != "Linux" ]]; then
    echo "skip: catchpoint matrix smoke requires Linux + GDB"
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

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/gdb-agent-catchpoint-smoke.XXXXXX")"
socket_path="$work_dir/gdb-agent.sock"
daemon_log="$work_dir/daemon.log"
weaknesses="$work_dir/weaknesses.txt"
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
    echo "catchpoint matrix smoke failed at line $line with exit code $code" >&2
    echo "work dir: $work_dir" >&2
    echo "daemon log:" >&2
    cat "$daemon_log" >&2 || true
    echo "recorded weaknesses:" >&2
    cat "$weaknesses" >&2 || true
    exit "$code"
}

trap cleanup EXIT
trap 'on_error "$LINENO" "$?"' ERR

record_weakness() {
    printf '%s\n' "- $*" >>"$weaknesses"
}

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
    local problem="$2"
    local args="$3"
    cat >"$path" <<TASK
### problem

$problem

### executable

$target

### working directory

$repo_root

### args

$args

### run timeout

30000

### core dump

TASK
}

artifact_check() {
    local assets="$1"
    local report="$2"
    local expected_event="$3"
    local expected_selector="$4"
    python3 - "$assets" "$report" "$expected_event" "$expected_selector" <<'PY'
import json
import pathlib
import re
import sys

assets = pathlib.Path(sys.argv[1])
report = pathlib.Path(sys.argv[2])
expected_event = sys.argv[3]
expected_selector = sys.argv[4]

index = json.loads((assets / "evidence" / "index.json").read_text())
summary = json.loads((assets / "session_summary.json").read_text())
probes = json.loads((assets / "probes.json").read_text())
evidence = index.get("evidence", [])
ids = {item["id"] for item in evidence}
kinds = [item["kind"] for item in evidence]
if summary.get("evidence_count") != len(evidence):
    raise SystemExit(f"evidence_count mismatch: {summary.get('evidence_count')} vs {len(evidence)}")
if summary.get("probe_hit_count", 0) < 1:
    raise SystemExit("expected at least one probe hit")
if "CatchpointHit" not in kinds:
    raise SystemExit(f"missing CatchpointHit evidence; saw {sorted(set(kinds))}")
for item in evidence:
    for key in ("view_file", "raw_file", "summary_file"):
        if not pathlib.Path(item[key]).exists():
            raise SystemExit(f"{key} missing for {item['id']}: {item[key]}")
active = [
    item for item in probes.get("probes", [])
    if item.get("kind") == "catchpoint"
    and item.get("event") == expected_event
    and item.get("selector", "") == expected_selector
]
if not active:
    raise SystemExit(f"missing catchpoint metadata for {expected_event}/{expected_selector}")
if active[0].get("hit_count", 0) < 1:
    raise SystemExit("catchpoint metadata did not record a hit")
report_ids = set(re.findall(r"\bE\d{4}\b", report.read_text()))
missing = report_ids - ids
if missing:
    raise SystemExit(f"report references missing evidence ids: {sorted(missing)}")
print("catchpoint artifact consistency ok")
PY
}

assert_tool_error_artifact() {
    local assets="$1"
    if ! grep -F '"kind":"ToolError"' "$assets/evidence/index.json" >/dev/null; then
        echo "expected ToolError evidence in $assets" >&2
        exit 1
    fi
}

run_catchpoint_hit_session() {
    local session="$1"
    local mode="$2"
    local event="$3"
    local selector="$4"
    local action_json="$5"

    local task="$work_dir/${session}.task.md"
    local replay="$work_dir/${session}.replay.json"
    local report="$work_dir/${session}.report.md"
    local assets="$work_dir/${session}.report.assets"
    write_task "$task" "Catchpoint matrix $event $selector." "$mode"
    printf '%s\n' "$action_json" >"$replay"

    local create
    create="$("$agent" create "$task" --socket "$socket_path" --session "$session" --out "$report" --assets "$assets" --replay-before-run "$replay")"
    require_contains "$create" '"ok":true'

    if [[ "$create" != *'"state":"stopped"'* ]]; then
        local finish_response
        finish_response="$("$agent" finish "$session" --socket "$socket_path" --out "$report")"
        require_contains "$finish_response" '"ok":true'
        if grep -F '"kind":"ToolError"' "$assets/evidence/index.json" >/dev/null; then
            record_weakness "$event $selector catchpoint was not supported by this GDB; ToolError evidence was recorded"
            return 0
        fi
        echo "catchpoint did not stop and no ToolError explains it: $create" >&2
        exit 1
    fi

    local probes
    probes="$(run_action "$session" '{"action":"probe_list"}')"
    require_contains "$probes" '"kind":"catchpoint"'
    require_contains "$probes" "\"event\":\"$event\""
    require_contains "$probes" "\"selector\":\"$selector\""
    require_contains "$probes" '"hit_count":1'

    local finish_response
    finish_response="$("$agent" finish "$session" --socket "$socket_path" --out "$report")"
    require_contains "$finish_response" '"ok":true'
    require_file "$report"
    require_file "$assets/probes.json"
    require_file "$assets/session_summary.json"
    require_file "$assets/evidence/index.json"
    grep -F '"kind":"CatchpointHit"' "$assets/evidence/index.json" >/dev/null
    grep -R -F "\"event\": \"$event\"" "$assets/evidence" >/dev/null
    grep -R -F "\"selector\": \"$selector\"" "$assets/evidence" >/dev/null
    grep -F '"kind": "catchpoint"' "$assets/probes.json" >/dev/null
    grep -F "\"event\": \"$event\"" "$assets/probes.json" >/dev/null
    grep -F "\"selector\": \"$selector\"" "$assets/probes.json" >/dev/null
    grep -F '"probe_hit_count": 1' "$assets/session_summary.json" >/dev/null
    grep -F 'Probe Hit And On-Hit Evidence' "$report" >/dev/null
    artifact_check "$assets" "$report" "$event" "$selector"
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

run_catchpoint_hit_session \
    SYSCALL_ANY \
    syscall \
    syscall \
    "" \
    '{"action":"catchpoint_set","event":"syscall","comment":"catch any syscall","purpose":"catchpoint matrix generic syscall","on_hit":{"actions":[{"action":"threads"}],"failure_policy":"continue_on_error","timeout_ms":5000}}'

run_catchpoint_hit_session \
    SYSCALL_WRITE \
    syscall \
    syscall \
    write \
    '{"action":"catchpoint_set","event":"syscall","name":"write","comment":"catch write syscall","purpose":"catchpoint matrix selected syscall","on_hit":{"actions":[{"action":"threads"}],"failure_policy":"continue_on_error","timeout_ms":5000}}'

run_catchpoint_hit_session \
    FORK \
    fork \
    fork \
    "" \
    '{"action":"catchpoint_set","event":"fork","comment":"catch fork","purpose":"catchpoint matrix fork","on_hit":{"actions":[{"action":"threads"}],"failure_policy":"continue_on_error","timeout_ms":5000}}'

run_catchpoint_hit_session \
    EXEC \
    exec \
    exec \
    "" \
    '{"action":"catchpoint_set","event":"exec","comment":"catch exec","purpose":"catchpoint matrix exec","on_hit":{"actions":[{"action":"threads"}],"failure_policy":"continue_on_error","timeout_ms":5000}}'

validation_task="$work_dir/validation.task.md"
validation_report="$work_dir/validation.report.md"
validation_assets="$work_dir/validation.report.assets"
write_task "$validation_task" "Catchpoint validation errors." "probe"
validation_create="$("$agent" create "$validation_task" --socket "$socket_path" --session VALID --out "$validation_report" --assets "$validation_assets")"
require_contains "$validation_create" '"ok":true'
require_contains "$validation_create" '"signal":"SIGTRAP"'

unsupported="$(run_action VALID '{"action":"catchpoint_set","event":"not-real"}')"
require_contains "$unsupported" '"ok":false'
require_contains "$unsupported" '"unsupported catchpoint event"'
require_contains "$unsupported" '"evidence":"'

invalid_selector="$(run_action VALID '{"action":"catchpoint_set","event":"syscall","name":"write;bad"}')"
require_contains "$invalid_selector" '"ok":false'
require_contains "$invalid_selector" '"invalid syscall selector"'
require_contains "$invalid_selector" '"evidence":"'

syscall_alias="$(run_action VALID '{"action":"catchpoint_set","event":"syscall","syscall":"write","comment":"alias selector","purpose":"validate syscall field alias"}')"
require_contains "$syscall_alias" '"ok":true'
require_contains "$syscall_alias" '"event":"syscall"'
require_contains "$syscall_alias" '"selector":"write"'

finish_validation="$("$agent" finish VALID --socket "$socket_path" --out "$validation_report")"
require_contains "$finish_validation" '"ok":true'
assert_tool_error_artifact "$validation_assets"

shutdown_response="$("$agent" shutdown --socket "$socket_path")"
require_contains "$shutdown_response" '"ok":true'
wait "$daemon_pid" >/dev/null 2>&1 || true
daemon_pid=""

if [[ -s "$weaknesses" ]]; then
    echo "catchpoint matrix recorded non-blocking weaknesses:"
    cat "$weaknesses"
fi

echo "smoke ok: catchpoint matrix flow passed"
