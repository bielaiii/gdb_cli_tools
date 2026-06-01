# Next CLI Task

## 目标

完成 Core Dump Mode 的 MVP hardening，让 core dump task 在 daemon/live flow 中有稳定回归覆盖、
清晰的 session/report 表达，以及明确的静态调试边界。

本轮聚焦：

1. 增加 Linux + GDB core dump mode smoke。
2. 明确 core mode 是静态取证模式。
3. 在 core mode 下拒绝不适用的动态 action。
4. 让 session summary、snapshot、report 和 evidence index 稳定呈现 core load 结果。
5. 同步更新用户文档和 AI handoff/progress。

## 背景语义

- Core Dump Mode 加载 executable + core dump，不启动新的 inferior run。
- Core dump 是历史现场，不能可靠继续运行，也不能设置未来命中用的 breakpoint/watchpoint。
- 工具应支持静态取证 action，例如 backtrace、threads、frame select、locals、args、evaluate
  和 hypothesis check。
- 不适用于 core mode 的 action 应稳定拒绝并记录 `ToolError` evidence，而不是把底层 GDB
  错误伪装成成功 action。
- raw evidence 仍是权威来源，summary/report 只是低噪声视图。

## 范围

### 1. Core dump smoke

新增或扩展 smoke，覆盖 Linux + GDB 下的真实 core load flow。

要求：

- 不依赖系统 `core_pattern` 或 shell core dump 限制。
- 可以使用 GDB batch `generate-core-file` 生成测试 core。
- 缺少 GDB 或非 Linux 时按现有项目口径 skip。
- 通过 CTest 运行。

Smoke 至少覆盖：

- 生成 core dump fixture。
- 构造临时 task file，包含 `core dump` 字段。
- `gdb-agent check` 能显示 core dump。
- daemon `create` 加载 core session。
- `status` 显示 `mode:"core"` 和 `stop_reason:"core_loaded"`。
- 静态 action 可执行：
  - `backtrace`
  - `threads`
  - `frame_select`
  - `args_info`
  - `locals`
  - `evaluate`
- 不适用 action 被拒绝：
  - `run`
  - `continue`
- finish 后检查：
  - report
  - `task.normalized.json`
  - `session_summary.json`
  - `session_snapshot.json`
  - `evidence/index.json`

### 2. Core mode state guard

在 core mode 下稳定拒绝动态 action：

- `run`
- `continue`
- `breakpoint_set`
- `watchpoint_set`
- `catchpoint_set`
- `probe_enable`
- `probe_disable`
- `probe_delete`

要求：

- 返回 `ok:false`。
- response 包含 `action`、`error` 和 `evidence`。
- 写入 `ToolError` evidence。
- 不影响静态取证 action 和 hypothesis check。

### 3. Session/report artifacts

强化 core mode artifacts：

- daemon `create` response 应能表达 `mode:"core"`。
- `session_summary.json` 应记录：
  - `mode: "core"`
  - `core_dump`
  - `core_loaded`
- `session_snapshot.json` 保留 core dump task 信息。
- report 明确展示 Core Dump mode，以及 core dump 已加载并执行静态取证。
- evidence index 能找到 `Core load` 的 `SessionEvent` evidence。

### 4. 文档同步

同步更新：

- `docs/task_format.md`
- `docs/task_format.en.md`
- `docs/agent_actions.md`
- `docs/agent_actions.en.md`
- `docs/evidence_model.md`
- `docs/evidence_model.en.md`

如新增或明确项目级语义，更新：

- `docs/ai/decision.md`

任务结束时更新：

- `docs/ai/progress.md`
- `docs/ai/handoff.md`

## 不做

- 不扩展新的 core-specific 高层 action。
- 不支持在 core mode 下继续运行 inferior。
- 不扩展 replay/probe/hypothesis schema。
- 不引入 PTY 或交互式 stdin。
- 不为了 macOS live/core debugging 做兼容；目标运行平台仍是 Linux。
- 不做大型 report 重构。

## 完成标准

- 新增 CTest 能覆盖 core dump mode smoke。
- Linux + GDB 环境实际生成并加载 core dump。
- Core Dump Mode 下静态 action 可用，动态 action 稳定拒绝并记录 `ToolError`。
- `session_summary.json`、`session_snapshot.json`、report 和 evidence index 能稳定体现 core
  load 结果。
- 文档说明 core mode 的静态边界和可用 action。
- `docs/ai/progress.md` 和 `docs/ai/handoff.md` 记录实际完成、验证结果和限制。
