#!/usr/bin/env bash
set -Eeuo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
build_dir="$repo_root/build"
agent="$build_dir/gdb-agent"
target="$build_dir/segfault"

if [[ "${GDB_AGENT_FORCE_LIVE_TEST:-0}" != "1" && "$(uname -s)" != "Linux" ]]; then
    echo "skip: record flow smoke requires Linux + GDB"
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

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/gdb-agent-record-flow.XXXXXX")"
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
    echo "record flow smoke failed at line $line with exit code $code" >&2
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

record smoke segfault

### executable

$target

### working directory

$repo_root

### args


### core dump

TASK
}

task="$work_dir/record_task.md"
write_task "$task"

"$agent" daemon --socket "$socket_path" >"$daemon_log" 2>&1 &
daemon_pid="$!"
for _ in {1..50}; do
    [[ -S "$socket_path" ]] && break
    sleep 0.1
done
if [[ ! -S "$socket_path" ]]; then
    echo "daemon socket was not created" >&2
    exit 1
fi

report1="$work_dir/record1.md"
assets1="$work_dir/record1.assets"
create1="$("$agent" create "$task" --session R1 --socket "$socket_path" --out "$report1" --assets "$assets1")"
require_contains "$create1" '"ok":true'
require_contains "$create1" '"state":"stopped"'

start="$(run_action R1 '{"action":"record_start","name":"record-smoke","failure_policy":"stop_on_error"}')"
require_contains "$start" '"ok":true'
require_contains "$start" '"action":"record_start"'

bt="$(run_action R1 '{"action":"backtrace"}')"
require_contains "$bt" '"ok":true'

locals="$(run_action R1 '{"action":"locals"}')"
require_contains "$locals" '"ok":true'

status="$(run_action R1 '{"action":"record_status"}')"
require_contains "$status" '"ok":true'
require_contains "$status" '"step_count":2'

stop="$(run_action R1 '{"action":"record_stop"}')"
require_contains "$stop" '"ok":true'
require_contains "$stop" '"step_count":2'
require_contains "$stop" '.gar'
require_contains "$stop" '.json'

gar="$assets1/replay/record-smoke.gar"
export_json="$assets1/replay/record-smoke.json"
if [[ ! -f "$gar" || ! -f "$export_json" ]]; then
    echo "record artifacts were not written" >&2
    exit 1
fi

python3 - "$gar" "$export_json" <<'PY'
import json
import pathlib
import sys

gar = pathlib.Path(sys.argv[1])
export = pathlib.Path(sys.argv[2])
data = gar.read_bytes()
if not data.startswith(b"GDBA_REC1"):
    raise SystemExit("record binary magic mismatch")
plan = json.loads(export.read_text())
if plan.get("record_artifact_authority") != "binary":
    raise SystemExit("export does not mark binary authority")
actions = plan.get("actions", [])
if [step.get("action", {}).get("action") for step in actions] != ["backtrace", "locals"]:
    raise SystemExit(f"unexpected recorded actions: {actions}")
fingerprint = plan.get("task", {}).get("fingerprint")
if not fingerprint:
    raise SystemExit("missing task fingerprint in record export")
PY

report2="$work_dir/record2.md"
assets2="$work_dir/record2.assets"
create2="$("$agent" create "$task" --session R2 --socket "$socket_path" --out "$report2" --assets "$assets2")"
require_contains "$create2" '"ok":true'

replay="$("$agent" replay R2 --file "$gar" --socket "$socket_path")"
require_contains "$replay" '"ok":true'
require_contains "$replay" '"run_evidence"'
require_contains "$replay" '"action_name":"backtrace"'
require_contains "$replay" '"action_name":"locals"'

"$agent" finish R1 --socket "$socket_path" --out "$report1" >/dev/null
"$agent" finish R2 --socket "$socket_path" --out "$report2" >/dev/null

python3 - "$assets2" "$report2" <<'PY'
import json
import pathlib
import sys

assets = pathlib.Path(sys.argv[1])
report = pathlib.Path(sys.argv[2])
index = json.loads((assets / "evidence" / "index.json").read_text())
kinds = [item.get("kind") for item in index.get("evidence", [])]
if "ReplayRun" not in kinds or "ReplayStep" not in kinds:
    raise SystemExit("missing replay evidence after record artifact replay")
text = report.read_text()
if "Replay Execution Audit" not in text:
    raise SystemExit("report missing Replay Execution Audit")
PY

echo "smoke ok: high-level record, binary artifact, export, and replay passed"
