# Agent Actions MVP（中文版）

`gdb-agent serve` 会基于 task file 启动一个交互式 GDB/MI session。MVP 会自动运行
目标程序、等待 stop event，并收集崩溃相关 evidence。之后它从 stdin 每行接收一个
action。stdin 到达 EOF 时，session 结束并写出报告。

英文版文档保留在：

```text
docs/agent_actions.en.md
```

## Daemon 多轮调用

面向多轮 Agent 使用时，推荐先启动 Unix socket daemon，然后通过多个 CLI 调用访问同一个
live session：

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

`finish` 会写报告并关闭该 session。`close` 只关闭 session，不写最终报告。`list`
返回 daemon 中所有 live session，`status` 返回指定 session 的 stop 状态和 evidence
数量，`shutdown` 关闭所有 live session 并移除 Unix socket。

## 支持的 Action

MVP action 行保持有意的小而稳定：

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

## Replay

保存的 replay plan 会同时写成兼容 JSONL 文件和结构化 `replay/<name>.json` plan。
可以使用 `--replay-before-run plan.json` 在第一次运行前应用断点等 action。

```json
{"action":"breakpoint_set","location":"examples/segfault.cpp:14","condition":"session == 0"}
```

结构化 replay plan：

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

每个 replay step 都会记录 `ReplayStep` evidence。Replay 失败时记录 `ToolError`
evidence，并继续执行后续 enabled step。

## Probe 和 On-hit Action

断点和观察点可以带上 `condition`、`comment`、`purpose` 和 `on_hit` metadata。
Probe metadata 会持久化到 `assets/probes.json`；probe 命中时记录为
`BreakpointHit` 或 `WatchpointHit` evidence。如果 GDB 拒绝 probe 或 condition，
action 返回 `ok:false` 并记录 `ToolError` evidence。

使用 `probe_list` 可以捕获 GDB 的 breakpoint/watchpoint 表，并返回工具保存的 metadata，
包括 comment、purpose、hit count 和 on-hit action。

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

## 状态保护

Action 会先根据 live session 状态做校验。例如：

- `backtrace`、`locals`、`evaluate` 和 hypothesis check 需要 inferior 已停止或处于
  core mode。
- `continue` 需要 stopped state。
- `finish` 需要 stopped、exited 或 error state。

被拒绝的 action 会记录为 `ToolError` evidence。

## Hypothesis Workflow

Hypothesis 记录会同时写成单个 hypothesis 的 Markdown 文件，以及结构化
`assets/hypotheses/index.json`。工具 check 与 Agent conclusion 分开记录。最终报告中的
`Agent Inference` 和 `Final Agent Conclusion` 来自：

- `finish_session`
- `gdb-agent finish --agent-inference`
- `gdb-agent finish --final-conclusion`

默认接口是 action based。`raw_mi` 只是高级 escape hatch，必须包含
`risk: "advanced"`，并且会被记录为 evidence。
