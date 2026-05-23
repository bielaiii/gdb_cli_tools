# Agent Actions MVP

`gdb-agent serve` starts an interactive GDB/MI session for a task file. The MVP
automatically runs the target, waits for a stop event, and collects segfault
evidence. After that, it accepts one action per stdin line. If stdin reaches
EOF, the session is finished and a report is written.

Supported action lines are intentionally small in MVP form:

```json
{"action":"backtrace"}
{"action":"locals"}
{"action":"registers"}
{"action":"threads"}
{"action":"frame_select","frame":1}
{"action":"evaluate","expression":"ptr"}
{"action":"breakpoint_set","location":"examples/segfault.cpp:14","condition":"session == 0"}
{"action":"continue"}
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
