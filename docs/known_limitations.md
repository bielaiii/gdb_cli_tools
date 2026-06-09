# Known Limitations

本文件集中记录当前 MVP 的明确限制。这里列出的限制是当前设计边界或后续工作项，不应被误解为
当前 MVP 的 blocker。

## 平台

- 目标运行平台只支持 Linux。
- Live GDB session 需要系统可用 `gdb`。
- macOS 可以用于文档、构建和不依赖 GDB 的测试；macOS live GDB 失败不作为当前阶段阻塞项。

## GDB / I/O

- 不支持 PTY。
- 不支持交互式 inferior stdin。
- Inferior stdin 只能是 `/dev/null` 或 task file 的 `stdin` 字段指定的文件。
- Inferior stdout/stderr 会重定向到 assets 并作为 `InferiorOutput` evidence 捕获。
- 非 PTY 可能影响目标程序的 `isatty` 行为；report 会记录该限制。

## Session 恢复

- `session_snapshot.json` 不是 live GDB 会话恢复文件。
- `session_summary.json` 不是 live GDB 会话恢复文件。
- 这些文件是报告 artifacts 和历史记录，不能恢复旧 GDB 进程、frame、thread、inferior memory
  或临时断点。
- 重启后的恢复方式是 replay 高层 action，不是恢复旧进程。
- Replay 会在新 session 中重新执行 action，并产生新的 evidence id。

## Core Dump Mode

- Core Dump Mode 是静态取证模式。
- 支持静态 action，例如 `backtrace`、`threads`、`frame_select`、`locals`、`args_info`、
  `evaluate` 和 `hypothesis_check`。
- 不支持动态 action/probe 操作，例如 `run`、`continue`、`breakpoint_set`、`watchpoint_set`、
  `catchpoint_set` 和 probe enable/disable/delete。
- 被拒绝的动态 action 会记录 `ToolError` evidence。

## Actions

- Agent 默认应使用高层 action，而不是 raw MI。
- `raw_mi` 是高级 escape hatch，必须显式包含 `risk:"advanced"`。
- `catchpoint_set` 支持 C++ exception 的 `catch throw` / `catch catch`，以及 Linux live
  session 中常用的 `catch syscall`、`catch fork`、`catch vfork` 和 `catch exec`。
- syscall selector 只接受字符串 syscall 名或整数 syscall id；字符串只允许字母、数字和下划线。
- 具体 catchpoint command 是否可用仍取决于当前 Linux/GDB/target 组合；不支持时工具会返回
  `ok:false` 并保留 `ToolError` 与原始 command evidence。
- `raw_mi` 不能作为 on-hit action。

## Evidence 和 Summary

- Raw evidence 是审计来源。
- Summary 是有损、低噪声视图，不能替代 raw evidence。
- Summary 可能经过 MI stream 解码、sanitizer、摘要化或截断。
- `lossy_summary` 和 `truncated` 标记用于提醒 Agent 不要把 summary 当作完整原文。
- Sanitizer 不是完整 C++ demangler。
- 当前 sanitizer 只做有限降噪，例如 `std::string` 归一化、常见 STL 容器
  allocator/hash/comparator 噪声压缩、智能指针 deleter 噪声压缩、`std::pair<const K, V>`
  key const 压缩、工作目录路径相对化和 backtrace/thread 稳定摘要。

## Report

- Report 是调试报告草稿，不是自动根因结论。
- Report 会引用 evidence id，并提供 Agent inference / final conclusion 区域。
- 如果 Agent 没有提供 inference/conclusion，report 会明确说明缺失，而不会自动补结论。
- Tool Errors 是工具执行或 GDB 拒绝的证据，不等于问题根因。

## Hypothesis Workflow

- `hypothesis_check` 是工具级 assertion，不是最终根因判断。
- `passed` 表示 assertion 对 observed summary 成立，不表示 hypothesis 自动被证明。
- `failed` 表示 assertion 对 observed summary 不成立，不表示 hypothesis 自动被证伪。
- `unknown` 表示工具无法判断该 check，不支持也不反驳 hypothesis。
- `observed` 来自有损 summary；关键判断前应检查 linked raw evidence。
- Agent 的推理和最终结论必须写入 `agent_inference` 和 `final_agent_conclusion`。

## Assertion 支持范围

- 字符串/null assertion 已支持基础集合：`contains`、`not_contains`、`is_null`、`non_null`、
  `equals`、`not_equals`。
- 整数 assertion 已支持：`greater_than`、`less_than`、`greater_equal`、`less_equal`、
  `equals_number`、`not_equals_number` 和 `between`。
- 地址 assertion 已支持：`address_non_null` 和 `address_equals`。
- Numeric assertion 当前只支持整数，不支持浮点数。
- `changed` 或跨 check 历史比较尚未实现。
- 多个不同整数或地址会返回 `unknown`，避免从有损 summary 中猜测。

## 当前非目标

- 不支持交互式终端调试体验。
- 不做自动根因分析器。
- 不把 snapshot 当持久化 debugger。
- 不做完整 C++ demangling。
- 不为了 macOS live GDB 做兼容。
