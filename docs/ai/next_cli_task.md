# Next CLI Task: split probe, replay, and hypothesis runtimes

## 目标

下一轮继续做 **行为保持型架构重构**，把已经从 `src/cli.cpp` 拆出的
`src/cli/action_dispatch.cpp` 进一步按子系统拆分：

1. 拆出 probe / on-hit runtime。
2. 拆出 replay runtime。
3. 拆出 hypothesis store。

本轮不新增功能、不改变用户可见行为、不改变任何外部 JSON schema。所有 CLI/daemon command、
action request、action response、evidence、report、replay plan 和 smoke 断言都必须保持兼容。

## 当前背景

上一轮已经完成：

- 新增 `src/cli/action_context.hpp`。
- 新增 `src/cli/action_dispatch.hpp` / `.cpp`。
- `dispatch_action(ActionContext&, const ActionRequest&)` 已经从 `src/cli.cpp` 移到 action dispatch
  模块。
- `src/cli.cpp` 现在主要保留 CLI/daemon/session/report 顶层 orchestration。

但 `src/cli/action_dispatch.cpp` 现在承载了过多子系统 helper：

- `ProbeState` 仍在 `src/cli/action_context.hpp`。
- probe metadata、hit attribution、on-hit execution、probe hit evidence 和 finish-time
  `probes.json` 写入仍由 action dispatch 模块承担。
- replay execution、failure policy、ReplayStep / ReplayRun evidence 仍在 action dispatch 模块。
- hypothesis markdown/index 写入和 hypothesis runtime state 仍和 action dispatch 耦合。

本轮目标是继续小步拆分这些已成型子系统，同时保持 action dispatch 的外部行为不变。

## 实现范围

允许修改：

- `src/cli/action_context.hpp`
- `src/cli/action_dispatch.hpp`
- `src/cli/action_dispatch.cpp`
- 新增 `src/workflow/probe_runtime.hpp` / `.cpp`
- 新增 `src/replay/replay_runtime.hpp` / `.cpp`
- 新增 `src/workflow/hypothesis_store.hpp` / `.cpp`
- `CMakeLists.txt`，用于接入新增 `.cpp`。
- 相关测试 target 的 CMake 源文件列表，仅当新增链接单元导致测试 target 需要补源文件。
- `docs/ai/progress.md`
- `docs/ai/handoff.md`
- `docs/ai/decision.md`，仅当新增或改变项目级决策；本轮默认不需要。

原则上不要修改：

- `src/cli.cpp`，除非 include 或调用接口需要小幅调整。
- `src/cli/action.hpp` / `.cpp`，除非拆分后必须补最小 forward declaration 或 include。
- `src/replay/replay_plan.hpp` / `.cpp`，除非 replay runtime 需要更清晰地复用既有 plan reader/helper。
- `docs/agent_actions.md` / `.en.md`
- `docs/evidence_model.md` / `.en.md`
- `docs/task_format.md` / `.en.md`
- `docs/known_limitations.md`
- report 内容。
- GDB/MI parser、type sanitizer、hypothesis assertion 行为。

禁止：

- 新增或删除用户可见 action。
- 修改 CLI 命令语法。
- 修改 action JSON schema。
- 修改 response JSON 字段名、字段含义或多行输出顺序。
- 修改 replay plan schema。
- 修改 report schema。
- 修改 evidence schema 或 raw evidence 文件布局。
- 引入 protobuf、IDL/codegen、第三方 JSON 库或大范围格式化。
- 做 unrelated cleanup，例如拆 daemon server、改 EvidenceStore index 写入策略、扩展 hypothesis
  assertion、改 summary/sanitizer 行为。

## 具体要求

### 1. 拆出 probe / on-hit runtime

新增 `src/workflow/probe_runtime.hpp` / `.cpp`。

迁移内容：

- `ProbeState` 及其嵌套类型：
  - `OnHitPolicy`
  - `OnHitActionResult`
  - `ProbeHitSnapshot`
  - `ProbeInfo`
  - 与 probe 直接相关的状态字段，例如 `probes_by_number`
- probe JSON / typed result helper：
  - `on_hit_policy_json`
  - `write_probe_snapshot`
  - probe metadata JSON helper
  - `probe_info_result_object`
  - `probe_array_result`
- probe hit / on-hit helper：
  - watchpoint stop attribution。
  - probe hit snapshot 准备。
  - on-hit action execution。
  - `OnHitAction` evidence 写入。
  - `BreakpointHit` / `WatchpointHit` / `CatchpointHit` evidence 写入。
  - `handle_probe_stop` 或等价公开入口。

建议公开入口保持窄接口，例如：

```cpp
std::vector<ActionResult> handle_probe_stop(ActionContext &context, const CommandResult &result);
void write_probe_snapshot(GdbSession &session, const ProbeState &probe_state);
ResultArray probe_array_result(const ProbeState &probe_state, bool include_deleted = false);
std::string probe_array_json(const ProbeState &probe_state, bool include_deleted = false);
```

`action_dispatch.cpp` 里的 probe action handler 仍可以负责调用 GDB 设置 breakpoint/watchpoint/
catchpoint，然后把 metadata 写入 `context.probe_state`。本轮不要求把所有 probe action handler 也
移走，但 on-hit runtime 和 hit attribution 不应继续留在 action dispatch 里。

### 2. 拆出 replay runtime

新增 `src/replay/replay_runtime.hpp` / `.cpp`。

迁移内容：

- replay runtime result types：
  - `ReplayStepRunResult`
  - `ReplayRunResult`
  - `ReplayActionStep`
- replay execution helper：
  - step result typed object/json fragment 生成。
  - replay run result action 生成。
  - `ReplayRun` evidence 写入。
  - skipped step evidence 写入。
  - step execution。
  - structured replay plan execution。
  - JSONL replay execution。
  - legacy single-action replay execution。
  - `replay_action_file` 或等价公开入口。

保持边界：

- `src/replay/replay_plan.*` 继续负责 artifact schema 写入、校验、fingerprint、policy normalize
  等既有职责。
- `src/replay/replay_runtime.*` 负责执行 runtime plan 和写 execution evidence。
- `action_dispatch.cpp` 的 `Replay` action handler 只解析 request payload、定位 replay 文件，然后调用
  replay runtime。

建议公开入口：

```cpp
ActionOutput replay_action_file(ActionContext &context,
                                const std::filesystem::path &path,
                                bool force = false,
                                const std::string &failure_policy_override = "");
```

### 3. 拆出 hypothesis store

新增 `src/workflow/hypothesis_store.hpp` / `.cpp`。

迁移内容：

- hypothesis runtime state types：
  - `HypothesisCheck`
  - `HypothesisRecord`
  - `hypotheses_by_id`
  - `hypothesis_counter`
- hypothesis persistence helper：
  - hypothesis id 分配。
  - hypothesis markdown 文件路径。
  - hypothesis markdown append/write。
  - `assets/hypotheses/index.json` 写入。
- 如果能保持接口清晰，可以把 `hypothesis_create`、`hypothesis_check` 的持久化部分抽成 helper，
  但不要求把整个 action handler 移出 action dispatch。

建议做法：

- `ProbeState` 可以继续作为当前运行期总状态类型，但其 hypothesis 子状态应使用来自
  `hypothesis_store.hpp` 的类型，避免在 probe runtime 中定义 hypothesis 结构。
- action dispatch 中 hypothesis handler 仍负责执行 GDB expression 和调用
  `evaluate_hypothesis_assertion`，但 markdown/index 持久化应通过 hypothesis store helper 完成。

### 4. 保持 ActionContext 清晰

拆分后 `ActionContext` 应继续保持小而明确：

```cpp
struct ActionContext {
    GdbSession &session;
    const DebugTask *task = nullptr;
    SessionOutcome *outcome = nullptr;
    ProbeState &probe_state;
};
```

如果为了拆 hypothesis state 需要重命名 `ProbeState`，可以改成更中性的 `ActionRuntimeState`，
但不强制。本轮优先行为保持和低风险拆分，不为了命名做大范围 churn。

## 保持外部行为完全兼容

必须保持：

- CLI/daemon 输出 JSON 字段和顺序尽量不变。
- on-hit 子 action response 和主 action response 的多行输出顺序不变。
- replay `continue_on_error` / `stop_on_error` / skipped step 行为不变。
- `ToolError`、`command_evidence`、probe hit、hypothesis index、session summary、report/assets
  引用保持兼容。
- `raw_mi` 仍必须显式 `risk:"advanced"`。
- Core Dump Mode state guard 行为不变。
- `assets/probes.json`、`assets/hypotheses/index.json`、ReplayStep / ReplayRun / OnHitAction evidence
  字段语义不变。

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
- `daemon_action_flow`、`edge_case_flow`、`capability_matrix_flow`、`catchpoint_matrix_flow`、
  `mi_summary_live_flow` 等 live smoke 输出兼容。
- on-hit/replay 多行 response 顺序不变。
- replay mismatch、force warning、failure policy、skipped step 行为不变。
- hypothesis create/check/conclude 后 markdown 和 `assets/hypotheses/index.json` 字段不变。
- probe hit evidence、on-hit evidence、finish-time `assets/probes.json` 字段不变。
- Core Dump Mode guard 行为不变。

## 完成标准

本轮完成时应满足：

- 存在 `src/workflow/probe_runtime.hpp` / `.cpp`。
- 存在 `src/replay/replay_runtime.hpp` / `.cpp`。
- 存在 `src/workflow/hypothesis_store.hpp` / `.cpp`。
- `ProbeState` 不再定义在 `src/cli/action_context.hpp`，而是由更合适的 workflow/runtime header 提供。
- `src/cli/action_dispatch.cpp` 不再承载完整 probe/on-hit runtime、replay execution 和 hypothesis
  store persistence。
- action runtime 仍使用 typed `ActionRequest` / `ActionOutput`，没有回退到内部 JSON/string 协议。
- 外部行为与现有 CTest/smoke 完全兼容。
- `docs/ai/progress.md` 和 `docs/ai/handoff.md` 已记录本轮实际完成内容和验证结果。
- 如无新增项目级决策，不更新 `docs/ai/decision.md`。
- 本轮相关改动已按仓库约定 commit 并 push。
