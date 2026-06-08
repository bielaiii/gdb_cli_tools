# MVP Acceptance

本文件定义当前 MVP 的验收标准。它面向后续 Agent 和用户，用来判断当前工具是否处在可
dogfood、可交接、可继续扩展的状态。

## 验收口径

MVP 的目标不是自动根因分析，而是提供稳定的 GDB/MI 调试执行层和可审计 evidence 链路。
验收时应确认工具能完成一轮最小 Agent 调试流程：

1. 校验 task file。
2. 创建 live session。
3. 执行高层 action。
4. 保存 raw evidence、summary、Markdown view 和 index。
5. 让 Agent 基于 evidence 写 inference/conclusion。
6. finish 后生成 report、session summary 和 snapshot。

最终根因结论必须来自 Agent，不来自工具自动判断。

## 平台

- 目标运行平台：Linux。
- Live session、daemon smoke、core dump smoke 需要系统可用 `gdb`。
- macOS 可用于文档、构建和不依赖 GDB 的测试；macOS live GDB 失败不阻塞 MVP。

## 构建和基础校验

MVP 必须能从干净构建目录完成配置和构建：

```bash
cmake -S . -B build
cmake --build build
```

示例 task file 必须能通过校验：

```bash
./build/gdb-agent check examples/segfault_task.md
```

预期至少包含：

```text
ok
executable: ...
working directory: ...
stdin: /dev/null
run timeout ms: 30000
```

## Daemon Flow

MVP 应支持多轮 daemon 调试流程：

```bash
./build/gdb-agent daemon --socket /tmp/gdb-agent.sock
./build/gdb-agent create examples/segfault_task.md \
  --socket /tmp/gdb-agent.sock \
  --session S1 \
  --out report.md \
  --assets report.assets
./build/gdb-agent action S1 '{"action":"backtrace"}' --socket /tmp/gdb-agent.sock
./build/gdb-agent finish S1 --socket /tmp/gdb-agent.sock --out report.md
```

验收点：

- `create` 返回 session metadata 和当前 stop 状态。
- `action` 返回 `ok`、`action` 和 evidence 相关字段。
- `finish` 写出 report 和 assets，并关闭该 session。
- `close` 只关闭 session，不代表定位完成；finish 才表示本轮调试流程结束。

## Run Mode

Run Mode 的最小验收目标是：程序运行到 crash、breakpoint、watchpoint、timeout interrupt、
normal exit 或其他 stop event 后，工具能保存对应 evidence，并允许 Agent 继续执行静态高层
action。

示例 `examples/segfault_task.md` 应能覆盖最小 crash/stop 取证路径。Report 中应能看到：

- task 配置。
- session summary。
- evidence summary。
- raw evidence index。
- limitations。

## Core Dump Mode

Core Dump Mode 是静态取证模式。MVP 验收点：

- task file 中提供 `core dump` 时，工具加载 executable + core dump。
- 静态 action 可用，例如 `backtrace`、`threads`、`frame_select`、`locals`、`args_info`、
  `evaluate` 和 `hypothesis_check`。
- 动态 action 会被拒绝并记录 `ToolError` evidence，例如 `run`、`continue`、
  `breakpoint_set`、`watchpoint_set`、`catchpoint_set` 和 probe mutation。
- `session_summary.json` 记录 core dump 路径和是否成功 loaded。

## Evidence 验收

每条 evidence 必须保留可审计链路：

- raw file。
- low-noise summary。
- human-readable Markdown view。
- evidence index entry。
- raw SHA-256。
- raw byte count、kept summary byte count、`truncated` 和 `lossy_summary` 标记。

Raw evidence 是审计来源；summary 是有损、低噪声视图，不能替代 raw。

## Report 验收

Report 必须引用 evidence id，而不是只复制 summary。至少应包含：

- Task。
- Session Summary。
- Tool Observations。
- Tool Errors，如果有 `ToolError` evidence。
- Evidence Summary。
- Hypotheses，如果存在 hypothesis artifacts。
- Agent Inference。
- Final Agent Conclusion。
- Limitations。
- Raw Evidence Index。

Report 是报告草稿，不是自动根因结论。

## Replay / Probe / Hypothesis

MVP 至少需要有可运行 smoke 或文档入口说明这些能力：

- replay：保存高层 action，在同类 session 中重放，并产生新的 evidence id。
- probe：breakpoint/watchpoint/catchpoint metadata、on-hit action 和 hit evidence。
- hypothesis workflow：create/check/conclude，且工具 observation 与 Agent inference/conclusion 分离。

相关入口：

- `docs/agent_actions.md`
- `docs/evidence_model.md`
- `docs/mvp_quickstart.md`

## 回归入口

基础回归：

```bash
cmake --build build
./build/gdb-agent check examples/segfault_task.md
git diff --check
```

不依赖 GDB 的单元测试：

```bash
./build/hypothesis_assertion_tests
./build/mi_summary_tests
./build/replay_plan_tests
./build/task_parser_tests
```

完整回归入口：

```bash
ctest --test-dir build --output-on-failure
```

当前 Linux + GDB 环境下，CTest 应实际运行 daemon/action、core dump、edge case 和 capability
matrix smoke。没有 GDB 的环境需要明确说明 live smoke 未运行或被跳过的原因。

## 非阻塞限制

以下限制不阻塞当前 MVP：

- 不支持 PTY。
- 不支持交互式 inferior stdin。
- Core Dump Mode 不支持动态 action/probe 操作。
- `catchpoint_set` 当前只支持 `catch throw` 和 `catch catch`。
- Numeric assertion 当前只支持整数，不支持浮点数。
- Sanitizer 不是完整 C++ demangler。

完整列表见 `docs/known_limitations.md`。
