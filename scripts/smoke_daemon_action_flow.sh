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

invalid_response="$("$agent" action S1 '{"action":"catchpoint_set","event":"syscall"}' --socket "$socket_path")"
require_contains "$invalid_response" '"ok":false'
require_contains "$invalid_response" '"action":"catchpoint_set"'
require_contains "$invalid_response" '"error":"unsupported catchpoint event"'
require_contains "$invalid_response" '"evidence":"'

finish_response="$("$agent" finish S1 --socket "$socket_path" --out "$report_path")"
require_contains "$finish_response" '"ok":true'
require_contains "$finish_response" '"report":"'
require_contains "$finish_response" '"assets":"'

require_file "$report_path"
require_file "$assets_dir/session_snapshot.json"
require_file "$assets_dir/session_summary.json"
require_file "$assets_dir/evidence/index.json"
require_file "$assets_dir/probes.json"

grep -F '"kind": "ToolError"' "$assets_dir/evidence/index.json" >/dev/null
grep -F '"kind": "catchpoint"' "$assets_dir/probes.json" >/dev/null
grep -F '"event": "throw"' "$assets_dir/probes.json" >/dev/null

shutdown_response="$("$agent" shutdown --socket "$socket_path")"
require_contains "$shutdown_response" '"ok":true'
wait "$daemon_pid" >/dev/null 2>&1 || true
daemon_pid=""

echo "smoke ok: daemon/action flow and catchpoint_set passed"
