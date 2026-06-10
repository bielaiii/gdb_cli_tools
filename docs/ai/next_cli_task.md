# Next CLI Task: extract ActionContext and split action dispatch

## 目标

下一轮做一次 **行为保持型架构重构**：从 `src/cli.cpp` 中抽出 action 执行上下文和 action
dispatch 边界，为后续拆分 probe runtime、replay runtime、hypothesis store 和 daemon server
打基础。

本轮不新增功能、不改变用户可见行为、不改变任何外部 JSON schema。所有 CLI/daemon command、
action request、action response、evidence、report、replay plan 和 smoke 断言都必须保持兼容。

## 当前背景

当前已经完成 D012：内部 action 流程使用 typed structs，JSON/string 只作为边界格式。
`ActionRequest` / `ActionOutput` 已经存在，on-hit 和 replay runtime 也不再通过 response 文本反解析。

但 `src/cli.cpp` 仍然承担过多职责：

- CLI / daemon command routing。
- action dispatch 和 action handler。
- action state guard 和 error evidence 写入。
- probe store / on-hit runtime glue。
- replay runtime glue。
- hypothesis markdown/index glue。
- finish/report/session artifact orchestration。

本轮只做第一步：抽出 `ActionContext`，并把 action dispatch 从 `src/cli.cpp` 拆到独立模块。
不要顺手拆 probe/replay/hypothesis/daemon；那些是后续任务。

## 实现范围

允许修改：

- `src/cli/action.hpp`
- `src/cli/action.cpp`
- `src/cli.cpp`
- 新增 `src/cli/action_context.hpp` / `.cpp`，如果需要。
- 新增 `src/cli/action_dispatch.hpp` / `.cpp`，用于承载 action dispatch 和直接相关 helper。
- `CMakeLists.txt`，仅用于接入新增 `.cpp`。
- `tests/replay_plan_tests.cpp`，仅当新增链接单元导致测试 target 需要补源文件。
- `docs/ai/progress.md`
- `docs/ai/handoff.md`
- `docs/ai/decision.md`，仅当新增或改变项目级决策；本轮默认不需要。

原则上不要修改：

- `docs/agent_actions.md` / `.en.md`
- `docs/evidence_model.md` / `.en.md`
- `docs/task_format.md` / `.en.md`
- `docs/known_limitations.md`
- report 内容。
- replay plan schema。
- evidence schema 或 raw evidence 文件布局。
- GDB/MI parser、type sanitizer、hypothesis assertion 行为。

禁止：

- 新增或删除用户可见 action。
- 修改 CLI 命令语法。
- 修改 action JSON schema。
- 修改 response JSON 字段名、字段含义或多行输出顺序。
- 修改 replay plan schema。
- 修改 report schema。
- 修改 evidence raw 文件布局。
- 引入 protobuf、IDL/codegen、第三方 JSON 库或大范围格式化。
- 做 unrelated cleanup，例如拆 daemon、重写 probe runtime、改 EvidenceStore index 写入策略。

## 具体要求

### 1. 引入 ActionContext

新增一个小的上下文类型，集中 action handler 当前需要的 runtime 依赖。建议形态：

```cpp
struct ActionContext {
    GdbSession &session;
    const DebugTask *task = nullptr;
    SessionOutcome *outcome = nullptr;
    ProbeState &probe_state;
};
```

具体字段名可按现有代码风格调整，但必须满足：

- `dispatch_action` 不再接收一长串 `GdbSession&`、`DebugTask*`、`SessionOutcome*`、
  `ProbeState&` 参数。
- handler 内部仍通过 typed `ActionRequest` / payload 工作，不退回 JSON/string transport。
- 不引入全局 mutable state。
- 不改变 session ownership；`ActionContext` 只借用现有对象。

### 2. 拆出 action dispatch 模块

新增 `src/cli/action_dispatch.hpp` / `.cpp`，把 action dispatch 的公开入口移出
`src/cli.cpp`。

目标入口建议为：

```cpp
ActionOutput dispatch_action(ActionContext &context, const ActionRequest &request);
```

迁移内容：

- `dispatch_action` switch。
- 与 action dispatch 直接相关、且迁移后不会扩大职责的 helper。
- action state guard result 生成。
- action validation error 到 `ToolError` evidence 的转换 helper。

保留在 `src/cli.cpp`：

- CLI main command parsing。
- daemon socket/request handling。
- session registry / create / status / finish / close / shutdown orchestration。
- report finish 调用。
- 文件级 top-level flow glue。

不要在本轮拆：

- probe runtime/on-hit 执行到独立模块。
- replay runtime 到独立模块。
- hypothesis store 到独立模块。
- daemon server 到独立模块。

如果某些 helper 同时被 `src/cli.cpp` 和 `action_dispatch.cpp` 使用，可以先放在最小可见范围的
header 或保留在原文件并用窄接口调用；不要为了“完美拆分”做大范围移动。

### 3. 保持外部行为完全兼容

必须保持：

- CLI/daemon 输出 JSON 字段和顺序尽量不变。
- on-hit 子 action response 和主 action response 的多行输出顺序不变。
- replay `continue_on_error` / `stop_on_error` / skipped step 行为不变。
- `ToolError`、`command_evidence`、probe hit、hypothesis index、session summary、report/assets
  引用保持兼容。
- `raw_mi` 仍必须显式 `risk:"advanced"`。
- Core Dump Mode state guard 行为不变。

### 4. 保持测试和构建边界清晰

- 新增 `.cpp` 后更新 `CMakeLists.txt` 的 `gdb-agent` target。
- 如果 `replay_plan_tests` 或其他 test target 因链接依赖需要新增源文件，最小补齐。
- 不新增 live smoke；本轮是行为保持型重构，现有 CTest 已覆盖足够。

## 验证

至少运行：

```bash
cmake --build build
./build/gdb-agent check examples/segfault_task.md
./build/task_parser_tests
./build/replay_plan_tests
./build/hypothesis_assertion_tests
./build/mi_summary_tests
./build/type_sanitizer_tests
ctest --test-dir build --output-on-failure
git diff --check
```

重点确认：

- `ctest` 仍全部通过。
- `daemon_action_flow`、`capability_matrix_flow`、`catchpoint_matrix_flow`、
  `mi_summary_live_flow` 等 live smoke 的输出兼容。
- on-hit/replay 多行 response 顺序不变。
- Core Dump Mode guard 行为不变。
- 没有引入新的用户可见字段或删除旧字段。

## 完成标准

本轮完成时应满足：

- 存在明确的 `ActionContext` 类型。
- `dispatch_action` 公开入口已从 `src/cli.cpp` 移到独立 action dispatch 模块。
- `src/cli.cpp` 不再直接承载完整 action dispatch switch。
- action runtime 仍使用 typed `ActionRequest` / `ActionOutput`，没有回退到内部 JSON/string 协议。
- 外部行为与现有 CTest/smoke 完全兼容。
- `docs/ai/progress.md` 和 `docs/ai/handoff.md` 已记录本轮实际完成内容和验证结果。
- 如无新增项目级决策，不更新 `docs/ai/decision.md`。
- 本轮相关改动已按仓库约定 commit 并 push。
