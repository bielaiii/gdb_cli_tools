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
gdb-agent save-action S1 action.json --name repro-checks --socket /tmp/gdb-agent.sock
gdb-agent replay S1 repro-checks --socket /tmp/gdb-agent.sock
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
{"action":"replay","name":"fd-checks"}
{"action":"hypothesis_create","id":"H-stale-session","title":"session is null before dereference"}
{"action":"hypothesis_check","hypothesis":"H-stale-session","description":"session argument is null","expression":"session","assertion":"is_null"}
{"action":"hypothesis_conclude","hypothesis":"H-stale-session","conclusion":"Supported","inference":"The check shows session is null at the breakpoint."}
{"action":"raw_mi","params":{"command":"-interpreter-exec console \"show args\"","risk":"advanced"}}
{"action":"finish_session","agent_inference":"The evidence supports a null session argument before dereference.","final_conclusion":"Root cause is outside the tool's judgment; the agent concludes the crash path dereferences a null session."}
```

Saved replay plans are written as both a compatibility JSONL file and a
structured `replay/<name>.json` plan. Use `--replay-before-run plan.json` to
apply actions such as breakpoints before the first run.

```json
{"action":"breakpoint_set","location":"examples/segfault.cpp:14","condition":"session == 0"}
```

Breakpoints, watchpoints, and the minimal catchpoint action may include
`comment`, `purpose`, and `on_hit` metadata; breakpoints and watchpoints also
support `condition`. Runtime probe metadata is authoritative in the in-memory
`ProbeState`; `assets/probes.json` is generated only as a final snapshot during
`finish`/report writing. Probe hits are recorded as `BreakpointHit`,
`WatchpointHit`, or `CatchpointHit` evidence and include the relevant metadata
snapshot for that hit. If GDB rejects a probe or its condition, the action
returns `ok:false` and records `ToolError` evidence.
Use `probe_list` to capture GDB's breakpoint/watchpoint/catchpoint table and
return the tool's stored metadata, including comments, purpose, hit count, and
on-hit actions; it does not treat `probes.json` as a runtime synchronization
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
  "on_hit": [
    {"action":"args_info"},
    {"action":"backtrace"}
  ]
}
```

Structured replay plan:

```json
{
  "schema": "gdb-agent-replay-plan-v1",
  "id": "replay-bt-check",
  "name": "bt-check",
  "actions": [
    {
      "id": "a1",
      "name": "backtrace",
      "enabled": true,
      "tags": [],
      "action": {"action":"backtrace"}
    }
  ]
}
```

Each replayed step records `ReplayStep` evidence. Replay failures record
`ToolError` evidence and replay continues with later enabled steps.

Actions are checked against the live session state before execution. For
example, `backtrace`, `locals`, `evaluate`, and hypothesis checks require a
stopped inferior or core mode; `continue` requires stopped state; `finish`
requires stopped, exited, or error state. Rejected actions are recorded as
`ToolError` evidence.

Hypothesis records are written both as per-hypothesis Markdown files and as a
structured `assets/hypotheses/index.json`. Tool checks are recorded separately
from agent conclusions. Final report sections `Agent Inference` and
`Final Agent Conclusion` are populated from `finish_session`,
`gdb-agent finish --agent-inference`, and `--final-conclusion`.

The default interface is action based. `raw_mi` is an advanced escape hatch and
must include `risk: "advanced"`; it is recorded as evidence.
