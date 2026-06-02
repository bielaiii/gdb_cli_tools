#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
build_dir="$repo_root/build"
agent="$build_dir/gdb-agent"
target="$build_dir/capability_fixture"

if [[ "${GDB_AGENT_FORCE_LIVE_TEST:-0}" != "1" && "$(uname -s)" != "Linux" ]]; then
    echo "skip: capability matrix smoke requires Linux + GDB"
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

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/gdb-agent-capability-smoke.XXXXXX")"
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
    echo "capability matrix smoke failed at line $line with exit code $code" >&2
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

save_named_action() {
    local session="$1"
    local payload="$2"
    local name="$3"
    local policy="$4"
    action_seq=$((action_seq + 1))
    local action_file="$work_dir/save-action-$action_seq.json"
    printf '%s\n' "$payload" >"$action_file"
    "$agent" save-action "$session" "$action_file" --name "$name" --failure-policy "$policy" --socket "$socket_path"
}

write_task() {
    local path="$1"
    local problem="$2"
    local args="$3"
    local stdin_path="${4:-}"
    local env_text="${5:-}"
    local core_path="${6:-}"
    cat >"$path" <<TASK
### problem

$problem

### executable

$target

### working directory

$repo_root

### args

$args

### stdin

$stdin_path

### env

$env_text

### run timeout

30000

### core dump

$core_path
TASK
}

artifact_check() {
    local assets="$1"
    local report="$2"
    local expected_kinds="$3"
    python3 - "$assets" "$report" "$expected_kinds" <<'PY'
import json
import pathlib
import re
import sys

assets = pathlib.Path(sys.argv[1])
report = pathlib.Path(sys.argv[2])
expected = [item for item in sys.argv[3].split(",") if item]
index = json.loads((assets / "evidence" / "index.json").read_text())
summary = json.loads((assets / "session_summary.json").read_text())
evidence = index.get("evidence", [])
ids = {item["id"] for item in evidence}
kinds = [item["kind"] for item in evidence]
if not evidence:
    raise SystemExit("evidence index is empty")
for item in evidence:
    for key in ("view_file", "raw_file", "summary_file"):
        path = pathlib.Path(item[key])
        if not path.exists():
            raise SystemExit(f"{key} missing for {item['id']}: {path}")
if summary.get("evidence_count") != len(evidence):
    raise SystemExit(f"evidence_count mismatch: {summary.get('evidence_count')} vs {len(evidence)}")
for kind in expected:
    if kind not in kinds:
        raise SystemExit(f"missing expected evidence kind: {kind}; saw {sorted(set(kinds))}")
report_ids = set(re.findall(r"\bE\d{4}\b", report.read_text()))
missing = report_ids - ids
if missing:
    raise SystemExit(f"report references missing evidence ids: {sorted(missing)}")
print("artifact consistency ok")
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

probe_task="$work_dir/probe_task.md"
probe_report="$work_dir/probe-report.md"
probe_assets="$work_dir/probe-report.assets"
write_task "$probe_task" "Capability matrix probe session." "probe"

create_probe="$("$agent" create "$probe_task" --socket "$socket_path" --session P1 --out "$probe_report" --assets "$probe_assets")"
require_contains "$create_probe" '"ok":true'
require_contains "$create_probe" '"state":"stopped"'
require_contains "$create_probe" '"signal":"SIGTRAP"'

watch_set="$(run_action P1 '{"action":"watchpoint_set","expression":"g_watch_value","comment":"watch global mutation","purpose":"capability matrix watchpoint","on_hit":{"actions":[{"action":"evaluate","expression":"g_watch_value"},{"action":"not_a_real_action"},{"action":"backtrace"}],"failure_policy":"stop_on_error","timeout_ms":5000,"max_output_bytes":4096,"max_summary_lines":40}}')"
require_contains "$watch_set" '"ok":true'
require_contains "$watch_set" '"action":"watchpoint_set"'
require_contains "$watch_set" '"watchpoint":"'

break_set="$(run_action P1 '{"action":"breakpoint_set","location":"matrix_breakpoint_site","condition":"value == 7","comment":"break on value propagation","purpose":"capability matrix breakpoint","on_hit":{"actions":[{"action":"evaluate","expression":"value"},{"action":"not_a_real_action"},{"action":"backtrace"}],"failure_policy":"continue_on_error","timeout_ms":5000,"max_output_bytes":4096,"max_summary_lines":40}}')"
require_contains "$break_set" '"ok":true'
require_contains "$break_set" '"action":"breakpoint_set"'
require_contains "$break_set" '"breakpoint":"'

stop_break_set="$(run_action P1 '{"action":"breakpoint_set","location":"matrix_stop_policy_site","condition":"value == 7","comment":"stop on on-hit failure","purpose":"capability matrix stop_on_error","on_hit":{"actions":[{"action":"evaluate","expression":"value"},{"action":"not_a_real_action"},{"action":"backtrace"}],"failure_policy":"stop_on_error","timeout_ms":5000,"max_output_bytes":4096,"max_summary_lines":40}}')"
require_contains "$stop_break_set" '"ok":true'
require_contains "$stop_break_set" '"action":"breakpoint_set"'
require_contains "$stop_break_set" '"breakpoint":"'

catch_set="$(run_action P1 '{"action":"catchpoint_set","event":"throw","comment":"catch fixture throw","purpose":"capability matrix catchpoint"}')"
require_contains "$catch_set" '"ok":true'
require_contains "$catch_set" '"action":"catchpoint_set"'
require_contains "$catch_set" '"event":"throw"'

probe_list="$(run_action P1 '{"action":"probe_list"}')"
require_contains "$probe_list" '"kind":"watchpoint"'
require_contains "$probe_list" '"kind":"breakpoint"'
require_contains "$probe_list" '"kind":"catchpoint"'
require_contains "$probe_list" '"comment":"watch global mutation"'
require_contains "$probe_list" '"purpose":"capability matrix breakpoint"'
require_contains "$probe_list" '"purpose":"capability matrix stop_on_error"'
require_contains "$probe_list" '"condition":"value == 7"'

raw_missing_risk="$(run_action P1 '{"action":"raw_mi","command":"-gdb-version"}')"
require_contains "$raw_missing_risk" '"ok":false'
require_contains "$raw_missing_risk" '"raw_mi requires risk=advanced"'
require_contains "$raw_missing_risk" '"action":"raw_mi"'
require_contains "$raw_missing_risk" '"evidence":"'

raw_ok="$(run_action P1 '{"action":"raw_mi","command":"-gdb-version","risk":"advanced"}')"
require_contains "$raw_ok" '"ok":true'
require_contains "$raw_ok" '"action":"raw_mi"'
require_contains "$raw_ok" '"evidence":"'

watch_hit="$(run_action P1 '{"action":"continue","deadline_ms":30000}')"
require_contains "$watch_hit" '"ok":true'
require_contains "$watch_hit" '"stop_reason":"watchpoint-trigger"'
require_contains "$watch_hit" '"action":"not_a_real_action"'
require_contains "$watch_hit" '"error":"unsupported action"'

break_hit="$(run_action P1 '{"action":"continue","deadline_ms":30000}')"
require_contains "$break_hit" '"ok":true'
require_contains "$break_hit" '"stop_reason":"breakpoint-hit"'
require_contains "$break_hit" '"action":"not_a_real_action"'
require_contains "$break_hit" '"error":"unsupported action"'

stop_hit="$(run_action P1 '{"action":"continue","deadline_ms":30000}')"
require_contains "$stop_hit" '"ok":true'
require_contains "$stop_hit" '"stop_reason":"breakpoint-hit"'
require_contains "$stop_hit" '"action":"not_a_real_action"'

catch_hit="$(run_action P1 '{"action":"continue","deadline_ms":30000}')"
require_contains "$catch_hit" '"ok":true'
require_contains "$catch_hit" '"stop_reason":"breakpoint-hit"'

hypothesis_create="$(run_action P1 '{"action":"hypothesis_create","id":"H-capability-value","title":"watch value reaches breakpoint","description":"Probe evidence should expose g_watch_value and value at stopped frames."}')"
require_contains "$hypothesis_create" '"ok":true'

hypothesis_pass="$(run_action P1 '{"action":"hypothesis_check","hypothesis":"H-capability-value","description":"watch value is visible","expression":"g_watch_value","assertion":"contains","expected":"= 7"}')"
require_contains "$hypothesis_pass" '"status":"passed"'
require_contains "$hypothesis_pass" '"observed":"'

hypothesis_fail="$(run_action P1 '{"action":"hypothesis_check","hypothesis":"H-capability-value","description":"watch value is not stale","expression":"g_watch_value","assertion":"equals","expected":"$1 = 99"}')"
require_contains "$hypothesis_fail" '"status":"failed"'

hypothesis_unknown="$(run_action P1 '{"action":"hypothesis_check","hypothesis":"H-capability-value","description":"unknown assertion is auditable","expression":"g_watch_value","assertion":"numeric_greater_than","expected":"0"}')"
require_contains "$hypothesis_unknown" '"status":"unknown"'
require_contains "$hypothesis_unknown" '"error_evidence":"'

hypothesis_conclude="$(run_action P1 '{"action":"hypothesis_conclude","hypothesis":"H-capability-value","conclusion":"Fixture value path explained","inference":"Agent inference is stored separately from tool check status."}')"
require_contains "$hypothesis_conclude" '"ok":true'

probe_after_hits="$(run_action P1 '{"action":"probe_list"}')"
require_contains "$probe_after_hits" '"hit_count":1'

save_named_action P1 '{"action":"backtrace"}' capability-continue continue_on_error >/dev/null
save_named_action P1 '{"action":"not_a_real_action"}' capability-continue continue_on_error >/dev/null
save_named_action P1 '{"action":"threads"}' capability-stop stop_on_error >/dev/null
save_named_action P1 '{"action":"not_a_real_action"}' capability-stop stop_on_error >/dev/null
save_named_action P1 '{"action":"locals"}' capability-stop stop_on_error >/dev/null
continue_plan="$probe_assets/replay/capability-continue.json"
stop_plan="$probe_assets/replay/capability-stop.json"
require_file "$continue_plan"
require_file "$stop_plan"

finish_probe="$("$agent" finish P1 --socket "$socket_path" --out "$probe_report")"
require_contains "$finish_probe" '"ok":true'
require_file "$probe_report"
require_file "$probe_assets/probes.json"
require_file "$probe_assets/hypotheses/index.json"
grep -F '"kind":"WatchpointHit"' "$probe_assets/evidence/index.json" >/dev/null
grep -F '"kind":"BreakpointHit"' "$probe_assets/evidence/index.json" >/dev/null
grep -F '"kind":"CatchpointHit"' "$probe_assets/evidence/index.json" >/dev/null
grep -F '"kind":"OnHitAction"' "$probe_assets/evidence/index.json" >/dev/null
grep -R -F '"status": "skipped"' "$probe_assets/evidence" >/dev/null
grep -F '"kind": "watchpoint"' "$probe_assets/probes.json" >/dev/null
grep -F '"kind": "catchpoint"' "$probe_assets/probes.json" >/dev/null
grep -F '"probe_hit_count": 4' "$probe_assets/session_summary.json" >/dev/null
grep -F '"on_hit_action_count": 9' "$probe_assets/session_summary.json" >/dev/null
grep -F '"on_hit_error_count": 3' "$probe_assets/session_summary.json" >/dev/null
grep -F '"status": "passed"' "$probe_assets/hypotheses/index.json" >/dev/null
grep -F '"status": "failed"' "$probe_assets/hypotheses/index.json" >/dev/null
grep -F '"status": "unknown"' "$probe_assets/hypotheses/index.json" >/dev/null
grep -F 'Final agent conclusion: `Fixture value path explained`' "$probe_report" >/dev/null
artifact_check "$probe_assets" "$probe_report" "BreakpointHit,WatchpointHit,CatchpointHit,OnHitAction,GdbCommand,ToolError"

replay_task="$work_dir/replay_task.md"
replay_report="$work_dir/replay-report.md"
replay_assets="$work_dir/replay-report.assets"
write_task "$replay_task" "Capability replay matching task." "thread-crash"
create_replay="$("$agent" create "$replay_task" --socket "$socket_path" --session R1 --out "$replay_report" --assets "$replay_assets")"
require_contains "$create_replay" '"ok":true'
replay_continue="$("$agent" replay R1 --file "$continue_plan" --force --failure-policy continue_on_error --socket "$socket_path")"
require_contains "$replay_continue" '"force":true'
require_contains "$replay_continue" '"task_metadata_match":false'
require_contains "$replay_continue" '"failure_policy":"continue_on_error"'
require_contains "$replay_continue" '"action_name":"not_a_real_action"'
replay_stop="$("$agent" replay R1 --file "$stop_plan" --force --socket "$socket_path")"
require_contains "$replay_stop" '"force":true'
require_contains "$replay_stop" '"failure_policy":"stop_on_error"'
require_contains "$replay_stop" '"status":"skipped"'
finish_replay="$("$agent" finish R1 --socket "$socket_path" --out "$replay_report")"
require_contains "$finish_replay" '"ok":true'
grep -F '"kind":"ReplayWarning"' "$replay_assets/evidence/index.json" >/dev/null
grep -F '"kind":"ReplayStep"' "$replay_assets/evidence/index.json" >/dev/null
grep -F '"replay_warning_count": 2' "$replay_assets/session_summary.json" >/dev/null
artifact_check "$replay_assets" "$replay_report" "ReplayWarning,ReplayStep,ReplayRun,ToolError"

reject_task="$work_dir/reject_task.md"
reject_report="$work_dir/reject-report.md"
reject_assets="$work_dir/reject-report.assets"
write_task "$reject_task" "Capability replay mismatch rejection task." "io"
create_reject="$("$agent" create "$reject_task" --socket "$socket_path" --session R2 --out "$reject_report" --assets "$reject_assets")"
require_contains "$create_reject" '"ok":true'
replay_reject="$("$agent" replay R2 --file "$continue_plan" --socket "$socket_path")"
require_contains "$replay_reject" '"ok":false'
require_contains "$replay_reject" '"task_metadata_match":false'
require_contains "$replay_reject" '"error_evidence":"'
finish_reject="$("$agent" finish R2 --socket "$socket_path" --out "$reject_report")"
require_contains "$finish_reject" '"ok":true'
grep -F '"kind":"ToolError"' "$reject_assets/evidence/index.json" >/dev/null

continue_task="$work_dir/continue_after_task.md"
continue_report="$work_dir/continue-after-report.md"
continue_assets="$work_dir/continue-after-report.assets"
write_task "$continue_task" "Capability continue-after-hit task." "probe"
create_continue="$("$agent" create "$continue_task" --socket "$socket_path" --session CA1 --out "$continue_report" --assets "$continue_assets")"
require_contains "$create_continue" '"ok":true'
continue_bp="$(run_action CA1 '{"action":"breakpoint_set","location":"matrix_breakpoint_site","comment":"auto continue after breakpoint","purpose":"verify continue_after_hit","on_hit":{"actions":[{"action":"evaluate","expression":"value"}],"failure_policy":"continue_on_error","continue_after_hit":true,"timeout_ms":5000}}')"
require_contains "$continue_bp" '"ok":true'
continue_run="$(run_action CA1 '{"action":"continue","deadline_ms":30000}')"
require_contains "$continue_run" '"action":"continue"'
finish_continue="$("$agent" finish CA1 --socket "$socket_path" --out "$continue_report")"
require_contains "$finish_continue" '"ok":true'
grep -R -F '"action_name": "continue_after_hit"' "$continue_assets/evidence" >/dev/null

io_task="$work_dir/io_task.md"
io_report="$work_dir/io-report.md"
io_assets="$work_dir/io-report.assets"
stdin_file="$work_dir/stdin.txt"
printf 'matrix input\n' >"$stdin_file"
write_task "$io_task" "Capability inferior I/O task." "io" "$stdin_file" "MATRIX_ENV=fixture-env"
create_io="$("$agent" create "$io_task" --socket "$socket_path" --session IO1 --out "$io_report" --assets "$io_assets")"
require_contains "$create_io" '"ok":true'
require_contains "$create_io" '"state":"exited"'
finish_io="$("$agent" finish IO1 --socket "$socket_path" --out "$io_report")"
require_contains "$finish_io" '"ok":true'
grep -R -F 'stdin=matrix input' "$io_assets/evidence" >/dev/null
grep -R -F 'env=fixture-env' "$io_assets/evidence" >/dev/null
grep -R -F 'stderr=capability-fixture' "$io_assets/evidence" >/dev/null
artifact_check "$io_assets" "$io_report" "InferiorOutput,EnvironmentInfo,StopEvent"

thread_task="$work_dir/thread_task.md"
thread_report="$work_dir/thread-report.md"
thread_assets="$work_dir/thread-report.assets"
write_task "$thread_task" "Capability thread crash task." "thread-crash"
create_thread="$("$agent" create "$thread_task" --socket "$socket_path" --session T1 --out "$thread_report" --assets "$thread_assets")"
require_contains "$create_thread" '"ok":true'
require_contains "$create_thread" '"signal":"SIGSEGV"'
for payload in \
    '{"action":"backtrace"}' \
    '{"action":"threads"}' \
    '{"action":"frame_select","frame":0}' \
    '{"action":"args_info"}' \
    '{"action":"locals"}' \
    '{"action":"registers"}' \
    '{"action":"evaluate","expression":"node"}'; do
    response="$(run_action T1 "$payload")"
    require_contains "$response" '"ok":true'
    require_contains "$response" '"evidence":"'
done
finish_thread="$("$agent" finish T1 --socket "$socket_path" --out "$thread_report")"
require_contains "$finish_thread" '"ok":true'
artifact_check "$thread_assets" "$thread_report" "GdbCommand,StopEvent"

core_file="$work_dir/capability.core"
core_gdb_log="$work_dir/core-gdb.log"
if ! gdb --batch -q \
        -ex 'set debuginfod enabled off' \
        -ex 'set confirm off' \
        -ex 'break matrix_core_stop' \
        -ex 'run core' \
        -ex "generate-core-file $core_file" \
        -ex 'quit' \
        --args "$target" core >"$core_gdb_log" 2>&1; then
    echo "skip: failed to generate fixture core file with gdb"
    cat "$core_gdb_log" >&2 || true
    exit 0
fi

if [[ ! -s "$core_file" ]]; then
    echo "skip: gdb did not produce a usable fixture core file"
    cat "$core_gdb_log" >&2 || true
    exit 0
fi

core_task="$work_dir/core_task.md"
core_report="$work_dir/core-report.md"
core_assets="$work_dir/core-report.assets"
write_task "$core_task" "Capability core mode task." "core" "" "" "$core_file"
create_core="$("$agent" create "$core_task" --socket "$socket_path" --session C1 --out "$core_report" --assets "$core_assets")"
require_contains "$create_core" '"ok":true'
require_contains "$create_core" '"mode":"core"'
for payload in \
    '{"action":"backtrace"}' \
    '{"action":"threads"}' \
    '{"action":"args_info"}' \
    '{"action":"locals"}'; do
    response="$(run_action C1 "$payload")"
    require_contains "$response" '"ok":true'
    require_contains "$response" '"evidence":"'
done
core_frame_response="$(run_action C1 '{"action":"frame_select","frame":0}')"
require_contains "$core_frame_response" '"action":"frame_select"'
require_contains "$core_frame_response" '"evidence":"'
if [[ "$core_frame_response" == *'"ok":false'* ]]; then
    require_contains "$core_frame_response" '"command_evidence":"'
else
    require_contains "$core_frame_response" '"ok":true'
fi
core_eval_response="$(run_action C1 '{"action":"evaluate","expression":"node"}')"
require_contains "$core_eval_response" '"action":"evaluate"'
require_contains "$core_eval_response" '"evidence":"'
if [[ "$core_eval_response" == *'"ok":false'* ]]; then
    require_contains "$core_eval_response" '"command_evidence":"'
else
    require_contains "$core_eval_response" '"ok":true'
fi
for payload in \
    '{"action":"run"}' \
    '{"action":"continue"}' \
    '{"action":"breakpoint_set","location":"matrix_breakpoint_site"}' \
    '{"action":"watchpoint_set","expression":"g_watch_value"}' \
    '{"action":"catchpoint_set","event":"throw"}' \
    '{"action":"probe_enable","number":1}' \
    '{"action":"probe_disable","number":1}' \
    '{"action":"probe_delete","number":1}'; do
    response="$(run_action C1 "$payload")"
    require_contains "$response" '"ok":false'
    require_contains "$response" 'is not available in core mode'
    require_contains "$response" '"evidence":"'
done
finish_core="$("$agent" finish C1 --socket "$socket_path" --out "$core_report")"
require_contains "$finish_core" '"ok":true'
grep -F '"mode": "core"' "$core_assets/session_summary.json" >/dev/null
artifact_check "$core_assets" "$core_report" "SessionEvent,GdbCommand,ToolError"

shutdown_response="$("$agent" shutdown --socket "$socket_path")"
require_contains "$shutdown_response" '"ok":true'
wait "$daemon_pid" >/dev/null 2>&1 || true
daemon_pid=""

if [[ -s "$weaknesses" ]]; then
    echo "capability matrix recorded non-blocking weaknesses:"
    cat "$weaknesses"
fi

echo "smoke ok: capability matrix flow passed"
