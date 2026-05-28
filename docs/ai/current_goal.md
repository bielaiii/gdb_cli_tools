# Current Goal

## 背景

本项目是一个面向 AI Agent 的 GDB 交互与证据管理工具。设计来源以
`final_feature.md` 和 `design.md` 为准：AI Agent 负责分析、提出假设、
选择调试动作和给出最终判断；工具负责稳定执行 GDB 操作、管理 live
session、保存原始证据、生成结构化摘要，并辅助输出可审计的调试报告。

项目不追求把工具做成自动根因分析器。工具只提供执行层、证据层和报告草稿，
最终根因结论必须由 AI Agent 明确给出。

## 当前目标

当前阶段的目标是把 MVP 收敛成一个可靠的 Interactive Session 工具：

1. 通过 Markdown task file 描述调试目标。
2. 启动长驻 GDB/MI live session。
3. 让 AI Agent 通过高层 action 调试程序，而不是直接输入 MI 命令。
4. 支持 Run Mode 和最小 Core Dump Mode。
5. 在每次取证时保存原始 MI、低噪声 summary、human-readable view 和 evidence index。
6. 支持 repeat-run/replay：保存特定 action，在重启或新 session 后一次性重放。
7. 支持 breakpoint/watchpoint 的 metadata、命中记录和 on-hit 自动动作。
8. 支持 hypothesis workflow，并清晰区分工具观察和 AI 结论。
9. 在 finish 时生成 Markdown 报告、session snapshot 和 session summary。

目标运行平台是 Linux。macOS 可用于非 live debugging 的开发校验，但 live GDB session
是否可跑不作为当前阶段验收依据。

## 当前用户价值

工具要让 AI Agent 在排查 crash、core dump、断点验证和假设验证时更可靠：

- 减少 GDB 输出噪声和 token 成本。
- 保留原始证据，避免只依赖摘要。
- 让调试动作可重复、可重放、可审计。
- 强迫定位流程走过“提出假设 -> 设计检查 -> 执行检查 -> 记录证据 -> 更新结论”。

## 非目标

当前阶段不做以下事情：

1. 不支持 PTY 或交互式 inferior stdin。
2. 不把 `session_snapshot.json` 当作可恢复的活 GDB 会话。
3. 不默认暴露 raw MI 给 AI Agent；`raw_mi` 只是高级 escape hatch。
4. 不把工具输出包装成自动根因结论。
5. 不引入独立 Batch Mode；一次性默认取证应作为 live session 的 initial/replay action。

## 近期完成标准

近期工作完成时，应满足：

- `docs/task_format.md` 能让 Agent 正确编写任务文件。
- `docs/agent_actions.md` 覆盖当前可用 action 和 daemon 调用方式。
- `docs/evidence_model.md` 解释 evidence、raw、summary、snapshot、session log 的关系。
- 代码与文档对齐：新增 action、报告字段、证据字段或 session 语义时，同步更新文档。
- 示例任务和 replay 示例可以作为最小回归用例运行。
