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
require_contains "$backtrace_response" '"ok":true'
require_contains "$backtrace_response" '"action":"backtrace"'
require_contains "$backtrace_response" '"evidence":"'

threads_response="$("$agent" action C1 '{"action":"threads"}' --socket "$socket_path")"
require_contains "$threads_response" '"ok":true'
require_contains "$threads_response" '"action":"threads"'
require_contains "$threads_response" '"evidence":"'

frame_response="$("$agent" action C1 '{"action":"frame_select","frame":0}' --socket "$socket_path")"
require_contains "$frame_response" '"ok":true'
require_contains "$frame_response" '"action":"frame_select"'

args_response="$("$agent" action C1 '{"action":"args_info"}' --socket "$socket_path")"
require_contains "$args_response" '"ok":true'
require_contains "$args_response" '"action":"args_info"'

locals_response="$("$agent" action C1 '{"action":"locals"}' --socket "$socket_path")"
require_contains "$locals_response" '"ok":true'
require_contains "$locals_response" '"action":"locals"'

evaluate_response="$("$agent" action C1 '{"action":"evaluate","expression":"session"}' --socket "$socket_path")"
require_contains "$evaluate_response" '"ok":true'
require_contains "$evaluate_response" '"action":"evaluate"'
require_contains "$evaluate_response" '"evidence":"'

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
grep -F '"mode": "core"' "$assets_dir/session_snapshot.json" >/dev/null
grep -F '"kind":"SessionEvent"' "$assets_dir/evidence/index.json" >/dev/null
grep -F '"title":"Core load"' "$assets_dir/evidence/index.json" >/dev/null
grep -F '"kind":"ToolError"' "$assets_dir/evidence/index.json" >/dev/null
grep -F '## Session Summary' "$report_path" >/dev/null
grep -F -- '- Mode: Core Dump' "$report_path" >/dev/null
grep -F 'The core dump was loaded and static crash evidence was collected.' "$report_path" >/dev/null
grep -F 'Core load' "$report_path" >/dev/null

shutdown_response="$("$agent" shutdown --socket "$socket_path")"
require_contains "$shutdown_response" '"ok":true'
wait "$daemon_pid" >/dev/null 2>&1 || true
daemon_pid=""

echo "smoke ok: core dump mode load, static actions, guards, and report passed"
