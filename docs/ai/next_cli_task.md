# Next CLI Task: replay setup plan and report auditability hardening

## 目标

下一轮进入 Execution mode，聚焦 **Replay setup plan 和 report auditability**。

本轮不是重做 replay 基础能力，也不新增用户可见 action。目标是把 replay 从“能执行 action
列表”推进到“Agent 可审计、可复用的调试复现计划”：

1. 验证 `save_action` 保存 probe setup plan。
2. 验证 `--replay-before-run` 在新 session 初始运行前应用 probe setup。
3. 验证 replay 设置的 probe 能真实命中并产生 hit/on-hit evidence。
4. 验证 replay step 的 success / failed / skipped / mismatch warning 在 report 中可读、可追溯。
5. 全程保持内部 action/replay/on-hit 控制流使用 typed structs / structured result，不回退到 JSON
   string 或 response text 反解析。

## 当前背景

当前已具备：

- `save_action`、JSONL 和结构化 replay plan。
- replay plan schema：`gdb-agent-replay-plan-v1`。
- task fingerprint 校验、force mismatch warning。
- `continue_on_error` / `stop_on_error` failure policy。
- `ReplayStep` 和 `ReplayRun` evidence。
- `--replay-before-run`，可在 create 初始运行前执行 replay action。
- action runtime、replay runtime 和 on-hit runtime 已改为 typed `ActionRequest` /
  `ActionOutput`，不再依赖 response 文本判断成功/失败。
- `examples/workflow_fixture.cpp` 和 `scripts/smoke_real_workflow_flow.sh` 已提供真实 workflow
  fixture 和 live/replay/core 组合回归。

当前缺口：

- `--replay-before-run` 还缺少专门的 replay setup plan smoke：保存 probe setup 后，在新 session
  初始运行前应用，并验证 probe/on-hit 真正生效。
- report 对 replay plan、replay run 和 replay step 的审计展示仍可增强；Agent 应能直接看懂每个
  replay step 跑了什么、成功/失败/跳过原因、关联 evidence 在哪里。
- replay plan tags / metadata 的使用约定和展示还不够明确。
- 组合失败路径需要更聚焦地覆盖：
  - replay step 成功。
  - replay step action validation failure / unsupported action。
  - `stop_on_error` 后续 skipped step。
  - task fingerprint mismatch 默认拒绝。
  - force replay warning。

## 结构化数据约束

本轮特别注意：**不要把 replay 内部控制流重新做成 JSON/string 协议**。

必须遵守：

- replay runtime 继续消费 typed `ActionRequest` 和 typed `ActionOutput` / `ActionResult`。
- replay success/failure/skipped 判断必须依赖 `ActionResult.ok`、`ActionResult.error`、
  evidence id、command evidence id 等结构化字段。
- on-hit action 执行也必须继续使用 typed request/result。
- report 聚合若需要读取 replay evidence，应优先解析已有 structured evidence payload 或已有
  structured session artifacts；不要通过 grep response line 或解析 human-readable report 反推状态。
- 如果需要新增 report 内部数据结构，应使用明确 struct / enum / typed field，而不是拼接 JSON 字符串
  再反解析。
- 外部 artifact 仍然可以是 JSON/Markdown：CLI/daemon 输入输出、replay plan、evidence、
  session files 和 report 都保持现有边界格式。

禁止：

- 在 replay/on-hit/dispatcher 内部重新解析 response JSON 文本来判断 `ok:false`。
- 把 `action_result_line()` 的输出作为内部 replay/report 数据源。
- 引入 protobuf、IDL/codegen 或第三方 JSON 库。
- 为了 report 方便而改变 replay plan schema 或 raw evidence 文件布局。

## 实现范围

允许修改：

- 新增 replay-focused smoke，例如 `scripts/smoke_replay_setup_plan_flow.sh`。
- 复用或小幅扩展 `examples/workflow_fixture.cpp`，仅当 replay setup plan smoke 确实需要。
- `CMakeLists.txt`，用于接入新增 CTest。
- replay/report 相关源码，小范围增强 auditability，例如：
  - `src/report/report.cpp`
  - `src/replay/replay_runtime.cpp`
  - `src/replay/replay_runtime.hpp`
  - `src/replay/replay_plan.cpp`
  - `src/replay/replay_plan.hpp`
  - 必要的 typed result helper。
- 文档同步：
  - `docs/agent_actions.md` / `.en.md`，如果 replay 使用方式或 report 展示说明变化。
  - `docs/evidence_model.md` / `.en.md`，如果 ReplayStep / ReplayRun report 聚合说明变化。
  - `docs/mvp_quickstart.md`，如果补充 Agent replay playbook。
  - `docs/known_limitations.md`，如果发现新的明确限制。
  - `docs/ai/progress.md`
  - `docs/ai/handoff.md`
  - `docs/ai/decision.md` 仅当新增或改变项目级决策；本轮默认不需要。

原则上不要修改：

- 用户可见 action 列表。
- CLI 命令语法。
- action JSON schema。
- replay plan schema，除非实现中证明现有 schema 无法表达必要审计信息；本轮默认不改。
- evidence raw 文件布局。
- snapshot/session summary 既有字段含义。
- GDB/MI parser、type sanitizer、hypothesis assertion 语义。
- `src/cli.cpp` session lifecycle / daemon glue 架构拆分。

## 具体要求

### 1. Replay setup plan smoke

新增 `scripts/smoke_replay_setup_plan_flow.sh` 并接入 CTest。

建议复用 `workflow_fixture`，流程如下：

1. Session A：
   - create live session，停在 fixture 初始 `SIGTRAP`。
   - 使用 `save_action` 保存一组 probe setup actions 到同一个 plan，例如：
     - `breakpoint_set`，带 `comment` / `purpose` / `condition` / on-hit action。
     - `watchpoint_set`，带 metadata 和 on-hit action。
     - `catchpoint_set`，优先使用稳定的 `syscall write` 或 fixture 当前最稳定 catchpoint。
   - 保存至少一个故意失败 action 和一个后续 action 到单独的 `stop_on_error` replay plan，用于验证
     failed/skipped。
   - finish 后确认 replay plan artifact 存在。

2. Session B：
   - 使用同一个 task，并通过 `--replay-before-run` 应用 Session A 保存的 probe setup plan。
   - create 时 initial run 应带着 replay 设置的 probes 执行。
   - 确认 replay-before-run 设置的 breakpoint/watchpoint/catchpoint 能真实命中。
   - 确认 on-hit action 产生 `OnHitAction` evidence。
   - 确认 `BreakpointHit` / `WatchpointHit` / `CatchpointHit` evidence 和 probe metadata 中能看到
     replay setup plan 设置的 comment/purpose/condition/event/selector。
   - finish 后检查 `probes.json`、`session_summary.json`、report 和 evidence index。

3. Session C：
   - replay Session A 保存的 `stop_on_error` plan。
   - 验证第一步成功、第二步失败、第三步 skipped。
   - 检查 `ReplayStep` / `ReplayRun` / `ToolError` evidence、response 和 report 展示。

4. Session D：
   - 使用不同 task 触发 fingerprint mismatch。
   - 不带 `force` 时默认拒绝，并写 `ToolError` / error evidence。
   - 带 `force` 时允许执行，并记录 `ReplayWarning`。
   - 检查 report 和 session summary 中 warning/error 计数。

### 2. Report auditability 增强

如果现有 report 对 replay 不够清楚，应小范围增强 Replay 区域。

Agent 打开 report 后，应能看到或追溯：

- replay plan file / name。
- source session id。
- task fingerprint match / mismatch / force warning。
- plan-level failure policy。
- 每个 replay run 的 evidence id。
- 每个 step 的：
  - index / step id。
  - action name。
  - status：`success` / `failed` / `skipped`。
  - failure policy。
  - action evidence。
  - error evidence。
  - skip reason。
- force replay warning 对应的 evidence id。

实现要求：

- 优先从 `ReplayRun` / `ReplayStep` structured evidence payload 聚合，不要从 human-readable
  report 文本反解析。
- 如果需要新增 report helper，使用 typed structs，例如 `ReplayRunSummary`、
  `ReplayStepSummary`、`ReplayWarningSummary`。
- 不要改变 replay plan schema；report 可以增强 Markdown 展示。

### 3. Replay plan tags / metadata 使用约定

检查现有 replay plan 是否已有 tags 字段。

- 如果已有 tags：
  - smoke 覆盖 tags 写入和 report/docs 展示。
  - 文档说明推荐 tags，例如 `probe-setup`、`repro`、`hypothesis`、`regression`。
- 如果没有 tags：
  - 本轮不要贸然扩展 schema。
  - 在 handoff 中记录“tags 展示延后，避免 schema churn”。

无论是否实现 tags 展示，都应让 docs 更清楚说明：

- 什么 action 适合保存成 setup plan。
- 什么时候用 `--replay-before-run`。
- `force` 的风险。
- task fingerprint mismatch 的含义。
- replay 会产生新的 evidence id，不能复用旧 evidence id。

### 4. Artifact consistency

新增 smoke 应明确检查：

- replay plan JSON 文件存在，并仍使用 `gdb-agent-replay-plan-v1`。
- replay plan 包含 task metadata / fingerprint。
- report 文件存在。
- assets 目录存在。
- `session_summary.json` 存在。
- `session_snapshot.json` 存在。
- `task.normalized.json` 存在。
- `evidence/index.json` 存在。
- index 中引用的 raw/summary/view 文件存在。
- `ReplayStep`、`ReplayRun`、`ReplayWarning`、`ToolError` evidence 按路径出现。
- replay setup plan 导致的 `BreakpointHit` / `WatchpointHit` / `CatchpointHit` /
  `OnHitAction` evidence 出现。
- report 引用的 evidence id 都存在于 evidence index。

## 保持外部行为兼容

必须保持：

- CLI/daemon action JSON schema 兼容。
- response 字段含义兼容。
- replay plan schema 兼容，除非明确记录并同步文档；本轮默认不改。
- evidence raw 文件布局兼容。
- report 仍是草稿，不自动宣称根因。
- Core Dump Mode 仍是静态取证模式。
- `raw_mi` 仍必须显式 `risk:"advanced"`，且不能作为 on-hit action。
- 内部 replay/on-hit flow 继续使用 typed structured data。

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
./scripts/smoke_replay_setup_plan_flow.sh
ctest --test-dir build --output-on-failure
git diff --check
```

如果当前环境没有 `gdb`，则：

- 仍需运行不依赖 GDB 的构建和单元测试。
- 在 `docs/ai/handoff.md` 和最终回复中明确记录 live replay smoke 未运行原因。

## 完成标准

本轮完成时应满足：

- 存在 replay setup plan smoke，并接入 CTest。
- smoke 覆盖 `save_action` 保存 probe setup plan。
- smoke 覆盖 `--replay-before-run` 应用 probe setup plan，并真实产生 probe hit / on-hit evidence。
- smoke 覆盖 replay success、failed、skipped、mismatch rejected、force warning。
- report 能清楚展示或追溯 replay run / replay step / warning / error evidence。
- docs 说明 replay setup plan、`--replay-before-run`、force mismatch 和新 evidence id 语义。
- 内部实现保持 typed `ActionRequest` / `ActionOutput` / structured result，不引入 response text
  反解析。
- 未改变用户可见 action schema、CLI 语法、evidence raw 文件布局或 snapshot/session summary
  既有字段含义。
- 如无必要，不改变 replay plan schema；若确实改变，必须同步 docs 和 decision。
- `docs/ai/progress.md` 和 `docs/ai/handoff.md` 已记录实际完成内容、验证结果和遗留限制。
- 如无新增项目级决策，不更新 `docs/ai/decision.md`。
- 本轮相关改动已按仓库约定 commit 并 push。
