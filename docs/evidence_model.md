# Evidence Model MVP（中文版）

每个会捕获数据的 GDB action 都会创建一条 evidence entry。

英文版文档保留在：

```text
docs/evidence_model.en.md
```

## 文件布局

MVP evidence 文件写在：

```text
<assets>/evidence/
```

优先打开顶层 Markdown 文件。它是 human-readable view：

```text
<assets>/evidence/E0002.backtrace.md
```

Raw MI 会故意放在更深一层：

```text
<assets>/evidence/raw/E0002.backtrace.mi.txt
```

面向 Agent 的紧凑 summary 单独保存：

```text
<assets>/evidence/summary/E0002.backtrace.summary.txt
```

## Evidence Entry 字段

每条 evidence 包含：

- id，例如 `E0001`
- kind，例如 `GdbCommand`、`StopEvent` 或 `ToolError`
- title
- command 或 action name
- human-readable Markdown view
- raw file
- summary file
- raw SHA-256
- capture timestamp
- raw byte count 和 kept summary byte count
- `truncated` 和 `lossy_summary` 标记
- record attribution 字段：`included_records`、`related_records` 和 `concurrent_records`
- raw MI audit 字段：`raw_records`，每条记录包含 sequence、token、record kind、
  result/async class 和 stream type（如果适用）

机器可读的 evidence index 是：

```text
<assets>/evidence/index.json
```

进入 live session 后的 action 校验失败会写 `ToolError` evidence。`backtrace`、`locals`、
`args_info`、`registers`、`threads`、`frame_select`、`evaluate`、`hypothesis_check`、`run`、
`continue` 等 GDB-backed action 如果底层 GDB 返回 `result_class=error`，会同时保留原始
command evidence，并写 `ToolError` evidence；action response 中的 `command_evidence` 指向
原始命令 evidence，`evidence` 指向错误 evidence。静态取证命令 timeout 也按结构化失败记录；
`run`/`continue` 的 run deadline 是 inferior 运行超时/中断语义，不等同于 GDB command failure。

它包含与每个 evidence Markdown view 相同的 metadata。Raw 文件完整保留；summary 文件受
byte limit 限制，该限制记录在 index 中。如果 summary 被截断，`truncated` 会被设置为
`true`。只要 summary 经历过 sanitizer、MI stream 解码、摘要化或截断，就应被视为有损，
并设置 `lossy_summary`。

`raw_records` 用于审计 raw MI 的结构，而不替代 raw 文件。当前 record kind 包括：
`result`、`async`、`stream`、`prompt` 和 `unknown`；stream type 包括 `console`、`target`
和 `log`。`included_records`、`related_records`、`concurrent_records` 继续表示 evidence
归属关系，raw hash、raw byte count 和 kept summary byte count 仍是完整性审计字段。

## Session 文件

完整 MI session stream 保存为：

```text
<assets>/logs/session.mi.raw.log
```

MVP 还会写机器可读的 session 文件：

```text
<assets>/task.normalized.json
<assets>/session_summary.json
<assets>/session_snapshot.json
```

`session_snapshot.json` 和 `session_summary.json` 是历史记录与报告输入，不代表 live GDB
session，也不能用于恢复旧 GDB 进程。重启后的恢复方式应该是 replay 高层 action。

`session_summary.json` 会记录 `replay_step_count`、`replay_warning_count`、
`probe_hit_count`、`on_hit_action_count` 和 `on_hit_error_count`，用于快速判断本 session
是否执行过 replay，是否存在 force replay 或旧 plan 兼容警告，以及 probe/on-hit 是否产生了
命中证据或错误。

core dump task 的 `session_summary.json` 还会记录 `core_dump` 路径和 `core_loaded`。
core mode 是静态调试对象；加载 core 会写 `Core load` 的 `SessionEvent` evidence，后续
`backtrace`、`threads`、`frame_select`、`locals`、`args_info` 和 `evaluate` 等静态取证
action 会产生普通 evidence。`run`、`continue` 和 probe mutation 在 core mode 下会被状态保护
拒绝，并写 `ToolError` evidence。

## Replay Evidence

Replay Store 只保存和重放高层 action。结构化 plan 使用
`gdb-agent-replay-plan-v1`，包含 `schema_version`、plan name、tags、source session id、
created_at、task metadata、task fingerprint、plan-level failure policy 和 action list。
`session_snapshot.json` 不是 replay 输入；跨 session 复现必须使用 replay plan 或 JSONL
高层 action。

每次 replay 会写一条 `ReplayRun` evidence，保存 replay result 的整体快照，包括 plan
名称、schema version、force 状态、task metadata 是否匹配、warning、error 和 step
结果列表。

每个 replay step 都会写 `ReplayStep` evidence。该 evidence 的 summary 中包含：

- plan name
- step id 和 index
- action name
- action JSON
- status：`success`、`failed` 或 `skipped`
- failure policy：`continue_on_error` 或 `stop_on_error`
- action evidence id
- error evidence id
- skip reason

如果 replay action 返回 `ok:false` 或执行异常，会额外写 `ToolError` evidence，并在
`ReplayStep` 中引用 `error_evidence`。如果 plan task fingerprint 与当前 task 不匹配，
默认拒绝 replay 并写 `ToolError` evidence；force replay 时写 `ReplayWarning` evidence，
并在 replay result 中保留 warning、`force:true` 和 `task_metadata_match:false`。

旧 JSONL replay 文件和缺少 failure policy 的旧 plan 默认按 `continue_on_error` 执行。
缺少 task metadata 或 fingerprint 的旧 plan 可以读取，但会产生 warning；无法识别的
schema 或 schema version 会被稳定拒绝。

## Probe Store 快照

Probe 的运行期权威状态是内存中的 `ProbeState`。`assets/probes.json` 只在
`finish`/报告写出阶段从 `ProbeState` 派生生成，是最终报告快照，不是运行时同步数据库，
也不是 live GDB session 恢复文件。

运行中使用 `probe_list` 观察当前 active probe metadata；默认不返回已经 `probe_delete` 的
历史 probe。跨 session 复现应依赖 replay 高层 action，不要读取旧 `probes.json` 恢复断点、
观察点或 catchpoint。`assets/probes.json` 可保留 deleted 历史项，但必须用 `deleted:true`
明确标记，避免把历史 probe 误认为仍然 live。

Probe 命中 evidence（`BreakpointHit`、`WatchpointHit`、`CatchpointHit`）会保存当次命中的
必要 metadata 快照，例如 number、kind、location/expression/event、condition、comment、
purpose、hit count 和 `on_hit_policy`。即使 session 异常结束，这些命中 evidence 仍能解释
当时为什么停住。
如果 watchpoint stop record 缺少 probe number，工具只会在唯一 active watchpoint 可判断时
归属并执行 on-hit；无法唯一归属时会记录降级 watchpoint evidence，不伪造 probe number。

如果 probe 配置了 on-hit policy，每个自动 action 会额外写入 `OnHitAction` evidence。
该 evidence 记录 action name、status（`success`、`failed` 或 `skipped`）、
`failure_policy`、本 action 新产生的 `action_evidence_ids`、`error_evidence`、
`skip_reason` 和经过 policy 预算截断的 response 摘要。底层 action 自己产生的 raw/summary
evidence 仍独立保留。

命中 evidence 会汇总：

- `on_hit_policy`
- `on_hit_results`
- `on_hit_evidence_ids`
- `on_hit_error_ids`

`stop_on_error` 触发时，后续 on-hit action 不会执行，但会以 `skipped` 状态写入
`OnHitAction` evidence，便于 Agent 区分“没有配置”“执行成功”和“因前序错误跳过”。
`continue_after_hit:true` 会追加一个自动 continue，并作为 `continue_after_hit` result 记录；
报告只记录该行为，不把它解释为根因判断。

## Hypothesis Artifacts

Hypothesis workflow 的机器可读入口是：

```text
<assets>/hypotheses/index.json
```

单个 hypothesis 的 human-readable 记录写在：

```text
<assets>/hypotheses/<hypothesis-id>.md
```

`index.json` 使用 `gdb-agent-hypotheses-v1` schema。每个 hypothesis entry 包含：

- `id`
- `title`
- `description`
- `tool_status`
- `agent_conclusion`
- `agent_inference`
- `checks`

每个 check entry 包含：

- `id` 和 `check_id`
- `description`
- `expression`
- `assertion`
- `expected`
- `observed`
- `status`：`passed`、`failed` 或 `unknown`
- `evidence`
- `error_evidence`

`observed` 来自该次 `hypothesis_check` 新产生的 evidence summary，是低噪声、有损视图；
`evidence` 指向原始 GDB 取证 entry，raw evidence 仍是权威来源。`status` 是工具对
`observed` 执行 assertion 后的结果，不是根因结论。

numeric assertion（`greater_than`、`less_than`、`greater_equal`、`less_equal`、
`equals_number`、`not_equals_number`、`between`）会从 `observed` 和 `expected` 中解析整数。
`between` 的 `expected` 格式为 `LOW..HIGH`，闭区间判断。整数支持十进制、负数和 `0x`
十六进制；不支持浮点数。地址 assertion（`address_non_null`、`address_equals`）解析唯一
`0x...` 十六进制地址。未知 assertion、缺少必需 `expected`、无法解析整数/地址、`between`
边界无效或 LOW 大于 HIGH、存在多个不同整数/地址时，会产生 `status:"unknown"`，并通过
`error_evidence` 引用一条 `ToolError` evidence。`unknown` 只表示工具无法判断该 check，不表示
支持或反驳 hypothesis。

最终报告会从 `index.json` 聚合 hypothesis、checks、evidence id、Agent inference 和 final
agent conclusion。报告的 Hypotheses 区域会展示 observed 摘要，并把 `unknown` 或带
`error_evidence` 的 check 列为需要注意的项，并尽量展示对应错误 summary；报告还会单独汇总
`ToolError` evidence，并在存在 `command_evidence` 时展示原始 GDB command evidence 链路。如果
index 缺失或无法解析，报告会降级为列出单个 Markdown 文件。

## 报告引用规则

报告应该引用 evidence id，而不是只依赖 summary。报告现在也会包含每条 evidence 的
raw hash，方便 Agent 验证报告引用的 raw 文件没有变化。

Raw 文件和 session MI log 是审计时的权威来源；summary 是低噪声、有损视图，只适合作为
Agent 上下文入口。

Summary 层会做有限降噪，例如 C++ `std::string` 类型归一化、常见 allocator/comparator/hash
噪声压缩、`std::pair<const K, V>` key const 噪声压缩、路径相对化，以及 backtrace/thread 的
稳定摘要。所有这些都不修改 raw MI。
