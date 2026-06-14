# Handoff

日期：2026-06-14

## 本轮完成

- 执行 `docs/ai/next_cli_task.md` 中的 `session concurrency and operation executor` 任务。
- 新增同步 session executor：
  - `src/cli/session_executor.hpp`
  - `src/cli/session_executor.cpp`
- `SessionOperationExecutor` 现在是普通 action、replay step 和 on-hit action 的统一 typed
  execution boundary。
- `GdbSession` 新增 per-session `std::recursive_mutex` operation lock：
  - executor 在执行 action 前持锁。
  - 同一 session 的 GDB/MI action 执行被串行化。
  - recursive mutex 允许 `run` / `continue` 内部触发 on-hit 子 action 时重入 executor。
- 迁移调用路径：
  - 普通 action：`handle_action_request` -> `execute_session_operation` -> `dispatch_action`。
  - replay step：`replay_runtime` 使用 `execute_session_operation(... ReplayStep)`。
  - on-hit action：`probe_runtime` 使用 `execute_session_operation(... OnHitAction)`。
  - `continue_after_hit` 也走 on-hit executor origin。
- 本轮没有新增 OS thread、后台 worker 或 coroutine queue；建立的是同步 executor/lock 边界。
- 本轮没有新增 `record_start` / `record_stop` / `record_status` 等用户可见 record action。
- 本轮没有实现二进制 action group 持久化；record 后续应接在 executor 的 action accepted /
  observed metadata 位置。
- 更新 `CMakeLists.txt`，把 `src/cli/session_executor.cpp` 加入 `gdb-agent`。
- 更新 `docs/ai/progress.md`。
- `docs/ai/decision.md` 已新增：
  - D013：高层 action record 是 replay 的运行期来源。
  - D014：先建立 session 串行执行边界，再实现 record。
- `docs/ai/next_cli_task.md` 已更新为本轮执行任务记录。

## 验证

- `cmake --build build`
  - 结果：通过。
- `./build/gdb-agent check examples/segfault_task.md`
  - 结果：通过，输出 `ok`。
- `./build/replay_plan_tests`
  - 结果：通过，输出 `replay_plan_tests ok`。
- `ctest --test-dir build --output-on-failure`
  - 结果：通过，`15/15 tests passed`。
- `./scripts/smoke_replay_setup_plan_flow.sh`
  - 结果：通过。
  - 覆盖 replay setup plan、`--replay-before-run`、report audit 和 mismatch handling。
- `./scripts/smoke_daemon_action_flow.sh`
  - 结果：通过。
  - 覆盖 daemon/action flow、catchpoint_set、on-hit policy 和 restart replay。
- `git diff --check`
  - 结果：通过。

## 完成标准审计

- 代码中存在清晰 session operation executor：`SessionOperationExecutor`。
- 同一 session 的 action 执行通过 per-session operation mutex 串行化。
- 普通 action 已通过 executor。
- replay step 已通过 executor。
- on-hit action 和 `continue_after_hit` 已通过 executor。
- replay/on-hit 仍依赖 typed `ActionResult.ok` 和 `ActionResult.error`，不解析 response JSON 文本控制流程。
- 现有 replay、on-hit、core mode、hypothesis、evidence/report 行为在全量 CTest 和相关 smoke 下无回归。
- 为后续 record append intent / observed metadata 留出了 `SessionOperationOrigin` 和 executor 入口。

## 限制和注意事项

- 本轮没有引入真正的 per-session worker thread 或 async queue；当前是同步 executor + lock。
- `GdbSession::command` 等低层 API 仍可被 executor 之外的启动/初始化路径调用；这些路径发生在 session
  发布给 daemon/Agent 之前，当前不构成 live action 并发入口。
- `dispatch_action` 仍承载主要 action handler 逻辑；本轮只建立执行边界，没有拆分每个 action handler。
- 后续实现 record 时，应在 executor 中记录被 session 接受的高层 action intent，并避免记录
  `record_*`、`replay`、`finish_session`、`save_action` 和默认未 opt in 的 `raw_mi`。
