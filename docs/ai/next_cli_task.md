# Next CLI Task: real workflow smoke for core/replay/probe/on-hit evidence robustness

## 目标

下一轮进入 Execution mode，做一轮 **真实多步骤工作流打磨**。

本轮不追求新增大量 action，也不继续泛泛扩展 MI summary / sanitizer。目标是用一个更接近真实
Agent 调试过程的 fixture 和 smoke，把已经实现的 core、replay、probe、on-hit、hypothesis、
report 和 evidence 链路串起来验证，并小范围修复 smoke 暴露出的真实问题。

完成后，项目应从“各子系统分别可用”提升到：

> 有一条可重复运行的真实多步骤 Agent 调试流，能同时验证 probe/on-hit 取证、replay 复现、
> core 静态取证、hypothesis 检查、report 聚合和 evidence 文件引用一致性。

## 当前背景

当前状态：

- Core Dump Mode 已经支持静态 action，并能拒绝动态 action/probe 操作。
- Replay 已经支持 JSONL、结构化 replay plan、task fingerprint、force mismatch warning、
  `continue_on_error` / `stop_on_error`、`ReplayStep` 和 `ReplayRun` evidence。
- Probe / on-hit 已经支持 breakpoint、watchpoint、catchpoint、metadata、hit evidence、
  on-hit action、failure policy、skipped action 和 `continue_after_hit`。
- Hypothesis workflow 已经支持 create/check/conclude、结构化 check result、assertion、
  markdown/index 持久化和 report 聚合。
- 最近已经完成 action runtime、probe runtime、replay runtime 和 hypothesis store 的架构拆分。

当前缺口不是“有没有这些功能”，而是：

- 是否有一条真实多步骤 workflow 同时覆盖这些子系统。
- 失败、skipped、guard rejected、command evidence 等路径在组合场景下是否稳定。
- report、session summary、snapshot、evidence index、raw/summary/view 文件引用是否在复杂 flow
  下仍然一致。
- replay 从一个 session 保存、另一个 session 执行时，Agent 是否能清楚审计每一步结果。

## 实现范围

允许修改：

- 新增一个真实工作流 fixture，例如 `examples/workflow_fixture.cpp`。
- 新增一个 Linux + GDB smoke 脚本，例如 `scripts/smoke_real_workflow_flow.sh`。
- `CMakeLists.txt`，用于接入新增 fixture target 和 CTest。
- 必要的源码小修，前提是 smoke 暴露了真实行为问题，例如：
  - 某个失败路径没有稳定 `ToolError` 或 `command_evidence`。
  - report 对 replay/probe/core 组合结果展示不清楚。
  - probe hit / on-hit / replay evidence 缺少已有 schema 内可表达的关键 metadata。
  - core mode 某类 GDB command error 被误报为成功。
  - session summary 或 evidence index 计数/引用在组合 flow 下不一致。
- 文档同步：
  - `docs/ai/progress.md`
  - `docs/ai/handoff.md`
  - `docs/ai/decision.md` 仅当新增或改变项目级决策；本轮默认不需要。
  - 如果用户可见行为、限制或 report/evidence 说明变化，再同步更新：
    - `docs/agent_actions.md` / `.en.md`
    - `docs/evidence_model.md` / `.en.md`
    - `docs/known_limitations.md`
    - `docs/mvp_acceptance.md`

原则上不要修改：

- action JSON schema。
- CLI 命令语法。
- replay plan schema。
- evidence raw 文件布局。
- `session_snapshot.json` / `session_summary.json` 的既有字段含义。
- GDB/MI parser、type sanitizer、hypothesis assertion 语义，除非新增 workflow 暴露了明确回归。
- `src/cli.cpp` 的架构拆分；session lifecycle / daemon glue 拆分留给后续单独任务。

禁止：

- 新增与本轮 workflow 无关的 action。
- 做 unrelated cleanup 或大范围格式化。
- 把 summary 当 raw evidence 使用。
- 把工具输出改成自动根因结论。
- 因为某个 Linux/GDB/target 组合不支持 catchpoint 就伪装成功；应返回结构化失败和 evidence。

## 具体要求

### 1. 新增真实工作流 fixture

新增一个 fixture，目标是模拟一轮真实 Agent 调试，而不是只做单点能力矩阵。

fixture 应尽量满足：

- 有稳定函数调用链，便于 `backtrace` / `frame_select` / `locals`。
- 有一个可稳定观察的全局或局部变量，便于 `watchpoint_set` 和 `evaluate`。
- 有一个可稳定命中的 breakpoint 位置。
- 有一个可用于 `hypothesis_check` 的变量或指针。
- 有至少一个可选路径能生成 core dump 或被 GDB `generate-core-file` 捕获。
- 尽量包含一个 catchpoint 可用路径，例如 syscall、fork、exec 或 C++ exception；如果某个 target
  组合不稳定，可以在 smoke 中记录 capability 差异并跳过该子断言。

建议 fixture 支持命令行 mode，例如：

```text
probe
replay
core
```

也可以复用现有 `examples/capability_fixture.cpp`，但只有在不会让该 fixture 继续膨胀到难维护时才复用。
如果真实 workflow 需要更清晰的行为，优先新增独立 fixture。

### 2. 新增真实 workflow smoke

新增 `scripts/smoke_real_workflow_flow.sh` 或等价脚本，并接入 CTest。

smoke 至少覆盖一个完整 live session：

1. 启动 daemon。
2. create live session。
3. 设置带 metadata 的 breakpoint。
4. 设置带 metadata 的 watchpoint。
5. 设置至少一个 catchpoint；优先选择当前环境稳定的 syscall/fork/exec/throw/catch 之一。
6. 为至少一个 probe 配置 on-hit policy，覆盖：
   - 一个成功 on-hit action。
   - 一个失败 on-hit action。
   - `stop_on_error` 导致后续 on-hit action `skipped`。
   - 如 fixture 稳定，覆盖 `continue_after_hit:true`。
7. `continue` 或 `run` 到真实 stop。
8. 执行静态取证 action：
   - `backtrace`
   - `locals`
   - `evaluate`
   - 必要时 `threads` / `frame_select`
9. 执行 hypothesis workflow：
   - `hypothesis_create`
   - 至少一个 `passed` 或 `failed` 的 `hypothesis_check`
   - 至少一个 `unknown` 或 GDB command failure 路径，确保 error evidence 可审计
   - `hypothesis_conclude`
10. 保存 replay plan。
11. finish live session，检查 report/assets。

然后覆盖 replay 跨 session：

1. 创建第二个 live session。
2. replay 第一轮保存的 plan。
3. 覆盖至少一个成功 step。
4. 覆盖至少一个失败 step。
5. 覆盖 `continue_on_error` 或 `stop_on_error` 的 skipped 行为。
6. 检查 `ReplayStep` / `ReplayRun` evidence、response 和 report/assets 引用一致。

最后覆盖 core flow：

1. 生成或加载 fixture core。
2. create core session。
3. 执行至少两个静态 action，例如 `backtrace`、`threads`、`locals`、`evaluate`。
4. 执行至少一个动态 action/probe 操作，确认 core mode state guard 拒绝并写 `ToolError` evidence。
5. finish core session，检查 report、session summary、snapshot 和 evidence index。

### 3. 检查 artifacts 一致性

smoke 应明确检查：

- report 文件存在。
- assets 目录存在。
- `session_snapshot.json` 存在。
- `session_summary.json` 存在。
- `task.normalized.md` 存在。
- `evidence/index.json` 存在。
- index 中引用的 raw/summary/view 文件存在。
- `assets/probes.json` 在 finish 后存在，并包含 expected metadata / deleted 状态。
- 如果生成 hypothesis，`assets/hypotheses/index.json` 存在并包含 expected check status。
- report 中能看到关键区域：
  - `Tool Observations`
  - `Tool Errors`，如果本轮制造了工具/GDB error。
  - `Evidence Summary`
  - `Hypotheses`
  - `Raw Evidence Index`
  - replay/probe/core 相关 evidence id。

### 4. 小范围修复真实问题

如果新增 smoke 暴露真实问题，允许在本轮修复，但必须保持范围窄。

优先修复：

- response `ok` 与底层 GDB command result 不一致。
- 缺少 `ToolError` evidence。
- 缺少 `command_evidence` 链接。
- probe hit / on-hit / replay step 缺少已有 schema 内应该有的 evidence id。
- report 没有列出关键失败或 evidence 链接。
- session summary 计数不符合实际 evidence。

不要在本轮扩展：

- 新 action。
- 新 replay plan schema。
- 新 hypothesis assertion。
- 大规模 report redesign。
- EvidenceStore index 写入策略重构。
- `src/cli.cpp` session lifecycle 拆分。

## 保持外部行为兼容

必须保持：

- CLI/daemon action JSON schema 兼容。
- response 字段含义兼容。
- replay plan schema 兼容。
- evidence schema 和 raw 文件布局兼容。
- report 仍是草稿，不自动宣称根因。
- Core Dump Mode 仍是静态取证模式。
- `raw_mi` 仍必须显式 `risk:"advanced"`，且不能作为 on-hit action。

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
./scripts/smoke_real_workflow_flow.sh
ctest --test-dir build --output-on-failure
git diff --check
```

如果当前环境没有 `gdb`，则：

- 仍需运行不依赖 GDB 的构建和单元测试。
- 在 `docs/ai/handoff.md` 和最终回复中明确记录 live smoke / core flow 未运行原因。

## 完成标准

本轮完成时应满足：

- 存在一个真实 workflow fixture，或现有 fixture 被清晰扩展并保持可维护。
- 存在一个新的 Linux + GDB workflow smoke，并接入 CTest。
- smoke 覆盖 live probe/on-hit flow、replay 跨 session flow 和 core mode flow。
- smoke 覆盖成功、失败、skipped 和 core-mode guard rejected 四类结果。
- report、session summary、snapshot、evidence index、raw/summary/view 文件引用一致。
- `ReplayStep` / `ReplayRun`、`BreakpointHit` / `WatchpointHit` / `CatchpointHit`、
  `OnHitAction`、`ToolError` 和 hypothesis artifacts 至少覆盖本轮设计的关键路径。
- 未改变外部 action schema、response schema、replay plan schema、evidence raw 文件布局或
  snapshot/session summary 既有字段含义。
- `docs/ai/progress.md` 和 `docs/ai/handoff.md` 已记录实际完成内容、验证结果和遗留限制。
- 如无新增项目级决策，不更新 `docs/ai/decision.md`。
- 本轮相关改动已按仓库约定 commit 并 push。
