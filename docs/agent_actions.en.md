# Agent Actions MVP

`gdb-agent serve` starts an interactive GDB/MI session for a task file. The MVP
automatically runs the target, waits for a stop event, and collects segfault
evidence. After that, it accepts one action per stdin line. If stdin reaches
EOF, the session is finished and a report is written.

For multi-call Agent usage, start the Unix socket daemon and address the same
live session across separate CLI invocations:

```bash
gdb-agent daemon --socket /tmp/gdb-agent.sock
gdb-agent create examples/segfault_task.md --socket /tmp/gdb-agent.sock --session S1 --out report.md --assets report.assets
gdb-agent list --socket /tmp/gdb-agent.sock
gdb-agent status S1 --socket /tmp/gdb-agent.sock
gdb-agent action S1 '{"action":"evaluate","expression":"session"}' --socket /tmp/gdb-agent.sock
gdb-agent action S1 action.json --socket /tmp/gdb-agent.sock
gdb-agent save-action S1 action.json --name repro-checks --failure-policy stop_on_error --socket /tmp/gdb-agent.sock
gdb-agent replay S1 repro-checks --socket /tmp/gdb-agent.sock
gdb-agent replay S1 --file report.assets/replay/repro-checks.json --force --socket /tmp/gdb-agent.sock
gdb-agent finish S1 --socket /tmp/gdb-agent.sock --out report.md \
  --agent-inference inference.md \
  --final-conclusion conclusion.md
gdb-agent shutdown --socket /tmp/gdb-agent.sock
```

`finish` writes the report and closes that session. `close` closes the session
without writing the final report. `list` returns all live daemon sessions,
`status` returns one session's stop state and evidence count, and `shutdown`
closes every live session and removes the Unix socket.

Supported action lines are intentionally small in MVP form:

```json
{"action":"backtrace"}
{"action":"locals"}
{"action":"registers"}
{"action":"threads"}
{"action":"args_info"}
{"action":"frame_select","frame":1}
{"action":"evaluate","expression":"ptr"}
{"action":"breakpoint_set","location":"examples/segfault.cpp:14","condition":"session == 0"}
{"action":"watchpoint_set","expression":"global_counter","condition":"global_counter > 10"}
{"action":"catchpoint_set","event":"throw"}
{"action":"catchpoint_set","event":"catch"}
{"action":"catchpoint_set","event":"syscall","name":"write"}
{"action":"catchpoint_set","event":"fork"}
{"action":"catchpoint_set","event":"exec"}
{"action":"probe_list"}
{"action":"probe_disable","number":1}
{"action":"probe_enable","number":1}
{"action":"probe_delete","number":1}
{"action":"continue"}
{"action":"save_action","name":"fd-checks","saved_action":"{\"action\":\"backtrace\"}"}
{"action":"save_action","name":"fd-checks","failure_policy":"stop_on_error","saved_action":{"action":"backtrace"}}
{"action":"replay","name":"fd-checks","failure_policy":"stop_on_error"}
{"action":"replay","file":"report.assets/replay/fd-checks.json","force":true}
{"action":"hypothesis_create","id":"H-stale-session","title":"session is null before dereference"}
{"action":"hypothesis_check","hypothesis":"H-stale-session","description":"session argument is null","expression":"session","assertion":"is_null"}
{"action":"hypothesis_conclude","hypothesis":"H-stale-session","conclusion":"Supported","inference":"The check shows session is null at the breakpoint."}
{"action":"raw_mi","params":{"command":"-interpreter-exec console \"show args\"","risk":"advanced"}}
{"action":"finish_session","agent_inference":"The evidence supports a null session argument before dereference.","final_conclusion":"Root cause is outside the tool's judgment; the agent concludes the crash path dereferences a null session."}
```

## Failure Semantics

When an action reaches a live session context, tool-side validation failures
return `ok:false` and write `ToolError` evidence. Examples include a missing
`action` field, `evaluate` without `expression`, probe actions without
`location`/`expression`/`number`, `raw_mi` without `risk:"advanced"`, forbidden
`raw_mi` usage inside on-hit actions, and replay file or replay plan validation
failures.

Actions such as `backtrace`, `locals`, `args_info`, `registers`, `threads`,
`frame_select`, `evaluate`, `hypothesis_check`, `run`, and `continue` keep the
raw command/control-command output as evidence. If GDB returns
`result_class=error`, the action returns `ok:false`; the response includes the
error `evidence` and the raw command's `command_evidence`. Static evidence
commands that time out are also reported as structured failures. `run`/`continue`
run deadlines still mean the inferior ran until the tool interrupted it; they
are not treated as GDB command failures. Agents can see whether GDB rejected the
action without opening raw MI, while the raw MI remains available for audit.

Saved replay plans are written as both a compatibility JSONL file and a
structured `replay/<name>.json` plan. Use `--replay-before-run plan.json` to
apply setup actions such as breakpoints, watchpoints, and catchpoints before
the first run. The new session installs the probes first, then performs the
initial `run`.
The default structured-plan `failure_policy` is `continue_on_error`; plans or
individual steps may use `stop_on_error`. Older JSONL files and older
structured plans without a policy default to `continue_on_error`.

CLI examples:

```bash
gdb-agent save-action S1 action.json --name repro-checks --failure-policy stop_on_error
gdb-agent replay S1 repro-checks --failure-policy stop_on_error
gdb-agent replay S1 --file report.assets/replay/repro-checks.json --force
```

Before replaying a structured plan, the tool checks `schema`,
`schema_version`, and task fingerprint metadata. A task mismatch is rejected by
default and recorded as `ToolError` evidence. Explicit `force:true` or CLI
`--force` allows the replay, but the result and `ReplayWarning` evidence record
the mismatch warning.

```json
{"action":"breakpoint_set","location":"examples/segfault.cpp:14","condition":"session == 0"}
```

Breakpoints, watchpoints, and the minimal catchpoint action may include
`comment`, `purpose`, and `on_hit` metadata; breakpoints and watchpoints also
support `condition`. Runtime probe metadata is authoritative in the in-memory
`ProbeState`; `assets/probes.json` is generated only as a final snapshot during
`finish`/report writing. Probe hits are recorded as `BreakpointHit`,
`WatchpointHit`, or `CatchpointHit` evidence and include the relevant metadata
snapshot for that hit, the on-hit policy, each on-hit action result, and the
new evidence ids produced by this hit. If GDB rejects a probe or its condition,
the action returns `ok:false` and records `ToolError` evidence.
Use `probe_list` to capture GDB's breakpoint/watchpoint/catchpoint table and
return active tool metadata, including comments, purpose, hit count, and on-hit
policy; it does not treat `probes.json` as a runtime synchronization database.
After `probe_delete`, default `probe_list` output no longer includes the deleted
probe, so agents do not mistake historical probes for live ones. Final
`assets/probes.json` may still retain deleted history, but deleted entries are
marked with `deleted:true`.

The catchpoint action supports C++ exception, Linux syscall, fork/vfork, and
exec events:

```json
{"action":"catchpoint_set","event":"throw","comment":"stop on C++ throw","purpose":"exception path"}
{"action":"catchpoint_set","event":"catch","comment":"stop on C++ catch","purpose":"exception handler path"}
{"action":"catchpoint_set","event":"syscall","comment":"stop on any syscall","purpose":"syscall path"}
{"action":"catchpoint_set","event":"syscall","name":"write","comment":"stop on write syscall","purpose":"I/O path"}
{"action":"catchpoint_set","event":"syscall","syscall":"write","comment":"stop on write syscall","purpose":"I/O path"}
{"action":"catchpoint_set","event":"fork","comment":"stop on fork","purpose":"process creation"}
{"action":"catchpoint_set","event":"vfork","comment":"stop on vfork","purpose":"process creation"}
{"action":"catchpoint_set","event":"exec","comment":"stop on exec","purpose":"exec path"}
```

`event:"throw"` maps to GDB `catch throw`, and `event:"catch"` maps to GDB
`catch catch`. `event:"syscall"` maps to `catch syscall`; with `name` or
`syscall`, it maps to `catch syscall <selector>`. `event:"fork"`,
`event:"vfork"`, and `event:"exec"` map to their matching GDB catch commands.

The syscall selector may be a string syscall name or an integer syscall id.
String selectors only allow letters, digits, and underscores, so arbitrary GDB
console fragments cannot be appended to the command. Action responses,
`probe_list`, `assets/probes.json`, and `CatchpointHit` evidence record both
`event` and `selector`; when no selector is provided, `selector` is an empty
string.

Other `event` values or invalid syscall selectors return stable `ok:false`
output and write `ToolError` evidence. If a Linux/GDB environment does not
support one of these catch commands, the tool returns a structured failure and
keeps the raw command evidence instead of pretending success.

```json
{
  "action": "breakpoint_set",
  "location": "examples/segfault.cpp:14",
  "comment": "stop before null session dereference",
  "purpose": "hypothesis_check",
  "on_hit": {
    "actions": [
      {"action":"args_info"},
      {"action":"backtrace"}
    ],
    "timeout_ms": 5000,
    "max_output_bytes": 8192,
    "max_summary_lines": 80,
    "failure_policy": "continue_on_error",
    "continue_after_hit": false
  }
}
```

The legacy array form, such as `on_hit: [{"action":"backtrace"}]`, remains
compatible and is treated as `actions` with default policy values. The policy
fields are:

- `actions`: high-level actions to run in order after a probe hit; `raw_mi` is
  not allowed as an on-hit action.
- `timeout_ms`: default timeout/deadline for each on-hit action, default `5000`.
- `max_output_bytes`: response budget for the `OnHitAction` wrapper evidence,
  default `8192`; the underlying action evidence still follows normal evidence
  store retention rules.
- `max_summary_lines`: response line budget for the `OnHitAction` wrapper
  evidence, default `80`.
- `failure_policy`: `continue_on_error` or `stop_on_error`. With
  `stop_on_error`, later on-hit actions are recorded as `skipped`.
- `continue_after_hit`: default `false`. When set to `true`, the tool appends
  an automatic `continue` after successful on-hit actions and records it as a
  `continue_after_hit` result; this can let the inferior run to another stop
  event.

Each on-hit action writes `OnHitAction` evidence with status (`success`,
`failed`, or `skipped`), action evidence ids, error evidence, and skip reason.
The hit evidence aggregates `on_hit_policy`, `on_hit_results`,
`on_hit_evidence_ids`, and `on_hit_error_ids`.
If a GDB watchpoint stop record does not include a probe number, the tool only
attributes the stop and runs on-hit actions when there is exactly one active
watchpoint. If the stop cannot be uniquely attributed, the tool records
degraded watchpoint-related evidence instead of inventing a probe number.

Structured replay plan:

```json
{
  "schema": "gdb-agent-replay-plan-v1",
  "schema_version": 1,
  "id": "replay-bt-check",
  "name": "bt-check",
  "tags": [],
  "source_session_id": "S1",
  "created_at": "2026-05-31T00:00:00Z",
  "failure_policy": "stop_on_error",
  "task": {
    "problem_summary": "segfault in callback",
    "executable": "/abs/path/build/segfault",
    "working_directory": "/abs/path",
    "args": "",
    "argv": [],
    "core_dump": null,
    "fingerprint": "fnv1a64:..."
  },
  "actions": [
    {
      "id": "a1",
      "name": "backtrace",
      "enabled": true,
      "tags": [],
      "failure_policy": null,
      "action": {"action":"backtrace"}
    }
  ]
}
```

The replay result includes each step's `index`, `step_id`, `action_name`,
`status` (`success`, `failed`, or `skipped`), `failure_policy`, `evidence`,
`action_evidence`, `error_evidence`, `skip_reason`, and the replay run's
`run_evidence`. Each replay records `ReplayRun` evidence, and every step
records `ReplayStep` evidence. Failed action responses or execution exceptions
also record `ToolError` evidence. When `stop_on_error` triggers, later steps
are recorded as `skipped` with a skip reason.
The final report builds a `Replay Execution Audit` section from structured
`ReplayRun`, `ReplayStep`, and `ReplayWarning` evidence. It summarizes the
plan, force state, task fingerprint match state, step success/failure/skipped
status, action evidence, error evidence, and warning evidence without parsing
replay response text.

Replay also works in Core Dump Mode, but it still follows the core static
evidence boundary. Static steps such as `backtrace`, `threads`,
`frame_select`, `locals`, `args_info`, `evaluate`, and `registers` may run.
Dynamic steps such as `run`, `continue`, breakpoint/watchpoint/catchpoint
setup, and probe mutation fail through the core-mode state guard and write
`ToolError` evidence. In mixed plans, `continue_on_error` continues to later
steps, while `stop_on_error` records later steps as `skipped`. The report's
`Replay Execution Audit` shows these success / failed / skipped steps and
their error evidence.

Actions are checked against the live session state before execution. For
example, `backtrace`, `locals`, `evaluate`, and hypothesis checks require a
stopped inferior or core mode; `continue` requires stopped state;
`breakpoint_set`, `watchpoint_set`, `catchpoint_set`, and `replay` can run in a
live session's ready, stopped, or exited state; and `finish` requires stopped,
exited, or error state. This allows `--replay-before-run` to install probe
setup plans before the initial run. Rejected actions are recorded as
`ToolError` evidence.

Core mode is a static debugging target. `run`, `continue`, `breakpoint_set`,
`watchpoint_set`, `catchpoint_set`, and probe enable/disable/delete actions are
rejected in core mode and recorded as `ToolError` evidence.

Core Dump Mode reports also include a `Core Dump Snapshot` section. It
summarizes the core path, executable, working directory, `core_loaded` state,
key static evidence links, and core guard rejections. This is a report
aggregation view, not a replacement for raw evidence or the session MI log.

Hypothesis records are written both as per-hypothesis Markdown files and as a
structured `assets/hypotheses/index.json`. Tool checks are recorded separately
from agent inference and conclusions; `hypothesis_check` represents a tool
observation plus an assertion result, not a root-cause judgment.

`hypothesis_check` runs `p <expression>` and uses the newly created evidence
summary as `observed`. The action result, the per-hypothesis Markdown file,
and `assets/hypotheses/index.json` include a structured check result:

- `hypothesis`
- `check_id`
- `description`
- `expression`
- `assertion`
- `expected`
- `observed`
- `status`: `passed`, `failed`, or `unknown`
- `evidence`
- `error_evidence`

Supported assertions are:

- `none`: no check, always `passed`.
- `contains`: `passed` when `observed` contains non-empty `expected`.
- `not_contains`: `passed` when `observed` does not contain non-empty
  `expected`.
- `is_null`: `passed` when `observed` contains common null markers such as
  `0x0`, `nullptr`, or `(nil)`.
- `non_null`: `passed` when `observed` does not contain common null markers.
- `equals`: `passed` when trimmed `observed` exactly equals non-empty
  `expected`.
- `not_equals`: `passed` when trimmed `observed` does not equal non-empty
  `expected`.
- `greater_than`: parses one integer from `observed` and one from non-empty
  `expected`; `passed` when `observed > expected`.
- `less_than`: `passed` when parsed integers satisfy `observed < expected`.
- `greater_equal`: `passed` when parsed integers satisfy `observed >= expected`.
- `less_equal`: `passed` when parsed integers satisfy `observed <= expected`.
- `equals_number`: `passed` when parsed integers are equal.
- `not_equals_number`: `passed` when parsed integers are different.
- `between`: `expected` uses `LOW..HIGH`; parses one integer from `observed`
  and passes when `LOW <= observed <= HIGH` with inclusive bounds.
- `address_non_null`: parses one hexadecimal address from `observed` and passes
  when it is not `0x0`.
- `address_equals`: parses one hexadecimal address from `observed` and one from
  non-empty `expected`; passes when they are equal.

Numeric assertions support decimal integers, negative integers, and `0x`
hexadecimal integers. They ignore GDB value-history prefixes such as `$1` in
`$1 = 42`. Floating-point values are not supported. Unknown assertions,
assertions that require `expected` when `expected` is empty, unparseable
integers, invalid `between` bounds, `between` LOW greater than HIGH,
unparseable hexadecimal addresses, or multiple different integers/addresses in
`observed`/`expected` return
`status:"unknown"` and record `ToolError` evidence. This means the tool could
not evaluate that check; it does not prove or disprove the hypothesis.

The final report's Hypotheses section aggregates hypothesis title, tool status,
checks, observed summaries, evidence ids, error evidence, agent inference, and
final agent conclusion from `assets/hypotheses/index.json`. Checks with
`status:"unknown"` or `error_evidence` are additionally listed as needing
attention, with the corresponding `ToolError` summary when available. If the
index is missing or cannot be parsed, the report falls back to listing the
per-hypothesis Markdown files. `observed` comes from a lossy summary; inspect
linked raw evidence before final conclusions.

The global final report sections `Agent Inference` and `Final Agent
Conclusion` are populated from `finish_session`,
`gdb-agent finish --agent-inference`, and `--final-conclusion`.

The default interface is action based. `raw_mi` is an advanced escape hatch and
must include `risk: "advanced"`; it is recorded as evidence.
