#!/usr/bin/env bash
set -Eeuo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
build_dir="$repo_root/build"
agent="$build_dir/gdb-agent"
target="$build_dir/workflow_fixture"

if [[ "${GDB_AGENT_FORCE_LIVE_TEST:-0}" != "1" && "$(uname -s)" != "Linux" ]]; then
    echo "skip: replay setup plan smoke requires Linux + GDB"
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

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/gdb-agent-replay-setup.XXXXXX")"
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
    echo "replay setup plan smoke failed at line $line with exit code $code" >&2
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

continue_until_watchpoint() {
    local session="$1"
    local response="${2:-}"
    for _ in {1..20}; do
        if [[ -z "$response" ]]; then
            response="$(run_action "$session" '{"action":"continue","deadline_ms":30000}')"
        fi
        require_contains "$response" '"ok":true'
        if [[ "$response" == *'"stop_reason":"watchpoint-trigger"'* ]]; then
            printf '%s\n' "$response"
            return 0
        fi
        if [[ "$response" == *'"signal":"SIGTRAP"'* ||
              "$response" == *'"stop_reason":"syscall-entry"'* ||
              "$response" == *'"stop_reason":"syscall-return"'* ]]; then
            response=""
            continue
        fi
        echo "unexpected stop while waiting for replay setup watchpoint:" >&2
        echo "$response" >&2
        exit 1
    done
    echo "timed out waiting for replay setup watchpoint" >&2
    exit 1
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
    local expected_kinds="$3"
    python3 - "$assets" "$report" "$expected_kinds" <<'PY'
import json
import pathlib
import re
import sys

assets = pathlib.Path(sys.argv[1])
report = pathlib.Path(sys.argv[2])
expected = [item for item in sys.argv[3].split(",") if item]

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
report_ids = set(re.findall(r"\bE\d{4}\b", report_text))
missing = report_ids - ids
if missing:
    raise SystemExit(f"report references missing evidence ids: {sorted(missing)}")
print("artifact consistency ok")
PY
}

require_replay_audit_report() {
    local report="$1"
    grep -F '## Replay Execution Audit' "$report" >/dev/null
    grep -F '### Replay Runs' "$report" >/dev/null
    grep -F '### Replay Steps' "$report" >/dev/null
    grep -F '| Run Evidence | Plan | File | OK | Force | Task Match | Policy | Warning Evidence | Error Evidence |' "$report" >/dev/null
    grep -F '| Run | Step | Action | Status | Policy | Step Evidence | Action Evidence | Error Evidence | Skip Reason |' "$report" >/dev/null
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

base_task="$work_dir/base_task.md"
base_report="$work_dir/base-report.md"
base_assets="$work_dir/base-report.assets"
write_task "$base_task" "Replay setup plan base task." "live"

create_base="$("$agent" create "$base_task" --socket "$socket_path" --session RS1 --out "$base_report" --assets "$base_assets")"
require_contains "$create_base" '"ok":true'
require_contains "$create_base" '"signal":"SIGTRAP"'

save_named_action RS1 '{"action":"watchpoint_set","expression":"g_breakpoint_node.value","comment":"replay setup watch breakpoint value","purpose":"replay setup watchpoint","on_hit":{"actions":[{"action":"evaluate","expression":"g_breakpoint_node.value"}],"failure_policy":"continue_on_error","timeout_ms":5000}}' replay-probe-setup continue_on_error >/dev/null
save_named_action RS1 '{"action":"breakpoint_set","location":"workflow_breakpoint_site","condition":"value == 11","comment":"replay setup breakpoint","purpose":"replay setup breakpoint","on_hit":{"actions":[{"action":"evaluate","expression":"value"}],"failure_policy":"continue_on_error","timeout_ms":5000}}' replay-probe-setup continue_on_error >/dev/null
save_named_action RS1 '{"action":"catchpoint_set","event":"throw","comment":"replay setup catch throw","purpose":"replay setup catchpoint","on_hit":{"actions":[{"action":"threads"}],"failure_policy":"continue_on_error","timeout_ms":5000}}' replay-probe-setup continue_on_error >/dev/null

save_named_action RS1 '{"action":"backtrace"}' replay-stop-audit stop_on_error >/dev/null
save_named_action RS1 '{"action":"not_a_real_action"}' replay-stop-audit stop_on_error >/dev/null
save_named_action RS1 '{"action":"locals"}' replay-stop-audit stop_on_error >/dev/null

setup_plan="$base_assets/replay/replay-probe-setup.json"
stop_plan="$base_assets/replay/replay-stop-audit.json"
require_file "$setup_plan"
require_file "$stop_plan"
grep -F '"schema": "gdb-agent-replay-plan-v1"' "$setup_plan" >/dev/null
grep -F '"tags": []' "$setup_plan" >/dev/null
grep -F '"source_session_id": "RS1"' "$setup_plan" >/dev/null
grep -F '"fingerprint":' "$setup_plan" >/dev/null
grep -F '"watchpoint_set"' "$setup_plan" >/dev/null
grep -F '"breakpoint_set"' "$setup_plan" >/dev/null
grep -F '"catchpoint_set"' "$setup_plan" >/dev/null

finish_base="$("$agent" finish RS1 --socket "$socket_path" --out "$base_report")"
require_contains "$finish_base" '"ok":true'
grep -F '## Replay Plans' "$base_report" >/dev/null
grep -F '| Plan File | Name | Tags | Source Session | Failure Policy | Task Fingerprint |' "$base_report" >/dev/null
grep -F 'replay-probe-setup' "$base_report" >/dev/null
grep -F 'RS1' "$base_report" >/dev/null
artifact_check "$base_assets" "$base_report" "EnvironmentInfo,StopEvent"

setup_report="$work_dir/setup-report.md"
setup_assets="$work_dir/setup-report.assets"
create_setup="$("$agent" create "$base_task" --socket "$socket_path" --session RS2 --out "$setup_report" --assets "$setup_assets" --replay-before-run "$setup_plan")"
require_contains "$create_setup" '"ok":true'

if [[ "$create_setup" == *'"signal":"SIGTRAP"'* ]]; then
    break_hit="$(run_action RS2 '{"action":"continue","deadline_ms":30000}')"
else
    echo "unexpected create stop for replay-before-run setup session:" >&2
    echo "$create_setup" >&2
    exit 1
fi
require_contains "$break_hit" '"ok":true'
require_contains "$break_hit" '"stop_reason":"breakpoint-hit"'
watch_hit="$(continue_until_watchpoint RS2)"
require_contains "$watch_hit" '"ok":true'
require_contains "$watch_hit" '"stop_reason":"watchpoint-trigger"'
catch_hit="$(run_action RS2 '{"action":"continue","deadline_ms":30000}')"
require_contains "$catch_hit" '"ok":true'
if [[ "$catch_hit" != *'"stop_reason":"breakpoint-hit"'* ]]; then
    echo "expected throw catchpoint stop after replay setup breakpoint:" >&2
    echo "$catch_hit" >&2
    exit 1
fi

finish_setup="$("$agent" finish RS2 --socket "$socket_path" --out "$setup_report")"
require_contains "$finish_setup" '"ok":true'
require_file "$setup_assets/probes.json"
grep -F '"comment": "replay setup watch breakpoint value"' "$setup_assets/probes.json" >/dev/null
grep -F '"purpose": "replay setup breakpoint"' "$setup_assets/probes.json" >/dev/null
grep -F '"event": "throw"' "$setup_assets/probes.json" >/dev/null
grep -F '"kind":"ReplayStep"' "$setup_assets/evidence/index.json" >/dev/null
grep -F '"kind":"ReplayRun"' "$setup_assets/evidence/index.json" >/dev/null
grep -F '"kind":"WatchpointHit"' "$setup_assets/evidence/index.json" >/dev/null
grep -F '"kind":"BreakpointHit"' "$setup_assets/evidence/index.json" >/dev/null
grep -F '"kind":"CatchpointHit"' "$setup_assets/evidence/index.json" >/dev/null
grep -F '"kind":"OnHitAction"' "$setup_assets/evidence/index.json" >/dev/null
python3 - "$setup_assets/session_summary.json" <<'PY'
import json
import pathlib
import sys

summary = json.loads(pathlib.Path(sys.argv[1]).read_text())
if summary.get("probe_hit_count", 0) < 3:
    raise SystemExit(f"expected at least 3 probe hits, saw {summary.get('probe_hit_count')}")
if summary.get("on_hit_action_count", 0) < 3:
    raise SystemExit(f"expected at least 3 on-hit actions, saw {summary.get('on_hit_action_count')}")
PY
require_replay_audit_report "$setup_report"
grep -F 'replay-probe-setup' "$setup_report" >/dev/null
grep -F 'watchpoint_set' "$setup_report" >/dev/null
grep -F 'breakpoint_set' "$setup_report" >/dev/null
grep -F 'catchpoint_set' "$setup_report" >/dev/null
artifact_check "$setup_assets" "$setup_report" "ReplayStep,ReplayRun,WatchpointHit,BreakpointHit,CatchpointHit,OnHitAction"

stop_report="$work_dir/stop-report.md"
stop_assets="$work_dir/stop-report.assets"
create_stop="$("$agent" create "$base_task" --socket "$socket_path" --session RS3 --out "$stop_report" --assets "$stop_assets")"
require_contains "$create_stop" '"ok":true'
replay_stop="$("$agent" replay RS3 --file "$stop_plan" --socket "$socket_path")"
require_contains "$replay_stop" '"ok":false'
require_contains "$replay_stop" '"task_metadata_match":true'
require_contains "$replay_stop" '"failure_policy":"stop_on_error"'
require_contains "$replay_stop" '"action_name":"backtrace"'
require_contains "$replay_stop" '"action_name":"not_a_real_action"'
require_contains "$replay_stop" '"status":"skipped"'
finish_stop="$("$agent" finish RS3 --socket "$socket_path" --out "$stop_report")"
require_contains "$finish_stop" '"ok":true'
grep -F '"replay_step_count": 3' "$stop_assets/session_summary.json" >/dev/null
grep -F '"kind":"ToolError"' "$stop_assets/evidence/index.json" >/dev/null
require_replay_audit_report "$stop_report"
grep -F '`not_a_real_action` | `failed`' "$stop_report" >/dev/null
grep -F '`locals` | `skipped`' "$stop_report" >/dev/null
grep -F 'previous step a2 failed under stop_on_error' "$stop_report" >/dev/null
artifact_check "$stop_assets" "$stop_report" "ReplayStep,ReplayRun,ToolError"

mismatch_task="$work_dir/mismatch_task.md"
mismatch_report="$work_dir/mismatch-report.md"
mismatch_assets="$work_dir/mismatch-report.assets"
write_task "$mismatch_task" "Replay setup plan fingerprint mismatch task." "live"
create_mismatch="$("$agent" create "$mismatch_task" --socket "$socket_path" --session RS4 --out "$mismatch_report" --assets "$mismatch_assets")"
require_contains "$create_mismatch" '"ok":true'
replay_reject="$("$agent" replay RS4 --file "$setup_plan" --socket "$socket_path")"
require_contains "$replay_reject" '"ok":false'
require_contains "$replay_reject" '"task_metadata_match":false'
require_contains "$replay_reject" '"error_evidence":"'
replay_force="$("$agent" replay RS4 --file "$setup_plan" --force --socket "$socket_path")"
require_contains "$replay_force" '"force":true'
require_contains "$replay_force" '"task_metadata_match":false'
require_contains "$replay_force" '"warning_evidence":"'
finish_mismatch="$("$agent" finish RS4 --socket "$socket_path" --out "$mismatch_report")"
require_contains "$finish_mismatch" '"ok":true'
grep -F '"kind":"ReplayWarning"' "$mismatch_assets/evidence/index.json" >/dev/null
grep -F '"kind":"ToolError"' "$mismatch_assets/evidence/index.json" >/dev/null
grep -F '"replay_warning_count": 1' "$mismatch_assets/session_summary.json" >/dev/null
require_replay_audit_report "$mismatch_report"
grep -F '### Replay Warnings' "$mismatch_report" >/dev/null
grep -F 'force replay accepted despite replay plan task fingerprint mismatch' "$mismatch_report" >/dev/null
artifact_check "$mismatch_assets" "$mismatch_report" "ReplayWarning,ReplayStep,ReplayRun,ToolError"

shutdown_response="$("$agent" shutdown --socket "$socket_path")"
require_contains "$shutdown_response" '"ok":true'
wait "$daemon_pid" >/dev/null 2>&1 || true
daemon_pid=""

echo "smoke ok: replay setup plan, replay-before-run, report audit, and mismatch handling passed"
