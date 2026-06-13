# Handoff

日期：2026-06-13

## 本轮完成

- 执行 `docs/ai/next_cli_task.md` 中的
  `replay setup plan and report auditability hardening` 任务。
- 补齐 `watchpoint_set` 状态守卫：
  - live session 的 ready/stopped/exited state 都允许设置 watchpoint。
  - core mode 仍拒绝 `watchpoint_set`，保持动态 probe 在 core 中不可用的既有约束。
  - 这使 `--replay-before-run` 能在第一次 `run` 前通过 setup plan 安装
    breakpoint/watchpoint/catchpoint。
- 扩展 `examples/workflow_fixture.cpp`：
  - live path 在现有 breakpoint/stop-policy/auto-continue 路径后新增稳定 C++ throw 事件。
  - 新增事件用于 replay setup plan smoke 的 catchpoint hit 覆盖，避免 syscall catchpoint 在
    启动阶段受环境噪声影响。
- 增强 `src/report/report.cpp`：
  - `Replay Plans` 区域改为结构化表格，展示 plan file、name、tags、source session、
    failure policy 和 task fingerprint。
  - 新增 `Replay Execution Audit` 区域，按结构化 `ReplayRun`、`ReplayStep` 和
    `ReplayWarning` evidence 生成 replay runs、steps 和 warnings 表。
  - 报告不从 replay response 文本反解析状态，继续使用结构化 evidence summary。
- 新增 `scripts/smoke_replay_setup_plan_flow.sh`：
  - RS1：通过 `save-action` 生成 probe setup plan 和 `stop_on_error` audit plan，检查
    schema、tags、source session、fingerprint 和 action。
  - RS2：用 `--replay-before-run` 应用 setup plan，确认 replay-set breakpoint、watchpoint、
    catchpoint 真实命中，并产生 on-hit evidence。
  - RS3：replay `stop_on_error` plan，覆盖 success、failed、skipped step 和 skip reason。
  - RS4：覆盖 task fingerprint mismatch 默认拒绝，以及 `--force` 后的 `ReplayWarning`。
  - 全流程检查 report/assets/session/evidence 引用一致性。
- 更新 `CMakeLists.txt`，新增 CTest `replay_setup_plan_flow`。
- 更新用户文档：
  - `docs/agent_actions.md`
  - `docs/agent_actions.en.md`
  - `docs/evidence_model.md`
  - `docs/evidence_model.en.md`
- 更新 `docs/ai/progress.md`，记录本轮 replay setup/report audit 进展。
- `docs/ai/decision.md` 未更新：本轮没有新增或改变项目级 decision。
- `docs/ai/next_cli_task.md` 保留本轮任务说明，并随本轮提交作为任务记录入库。

## 验证

- `cmake --build build`
  - 结果：通过。
- `./scripts/smoke_replay_setup_plan_flow.sh`
  - 结果：通过。
  - 覆盖 `save-action` 生成 setup plan、`--replay-before-run`、三类 probe 命中、on-hit、
    replay failed/skipped、mismatch reject/force warning 和 report audit。
- `./build/gdb-agent check examples/segfault_task.md`
  - 结果：通过，输出 `ok`。
- `ctest --test-dir build --output-on-failure`
  - 结果：15/15 tests passed。
  - 覆盖 segfault demo、daemon/action、core dump、edge case、capability matrix、catchpoint
    matrix、type sanitizer live flow、MI summary live flow、real workflow flow、
    replay setup plan flow 和所有单元测试。
- `git diff --check`
  - 结果：通过。

## 完成标准审计

- `save_action` 生成的 probe setup plan 已通过 smoke 检查，包含结构化 schema、空 tags、
  source session、task fingerprint 和高层 action。
- `--replay-before-run` 已在新 session 初始运行前真实应用 setup plan。
- replay-set breakpoint/watchpoint/catchpoint 均真实命中，并产生 hit/on-hit evidence。
- replay success、failed、skipped 和 mismatch warning 均能在 report 中通过
  `Replay Execution Audit` 追踪到结构化 evidence。
- 内部 replay/on-hit/dispatcher 仍使用 typed `ActionRequest` / `ActionOutput` /
  `ActionResult` 数据流；本轮未引入 response text JSON 反解析。
- 未改变 replay plan schema、action JSON schema、response schema、task format、evidence raw
  文件布局、GDB/MI parser、type sanitizer 或 hypothesis assertion 语义。

## 限制和注意事项

- 新增 `replay_setup_plan_flow` 是 Linux + GDB live smoke；没有 GDB 的环境会按既有口径 skip。
- 本轮 report audit 依赖现有 `ReplayRun` / `ReplayStep` / `ReplayWarning` summary JSON；
  如果未来这些 evidence schema 扩展，报告表格可继续按结构化字段补列。
- 本轮没有实现 replay plan tags 的用户输入接口；只验证并展示现有 schema 中的空 tags 字段。
- 本轮没有处理长期建议：更丰富的真实项目 core 样本、更多 watchpoint/catchpoint GDB 版本差异覆盖、
  或 `EvidenceStore::add*` 重写 index 的性能优化。
