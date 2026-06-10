# Handoff

日期：2026-06-10

## 本轮完成

- 执行 `docs/ai/next_cli_task.md` 中的
  `extract ActionContext and split action dispatch` 任务。
- 新增 `src/cli/action_context.hpp`：
  - 将 `ProbeState` 从 `src/cli.cpp` 移入共享 header。
  - 新增 `ActionContext`，集中借用 `GdbSession`、`DebugTask`、`SessionOutcome` 和 `ProbeState`。
- 新增 `src/cli/action_dispatch.hpp` / `src/cli/action_dispatch.cpp`：
  - 新增公开入口 `dispatch_action(ActionContext&, const ActionRequest&)`。
  - 新增 typed boundary `handle_action_request(ActionContext&, ...)` 和
    `handle_action_json(ActionContext&, ...)`。
  - 将原 `src/cli.cpp` 中完整 `switch (request.kind)` action dispatch 迁移到新模块。
  - 将 dispatch 直接依赖的 on-hit、replay、hypothesis、probe helper 一并迁移，避免
    `src/cli.cpp` 继续承载完整 action runtime。
- 更新 `src/cli.cpp`：
  - 保留 CLI/daemon/session/report 顶层 orchestration。
  - 在 create/serve/action/replay-before-run 等入口构造 `ActionContext` 后调用 action dispatch。
  - 保留 debug target metadata / session file / report 等顶层 flow glue。
- 更新 `CMakeLists.txt`，把 `src/cli/action_dispatch.cpp` 接入 `gdb-agent` target。
- 更新 `docs/ai/progress.md`，记录本轮架构重构完成情况。

## 验证

- `cmake --build build`
- `./build/gdb-agent check examples/segfault_task.md`
- `./build/task_parser_tests`
- `./build/replay_plan_tests`
- `./build/hypothesis_assertion_tests`
- `./build/mi_summary_tests`
- `./build/type_sanitizer_tests`
- `ctest --test-dir build --output-on-failure`
  - 结果：13/13 tests passed。
  - 覆盖 daemon/action、core dump、edge case、capability matrix、catchpoint matrix、
    type sanitizer 和 MI summary live flow。
- `git diff --check`

## 完成标准审计

- 存在明确的 `ActionContext` 类型。
- `dispatch_action` 公开入口已从 `src/cli.cpp` 移到 `src/cli/action_dispatch.cpp`。
- `src/cli.cpp` 不再直接承载完整 action dispatch switch。
- action runtime 仍使用 typed `ActionRequest` / `ActionOutput`；没有回退到内部 JSON/string 协议。
- 本轮没有改变用户可见 CLI 命令、action JSON schema、response JSON 字段语义、多行输出顺序、
  evidence schema、replay plan schema、report schema 或 raw evidence 文件布局。
- 本轮未新增项目级 decision，因此没有更新 `docs/ai/decision.md`。

## 限制和注意事项

- `src/cli/action_dispatch.cpp` 现在承载 dispatch 及其直接依赖的 on-hit/replay/hypothesis/probe
  helper；这是本轮刻意的小步迁移，不是最终模块边界。
- 后续可继续拆：
  - `probe_runtime`：probe metadata、hit attribution、on-hit execution。
  - `replay_runtime`：replay execution、failure policy、ReplayStep/ReplayRun evidence。
  - `hypothesis_store`：hypothesis markdown/index 持久化。
- `src/cli.cpp` 仍包含 daemon/session/report orchestration；本轮没有拆 daemon server。
