# Handoff

日期：2026-06-10

## 本轮完成

- 执行 `docs/ai/next_cli_task.md` 中的 `cli.cpp typed action refactor` 的第一阶段重构，聚焦 typed
  action 边界和去除 replay/on-hit 对 response 文本反解析的控制流依赖。
- 新增 `src/cli/action.hpp` / `src/cli/action.cpp`：
  - 定义 `ActionKind`、`ActionRequest` 和 `ActionResult`。
  - `ActionRequest` 在 action 输入边界从 JSON 解析常用 typed payload 字段。
  - `ActionResult` 统一表达 action `ok`、`finished`、error、evidence id、command evidence id、
    扩展字段和结构化 prelude response。
  - `action_result_to_json` / `action_result_line` 统一把 typed result dump 成现有 JSON response。
- 更新 `src/cli.cpp`：
  - `handle_action_line` 对外改为返回 `ActionResult`。
  - daemon action 和 `serve` stdin loop 通过 `action_result_line` 在边界输出 JSON。
  - replay step 执行直接使用 `ActionResult.ok` / `error` / evidence 字段判断 success/failure，
    不再解析 response 文本中的 `ok:false`。
  - on-hit action 执行直接使用 `ActionResult.ok` / `error` / evidence 字段判断 success/failure，
    不再解析 response 文本。
  - 删除旧的 replay/on-hit response 文本反解析 helper。
  - 保留结构化 `prelude_responses`，确保 on-hit/replay 子 action JSON 行仍按原顺序输出，维持现有
    smoke 和外部行为兼容。
- 更新 `CMakeLists.txt`，把 `src/cli/action.cpp` 加入 `gdb-agent` target。
- 更新 `docs/ai/decision.md`，新增 D012：
  - 内部 action 流程使用 typed structs。
  - JSON/string 只作为 CLI/daemon 边界、replay plan 和 evidence/report artifact 格式。
  - 暂不引入 protobuf、IDL/codegen 或第三方序列化库。
- 更新 `docs/ai/next_cli_task.md`，记录下一轮 typed action refactor 的任务分类和边界约束。
- 更新 `docs/ai/progress.md`，记录本轮 typed action boundary 的实际完成范围。

## 验证

- `cmake -S . -B build`
- `cmake --build build`
- `./build/gdb-agent check examples/segfault_task.md`
- `./build/task_parser_tests`
- `./build/replay_plan_tests`
- `./build/hypothesis_assertion_tests`
- `./build/mi_summary_tests`
- `./build/type_sanitizer_tests`
- `ctest --test-dir build --output-on-failure`
  - 结果：12/12 tests passed。
- `git diff --check`

## 限制和注意事项

- 本轮没有改变用户可见 action JSON schema、response 字段语义、evidence schema、replay plan
  schema、report schema 或 raw evidence 文件布局。
- 本轮没有引入 protobuf 或第三方 JSON 库。
- 本轮完成的是 typed action boundary 和 replay/on-hit 去 response 文本反解析；`src/cli.cpp`
  中的具体 action 分支、probe runtime、session runtime、daemon/client 物理拆分仍可继续做后续小步重构。
- 当前环境具备 Linux + GDB，完整 live smoke 已通过 CTest 实际运行。
