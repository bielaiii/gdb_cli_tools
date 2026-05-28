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

## Probe Store 快照

Probe 的运行期权威状态是内存中的 `ProbeState`。`assets/probes.json` 只在
`finish`/报告写出阶段从 `ProbeState` 派生生成，是最终报告快照，不是运行时同步数据库，
也不是 live GDB session 恢复文件。

运行中使用 `probe_list` 观察当前 probe metadata；跨 session 复现应依赖 replay 高层 action，
不要读取旧 `probes.json` 恢复断点、观察点或 catchpoint。

Probe 命中 evidence（`BreakpointHit`、`WatchpointHit`、`CatchpointHit`）会保存当次命中的
必要 metadata 快照，例如 number、kind、location/expression/event、condition、comment、
purpose 和 hit count。即使 session 异常结束，这些命中 evidence 仍能解释当时为什么停住。

## 报告引用规则

报告应该引用 evidence id，而不是只依赖 summary。报告现在也会包含每条 evidence 的
raw hash，方便 Agent 验证报告引用的 raw 文件没有变化。

Raw 文件和 session MI log 是审计时的权威来源；summary 是低噪声、有损视图，只适合作为
Agent 上下文入口。

Summary 层会做有限降噪，例如 C++ `std::string` 类型归一化、常见 allocator 噪声压缩、
路径相对化，以及 backtrace/thread 的稳定摘要。所有这些都不修改 raw MI。
