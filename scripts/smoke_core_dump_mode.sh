#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
build_dir="$repo_root/build"
agent="$build_dir/gdb-agent"
target="$build_dir/segfault"

if [[ "${GDB_AGENT_FORCE_LIVE_TEST:-0}" != "1" && "$(uname -s)" != "Linux" ]]; then
    echo "skip: core dump smoke requires Linux + GDB"
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

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/gdb-agent-core-smoke.XXXXXX")"
socket_path="$work_dir/gdb-agent.sock"
daemon_log="$work_dir/daemon.log"
gdb_log="$work_dir/gdb-gcore.log"
core_file="$work_dir/segfault.core"
task_file="$work_dir/core_task.md"
report_path="$work_dir/core-report.md"
assets_dir="$work_dir/core-report.assets"
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

artifact_check() {
    python3 - "$assets_dir" "$report_path" "$core_file" <<'PY'
import json
import pathlib
import re
import sys

assets = pathlib.Path(sys.argv[1])
report = pathlib.Path(sys.argv[2])
core = pathlib.Path(sys.argv[3])

required = [
    assets / "task.normalized.json",
    assets / "session_summary.json",
    assets / "session_snapshot.json",
    assets / "evidence" / "index.json",
]
for path in required:
    if not path.exists():
        raise SystemExit(f"missing artifact: {path}")
if not report.exists():
    raise SystemExit(f"missing report: {report}")

summary = json.loads((assets / "session_summary.json").read_text())
task = json.loads((assets / "task.normalized.json").read_text())
index = json.loads((assets / "evidence" / "index.json").read_text())
evidence = index.get("evidence", [])
ids = {item["id"] for item in evidence}
kinds = [item.get("kind") for item in evidence]
titles = [item.get("title") for item in evidence]

if summary.get("mode") != "core":
    raise SystemExit(f"expected core mode summary, saw {summary.get('mode')!r}")
if summary.get("core_dump") != str(core):
    raise SystemExit(f"core_dump mismatch: {summary.get('core_dump')!r} vs {core}")
if summary.get("core_loaded") is not True:
    raise SystemExit("expected core_loaded true")
if summary.get("evidence_count") != len(evidence):
    raise SystemExit(f"evidence_count mismatch: {summary.get('evidence_count')} vs {len(evidence)}")
if task.get("core_dump") != str(core):
    raise SystemExit(f"task core_dump mismatch: {task.get('core_dump')!r} vs {core}")

for item in evidence:
    for key in ("view_file", "raw_file", "summary_file"):
        path = pathlib.Path(item[key])
        if not path.exists():
            raise SystemExit(f"{key} missing for {item['id']}: {path}")

for kind in ("SessionEvent", "GdbCommand", "ReplayStep", "ReplayRun", "ToolError"):
    if kind not in kinds:
        raise SystemExit(f"missing evidence kind {kind}; saw {sorted(set(kinds))}")
for title in ("Core load", "Backtrace", "Core threads"):
    if title not in titles:
        raise SystemExit(f"missing evidence title {title}; saw {sorted(set(titles))}")

report_text = report.read_text()
for expected in (
    "## Core Dump Snapshot",
    "| Core Dump |",
    "| Core Loaded | `true` |",
    "### Core Evidence Links",
    "### Core Guard Rejections",
    "## Replay Execution Audit",
    "previous step a2 failed under stop_on_error",
):
    if expected not in report_text:
        raise SystemExit(f"report missing expected text: {expected}")
if str(core) not in report_text:
    raise SystemExit("report does not include core dump path")

report_ids = set(re.findall(r"\bE\d{4}\b", report_text))
missing = report_ids - ids
if missing:
    raise SystemExit(f"report references missing evidence ids: {sorted(missing)}")

print("artifact consistency ok")
PY
}

require_replay_steps() {
    local response="$1"
    shift
    python3 - "$response" "$@" <<'PY'
import json
import sys

lines = [line for line in sys.argv[1].splitlines() if line.strip()]
if not lines:
    raise SystemExit("empty replay response")
result = json.loads(lines[-1])
steps = {(step.get("action_name"), step.get("status")) for step in result.get("steps", [])}
for expected in sys.argv[2:]:
    action, status = expected.split(":", 1)
    if (action, status) not in steps:
        raise SystemExit(f"missing replay step {action}:{status}; saw {sorted(steps)}")
PY
}

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

cat >"$task_file" <<TASK
### problem

Load a generated core file for the segfault example and collect static evidence.

### executable

$target

### working directory

$repo_root

### args


### core dump

$core_file
TASK

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

check_response="$("$agent" check "$task_file")"
require_contains "$check_response" "ok"
require_contains "$check_response" "core dump:"

create_response="$("$agent" create "$task_file" --socket "$socket_path" --session C1 --out "$report_path" --assets "$assets_dir")"
require_contains "$create_response" '"ok":true'
require_contains "$create_response" '"session_id":"C1"'
require_contains "$create_response" '"mode":"core"'
require_contains "$create_response" '"state":"stopped"'
require_contains "$create_response" '"stop_reason":"core_loaded"'

status_response="$("$agent" status C1 --socket "$socket_path")"
require_contains "$status_response" '"ok":true'
require_contains "$status_response" '"mode":"core"'
require_contains "$status_response" '"stop_reason":"core_loaded"'

backtrace_response="$("$agent" action C1 '{"action":"backtrace"}' --socket "$socket_path")"
require_gdb_action_response "$backtrace_response" "backtrace"

threads_response="$("$agent" action C1 '{"action":"threads"}' --socket "$socket_path")"
require_gdb_action_response "$threads_response" "threads"

frame_response="$("$agent" action C1 '{"action":"frame_select","frame":0}' --socket "$socket_path")"
require_gdb_action_response "$frame_response" "frame_select"

args_response="$("$agent" action C1 '{"action":"args_info"}' --socket "$socket_path")"
require_gdb_action_response "$args_response" "args_info"

locals_response="$("$agent" action C1 '{"action":"locals"}' --socket "$socket_path")"
require_gdb_action_response "$locals_response" "locals"

evaluate_response="$("$agent" action C1 '{"action":"evaluate","expression":"session"}' --socket "$socket_path")"
require_gdb_action_response "$evaluate_response" "evaluate"

continue_response="$("$agent" action C1 '{"action":"continue"}' --socket "$socket_path")"
require_contains "$continue_response" '"ok":false'
require_contains "$continue_response" '"action":"continue"'
require_contains "$continue_response" '"error":"continue is not available in core mode"'
require_contains "$continue_response" '"evidence":"'

run_response="$("$agent" action C1 '{"action":"run"}' --socket "$socket_path")"
require_contains "$run_response" '"ok":false'
require_contains "$run_response" '"action":"run"'
require_contains "$run_response" '"error":"run is not available in core mode"'
require_contains "$run_response" '"evidence":"'

save_named_action C1 '{"action":"backtrace"}' core-continue-audit continue_on_error >/dev/null
save_named_action C1 '{"action":"continue"}' core-continue-audit continue_on_error >/dev/null
save_named_action C1 '{"action":"args_info"}' core-continue-audit continue_on_error >/dev/null

save_named_action C1 '{"action":"backtrace"}' core-stop-audit stop_on_error >/dev/null
save_named_action C1 '{"action":"breakpoint_set","location":"read_session_value"}' core-stop-audit stop_on_error >/dev/null
save_named_action C1 '{"action":"locals"}' core-stop-audit stop_on_error >/dev/null

continue_plan="$assets_dir/replay/core-continue-audit.json"
stop_plan="$assets_dir/replay/core-stop-audit.json"
require_file "$continue_plan"
require_file "$stop_plan"
grep -F '"schema": "gdb-agent-replay-plan-v1"' "$continue_plan" >/dev/null
grep -F '"fingerprint":' "$continue_plan" >/dev/null
grep -F '"failure_policy": "continue_on_error"' "$continue_plan" >/dev/null
grep -F '"failure_policy": "stop_on_error"' "$stop_plan" >/dev/null

replay_continue="$("$agent" replay C1 --file "$continue_plan" --socket "$socket_path")"
require_contains "$replay_continue" '"ok":false'
require_contains "$replay_continue" '"task_metadata_match":true'
require_contains "$replay_continue" '"failure_policy":"continue_on_error"'
require_contains "$replay_continue" '"error_evidence":"'
require_contains "$replay_continue" '"run_evidence":"'
require_replay_steps "$replay_continue" "backtrace:success" "continue:failed" "args_info:success"

replay_stop="$("$agent" replay C1 --file "$stop_plan" --socket "$socket_path")"
require_contains "$replay_stop" '"ok":false'
require_contains "$replay_stop" '"task_metadata_match":true'
require_contains "$replay_stop" '"failure_policy":"stop_on_error"'
require_contains "$replay_stop" 'previous step a2 failed under stop_on_error'
require_replay_steps "$replay_stop" "backtrace:success" "breakpoint_set:failed" "locals:skipped"

finish_response="$("$agent" finish C1 --socket "$socket_path" --out "$report_path")"
require_contains "$finish_response" '"ok":true'
require_contains "$finish_response" '"report":"'
require_contains "$finish_response" '"assets":"'

require_file "$report_path"
require_file "$assets_dir/task.normalized.json"
require_file "$assets_dir/session_summary.json"
require_file "$assets_dir/session_snapshot.json"
require_file "$assets_dir/evidence/index.json"

grep -F '"mode": "core"' "$assets_dir/session_summary.json" >/dev/null
grep -F '"core_loaded": true' "$assets_dir/session_summary.json" >/dev/null
grep -F '"core_dump": "' "$assets_dir/session_summary.json" >/dev/null
grep -F '"replay_step_count": 6' "$assets_dir/session_summary.json" >/dev/null
grep -F '"mode": "core"' "$assets_dir/session_snapshot.json" >/dev/null
grep -F '"kind":"SessionEvent"' "$assets_dir/evidence/index.json" >/dev/null
grep -F '"title":"Core load"' "$assets_dir/evidence/index.json" >/dev/null
grep -F '"kind":"ToolError"' "$assets_dir/evidence/index.json" >/dev/null
grep -F '"kind":"ReplayStep"' "$assets_dir/evidence/index.json" >/dev/null
grep -F '"kind":"ReplayRun"' "$assets_dir/evidence/index.json" >/dev/null
grep -F '## Session Summary' "$report_path" >/dev/null
grep -F '## Core Dump Snapshot' "$report_path" >/dev/null
grep -F -- '- Mode: Core Dump' "$report_path" >/dev/null
grep -F 'The core dump was loaded and static crash evidence was collected.' "$report_path" >/dev/null
grep -F 'Core load' "$report_path" >/dev/null
grep -F 'core-continue-audit' "$report_path" >/dev/null
grep -F 'core-stop-audit' "$report_path" >/dev/null
grep -F '`continue` | `failed`' "$report_path" >/dev/null
grep -F '`breakpoint_set` | `failed`' "$report_path" >/dev/null
grep -F '`locals` | `skipped`' "$report_path" >/dev/null

artifact_check

shutdown_response="$("$agent" shutdown --socket "$socket_path")"
require_contains "$shutdown_response" '"ok":true'
wait "$daemon_pid" >/dev/null 2>&1 || true
daemon_pid=""

echo "smoke ok: core dump mode load, static actions, guards, replay, and report audit passed"
