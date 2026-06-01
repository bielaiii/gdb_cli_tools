# Handoff

日期：2026-06-01

## 本轮完成

- 完成 Core Dump Mode MVP hardening。
- 新增 `scripts/smoke_core_dump_mode.sh`：
  - 使用 GDB batch 在 `read_session_value` 断点处 `generate-core-file` 生成 core。
  - 不依赖系统 `core_pattern` 或 shell core dump 限制。
  - Linux + GDB 缺失时按现有口径 skip。
- CMake/CTest 新增 `core_dump_mode`。
- daemon `create` core task response 现在包含 `mode:"core"`。
- `session_summary.json` 新增：
  - `core_dump`
  - `core_loaded`
- Core Dump Mode 下 state guard 会拒绝动态 action，并写 `ToolError` evidence：
  - `run`
  - `continue`
  - `breakpoint_set`
  - `watchpoint_set`
  - `catchpoint_set`
  - `probe_enable`
  - `probe_disable`
  - `probe_delete`
- core smoke 覆盖：
  - `gdb-agent check` core task 输出。
  - daemon create/status core session。
  - `backtrace`
  - `threads`
  - `frame_select`
  - `args_info`
  - `locals`
  - `evaluate`
  - core-mode `run` / `continue` 拒绝路径。
  - finish report。
  - `task.normalized.json`
  - `session_summary.json`
  - `session_snapshot.json`
  - `evidence/index.json`
- 同步更新：
  - `docs/task_format.md`
  - `docs/task_format.en.md`
  - `docs/agent_actions.md`
  - `docs/agent_actions.en.md`
  - `docs/evidence_model.md`
  - `docs/evidence_model.en.md`
  - `docs/ai/decision.md`
  - `docs/ai/progress.md`

## 验证

- `cmake --build build`
- `./scripts/smoke_core_dump_mode.sh`
- `ctest --test-dir build --output-on-failure`

当前 Linux 环境安装了 GDB，因此 `core_dump_mode` 和 `daemon_action_flow` 都实际执行了
Linux + GDB smoke，而不是 skip。

## 限制和注意事项

- 本轮没有扩展 Core Dump Mode 的新调试动作，只收敛 MVP 行为和回归覆盖。
- Core smoke 使用 GDB `generate-core-file` 生成断点现场 core，不依赖目标程序真实崩溃后由系统
  写出的 core 文件；后续仍建议用更多真实 core dump 和不同 GDB 输出版本验证兼容性。
- Core Dump Mode 被定义为静态取证模式；需要继续执行程序、命中 breakpoint/watchpoint 或
  on-hit 自动动作时，应使用 Run Mode。
- 本轮没有修改 replay/probe/hypothesis schema。
