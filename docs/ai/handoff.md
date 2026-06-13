# Handoff

日期：2026-06-13

## 本轮完成

- 执行 `docs/ai/next_cli_task.md` 中的
  `core dump report, metadata audit, and replay semantics` 任务。
- 修复 core 加载命令：
  - 将 `GdbSession::load_core` 从 `-target-select core <mi-quoted-path>` 改为通过
    `-interpreter-exec console "core-file <path>"` 执行。
  - 当前 GDB/MI 对 `-target-select core` 的 quoted path 会把引号当作路径字符，导致实际没有加载
    core；本轮 smoke 已覆盖真实 core 成功加载。
  - core load 失败时不再设置 `stop_reason:"core_loaded"` 或采集静态 core evidence；状态会进入
    error，`session_summary.json` 的 `core_loaded` 会保持 false。
- 增强 `src/report/report.cpp`：
  - Core Dump Mode 下新增 `## Core Dump Snapshot`。
  - 展示 mode、executable、working directory、core dump path、core loaded、state、stop reason、
    signal 和 problem 摘要。
  - 汇总 core load、files/libraries、threads、backtraces、current frame、frame args、locals、
    registers 等静态 evidence id。
  - 汇总 core-mode guard rejected 的 `ToolError` evidence，展示 action 和 reason。
- 扩展 `scripts/smoke_core_dump_mode.sh`：
  - 增加 Python artifact consistency audit，检查 task/session/report/evidence 互相一致。
  - 检查 `session_summary.json` 的 `mode:"core"`、`core_dump` exact path、`core_loaded:true` 和
    evidence count。
  - 检查 `task.normalized.json` core path、`session_snapshot.json`、`evidence/index.json`、
    raw/summary/view 文件、report evidence id 引用。
  - 新增 core mixed replay：
    - `core-continue-audit`：`backtrace` success、`continue` core guard failed、`args_info`
      success。
    - `core-stop-audit`：`backtrace` success、`breakpoint_set` core guard failed、`locals`
      skipped。
  - 检查 `ReplayStep`、`ReplayRun`、`ToolError` evidence 和 report `Replay Execution Audit`
    中的 failed/skipped/error evidence/skip reason。
- 更新用户文档：
  - `docs/agent_actions.md`
  - `docs/agent_actions.en.md`
  - `docs/evidence_model.md`
  - `docs/evidence_model.en.md`
- 更新 `docs/ai/progress.md`，记录本轮 core report、metadata audit 和 replay semantics 进展。
- `docs/ai/decision.md` 未更新：本轮没有新增或改变项目级 decision。
- `docs/ai/next_cli_task.md` 是本轮执行任务记录，会随本轮提交入库。

## 验证

- `cmake --build build`
  - 结果：通过。
- `./scripts/smoke_core_dump_mode.sh`
  - 结果：通过。
  - 覆盖真实 core 加载、静态 action、core guard rejected、Core Dump Snapshot、artifact
    consistency、core mixed replay success/failed/skipped 和 report replay audit。
- `./build/gdb-agent check examples/segfault_task.md`
  - 结果：通过，输出 `ok`。
- `ctest --test-dir build -R core_dump_mode --output-on-failure`
  - 结果：通过，`1/1 Test #3: core_dump_mode Passed`。
- `ctest --test-dir build --output-on-failure`
  - 结果：通过，`15/15 tests passed`。
- `./scripts/smoke_replay_setup_plan_flow.sh`
  - 结果：通过。
- `./build/replay_plan_tests`
  - 结果：通过。
- `git diff --check`
  - 结果：通过。

## 完成标准审计

- report 中存在 core-first 的 `Core Dump Snapshot` 区域。
- core report 区域展示 core path、core loaded 状态和关键静态 evidence 链路。
- core smoke 检查 task/session/evidence/report artifact 一致性。
- core replay mixed plan 覆盖 `continue_on_error` 下 success / failed / success。
- core replay mixed plan 覆盖 `stop_on_error` 下 success / failed / skipped。
- failed dynamic replay step 能追踪到 core guard `ToolError` evidence。
- report 的 replay audit 能展示 core replay step 的 failed/skipped/error evidence/skip reason。
- docs 已说明 core mode 下 replay mixed plan 语义。
- 未改变 core mode 静态取证边界。
- 未改变 action JSON schema、CLI 语法、replay plan schema、evidence raw 文件布局或 task format。

## 限制和注意事项

- 本轮没有新增多线程 core fixture；用户明确只要求 core report、metadata audit 和 core replay
  semantics。
- 本轮没有新增用户可见 action。
- `Core Dump Snapshot` 是 report 聚合视图，不替代 raw evidence 或 session MI log。
- core load 失败路径现在不伪装成 loaded，但本轮没有新增专门的 invalid-core smoke；当前 smoke
  覆盖真实有效 core load。
