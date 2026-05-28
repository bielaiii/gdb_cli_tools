# Decisions

本文件记录项目当前已经明确的设计决策。新增实现应默认遵守这些决策，除非先更新
`design.md` 和本文件。

## D001: 只保留 Interactive Session Mode

状态：Accepted

工具的唯一对外运行形态是长驻 live session。AI Agent 可以多轮提交 action，
直到明确 finish。一次性默认取证可以作为启动后的自动 action 或 replay plan，
但不作为独立 Batch Mode。

原因：

- 当前目标是持续对话式定位问题。
- GDB 的运行态、frame、thread 和 inferior memory 无法从 JSON 可靠恢复。
- 长会话更适合 Agent 持续提出假设并逐步验证。

## D002: GDB 接入使用 GDB/MI，不使用 PTY

状态：Accepted

工具只通过 GDB/MI 和非交互式 I/O 管理 GDB。inferior stdin 默认使用
`/dev/null`，也可以由 task file 指定输入文件；stdout/stderr 重定向到 assets
目录并作为 `InferiorOutput` evidence 捕获。

原因：

- MI 更适合结构化解析和状态机管理。
- PTY 行为会引入额外不确定性。
- 非 PTY 行为可能影响 `isatty`，报告需要记录该限制。

## D003: AI 默认使用高层 action

状态：Accepted

AI Agent 默认调用 `backtrace`、`evaluate`、`breakpoint_set`、`continue`、
`hypothesis_check` 等高层 action。`raw_mi` 只作为高级 escape hatch，并必须显式声明
`risk: "advanced"`。

原因：

- 降低 Agent 误用 MI 的概率。
- 让 action 能被状态机、证据系统、replay store 和报告系统统一审计。
- 保留必要的专家逃生通道。

## D004: Evidence 原始数据优先，summary 是有损视图

状态：Accepted

每次取证必须保留 raw MI 或原始文本，同时生成 summary 和 Markdown view。报告引用
evidence id，并记录 raw SHA-256。summary 可以被 sanitizer、截断或归一化，因此必须标记
`lossy_summary` 和 `truncated`。

原因：

- Agent 需要低噪声上下文做判断。
- 审计和复查必须能回到原始证据。
- C++ 模板类型、长路径和 MI 噪声需要被压缩，但不能替代原文。

## D005: Snapshot 不是 live session 恢复文件

状态：Accepted

`session_snapshot.json` 和 `session_summary.json` 只是历史记录和报告输入。
它们不能表示仍可继续执行的 GDB 进程，也不能用于恢复 frame、thread、breakpoint hit
等运行态。

原因：

- GDB 内部状态不可安全序列化。
- 错把 snapshot 当恢复文件会造成错误取证。
- 重启后需要重放高层 action，而不是恢复旧进程。

## D006: 重启重放保存高层 action

状态：Accepted

Replay Store 保存高层 action 列表，结构化文件使用 `gdb-agent-replay-plan-v1`。
重放时每步 action 在新 session 中重新执行，并产生新的 evidence id。失败步骤记录
`ToolError` evidence，是否继续由 replay/action policy 决定。

原因：

- 高层 action 比 raw MI 更稳定。
- 新 session 的证据不能复用旧 evidence id。
- replay 是 repeat-run 的核心能力。

## D007: Probe metadata 属于工具状态

状态：Accepted

breakpoint/watchpoint/catchpoint 需要保存 comment、purpose、condition、hit count、
last stop reason 和 on-hit action。运行期以内存 `ProbeState` 为权威状态；
`assets/probes.json` 只在 finish/report 写出阶段作为最终快照生成，不作为运行时同步数据库或
live GDB 恢复文件。Probe 命中时写入 `BreakpointHit`、`WatchpointHit` 或
`CatchpointHit` evidence，并保留当次命中的必要 metadata 快照。

原因：

- Agent 需要知道断点为什么存在，而不只是 GDB 编号。
- on-hit 自动动作可以降低重复取证成本。
- 命中记录能把 probe 设计和实际证据关联起来。
- 重启后的复现应依赖 replay 高层 action，而不是读取旧 `probes.json` 恢复 GDB 状态。

## D008: Hypothesis workflow 分离工具观察和 AI 结论

状态：Accepted

工具只记录 hypothesis、check、assertion result 和 evidence id。AI Agent 的推理和最终
结论必须单独写入 `agent_inference` 和 `final_agent_conclusion`。

原因：

- 工具不承担根因判断。
- 报告需要区分可验证观察和 Agent 推断。
- 这能降低“工具看起来自动下结论”的风险。

## D009: Timeout 语义分离

状态：Accepted

命令 timeout 和 inferior run deadline 是不同概念。run deadline 触发时，工具应该中断
inferior、记录停止原因并执行 light evidence collection。

原因：

- GDB 命令卡住和被调试程序长时间运行是两类故障。
- 报告和状态机需要准确表达发生了什么。

## D010: 目标运行平台只支持 Linux

状态：Accepted

项目的目标运行平台是 Linux。macOS 可以用于文档、编译、`check`、smoke test 等非 live
debugging 工作，但 macOS 上 GDB target、Mach-O、代码签名或架构不匹配导致的 live
session 失败不作为当前阶段阻塞项。

原因：

- 项目的核心目标是提供 Linux 上稳定的 GDB/MI 调试执行层。
- macOS GDB 支持、target 架构和调试权限差异会制造与产品目标无关的噪声。
- 回归测试可以分层：不依赖 GDB 的测试可在 macOS 跑，daemon/live session 测试以 Linux
  环境结果为准。
