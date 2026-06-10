# Handoff

日期：2026-06-10

## 本轮完成

- 执行 `docs/ai/next_cli_task.md` 中的 remaining string/Json internal transports 清理任务。
- 校正任务边界：不是消灭所有 KV，而是移除内部 JSON/string 协议；结果侧天然半开放 metadata
  保留结构化 KV。
- 更新 `src/cli/action.hpp` / `src/cli/action.cpp`：
  - `ActionRequest` 使用 `std::variant` typed payload，不再持有 raw `Json on_hit` 或
    `Json saved_action`。
  - on-hit action 和 save-action nested action 在边界解析为 typed `ActionRequest`。
  - timeout/deadline 增加显式设置标记，on-hit 默认注入不会覆盖用户显式设置。
  - `ActionResult` 删除 generic `Json fields` 和 `std::vector<Json> prelude_responses`。
  - 新增 `ResultField` / `ResultObject` / `ResultArray` 结构化 KV，以及 `ActionOutput`
    typed prelude/final 输出容器。
- 更新 `src/cli.cpp`：
  - 删除旧 `handle_action_line_legacy` / `handle_action_line` 字符串 handler 和 response 文本
    反解析 wrapper。
  - action dispatch 改为 `dispatch_action(..., const ActionRequest&) -> ActionOutput`。
  - `serve` 和 daemon action 只在边界把 `ActionOutput` dump 成 JSON 行。
  - on-hit policy 运行期保存 typed action request；on-hit 执行和 `continue_after_hit` 不再构造
    或修改 action JSON 字符串。
  - replay runtime 新增 typed `ReplayActionStep`；structured plan、JSONL 和 legacy single-action
    replay 文件读取后立即转换为 typed action，再进入 dispatcher。
- 更新 `src/replay/replay_plan.hpp` / `src/replay/replay_plan.cpp`：
  - `write_replay_plan` 内部 API 改为接收 `std::vector<ActionRequest>`。
  - replay plan 文件写出时从 typed action dump 现有 JSON schema。
- 更新 `tests/replay_plan_tests.cpp` 和 `CMakeLists.txt`：
  - replay plan test 使用 typed action 调用 writer。
  - test target 链接 `src/cli/action.cpp`。
- 更新 `docs/ai/progress.md` 和 `docs/ai/decision.md`：
  - 记录本轮执行结果。
  - D012 补充 `ActionOutput` 和结构化 KV 的边界决策。

## 验证

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

## 完成标准审计

- `ActionRequest` 不再持有 raw `Json` payload。
- `ActionResult` 不再持有 `Json fields` 或 `std::vector<Json> prelude_responses`。
- `handle_action_line_legacy` 已删除。
- on-hit policy 不再用 `std::vector<std::string>` 保存 action JSON。
- on-hit 执行不再 parse/dump action JSON 来修改 timeout/deadline。
- replay runtime 不再以 `std::string line` / action JSON string 作为执行输入。
- replay plan writer 的内部 API 不再接收 `std::vector<std::string>` action JSON。
- JSON/string 仍保留在 CLI/daemon 边界、artifact 写入、report/evidence/debug 文本和业务字符串字段。

## 限制和注意事项

- 本轮没有改变用户可见 action JSON schema、response 字段语义、evidence schema、replay plan
  schema、report schema 或 raw evidence 文件布局。
- 本轮没有引入 protobuf、IDL/codegen 或第三方 JSON 库。
- `src/cli.cpp` 仍然较大；本轮按任务只清理内部 transport，未继续做 probe/runtime/session 的物理拆分。
- 当前环境具备 Linux + GDB，完整 live smoke 已通过 CTest 实际运行。
