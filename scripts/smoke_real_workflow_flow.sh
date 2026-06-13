#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
build_dir="$repo_root/build"
agent="$build_dir/gdb-agent"
target="$build_dir/workflow_fixture"

if [[ "${GDB_AGENT_FORCE_LIVE_TEST:-0}" != "1" && "$(uname -s)" != "Linux" ]]; then
    echo "skip: real workflow smoke requires Linux + GDB"
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

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/gdb-agent-real-workflow.XXXXXX")"
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
    echo "real workflow smoke failed at line $line with exit code $code" >&2
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

json_field() {
    local text="$1"
    local field="$2"
    python3 - "$field" "$text" <<'PY'
import json
import sys

field = sys.argv[1]
text = sys.argv[2]
for line in text.splitlines():
    line = line.strip()
    if not line:
        continue
    try:
        obj = json.loads(line)
    except json.JSONDecodeError:
        continue
    if field in obj:
        print(obj[field])
        break
PY
}

require_gdb_action_response() {
    local response="$1"
    local action="$2"
    require_contains "$response" "\"action\":\"$action\""
    require_contains "$response" '"evidence":"'
    if [[ "$response" == *'"ok":false'* ]]; then
        require_contains "$response" '"command_evidence":"'
    else
        require_contains "$response" '"ok":true'
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
    local core_path="${4:-}"
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

$core_path
TASK
}

artifact_check() {
    local assets="$1"
    local report="$2"
    local expected_kinds="$3"
    local require_probes="${4:-0}"
    local require_hypotheses="${5:-0}"
    python3 - "$assets" "$report" "$expected_kinds" "$require_probes" "$require_hypotheses" <<'PY'
import json
import pathlib
import re
import sys

assets = pathlib.Path(sys.argv[1])
report = pathlib.Path(sys.argv[2])
expected = [item for item in sys.argv[3].split(",") if item]
require_probes = sys.argv[4] == "1"
require_hypotheses = sys.argv[5] == "1"

for required in [
    assets / "session_summary.json",
    assets / "session_snapshot.json",
    assets / "task.normalized.json",
    assets / "evidence" / "index.json",
]:
    if not required.exists():
        raise SystemExit(f"missing artifact: {required}")
if not report.exists():
    raise SystemExit(f"missing report: {report}")

index = json.loads((assets / "evidence" / "index.json").read_text())
summary = json.loads((assets / "session_summary.json").read_text())
evidence = index.get("evidence", [])
ids = {item["id"] for item in evidence}
kinds = [item["kind"] for item in evidence]
if not evidence:
    raise SystemExit("evidence index is empty")
if summary.get("evidence_count") != len(evidence):
    raise SystemExit(f"evidence_count mismatch: {summary.get('evidence_count')} vs {len(evidence)}")
for item in evidence:
    for key in ("view_file", "raw_file", "summary_file"):
        path = pathlib.Path(item[key])
        if not path.exists():
            raise SystemExit(f"{key} missing for {item['id']}: {path}")
for kind in expected:
    if kind not in kinds:
        raise SystemExit(f"missing expected evidence kind: {kind}; saw {sorted(set(kinds))}")

report_text = report.read_text()
for heading in ("## Tool Observations", "## Evidence Summary", "## Raw Evidence Index"):
    if heading not in report_text:
        raise SystemExit(f"missing report heading: {heading}")
if "ToolError" in expected and "## Tool Errors" not in report_text:
    raise SystemExit("expected Tool Errors report section")
if require_hypotheses and "## Hypotheses" not in report_text:
    raise SystemExit("expected Hypotheses report section")
if require_probes:
    probes_path = assets / "probes.json"
    if not probes_path.exists():
        raise SystemExit("missing probes.json")
    probes = json.loads(probes_path.read_text()).get("probes", [])
    if not any(item.get("kind") == "breakpoint" for item in probes):
        raise SystemExit("missing breakpoint metadata")
    if not any(item.get("kind") == "watchpoint" for item in probes):
        raise SystemExit("missing watchpoint metadata")
    if not any(item.get("kind") == "catchpoint" for item in probes):
        raise SystemExit("missing catchpoint metadata")
    if not any(item.get("deleted") is True for item in probes):
        raise SystemExit("expected deleted probe metadata")
    if "Probe Hit And On-Hit Evidence" not in report_text:
        raise SystemExit("missing probe hit report section")
if require_hypotheses:
    hypotheses_path = assets / "hypotheses" / "index.json"
    if not hypotheses_path.exists():
        raise SystemExit("missing hypotheses index")
    hypotheses = json.loads(hypotheses_path.read_text())
    statuses = {
        check.get("status")
        for hyp in hypotheses.get("hypotheses", [])
        for check in hyp.get("checks", [])
    }
    for status in ("passed", "failed", "unknown"):
        if status not in statuses:
            raise SystemExit(f"missing hypothesis status: {status}")
report_ids = set(re.findall(r"\bE\d{4}\b", report_text))
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

live_task="$work_dir/live_task.md"
live_report="$work_dir/live-report.md"
live_assets="$work_dir/live-report.assets"
write_task "$live_task" "Real workflow live probe, replay seed, and hypothesis session." "live"

create_live="$("$agent" create "$live_task" --socket "$socket_path" --session W1 --out "$live_report" --assets "$live_assets")"
require_contains "$create_live" '"ok":true'
require_contains "$create_live" '"session_id":"W1"'
require_contains "$create_live" '"state":"stopped"'
require_contains "$create_live" '"signal":"SIGTRAP"'

watch_set="$(run_action W1 '{"action":"watchpoint_set","expression":"g_workflow_value","comment":"watch workflow value mutation","purpose":"real workflow watchpoint","on_hit":{"actions":[{"action":"evaluate","expression":"g_workflow_value"},{"action":"not_a_real_action"},{"action":"backtrace"}],"failure_policy":"stop_on_error","timeout_ms":5000,"max_output_bytes":4096,"max_summary_lines":40}}')"
require_contains "$watch_set" '"ok":true'
require_contains "$watch_set" '"action":"watchpoint_set"'
require_contains "$watch_set" '"watchpoint":"'

break_set="$(run_action W1 '{"action":"breakpoint_set","location":"workflow_breakpoint_site","condition":"value == 11","comment":"break at propagated workflow value","purpose":"real workflow breakpoint","on_hit":{"actions":[{"action":"evaluate","expression":"value"},{"action":"not_a_real_action"},{"action":"backtrace"}],"failure_policy":"continue_on_error","timeout_ms":5000,"max_output_bytes":4096,"max_summary_lines":40}}')"
require_contains "$break_set" '"ok":true'
require_contains "$break_set" '"action":"breakpoint_set"'
require_contains "$break_set" '"breakpoint":"'

stop_break_set="$(run_action W1 '{"action":"breakpoint_set","location":"workflow_stop_policy_site","condition":"value == 11","comment":"stop on on-hit failure in workflow","purpose":"real workflow stop_on_error","on_hit":{"actions":[{"action":"evaluate","expression":"value"},{"action":"not_a_real_action"},{"action":"backtrace"}],"failure_policy":"stop_on_error","timeout_ms":5000,"max_output_bytes":4096,"max_summary_lines":40}}')"
require_contains "$stop_break_set" '"ok":true'
require_contains "$stop_break_set" '"action":"breakpoint_set"'
require_contains "$stop_break_set" '"breakpoint":"'

auto_break_set="$(run_action W1 '{"action":"breakpoint_set","location":"workflow_auto_continue_site","condition":"value == 11","comment":"auto continue after workflow breakpoint","purpose":"real workflow continue_after_hit","on_hit":{"actions":[{"action":"evaluate","expression":"value"}],"failure_policy":"continue_on_error","continue_after_hit":true,"timeout_ms":5000,"max_output_bytes":4096,"max_summary_lines":40}}')"
require_contains "$auto_break_set" '"ok":true'
require_contains "$auto_break_set" '"action":"breakpoint_set"'
require_contains "$auto_break_set" '"breakpoint":"'

catch_set="$(run_action W1 '{"action":"catchpoint_set","event":"syscall","name":"write","comment":"catch workflow write syscall","purpose":"real workflow catchpoint","on_hit":{"actions":[{"action":"threads"}],"failure_policy":"continue_on_error","timeout_ms":5000,"max_output_bytes":4096,"max_summary_lines":40}}')"
require_contains "$catch_set" '"ok":true'
require_contains "$catch_set" '"action":"catchpoint_set"'
require_contains "$catch_set" '"event":"syscall"'
require_contains "$catch_set" '"selector":"write"'
catch_number="$(json_field "$catch_set" "catchpoint")"
if [[ -z "$catch_number" ]]; then
    echo "failed to parse catchpoint number from response: $catch_set" >&2
    exit 1
fi

probe_list="$(run_action W1 '{"action":"probe_list"}')"
require_contains "$probe_list" '"kind":"watchpoint"'
require_contains "$probe_list" '"kind":"breakpoint"'
require_contains "$probe_list" '"kind":"catchpoint"'
require_contains "$probe_list" '"purpose":"real workflow breakpoint"'
require_contains "$probe_list" '"purpose":"real workflow catchpoint"'

watch_hit="$(run_action W1 '{"action":"continue","deadline_ms":30000}')"
require_contains "$watch_hit" '"ok":true'
require_contains "$watch_hit" '"stop_reason":"watchpoint-trigger"'
require_contains "$watch_hit" '"action":"not_a_real_action"'

break_hit="$(run_action W1 '{"action":"continue","deadline_ms":30000}')"
require_contains "$break_hit" '"ok":true'
require_contains "$break_hit" '"stop_reason":"breakpoint-hit"'
require_contains "$break_hit" '"action":"not_a_real_action"'
require_contains "$break_hit" '"action":"backtrace"'

stop_hit="$(run_action W1 '{"action":"continue","deadline_ms":30000}')"
require_contains "$stop_hit" '"ok":true'
require_contains "$stop_hit" '"stop_reason":"breakpoint-hit"'
require_contains "$stop_hit" '"action":"not_a_real_action"'

backtrace_response="$(run_action W1 '{"action":"backtrace"}')"
require_gdb_action_response "$backtrace_response" "backtrace"
locals_response="$(run_action W1 '{"action":"locals"}')"
require_gdb_action_response "$locals_response" "locals"
evaluate_response="$(run_action W1 '{"action":"evaluate","expression":"g_workflow_value"}')"
require_gdb_action_response "$evaluate_response" "evaluate"
threads_response="$(run_action W1 '{"action":"threads"}')"
require_gdb_action_response "$threads_response" "threads"
frame_response="$(run_action W1 '{"action":"frame_select","frame":0}')"
require_gdb_action_response "$frame_response" "frame_select"

hypothesis_create="$(run_action W1 '{"action":"hypothesis_create","id":"H-real-workflow-value","title":"workflow value is stable","description":"The workflow fixture should expose a stable global value and current frame value."}')"
require_contains "$hypothesis_create" '"ok":true'

hypothesis_pass="$(run_action W1 '{"action":"hypothesis_check","hypothesis":"H-real-workflow-value","description":"global workflow value reached expected state","expression":"g_workflow_value","assertion":"equals_number","expected":"11"}')"
require_contains "$hypothesis_pass" '"status":"passed"'

hypothesis_fail="$(run_action W1 '{"action":"hypothesis_check","hypothesis":"H-real-workflow-value","description":"global workflow value is not stale","expression":"g_workflow_value","assertion":"equals_number","expected":"10"}')"
require_contains "$hypothesis_fail" '"status":"failed"'

hypothesis_unknown="$(run_action W1 '{"action":"hypothesis_check","hypothesis":"H-real-workflow-value","description":"unknown assertion remains auditable","expression":"g_workflow_value","assertion":"not_supported_by_tool","expected":"11"}')"
require_contains "$hypothesis_unknown" '"status":"unknown"'
require_contains "$hypothesis_unknown" '"error_evidence":"'

hypothesis_conclude="$(run_action W1 '{"action":"hypothesis_conclude","hypothesis":"H-real-workflow-value","conclusion":"Workflow value path was verified by fixture checks","inference":"Agent inference remains separate from tool observations."}')"
require_contains "$hypothesis_conclude" '"ok":true'

auto_and_catch_hit="$(run_action W1 '{"action":"continue","deadline_ms":30000}')"
require_contains "$auto_and_catch_hit" '"ok":true'
require_contains "$auto_and_catch_hit" '"stop_reason":"syscall-entry"'

delete_catch="$(run_action W1 "{\"action\":\"probe_delete\",\"number\":$catch_number}")"
require_contains "$delete_catch" '"ok":true'
require_contains "$delete_catch" '"action":"probe_delete"'
require_contains "$delete_catch" "\"number\":$catch_number"

save_named_action W1 '{"action":"backtrace"}' real-workflow-stop stop_on_error >/dev/null
save_named_action W1 '{"action":"not_a_real_action"}' real-workflow-stop stop_on_error >/dev/null
save_named_action W1 '{"action":"locals"}' real-workflow-stop stop_on_error >/dev/null
replay_plan="$live_assets/replay/real-workflow-stop.json"
require_file "$replay_plan"

finish_live="$("$agent" finish W1 --socket "$socket_path" --out "$live_report")"
require_contains "$finish_live" '"ok":true'
require_file "$live_report"
require_file "$live_assets/probes.json"
require_file "$live_assets/hypotheses/index.json"
grep -F '"deleted": true' "$live_assets/probes.json" >/dev/null
grep -F '"kind":"WatchpointHit"' "$live_assets/evidence/index.json" >/dev/null
grep -F '"kind":"BreakpointHit"' "$live_assets/evidence/index.json" >/dev/null
grep -F '"kind":"CatchpointHit"' "$live_assets/evidence/index.json" >/dev/null
grep -F '"kind":"OnHitAction"' "$live_assets/evidence/index.json" >/dev/null
grep -R -F '"status": "skipped"' "$live_assets/evidence" >/dev/null
grep -R -F '"action_name": "continue_after_hit"' "$live_assets/evidence" >/dev/null
grep -F '"status": "passed"' "$live_assets/hypotheses/index.json" >/dev/null
grep -F '"status": "failed"' "$live_assets/hypotheses/index.json" >/dev/null
grep -F '"status": "unknown"' "$live_assets/hypotheses/index.json" >/dev/null
grep -F 'Final agent conclusion: `Workflow value path was verified by fixture checks`' "$live_report" >/dev/null
artifact_check "$live_assets" "$live_report" "BreakpointHit,WatchpointHit,CatchpointHit,OnHitAction,GdbCommand,ToolError" 1 1

replay_report="$work_dir/replay-report.md"
replay_assets="$work_dir/replay-report.assets"
create_replay="$("$agent" create "$live_task" --socket "$socket_path" --session W2 --out "$replay_report" --assets "$replay_assets")"
require_contains "$create_replay" '"ok":true'
require_contains "$create_replay" '"signal":"SIGTRAP"'
replay_response="$("$agent" replay W2 --file "$replay_plan" --socket "$socket_path")"
require_contains "$replay_response" '"ok":false'
require_contains "$replay_response" '"task_metadata_match":true'
require_contains "$replay_response" '"failure_policy":"stop_on_error"'
require_contains "$replay_response" '"action_name":"backtrace"'
require_contains "$replay_response" '"action_name":"not_a_real_action"'
require_contains "$replay_response" '"status":"skipped"'
finish_replay="$("$agent" finish W2 --socket "$socket_path" --out "$replay_report")"
require_contains "$finish_replay" '"ok":true'
grep -F '"kind":"ReplayStep"' "$replay_assets/evidence/index.json" >/dev/null
grep -F '"kind":"ReplayRun"' "$replay_assets/evidence/index.json" >/dev/null
grep -F '"kind":"ToolError"' "$replay_assets/evidence/index.json" >/dev/null
grep -F '"replay_step_count": 3' "$replay_assets/session_summary.json" >/dev/null
grep -F 'Replay step a3 skipped' "$replay_report" >/dev/null
artifact_check "$replay_assets" "$replay_report" "ReplayStep,ReplayRun,ToolError" 0 0

core_file="$work_dir/workflow.core"
core_gdb_log="$work_dir/core-gdb.log"
if ! gdb --batch -q \
        -ex 'set debuginfod enabled off' \
        -ex 'set confirm off' \
        -ex 'break workflow_core_capture' \
        -ex 'run core' \
        -ex "generate-core-file $core_file" \
        -ex 'quit' \
        --args "$target" core >"$core_gdb_log" 2>&1; then
    echo "skip: failed to generate workflow core file with gdb"
    cat "$core_gdb_log" >&2 || true
    exit 0
fi

if [[ ! -s "$core_file" ]]; then
    echo "skip: gdb did not produce a usable workflow core file"
    cat "$core_gdb_log" >&2 || true
    exit 0
fi

core_task="$work_dir/core_task.md"
core_report="$work_dir/core-report.md"
core_assets="$work_dir/core-report.assets"
write_task "$core_task" "Real workflow core static evidence session." "core" "$core_file"
create_core="$("$agent" create "$core_task" --socket "$socket_path" --session WC --out "$core_report" --assets "$core_assets")"
require_contains "$create_core" '"ok":true'
require_contains "$create_core" '"mode":"core"'
require_contains "$create_core" '"stop_reason":"core_loaded"'
for payload in \
    '{"action":"backtrace"}' \
    '{"action":"threads"}' \
    '{"action":"locals"}' \
    '{"action":"evaluate","expression":"g_workflow_value"}'; do
    response="$(run_action WC "$payload")"
    action_name="$(printf '%s' "$payload" | sed -n 's/.*"action":"\([^"]*\)".*/\1/p')"
    require_gdb_action_response "$response" "$action_name"
done
for payload in \
    '{"action":"continue"}' \
    '{"action":"breakpoint_set","location":"workflow_breakpoint_site"}' \
    '{"action":"watchpoint_set","expression":"g_workflow_value"}' \
    '{"action":"catchpoint_set","event":"syscall","name":"write"}' \
    '{"action":"probe_delete","number":1}'; do
    response="$(run_action WC "$payload")"
    require_contains "$response" '"ok":false'
    require_contains "$response" 'is not available in core mode'
    require_contains "$response" '"evidence":"'
done
finish_core="$("$agent" finish WC --socket "$socket_path" --out "$core_report")"
require_contains "$finish_core" '"ok":true'
grep -F '"mode": "core"' "$core_assets/session_summary.json" >/dev/null
grep -F '"core_loaded": true' "$core_assets/session_summary.json" >/dev/null
grep -F -- '- Mode: Core Dump' "$core_report" >/dev/null
artifact_check "$core_assets" "$core_report" "SessionEvent,GdbCommand,ToolError" 0 0

shutdown_response="$("$agent" shutdown --socket "$socket_path")"
require_contains "$shutdown_response" '"ok":true'
wait "$daemon_pid" >/dev/null 2>&1 || true
daemon_pid=""

echo "smoke ok: real workflow live probe/on-hit, replay, core, report, and evidence links passed"
