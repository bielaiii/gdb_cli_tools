# Handoff

日期：2026-06-10

## 本轮完成

- 执行 `docs/ai/next_cli_task.md` 中的
  `split probe, replay, and hypothesis runtimes` 任务，仅处理其中 1-3：
  - split probe / on-hit runtime
  - split replay runtime
  - split hypothesis store
- 新增 `src/workflow/probe_runtime.hpp` / `src/workflow/probe_runtime.cpp`：
  - `ProbeState` 已从 `src/cli/action_context.hpp` 移入 probe runtime header。
  - probe metadata、probe list result、probe snapshot、hit attribution、on-hit wrapper evidence 和
    on-hit action execution 已从 `src/cli/action_dispatch.cpp` 移出。
  - on-hit execution 继续使用 typed `ActionRequest` / `ActionOutput` 调用 action dispatcher。
- 新增 `src/replay/replay_runtime.hpp` / `src/replay/replay_runtime.cpp`：
  - replay JSONL/plan 读取、step execution、failure policy、ReplayStep/ReplayRun evidence 和
    replay final result 构造已从 `src/cli/action_dispatch.cpp` 移出。
  - `rebuild_replay_plan_from_jsonl` 移到 replay runtime，供 `save_action` 继续生成现有 replay
    plan artifact。
- 新增 `src/workflow/hypothesis_store.hpp` / `src/workflow/hypothesis_store.cpp`：
  - `HypothesisStore` 保存 hypothesis records 和 id counter。
  - hypothesis markdown 文件路径、index JSON 写入和 markdown append helper 已从 action dispatch
    移出。
  - `hypothesis_create` / `hypothesis_check` / `hypothesis_conclude` 仍在 action dispatch 中处理
    action payload、GDB evidence collection 和 response 构造。
- 更新 `src/cli/action_context.hpp`：
  - 只保留 `ActionContext`。
  - 不再定义 `ProbeState`。
- 更新 `src/cli/action_dispatch.hpp` / `src/cli/action_dispatch.cpp`：
  - action dispatch 继续公开 typed `dispatch_action` / `handle_action_request` /
    `handle_action_json`。
  - dispatch switch 只调用 probe/replay/hypothesis store API，不再承载完整 runtime/store 实现。
- 更新 `CMakeLists.txt`，把三个新源文件接入 `gdb-agent` target。
- 更新 `docs/ai/progress.md`，记录本轮完成情况和剩余建议。
- `docs/ai/decision.md` 未更新：本轮没有新增或改变项目级 decision，属于 D012 typed action
  boundary 下的物理模块拆分。

## 验证

- `cmake --build build`
  - 结果：通过。
- `./build/gdb-agent check examples/segfault_task.md`
  - 结果：通过，输出 `ok`。
- `ctest --test-dir build --output-on-failure`
  - 结果：13/13 tests passed。
  - 覆盖 daemon/action、core dump、edge case、capability matrix、catchpoint matrix、
    type sanitizer、MI summary live flow、replay plan、hypothesis assertion 和 task parser。
- `git diff --check`
  - 结果：通过。

## 完成标准审计

- `src/workflow/probe_runtime.hpp` / `.cpp` 已存在。
- `src/replay/replay_runtime.hpp` / `.cpp` 已存在。
- `src/workflow/hypothesis_store.hpp` / `.cpp` 已存在。
- `ProbeState` 不再定义在 `src/cli/action_context.hpp`。
- `src/cli/action_dispatch.cpp` 不再承载完整 probe/on-hit runtime、replay execution runtime 或
  hypothesis store persistence。
- action runtime 仍使用 typed `ActionRequest` / `ActionOutput`；没有回退到内部 JSON/string 协议。
- 未改变用户可见 action schema、response schema、evidence schema/raw 文件布局、replay plan
  schema、report schema、CLI 语法、GDB/MI parser、type sanitizer 或 hypothesis assertion 语义。

## 限制和注意事项

- `src/cli/action_dispatch.cpp` 仍保留 action dispatch switch，以及与 action payload 校验、
  GDB command evidence collection、response field 构造直接相关的逻辑；本轮不拆 session
  lifecycle、daemon server/client 或 report orchestration。
- `hypothesis_check` 的 assertion 语义没有扩展；本轮只移动持久化边界。
- `replay_plan` artifact schema 没有变化；本轮只拆出 replay execution runtime。
