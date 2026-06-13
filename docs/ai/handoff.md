# Handoff

日期：2026-06-13

## 本轮完成

- 执行 `docs/ai/next_cli_task.md` 中的
  `real workflow smoke for core/replay/probe/on-hit evidence robustness` 任务。
- 新增 `examples/workflow_fixture.cpp`：
  - `live` mode 先触发 `SIGTRAP`，便于 daemon `create` 后稳定停住。
  - live flow 随后触发 `g_workflow_value` watchpoint、两个普通 breakpoint、一个
    `continue_after_hit` breakpoint 和 `write` syscall catchpoint。
  - `core` mode 在 `workflow_core_capture` 暴露稳定 frame/global/pointer 状态，供 GDB
    batch 生成 core 后执行静态取证。
- 更新 `CMakeLists.txt`：
  - 新增 `workflow_fixture` target。
  - 新增 CTest `real_workflow_flow`。
- 新增 `scripts/smoke_real_workflow_flow.sh`：
  - 覆盖 live probe/on-hit flow：breakpoint、watchpoint、catchpoint metadata，on-hit success、
    failure、skipped 和 `continue_after_hit`。
  - 覆盖 hypothesis workflow：`hypothesis_create`、passed/failed/unknown
    `hypothesis_check`、`hypothesis_conclude`，并检查 hypotheses index 与 report。
  - live session 保存 `stop_on_error` replay plan。
  - 第二个 live session 使用相同 task 跨 session replay，覆盖 success、failed 和 skipped step，
    并检查 `ReplayStep` / `ReplayRun` / `ToolError` evidence。
  - core flow 使用 GDB `generate-core-file` 生成 fixture core，覆盖 core static action 和 dynamic
    action/probe guard rejected。
  - 统一检查 report、`task.normalized.json`、`session_summary.json`、`session_snapshot.json`、
    `evidence/index.json`、raw/summary/view 文件存在性和 report evidence id 引用一致性。
  - 检查 finish-time `probes.json` 同时包含 active metadata 和 deleted historical probe metadata。
- 更新 `docs/ai/progress.md`，记录本轮新增 workflow fixture/smoke 和验证范围。
- `docs/ai/decision.md` 未更新：本轮没有新增或改变项目级 decision。
- `docs/ai/next_cli_task.md` 保留本轮任务说明，并会随本轮提交作为任务记录入库。

## 验证

- `cmake --build build`
  - 结果：通过。
- `./build/gdb-agent check examples/segfault_task.md`
  - 结果：通过，输出 `ok`。
- `./build/task_parser_tests`
  - 结果：通过。
- `./build/replay_plan_tests`
  - 结果：通过。
- `./build/hypothesis_assertion_tests`
  - 结果：通过。
- `./build/mi_summary_tests`
  - 结果：通过。
- `./build/type_sanitizer_tests`
  - 结果：通过。
- `./scripts/smoke_real_workflow_flow.sh`
  - 结果：通过。
  - 覆盖 live probe/on-hit、replay 跨 session、core mode、report/assets/evidence 引用一致性。
- `ctest --test-dir build -R real_workflow_flow --output-on-failure`
  - 结果：通过。
- `ctest --test-dir build --output-on-failure`
  - 结果：14/14 tests passed。
  - 覆盖 segfault demo、daemon/action、core dump、edge case、capability matrix、catchpoint
    matrix、type sanitizer live flow、MI summary live flow、real workflow flow 和所有单元测试。
- `git diff --check`
  - 结果：通过。

## 完成标准审计

- 存在真实 workflow fixture：`examples/workflow_fixture.cpp`。
- 存在新的 Linux + GDB workflow smoke：`scripts/smoke_real_workflow_flow.sh`。
- smoke 已接入 CTest：`real_workflow_flow`。
- smoke 覆盖 live probe/on-hit flow、replay 跨 session flow 和 core mode flow。
- smoke 覆盖成功、失败、skipped 和 core-mode guard rejected 四类结果。
- smoke 检查 report、session summary、snapshot、evidence index、raw/summary/view 文件引用一致。
- smoke 覆盖 `ReplayStep` / `ReplayRun`、`BreakpointHit` / `WatchpointHit` / `CatchpointHit`、
  `OnHitAction`、`ToolError` 和 hypothesis artifacts。
- 未改变外部 action schema、response schema、replay plan schema、evidence raw 文件布局、
  snapshot/session summary 既有字段含义、GDB/MI parser、type sanitizer 或 hypothesis assertion
  语义。

## 限制和注意事项

- 本轮主要新增真实组合 workflow 回归，没有新增用户可见 action。
- `scripts/smoke_real_workflow_flow.sh` 是 Linux + GDB live smoke；没有 GDB 的环境会按既有口径
  skip。
- 本轮未处理 `EvidenceStore::add*` 全量重写 `evidence/index.json` 的长期性能建议。
- 本轮未拆 `src/cli.cpp` 中的 session lifecycle / daemon client glue。
