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
结构化 plan 必须包含 schema version、plan name、source session id、created_at、task
metadata、task fingerprint、failure policy 和 action list。重放时每步 action 在新
session 中重新执行，并产生新的 evidence id。失败步骤记录 `ReplayStep` 和/或
`ToolError` evidence，是否继续由 plan-level 或 step-level failure policy 决定。
旧 JSONL 和旧 plan 默认按 `continue_on_error` 兼容读取。

Replay 前必须校验 schema 和 task fingerprint。task 不匹配时默认拒绝执行并记录
`ToolError`；只有显式 force 时才允许执行，并通过 result 与 `ReplayWarning` evidence
记录 mismatch warning。

原因：

- 高层 action 比 raw MI 更稳定。
- 新 session 的证据不能复用旧 evidence id。
- replay 是 repeat-run 的核心能力。
- task fingerprint 能降低把 replay plan 用到错误调试目标上的风险。

## D007: Probe metadata 属于工具状态

状态：Accepted

breakpoint/watchpoint/catchpoint 需要保存 comment、purpose、condition、hit count、
last stop reason 和 on-hit policy。on-hit policy 包含高层 action 列表、timeout/output
预算、failure policy 和可选 `continue_after_hit`。运行期以内存 `ProbeState` 为权威状态；
`assets/probes.json` 只在 finish/report 写出阶段作为最终快照生成，不作为运行时同步数据库或
live GDB 恢复文件。`probe_list` 默认只返回 active/live probes；`probe_delete` 后的历史
probe 可以保留在最终 `assets/probes.json`，但必须标记 `deleted:true`，避免 Agent 把历史
metadata 当作仍可命中的 live probe。Probe 命中时写入 `BreakpointHit`、`WatchpointHit` 或
`CatchpointHit` evidence，并保留当次命中的必要 metadata 快照、on-hit result、on-hit
evidence id 和 error id。每个自动 on-hit action 额外写入 `OnHitAction` evidence。

原因：

- Agent 需要知道断点为什么存在，而不只是 GDB 编号。
- on-hit 自动动作可以降低重复取证成本。
- 命中记录能把 probe 设计和实际证据关联起来。
- 重启后的复现应依赖 replay 高层 action，而不是读取旧 `probes.json` 恢复 GDB 状态。

## D008: Hypothesis workflow 分离工具观察和 AI 结论

状态：Accepted

工具只记录 hypothesis、check、assertion result 和 evidence id。`hypothesis_check`
的 `status` 只允许表达工具级检查结果：`passed`、`failed` 或 `unknown`。未知 assertion
或缺少必需 `expected` 的 assertion 必须稳定记录为 `unknown`，并引用 `ToolError`
evidence；它不代表 hypothesis 被支持或反驳。AI Agent 的推理和最终结论必须单独写入
`agent_inference` 和 `final_agent_conclusion`。

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

## D011: Core Dump Mode 是静态取证模式

状态：Accepted

Core Dump Mode 加载 executable 和 core dump 后只支持静态取证 action，例如 `backtrace`、
`threads`、`frame_select`、`locals`、`args_info`、`evaluate` 和 hypothesis check。
`run`、`continue`、`breakpoint_set`、`watchpoint_set`、`catchpoint_set` 以及 probe
enable/disable/delete 在 core mode 下会被 state guard 拒绝，并记录 `ToolError` evidence。

原因：

- core dump 不是 live inferior，不能可靠继续运行或设置未来命中用的 probe。
- 明确拒绝动态 action 比把 GDB 错误伪装成普通 action result 更利于 Agent 判断下一步。
- Core Dump Mode 的 MVP 价值在于稳定离线取证，而不是模拟 live session。

## D012: 内部 action 流程使用 typed structs，JSON/string 只作为边界格式

状态：Accepted

内部 action 流程应使用普通 C++ `struct` 和 `enum class` 表达请求、payload、结果和状态。
CLI/daemon 输入边界可以把 JSON parse 成 typed `ActionRequest`；handler 只接收类型化
request，并返回 typed `ActionOutput` / `ActionResult`；CLI/daemon 输出边界再把 typed result
dump 成现有 JSON response。handler 不应直接写 `ostream`，不应返回拼接好的 JSON response
字符串，也不应把 response 文本作为 replay、on-hit 或其他内部控制流协议。

请求侧已知 action payload 应使用 action-specific typed struct。结果侧天然半开放的扩展字段
不需要硬拆成大量 action-specific result struct，可以保留 `ResultField` / `ResultObject` /
`ResultArray` 这样的结构化 KV；但 KV value 必须是结构化类型，不能把 JSON 文本或 generic
`Json` AST 当作内部结果协议。

JSON 和文本仍然是外部接口与审计 artifact 的格式：CLI/daemon 输入输出、replay plan/JSONL、
evidence payload、session files、report 和 human-readable debug/summary 可以继续使用 JSON、
Markdown 或普通文本。业务本身需要字符串的字段也继续使用 `std::string`，例如 GDB
expression、location、raw MI command、error message、evidence id、hypothesis title 和 debug
text。这里禁止的是把序列化后的 action/response 文本当成内部模块协议，而不是禁止业务字符串。

暂不引入 protobuf、IDL/codegen 或第三方序列化库。当前外部协议已经是 JSON，本项目现阶段需要解决
的是内部 action handler、replay 和 on-hit 逻辑对 JSON/string/ostream 的耦合，而不是跨语言二进制
协议或复杂 schema evolution。

原因：

- typed struct 能降低 handler 手写 JSON、字段遗漏、转义错误和 response 文本反解析带来的风险。
- replay 和 on-hit 可以直接依赖 `ActionResult.ok` 等结构化状态，而不是解析 `ok:false` 文本。
- 结构化 KV 保留了 action response metadata 的可扩展性，同时避免内部依赖 serialized JSON。
- 保持 JSON 作为外部边界格式可以兼容现有 CLI、daemon、smoke、report、evidence 和 replay plan。
- protobuf 会引入依赖、生成代码、CMake 集成和 schema 维护成本；对当前内部重构收益不足。

## D013: 高层 action record 是 replay 的运行期来源

状态：Accepted

工具需要提供与高层 replay 对称的高层 record 能力。record 记录的是 Agent 提交并被 session
接受的高层 action intent，而不是 GDB process record/reverse debugging，也不是旧 action 的执行
结果。replay 执行时必须重新执行这些 action，并产生新的 evidence id。

record 的运行期权威状态默认保存在 session 内存中，例如 `RecordingState` 持有当前 plan name、
failure policy、checkpoint policy 和 `std::vector<ActionRequest>`。这避免把 JSONL append log
当成产品层核心状态，也让普通 action、on-hit action 和 replay step 可以共享 typed action
pipeline。record/replay/on-hit 不应绕过 session 的串行 action 执行入口。

工具同时需要提供持久化 action 组的接口。持久化用于跨 gdb-agent 重启复用、审计和报告引用；
默认 record 行为可以先写入内存，但必须允许显式 checkpoint 或 stop 时落盘。推荐策略：

- `record_start` 默认创建内存 buffer。
- 普通高层 action 被接受后追加到内存 buffer。
- `record_stop` 将内存 buffer 写成可重放 plan artifact。
- 可选 checkpoint policy 支持每步落盘，降低 gdb-agent 自身 crash 或被 kill 时的丢失风险。
- `session_snapshot.json` 和 `session_summary.json` 仍不是 record/replay 恢复文件。

持久化格式优先考虑版本化二进制格式，作为工具内部 action 组 artifact 的权威存储。二进制格式需要
包含 schema magic/version、plan metadata、task fingerprint、failure policy、step list 和每步
typed action payload，并提供稳定的读取校验与错误报告。为了人工检查、review、diff 和文档示例，
必须同时提供字符串格式化接口，把二进制 action plan 导出为人类可读文本；该文本可以是 JSON、
JSONL 或 Markdown view，但它不是运行期权威状态。现有 JSON/JSONL replay plan 可以作为兼容读取和
human-readable export/import 格式保留，但新 record 抽象不应依赖 JSONL 作为唯一主存储。

默认不自动记录 `record_*`、`replay`、`finish_session` 和 `save_action` 自身。`raw_mi` 是高级
escape hatch，默认不进入自动 record，除非显式 opt in 并在 plan metadata 中标记 advanced risk。

原因：

- 有高层 replay 就需要对称的高层 record，避免 Agent 只能用 `save_action` 手动拼 plan。
- 内存 buffer 更符合 live session 的运行期语义，文件是持久化 artifact，而不是唯一状态源。
- 二进制格式更适合作为稳定、紧凑、可校验的内部 action 组存储；字符串格式更适合人工审计。
- checkpoint 解决的是 gdb-agent 自身退出或崩溃导致的 durability 问题；inferior segfault 不会让
  gdb-agent 内存 record 自动丢失。
- 串行 session executor 能保证 record 的 action 顺序、实际执行顺序和 replay 审计顺序一致。

## D014: 先建立 session 串行执行边界，再实现 record

状态：Accepted

在实现高层 record 前，应先拆出 session 级 action executor 或等价的串行操作队列。这里的目标不是
引入多个线程并共享同一份 GDB/session 状态，而是明确操作对象和执行边界：

- Agent/RPC 接入层负责接收请求、解析输入和定位 session。
- 每个 live session 有一个串行 action executor，负责按顺序执行普通 action、replay step、
  on-hit action 和后续 record append。
- GDB/MI transport 由该 session executor 独占写命令；MI reader 可以是 async reader/coroutine，
  但不能让多个调用方并发向同一个 GDB session 发命令。
- Evidence writer、record/replay store 可以作为独立模块，但通过 session executor 观察 action
  顺序和执行结果。

如果底层实现需要线程，推荐使用少量 worker/coroutine 和 per-session queue/actor 模型；不推荐让
Agent thread、replay thread、on-hit thread、record thread 分别直接操作共享 `GdbSession`、
`ProbeState`、`RecordingState` 或 GDB/MI pipe。跨对象通信应通过明确的 request/result 结构完成，
不要靠 shared mutable state 和大量 mutex 维持语义。

原因：

- GDB/MI 是状态机，同一 session 的命令必须串行化。
- record 必须记录 session 接受并按顺序执行的高层 action，不能旁路 action pipeline。
- replay、on-hit 和未来 record 都会触发嵌套 action；先建立 executor 边界能避免乱序和证据归属错误。
- 多线程不能解决 durability；record 持久性应依赖 checkpoint/atomic write，而不是线程数量。
