#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
build_dir="$repo_root/build"
agent="$build_dir/gdb-agent"
target="$build_dir/segfault"
task_file="$repo_root/examples/segfault_task.md"

if [[ "${GDB_AGENT_FORCE_LIVE_TEST:-0}" != "1" && "$(uname -s)" != "Linux" ]]; then
    echo "skip: edge-case smoke requires Linux + GDB"
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

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/gdb-agent-edge-smoke.XXXXXX")"
socket_path="$work_dir/gdb-agent.sock"
daemon_log="$work_dir/daemon.log"
weaknesses="$work_dir/weaknesses.txt"
report_path="$work_dir/report.md"
assets_dir="$work_dir/report.assets"
replay_report="$work_dir/replay-report.md"
replay_assets="$work_dir/replay-report.assets"
close_report="$work_dir/close-report.md"
close_assets="$work_dir/close-report.assets"
core_report="$work_dir/core-report.md"
core_assets="$work_dir/core-report.assets"
core_file="$work_dir/segfault.core"
core_task="$work_dir/core_task.md"
alt_task="$work_dir/alt_task.md"
gdb_log="$work_dir/gdb-gcore.log"
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
    echo "edge-case smoke failed at line $line with exit code $code" >&2
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

require_not_contains() {
    local text="$1"
    local unexpected="$2"
    if [[ "$text" == *"$unexpected"* ]]; then
        echo "expected response not to contain: $unexpected" >&2
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

require_action_error() {
    local response="$1"
    local expected_error="$2"
    require_contains "$response" '"ok":false'
    require_contains "$response" "$expected_error"
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

missing_status="$("$agent" status DOES_NOT_EXIST --socket "$socket_path")"
require_action_error "$missing_status" '"message":"session not found"'

missing_action="$("$agent" action DOES_NOT_EXIST '{"action":"backtrace"}' --socket "$socket_path")"
require_action_error "$missing_action" '"message":"session not found"'

missing_finish="$("$agent" finish DOES_NOT_EXIST --socket "$socket_path" --out "$work_dir/missing.md")"
require_action_error "$missing_finish" '"message":"session not found"'

missing_close="$("$agent" close DOES_NOT_EXIST --socket "$socket_path")"
require_action_error "$missing_close" '"message":"session not found"'

create_response="$("$agent" create "$task_file" --socket "$socket_path" --session S1 --out "$report_path" --assets "$assets_dir")"
require_contains "$create_response" '"ok":true'
require_contains "$create_response" '"session_id":"S1"'

invalid_json_stdout="$work_dir/invalid-json.out"
invalid_json_stderr="$work_dir/invalid-json.err"
if "$agent" action S1 '{not-json' --socket "$socket_path" >"$invalid_json_stdout" 2>"$invalid_json_stderr"; then
    echo "invalid JSON action unexpectedly succeeded" >&2
    exit 1
fi
grep -F 'error:' "$invalid_json_stderr" >/dev/null

missing_action_response="$("$agent" action S1 '{}' --socket "$socket_path")"
require_action_error "$missing_action_response" '"error":"missing action"'
require_contains "$missing_action_response" '"evidence":"'

missing_eval="$("$agent" action S1 '{"action":"evaluate"}' --socket "$socket_path")"
require_action_error "$missing_eval" '"error":"missing expression"'
require_contains "$missing_eval" '"action":"evaluate"'
require_contains "$missing_eval" '"evidence":"'

missing_breakpoint="$("$agent" action S1 '{"action":"breakpoint_set"}' --socket "$socket_path")"
require_action_error "$missing_breakpoint" '"error":"missing location"'
require_contains "$missing_breakpoint" '"action":"breakpoint_set"'
require_contains "$missing_breakpoint" '"evidence":"'

missing_watchpoint="$("$agent" action S1 '{"action":"watchpoint_set"}' --socket "$socket_path")"
require_action_error "$missing_watchpoint" '"error":"missing expression"'
require_contains "$missing_watchpoint" '"action":"watchpoint_set"'
require_contains "$missing_watchpoint" '"evidence":"'

raw_without_risk="$("$agent" action S1 '{"action":"raw_mi","command":"-gdb-version"}' --socket "$socket_path")"
require_action_error "$raw_without_risk" '"error":"raw_mi requires risk=advanced"'
require_contains "$raw_without_risk" '"action":"raw_mi"'
require_contains "$raw_without_risk" '"evidence":"'

frame_negative="$("$agent" action S1 '{"action":"frame_select","frame":-1}' --socket "$socket_path")"
require_action_error "$frame_negative" '"action":"frame_select"'
require_contains "$frame_negative" '"evidence":"'
require_contains "$frame_negative" '"command_evidence":"'

invalid_eval="$("$agent" action S1 '{"action":"evaluate","expression":"definitely_missing_symbol"}' --socket "$socket_path")"
require_action_error "$invalid_eval" '"action":"evaluate"'
require_contains "$invalid_eval" '"evidence":"'
require_contains "$invalid_eval" '"command_evidence":"'

hypothesis_create="$("$agent" action S1 '{"action":"hypothesis_create","id":"H-edge-command-error","title":"command error is not a successful check"}' --socket "$socket_path")"
require_contains "$hypothesis_create" '"ok":true'
hypothesis_command_error="$("$agent" action S1 '{"action":"hypothesis_check","hypothesis":"H-edge-command-error","description":"missing symbol should fail as command error","expression":"definitely_missing_symbol","assertion":"contains","expected":"anything"}' --socket "$socket_path")"
require_action_error "$hypothesis_command_error" '"action":"hypothesis_check"'
require_contains "$hypothesis_command_error" '"evidence":"'
require_contains "$hypothesis_command_error" '"command_evidence":"'

long_inline_payload="$(python3 - <<'PY'
import json
payload = {
    "action": "breakpoint_set",
    "location": "examples/segfault.cpp:14",
    "comment": "long inline json regression " + ("x" * 5000),
    "on_hit": {
        "actions": [{"action": "evaluate", "expression": "session"} for _ in range(12)],
        "failure_policy": "continue_on_error",
        "timeout_ms": 5000,
        "max_output_bytes": 4096,
        "max_summary_lines": 40,
    },
}
print(json.dumps(payload, separators=(",", ":")))
PY
)"
long_inline_response="$("$agent" action S1 "$long_inline_payload" --socket "$socket_path")"
require_contains "$long_inline_response" '"ok":true'
require_contains "$long_inline_response" '"action":"breakpoint_set"'

on_hit_raw="$("$agent" action S1 '{"action":"breakpoint_set","location":"examples/segfault.cpp:14","on_hit":{"actions":[{"action":"raw_mi","command":"-gdb-version","risk":"advanced"}]}}' --socket "$socket_path")"
require_action_error "$on_hit_raw" '"error":"invalid on_hit policy"'
require_contains "$on_hit_raw" '"evidence":"'

watchpoint_failure="$("$agent" action S1 '{"action":"watchpoint_set","expression":"definitely_missing_symbol"}' --socket "$socket_path")"
require_action_error "$watchpoint_failure" '"error":"failed to set watchpoint"'
require_contains "$watchpoint_failure" '"evidence":"'

bp_response="$("$agent" action S1 '{"action":"breakpoint_set","location":"examples/segfault.cpp:14","comment":"edge delete check"}' --socket "$socket_path")"
require_contains "$bp_response" '"ok":true'
bp_number="$(printf '%s' "$bp_response" | sed -n 's/.*"breakpoint":"\([^"]*\)".*/\1/p')"
if [[ -z "$bp_number" ]]; then
    echo "failed to extract breakpoint number from: $bp_response" >&2
    exit 1
fi

delete_response="$("$agent" action S1 "{\"action\":\"probe_delete\",\"number\":$bp_number}" --socket "$socket_path")"
require_contains "$delete_response" '"ok":true'

probe_after_delete="$("$agent" action S1 '{"action":"probe_list"}' --socket "$socket_path")"
require_contains "$probe_after_delete" '"ok":true'
require_not_contains "$probe_after_delete" "\"number\":\"$bp_number\""

"$agent" save-action S1 '{"action":"backtrace"}' --name edge-mixed --failure-policy stop_on_error --socket "$socket_path" >/dev/null
"$agent" save-action S1 '{"action":"not_a_real_action"}' --name edge-mixed --failure-policy stop_on_error --socket "$socket_path" >/dev/null
replay_plan="$assets_dir/replay/edge-mixed.json"
require_file "$replay_plan"
grep -F '"failure_policy": "stop_on_error"' "$replay_plan" >/dev/null

finish_response="$("$agent" finish S1 --socket "$socket_path" --out "$report_path")"
require_contains "$finish_response" '"ok":true'

require_file "$report_path"
require_file "$assets_dir/task.normalized.json"
require_file "$assets_dir/session_snapshot.json"
require_file "$assets_dir/session_summary.json"
require_file "$assets_dir/evidence/index.json"
require_file "$assets_dir/probes.json"

python3 - "$assets_dir" "$report_path" <<'PY'
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
for item in evidence:
    for key in ("view_file", "raw_file", "summary_file"):
        path = pathlib.Path(item[key])
        if not path.exists():
            raise SystemExit(f"{key} does not exist for {item['id']}: {path}")
if summary["evidence_count"] != len(evidence):
    raise SystemExit(f"summary evidence_count={summary['evidence_count']} index count={len(evidence)}")
report_ids = set(re.findall(r"\bE\d{4}\b", report.read_text()))
missing = report_ids - ids
if missing:
    raise SystemExit(f"report references evidence ids missing from index: {sorted(missing)}")
probes = json.loads((assets / "probes.json").read_text()).get("probes", [])
if not any(probe.get("deleted") is True for probe in probes):
    raise SystemExit("deleted probe history was not marked in probes.json")
print("artifact consistency ok")
PY

post_finish_action="$("$agent" action S1 '{"action":"backtrace"}' --socket "$socket_path")"
require_action_error "$post_finish_action" '"message":"session not found"'

repeat_finish="$("$agent" finish S1 --socket "$socket_path" --out "$report_path")"
require_action_error "$repeat_finish" '"message":"session not found"'

cat >"$alt_task" <<TASK
### problem

Same executable with different args to force replay task fingerprint mismatch.

### executable

$target

### working directory

$repo_root

### args

--edge-mismatch

### core dump

TASK

create_replay="$("$agent" create "$alt_task" --socket "$socket_path" --session S2 --out "$replay_report" --assets "$replay_assets")"
require_contains "$create_replay" '"ok":true'

replay_reject="$("$agent" replay S2 --file "$replay_plan" --socket "$socket_path")"
require_contains "$replay_reject" '"ok":false'
require_contains "$replay_reject" '"task_metadata_match":false'
require_contains "$replay_reject" 'replay plan task fingerprint mismatch'

replay_force="$("$agent" replay S2 --file "$replay_plan" --force --socket "$socket_path")"
require_contains "$replay_force" '"force":true'
require_contains "$replay_force" '"task_metadata_match":false'
require_contains "$replay_force" '"status":"failed"'
require_contains "$replay_force" '"action_name":"not_a_real_action"'
require_contains "$replay_force" '"failure_policy":"stop_on_error"'

finish_replay="$("$agent" finish S2 --socket "$socket_path" --out "$replay_report")"
require_contains "$finish_replay" '"ok":true'
require_file "$replay_assets/session_summary.json"
require_file "$replay_assets/evidence/index.json"
grep -F '"replay_step_count": 2' "$replay_assets/session_summary.json" >/dev/null
grep -F '"replay_warning_count": 1' "$replay_assets/session_summary.json" >/dev/null
grep -F '"kind":"ReplayWarning"' "$replay_assets/evidence/index.json" >/dev/null
grep -F '"kind":"ReplayStep"' "$replay_assets/evidence/index.json" >/dev/null

create_close="$("$agent" create "$task_file" --socket "$socket_path" --session CLOSE1 --out "$close_report" --assets "$close_assets")"
require_contains "$create_close" '"ok":true'
close_once="$("$agent" close CLOSE1 --socket "$socket_path")"
require_contains "$close_once" '"ok":true'
require_contains "$close_once" '"message":"closed"'
close_twice="$("$agent" close CLOSE1 --socket "$socket_path")"
require_action_error "$close_twice" '"message":"session not found"'
close_action="$("$agent" action CLOSE1 '{"action":"backtrace"}' --socket "$socket_path")"
require_action_error "$close_action" '"message":"session not found"'

if ! gdb --batch -q \
        -ex 'set debuginfod enabled off' \
        -ex 'set confirm off' \
        -ex 'break read_session_value' \
        -ex 'run' \
        -ex "generate-core-file $core_file" \
        -ex 'quit' \
        --args "$target" >"$gdb_log" 2>&1; then
    echo "skip: failed to generate core file with gdb"
    cat "$gdb_log" >&2 || true
    exit 0
fi

if [[ ! -s "$core_file" ]]; then
    echo "skip: gdb did not produce a usable core file"
    cat "$gdb_log" >&2 || true
    exit 0
fi

cat >"$core_task" <<TASK
### problem

Edge case core mode guard task.

### executable

$target

### working directory

$repo_root

### args


### core dump

$core_file
TASK

create_core="$("$agent" create "$core_task" --socket "$socket_path" --session C1 --out "$core_report" --assets "$core_assets")"
require_contains "$create_core" '"ok":true'
require_contains "$create_core" '"mode":"core"'

core_backtrace="$("$agent" action C1 '{"action":"backtrace"}' --socket "$socket_path")"
require_gdb_action_response "$core_backtrace" "backtrace"

for payload in \
    '{"action":"run"}' \
    '{"action":"continue"}' \
    '{"action":"breakpoint_set","location":"examples/segfault.cpp:14"}' \
    '{"action":"watchpoint_set","expression":"session"}' \
    '{"action":"catchpoint_set","event":"throw"}' \
    '{"action":"probe_enable","number":1}' \
    '{"action":"probe_disable","number":1}' \
    '{"action":"probe_delete","number":1}'; do
    response="$("$agent" action C1 "$payload" --socket "$socket_path")"
    require_contains "$response" '"ok":false'
    require_contains "$response" 'is not available in core mode'
    require_contains "$response" '"evidence":"'
done

finish_core="$("$agent" finish C1 --socket "$socket_path" --out "$core_report")"
require_contains "$finish_core" '"ok":true'
require_file "$core_report"
require_file "$core_assets/session_summary.json"
grep -F '"mode": "core"' "$core_assets/session_summary.json" >/dev/null
grep -F '"kind":"ToolError"' "$core_assets/evidence/index.json" >/dev/null

shutdown_response="$("$agent" shutdown --socket "$socket_path")"
require_contains "$shutdown_response" '"ok":true'
wait "$daemon_pid" >/dev/null 2>&1 || true
daemon_pid=""

if [[ -s "$weaknesses" ]]; then
    echo "edge smoke recorded non-blocking weaknesses:"
    cat "$weaknesses"
fi

echo "smoke ok: edge cases, replay mismatch, core guards, and artifact consistency passed"
