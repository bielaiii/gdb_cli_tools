#!/usr/bin/env bash
set -euo pipefail

mode="full"
if [[ "${1:-}" == "--check-only" ]]; then
    mode="check-only"
    shift
fi

if [[ "$#" -ne 0 ]]; then
    echo "usage: $0 [--check-only]" >&2
    exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
build_dir="$repo_root/build"
task_file="$repo_root/examples/segfault_task.md"

if [[ "$mode" == "full" ]]; then
    cmake -S "$repo_root" -B "$build_dir"
    cmake --build "$build_dir"
elif [[ ! -x "$build_dir/gdb-agent" || ! -x "$build_dir/segfault" ]]; then
    echo "missing build artifacts; run: cmake -S . -B build && cmake --build build" >&2
    exit 1
fi

check_output="$("$build_dir/gdb-agent" check "$task_file")"
printf '%s\n' "$check_output"

require_line() {
    local expected="$1"
    if ! grep -Fx -- "$expected" <<<"$check_output" >/dev/null; then
        echo "missing expected line: $expected" >&2
        exit 1
    fi
}

require_line "ok"
require_line "executable: $build_dir/segfault"
require_line "working directory: $repo_root"
require_line "argv:"
require_line "stdin: /dev/null"
require_line "run timeout ms: 30000"

printf '\nsmoke ok: segfault demo check output matched README\n'
