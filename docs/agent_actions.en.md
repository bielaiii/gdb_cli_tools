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

Saved replay plans are written as both a compatibility JSONL file and a
structured `replay/<name>.json` plan. Use `--replay-before-run plan.json` to
apply actions such as breakpoints before the first run.
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
return the tool's stored metadata, including comments, purpose, hit count, and
on-hit policy; it does not treat `probes.json` as a runtime synchronization
database.

The current catchpoint action only supports C++ exception throws:

```json
{"action":"catchpoint_set","event":"throw","comment":"stop on C++ throw","purpose":"exception path"}
```

Other `event` values return stable `ok:false` output and write `ToolError`
evidence.

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

Actions are checked against the live session state before execution. For
example, `backtrace`, `locals`, `evaluate`, and hypothesis checks require a
stopped inferior or core mode; `continue` requires stopped state; `finish`
requires stopped, exited, or error state. Rejected actions are recorded as
`ToolError` evidence.

Core mode is a static debugging target. `run`, `continue`, `breakpoint_set`,
`watchpoint_set`, `catchpoint_set`, and probe enable/disable/delete actions are
rejected in core mode and recorded as `ToolError` evidence.

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

Unknown assertions, and assertions that require `expected` when `expected` is
empty, return `status:"unknown"` and record `ToolError` evidence. This means
the tool could not evaluate that check; it does not prove or disprove the
hypothesis.

The final report's Hypotheses section aggregates hypothesis title, tool status,
checks, evidence ids, agent inference, and final agent conclusion from
`assets/hypotheses/index.json`. If the index is missing or cannot be parsed,
the report falls back to listing the per-hypothesis Markdown files.

The global final report sections `Agent Inference` and `Final Agent
Conclusion` are populated from `finish_session`,
`gdb-agent finish --agent-inference`, and `--final-conclusion`.

The default interface is action based. `raw_mi` is an advanced escape hatch and
must include `risk: "advanced"`; it is recorded as evidence.
