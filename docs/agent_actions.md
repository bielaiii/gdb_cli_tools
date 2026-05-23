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
gdb-agent finish S1 --socket /tmp/gdb-agent.sock --out report.md
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
{"action":"frame_select","frame":1}
{"action":"evaluate","expression":"ptr"}
{"action":"breakpoint_set","location":"examples/segfault.cpp:14","condition":"session == 0"}
{"action":"watchpoint_set","expression":"global_counter"}
{"action":"probe_disable","number":1}
{"action":"probe_enable","number":1}
{"action":"probe_delete","number":1}
{"action":"continue"}
{"action":"save_action","name":"fd-checks","saved_action":"{\"action\":\"backtrace\"}"}
{"action":"replay","name":"fd-checks"}
{"action":"hypothesis_create","id":"H-stale-session","title":"session is null before dereference"}
{"action":"hypothesis_check","hypothesis":"H-stale-session","description":"session argument is null","expression":"session","assertion":"is_null"}
{"action":"hypothesis_conclude","hypothesis":"H-stale-session","conclusion":"Supported","inference":"The check shows session is null at the breakpoint."}
{"action":"finish_session"}
```

Replay plans are JSONL files: one action object per line. Use
`--replay-before-run plan.jsonl` to apply actions such as breakpoints before
the first `-exec-run`.

```json
{"action":"breakpoint_set","location":"examples/segfault.cpp:14","condition":"session == 0"}
```

The default interface is action based. `raw_mi` is deliberately not implemented
in the MVP user surface.
