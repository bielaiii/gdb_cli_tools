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
gdb-agent save-action S1 action.json --name repro-checks --failure-policy stop_on_error --socket /tmp/gdb-agent.sock
gdb-agent replay S1 repro-checks --socket /tmp/gdb-agent.sock
gdb-agent replay S1 --file report.assets/replay/repro-checks.json --force --socket /tmp/gdb-agent.sock
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

## 失败语义

只要 action 已经进入某个 live session 的处理上下文，工具校验失败会返回 `ok:false`，
并写入 `ToolError` evidence。典型例子包括缺少 `action` 字段、`evaluate` 缺少
`expression`、probe action 缺少 `location`/`expression`/`number`、`raw_mi` 缺少
`risk:"advanced"`、on-hit 中使用禁止的 `raw_mi`，以及 replay plan 或 replay 文件校验失败。

`frame_select`、`evaluate`、`run`、`continue` 等需要执行 GDB command/control command 的
action 会保留原始 command evidence。如果 GDB 返回 `result_class=error`，action 返回
`ok:false`，response 会包含错误 `evidence` 和原始命令的 `command_evidence`。`frame_select`
和 `evaluate` 命令超时时也会按结构化失败处理；`run`/`continue` 的 run deadline 仍表示
inferior 运行超时/被工具中断，不等同于 GDB command 失败。Agent 不需要打开 raw MI 就能知道该
action 是否被 GDB 拒绝；raw MI 仍可用于审计。

## Replay

保存的 replay plan 会同时写成兼容 JSONL 文件和结构化 `replay/<name>.json` plan。
可以使用 `--replay-before-run plan.json` 在第一次运行前应用断点等 action。
结构化 plan 默认 `failure_policy` 是 `continue_on_error`；也可以使用
`stop_on_error`。step 可以用自己的 `failure_policy` 覆盖 plan-level policy。
旧 JSONL 和缺少 policy 的旧结构化 plan 按 `continue_on_error` 处理。

CLI 支持：

```bash
gdb-agent save-action S1 action.json --name repro-checks --failure-policy stop_on_error
gdb-agent replay S1 repro-checks --failure-policy stop_on_error
gdb-agent replay S1 --file report.assets/replay/repro-checks.json --force
```

`replay` 会在执行前检查 plan 的 `schema`、`schema_version` 和 task fingerprint。
如果 plan 的 task metadata 与当前 task 不匹配，默认拒绝执行并记录 `ToolError`
evidence。显式 `force:true` 或 CLI `--force` 会允许执行，但 result 与
`ReplayWarning` evidence 会记录 mismatch warning。

```json
{"action":"breakpoint_set","location":"examples/segfault.cpp:14","condition":"session == 0"}
```

结构化 replay plan：

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

`replay` 的 result 会包含每个 step 的 `index`、`step_id`、`action_name`、
`status`（`success`、`failed` 或 `skipped`）、`failure_policy`、`evidence`、
`action_evidence`、`error_evidence`、`skip_reason` 和本次 replay 的 `run_evidence`。
每次 replay 会记录 `ReplayRun` evidence；每个 step 都会记录
`ReplayStep` evidence；action 返回失败或执行异常时还会记录 `ToolError` evidence。
`stop_on_error` 触发后，后续 step 会以 `skipped` 记录，并带上 skip reason。

## Probe 和 On-hit Action

断点、观察点和最小 catchpoint 可以带上 `comment`、`purpose` 和 `on_hit` metadata；
断点和观察点还支持 `condition`。
运行期 probe metadata 以内存 `ProbeState` 为权威状态；`assets/probes.json` 只在
`finish`/报告写出阶段作为最终快照生成。probe 命中时记录为 `BreakpointHit`、
`WatchpointHit` 或 `CatchpointHit` evidence，并包含当次命中的必要 metadata 快照、
on-hit policy、每个 on-hit action 的执行结果和本次新产生的 evidence id。
如果 GDB 拒绝 probe 或 condition，action 返回 `ok:false` 并记录 `ToolError` evidence。

使用 `probe_list` 可以捕获 GDB 的 breakpoint/watchpoint/catchpoint 表，并返回工具保存的 active
metadata，包括 comment、purpose、hit count 和 on-hit policy；它不把 `probes.json` 当作运行时同步数据库。
`probe_delete` 后，默认 `probe_list` 不再返回已删除的 probe，避免 Agent 把历史 probe 误认为仍可命中。
最终 `assets/probes.json` 仍可保留 deleted 历史项，但必须标记 `deleted:true`。

本轮 catchpoint 只支持 C++ exception throw：

```json
{"action":"catchpoint_set","event":"throw","comment":"stop on C++ throw","purpose":"exception path"}
```

其他 `event` 会稳定返回 `ok:false`，并写入 `ToolError` evidence。

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

旧格式 `on_hit: [{"action":"backtrace"}]` 仍兼容读取，等价于只设置 `actions` 并使用默认
policy。新格式的字段含义：

- `actions`：命中后按顺序执行的高层 action 列表；`raw_mi` 不能作为 on-hit action。
- `timeout_ms`：单个 on-hit action 的 timeout/deadline 默认值，默认 `5000`。
- `max_output_bytes`：`OnHitAction` wrapper evidence 中 response 摘要的最大字节数，
  默认 `8192`；原始 action evidence 仍按 evidence store 规则保留。
- `max_summary_lines`：`OnHitAction` wrapper evidence 中 response 摘要的最大行数，
  默认 `80`。
- `failure_policy`：`continue_on_error` 或 `stop_on_error`。`stop_on_error` 下失败后的
  后续 on-hit action 会记录为 `skipped`。
- `continue_after_hit`：默认 `false`。设为 `true` 时，on-hit actions 成功执行后工具会追加
  一个自动 `continue`，并把它作为 `continue_after_hit` on-hit result 记录；这可能让 inferior
  继续运行到下一个 stop event。

每个 on-hit action 会产生 `OnHitAction` evidence，记录 status（`success`、`failed` 或
`skipped`）、action evidence ids、error evidence 和 skip reason。命中 evidence 会汇总
`on_hit_policy`、`on_hit_results`、`on_hit_evidence_ids` 和 `on_hit_error_ids`。
如果 GDB 的 watchpoint stop record 缺少 probe number，工具只会在当前存在唯一 active
watchpoint 时保守归属并执行 on-hit；无法唯一归属时会记录降级的 watchpoint 相关 evidence，
不会伪造确定性 probe number。

## 状态保护

Action 会先根据 live session 状态做校验。例如：

- `backtrace`、`locals`、`evaluate` 和 hypothesis check 需要 inferior 已停止或处于
  core mode。
- `continue` 需要 stopped state。
- core mode 是静态调试对象，`run`、`continue`、`breakpoint_set`、`watchpoint_set`、
  `catchpoint_set` 和 probe enable/disable/delete 会被拒绝，并记录 `ToolError` evidence。
- `finish` 需要 stopped、exited 或 error state。

被拒绝的 action 会记录为 `ToolError` evidence。

## Hypothesis Workflow

Hypothesis 记录会同时写成单个 hypothesis 的 Markdown 文件，以及结构化
`assets/hypotheses/index.json`。工具 check 与 Agent inference / conclusion 分开记录；
`hypothesis_check` 只表达工具级观察和 assertion 结果，不代表根因判断。

`hypothesis_check` 会执行 `p <expression>` 并使用本次新产生的 evidence summary 作为
`observed`。action result、单个 hypothesis Markdown 和 `assets/hypotheses/index.json`
都会包含结构化 check result：

- `hypothesis`
- `check_id`
- `description`
- `expression`
- `assertion`
- `expected`
- `observed`
- `status`：`passed`、`failed` 或 `unknown`
- `evidence`
- `error_evidence`

当前支持的 assertion：

- `none`：不检查，稳定返回 `passed`。
- `contains`：`observed` 包含非空 `expected` 时 `passed`。
- `not_contains`：`observed` 不包含非空 `expected` 时 `passed`。
- `is_null`：`observed` 包含常见 null marker，例如 `0x0`、`nullptr` 或 `(nil)` 时
  `passed`。
- `non_null`：`observed` 不包含常见 null marker 时 `passed`。
- `equals`：去掉 `observed` 首尾空白后与非空 `expected` 完全相等时 `passed`。
- `not_equals`：去掉 `observed` 首尾空白后与非空 `expected` 不相等时 `passed`。

未知 assertion，或者需要 `expected` 但 `expected` 为空的 assertion，会返回
`status:"unknown"`，并记录 `ToolError` evidence；这表示工具无法判定该 check，不表示
hypothesis 被证实或证伪。

最终报告的 Hypotheses 区域会从 `assets/hypotheses/index.json` 聚合 hypothesis title、
tool status、checks、evidence id、Agent inference 和 final agent conclusion。如果 index
缺失或无法解析，报告会降级为列出单个 hypothesis Markdown 文件。

最终报告中的全局 `Agent Inference` 和 `Final Agent Conclusion` 来自：

- `finish_session`
- `gdb-agent finish --agent-inference`
- `gdb-agent finish --final-conclusion`

默认接口是 action based。`raw_mi` 只是高级 escape hatch，必须包含
`risk: "advanced"`，并且会被记录为 evidence。
