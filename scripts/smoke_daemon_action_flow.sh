#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
build_dir="$repo_root/build"
agent="$build_dir/gdb-agent"
task_file="$repo_root/examples/segfault_task.md"

if [[ "${GDB_AGENT_FORCE_LIVE_TEST:-0}" != "1" && "$(uname -s)" != "Linux" ]]; then
    echo "skip: daemon/action live smoke requires Linux + GDB"
    exit 0
fi

if ! command -v gdb >/dev/null 2>&1; then
    echo "skip: gdb is not installed"
    exit 0
fi

if [[ ! -x "$agent" || ! -x "$build_dir/segfault" ]]; then
    echo "missing build artifacts; run: cmake -S . -B build && cmake --build build" >&2
    exit 1
fi

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/gdb-agent-daemon-smoke.XXXXXX")"
socket_path="$work_dir/gdb-agent.sock"
report_path="$work_dir/report.md"
assets_dir="$work_dir/report.assets"
report_path_replay="$work_dir/replay-report.md"
assets_dir_replay="$work_dir/replay-report.assets"
report_path_continue="$work_dir/continue-report.md"
assets_dir_continue="$work_dir/continue-report.assets"
daemon_log="$work_dir/daemon.log"
daemon_pid=""

cleanup() {
    if [[ -n "$daemon_pid" ]] && kill -0 "$daemon_pid" >/dev/null 2>&1; then
        "$agent" shutdown --socket "$socket_path" >/dev/null 2>&1 || true
        wait "$daemon_pid" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

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

create_response="$("$agent" create "$task_file" --socket "$socket_path" --session S1 --out "$report_path" --assets "$assets_dir")"
require_contains "$create_response" '"ok":true'
require_contains "$create_response" '"session_id":"S1"'

status_response="$("$agent" status S1 --socket "$socket_path")"
require_contains "$status_response" '"ok":true'
require_contains "$status_response" '"session_id":"S1"'

backtrace_response="$("$agent" action S1 '{"action":"backtrace"}' --socket "$socket_path")"
require_contains "$backtrace_response" '"ok":true'
require_contains "$backtrace_response" '"action":"backtrace"'
require_contains "$backtrace_response" '"evidence":"'

args_response="$("$agent" action S1 '{"action":"args_info"}' --socket "$socket_path")"
require_contains "$args_response" '"ok":true'
require_contains "$args_response" '"action":"args_info"'
require_contains "$args_response" '"evidence":"'

catch_response="$("$agent" action S1 '{"action":"catchpoint_set","event":"throw","comment":"stop on C++ throw","purpose":"daemon smoke"}' --socket "$socket_path")"
require_contains "$catch_response" '"ok":true'
require_contains "$catch_response" '"action":"catchpoint_set"'
require_contains "$catch_response" '"event":"throw"'
require_contains "$catch_response" '"evidence":"'

probe_response="$("$agent" action S1 '{"action":"probe_list"}' --socket "$socket_path")"
require_contains "$probe_response" '"ok":true'
require_contains "$probe_response" '"kind":"catchpoint"'
require_contains "$probe_response" '"event":"throw"'

invalid_response="$("$agent" action S1 '{"action":"catchpoint_set","event":"not-real"}' --socket "$socket_path")"
require_contains "$invalid_response" '"ok":false'
require_contains "$invalid_response" '"action":"catchpoint_set"'
require_contains "$invalid_response" '"error":"unsupported catchpoint event"'
require_contains "$invalid_response" '"evidence":"'

on_hit_breakpoint_response="$("$agent" action S1 '{"action":"breakpoint_set","location":"examples/segfault.cpp:14","comment":"on-hit smoke breakpoint","purpose":"verify on-hit policy evidence","on_hit":{"actions":[{"action":"evaluate","expression":"session"},{"action":"not_a_real_action"},{"action":"backtrace"}],"timeout_ms":5000,"max_output_bytes":4096,"max_summary_lines":40,"failure_policy":"stop_on_error","continue_after_hit":false}}' --socket "$socket_path")"
require_contains "$on_hit_breakpoint_response" '"ok":true'
require_contains "$on_hit_breakpoint_response" '"action":"breakpoint_set"'
require_contains "$on_hit_breakpoint_response" '"breakpoint":"'

on_hit_run_response="$("$agent" action S1 '{"action":"run","deadline_ms":30000}' --socket "$socket_path")"
require_contains "$on_hit_run_response" '"ok":true'
require_contains "$on_hit_run_response" '"action":"evaluate"'
require_contains "$on_hit_run_response" '"action":"not_a_real_action"'
require_contains "$on_hit_run_response" '"error":"unsupported action"'
require_contains "$on_hit_run_response" '"action":"run"'
require_contains "$on_hit_run_response" '"stop_reason":"breakpoint-hit"'

hypothesis_create_response="$("$agent" action S1 '{"action":"hypothesis_create","id":"H-smoke-null-session","title":"session pointer is null","description":"Verify the breakpoint stop exposes a null session pointer before read_session_value."}' --socket "$socket_path")"
require_contains "$hypothesis_create_response" '"ok":true'
require_contains "$hypothesis_create_response" '"action":"hypothesis_create"'
require_contains "$hypothesis_create_response" '"id":"H-smoke-null-session"'

hypothesis_pass_response="$("$agent" action S1 '{"action":"hypothesis_check","hypothesis":"H-smoke-null-session","description":"session is null at handle_request","expression":"session","assertion":"is_null"}' --socket "$socket_path")"
require_contains "$hypothesis_pass_response" '"ok":true'
require_contains "$hypothesis_pass_response" '"action":"hypothesis_check"'
require_contains "$hypothesis_pass_response" '"check_id":"C1"'
require_contains "$hypothesis_pass_response" '"status":"passed"'
require_contains "$hypothesis_pass_response" '"evidence":"'

hypothesis_fail_response="$("$agent" action S1 '{"action":"hypothesis_check","hypothesis":"H-smoke-null-session","description":"session is not non-null","expression":"session","assertion":"non_null"}' --socket "$socket_path")"
require_contains "$hypothesis_fail_response" '"ok":true'
require_contains "$hypothesis_fail_response" '"action":"hypothesis_check"'
require_contains "$hypothesis_fail_response" '"check_id":"C2"'
require_contains "$hypothesis_fail_response" '"status":"failed"'

hypothesis_unknown_response="$("$agent" action S1 '{"action":"hypothesis_check","hypothesis":"H-smoke-null-session","description":"unsupported assertion stays unknown","expression":"session","assertion":"unsupported_assertion"}' --socket "$socket_path")"
require_contains "$hypothesis_unknown_response" '"ok":true'
require_contains "$hypothesis_unknown_response" '"action":"hypothesis_check"'
require_contains "$hypothesis_unknown_response" '"check_id":"C3"'
require_contains "$hypothesis_unknown_response" '"status":"unknown"'
require_contains "$hypothesis_unknown_response" '"error_evidence":"'

hypothesis_conclude_response="$("$agent" action S1 '{"action":"hypothesis_conclude","hypothesis":"H-smoke-null-session","conclusion":"Supported by current checks","inference":"The tool observations show session is null at the breakpoint; the unsupported assertion is recorded separately as unknown."}' --socket "$socket_path")"
require_contains "$hypothesis_conclude_response" '"ok":true'
require_contains "$hypothesis_conclude_response" '"action":"hypothesis_conclude"'
require_contains "$hypothesis_conclude_response" '"conclusion":"Supported by current checks"'

save_response="$("$agent" save-action S1 '{"action":"backtrace"}' --name smoke-replay --failure-policy stop_on_error --socket "$socket_path")"
require_contains "$save_response" '"ok":true'
require_contains "$save_response" '"action":"save_action"'
require_contains "$save_response" '"failure_policy":"stop_on_error"'
require_contains "$save_response" '"plan":"'

finish_response="$("$agent" finish S1 --socket "$socket_path" --out "$report_path")"
require_contains "$finish_response" '"ok":true'
require_contains "$finish_response" '"report":"'
require_contains "$finish_response" '"assets":"'

require_file "$report_path"
require_file "$assets_dir/session_snapshot.json"
require_file "$assets_dir/session_summary.json"
require_file "$assets_dir/evidence/index.json"
require_file "$assets_dir/probes.json"
require_file "$assets_dir/replay/smoke-replay.json"
require_file "$assets_dir/hypotheses/index.json"
require_file "$assets_dir/hypotheses/H-smoke-null-session.md"

grep -F '"kind":"ToolError"' "$assets_dir/evidence/index.json" >/dev/null
grep -F '"kind":"BreakpointHit"' "$assets_dir/evidence/index.json" >/dev/null
grep -F '"kind":"OnHitAction"' "$assets_dir/evidence/index.json" >/dev/null
grep -F '"kind": "catchpoint"' "$assets_dir/probes.json" >/dev/null
grep -F '"event": "throw"' "$assets_dir/probes.json" >/dev/null
grep -F '"on_hit": {' "$assets_dir/probes.json" >/dev/null
grep -F '"failure_policy":"stop_on_error"' "$assets_dir/probes.json" >/dev/null
grep -F '"probe_hit_count": 1' "$assets_dir/session_summary.json" >/dev/null
grep -F '"on_hit_action_count": 3' "$assets_dir/session_summary.json" >/dev/null
grep -F '"on_hit_error_count": 1' "$assets_dir/session_summary.json" >/dev/null
grep -R -F '"on_hit_policy"' "$assets_dir/evidence" >/dev/null
grep -R -F '"on_hit_results"' "$assets_dir/evidence" >/dev/null
grep -R -F '"status": "skipped"' "$assets_dir/evidence" >/dev/null
grep -R -F '"action_evidence_ids"' "$assets_dir/evidence" >/dev/null
grep -F 'Probe Hit And On-Hit Evidence' "$report_path" >/dev/null
grep -F '"schema": "gdb-agent-replay-plan-v1"' "$assets_dir/replay/smoke-replay.json" >/dev/null
grep -F '"failure_policy": "stop_on_error"' "$assets_dir/replay/smoke-replay.json" >/dev/null
grep -F '"fingerprint":' "$assets_dir/replay/smoke-replay.json" >/dev/null
grep -F '"schema": "gdb-agent-hypotheses-v1"' "$assets_dir/hypotheses/index.json" >/dev/null
grep -F '"id": "H-smoke-null-session"' "$assets_dir/hypotheses/index.json" >/dev/null
grep -F '"check_id": "C1"' "$assets_dir/hypotheses/index.json" >/dev/null
grep -F '"status": "passed"' "$assets_dir/hypotheses/index.json" >/dev/null
grep -F '"status": "failed"' "$assets_dir/hypotheses/index.json" >/dev/null
grep -F '"status": "unknown"' "$assets_dir/hypotheses/index.json" >/dev/null
grep -F '"observed":' "$assets_dir/hypotheses/index.json" >/dev/null
grep -F '"error_evidence": "E' "$assets_dir/hypotheses/index.json" >/dev/null
grep -F '## Hypotheses' "$report_path" >/dev/null
grep -F 'H-smoke-null-session session pointer is null' "$report_path" >/dev/null
grep -F '| `C1` | session is null at handle_request' "$report_path" >/dev/null
grep -F 'Final agent conclusion: `Supported by current checks`' "$report_path" >/dev/null

create_replay_response="$("$agent" create "$task_file" --socket "$socket_path" --session S2 --out "$report_path_replay" --assets "$assets_dir_replay")"
require_contains "$create_replay_response" '"ok":true'
require_contains "$create_replay_response" '"session_id":"S2"'

replay_response="$("$agent" replay S2 --file "$assets_dir/replay/smoke-replay.json" --socket "$socket_path")"
require_contains "$replay_response" '"ok":true'
require_contains "$replay_response" '"action":"replay"'
require_contains "$replay_response" '"plan":"smoke-replay"'
require_contains "$replay_response" '"task_metadata_match":true'
require_contains "$replay_response" '"failure_policy":"stop_on_error"'
require_contains "$replay_response" '"status":"success"'
require_contains "$replay_response" '"evidence":"'

finish_replay_response="$("$agent" finish S2 --socket "$socket_path" --out "$report_path_replay")"
require_contains "$finish_replay_response" '"ok":true'

require_file "$report_path_replay"
require_file "$assets_dir_replay/session_summary.json"
require_file "$assets_dir_replay/evidence/index.json"

grep -F '"kind":"ReplayStep"' "$assets_dir_replay/evidence/index.json" >/dev/null
grep -F '"replay_step_count": 1' "$assets_dir_replay/session_summary.json" >/dev/null
grep -F 'Replay step a1 success' "$report_path_replay" >/dev/null

create_continue_response="$("$agent" create "$task_file" --socket "$socket_path" --session S3 --out "$report_path_continue" --assets "$assets_dir_continue")"
require_contains "$create_continue_response" '"ok":true'
require_contains "$create_continue_response" '"session_id":"S3"'

continue_breakpoint_response="$("$agent" action S3 '{"action":"breakpoint_set","location":"examples/segfault.cpp:14","comment":"auto continue smoke breakpoint","purpose":"verify continue_after_hit","on_hit":{"actions":[{"action":"evaluate","expression":"session"}],"timeout_ms":5000,"max_output_bytes":4096,"max_summary_lines":40,"failure_policy":"continue_on_error","continue_after_hit":true}}' --socket "$socket_path")"
require_contains "$continue_breakpoint_response" '"ok":true'

continue_run_response="$("$agent" action S3 '{"action":"run","deadline_ms":30000}' --socket "$socket_path")"
require_contains "$continue_run_response" '"ok":true'
require_contains "$continue_run_response" '"action":"continue"'
require_contains "$continue_run_response" '"signal":"SIGSEGV"'

finish_continue_response="$("$agent" finish S3 --socket "$socket_path" --out "$report_path_continue")"
require_contains "$finish_continue_response" '"ok":true'

require_file "$report_path_continue"
require_file "$assets_dir_continue/session_summary.json"
require_file "$assets_dir_continue/evidence/index.json"
grep -F '"probe_hit_count": 1' "$assets_dir_continue/session_summary.json" >/dev/null
grep -F '"on_hit_action_count": 2' "$assets_dir_continue/session_summary.json" >/dev/null
grep -F '"on_hit_error_count": 0' "$assets_dir_continue/session_summary.json" >/dev/null
grep -R -F '"action_name": "continue_after_hit"' "$assets_dir_continue/evidence" >/dev/null
grep -R -F '"continue_after_hit":true' "$assets_dir_continue/evidence" >/dev/null

shutdown_response="$("$agent" shutdown --socket "$socket_path")"
require_contains "$shutdown_response" '"ok":true'
wait "$daemon_pid" >/dev/null 2>&1 || true
daemon_pid=""

echo "smoke ok: daemon/action flow, catchpoint_set, on-hit policy, and restart replay passed"
