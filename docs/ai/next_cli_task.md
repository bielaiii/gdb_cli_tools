# Next CLI Task: core dump report, metadata audit, and replay semantics

## 目标

下一轮进入 Execution mode，聚焦 **Core Dump Mode 的可审计性和 replay 语义打磨**。

本轮只做用户指定的三项：

1. **Core dump 专用 summary / report 区域**。
2. **Core task metadata / artifact audit 加强**。
3. **Core replay 语义打磨**。

本轮不要做多线程 core fixture 扩展，不新增用户可见 action，不把 Core Dump Mode 变成动态调试模式。

完成后，Agent 打开 core dump 报告时应能快速确认：

- 当前 session 是否是 core mode。
- 加载的是哪个 executable 和 core dump。
- core 是否加载成功。
- 初始 thread/frame/backtrace/stop context 的证据在哪里。
- 哪些静态 action 成功执行。
- 哪些 dynamic action 在 core mode 下被 guard 拒绝。
- replay plan 在 core mode 下哪些 step 成功、哪些 step 因 D011 动态限制失败或 skipped。

## 当前背景

当前已具备：

- Core Dump Mode 最小能力。
- `session_summary.json` 已记录 `core_dump` 和 `core_loaded`。
- daemon `create` core task 返回 `mode:"core"`。
- Core Dump Mode 下支持静态 action：
  - `backtrace`
  - `threads`
  - `frame_select`
  - `locals`
  - `args_info`
  - `evaluate`
  - `registers`
  - `hypothesis_check`
- Core Dump Mode 下动态 action/probe 操作会被 state guard 拒绝，并写 `ToolError` evidence：
  - `run`
  - `continue`
  - `breakpoint_set`
  - `watchpoint_set`
  - `catchpoint_set`
  - `probe_enable`
  - `probe_disable`
  - `probe_delete`
- 已有 core smoke：
  - `scripts/smoke_core_dump_mode.sh`
  - CTest `core_dump_mode`
- 已有真实 workflow smoke 覆盖 core 静态 action 和 guard rejected 路径。
- report 已有通用 evidence、replay、hypothesis、probe 等区域，但缺少 core-first 的阅读入口。
- 最近已新增 `Replay Execution Audit`，能从结构化 `ReplayRun`、`ReplayStep` 和
  `ReplayWarning` evidence 汇总 replay 结果。

当前缺口：

- report 里没有专门的 `Core Dump` / `Core Snapshot` 区域帮助 Agent 第一眼理解 core 上下文。
- core task / session metadata 在 report 与 smoke 中的 hard expectation 还不够集中。
- core mode 下 replay plan 混合静态/动态 action 的语义还需要专门 smoke 固化：
  - 静态 action step 可以成功。
  - 动态 action step 应被 core guard 拒绝并记录 `ToolError`。
  - `continue_on_error` 应继续执行后续静态 step。
  - `stop_on_error` 应把后续 step 记录为 `skipped`。
  - report 的 replay audit 应能追踪这些 step。

## 必须遵守的设计约束

遵守 `docs/ai/decision.md`：

- D004：raw evidence 是权威，summary/report 是有损入口。
- D005：`session_snapshot.json` / `session_summary.json` 不是 live session 恢复文件。
- D006：replay 保存并重放高层 action；新 session 产生新的 evidence id。
- D011：Core Dump Mode 是静态取证模式。不要支持 core 下 `run` / `continue` / probe mutation。
- D012：内部 action/replay/on-hit 控制流使用 typed structs；不要把 response JSON 文本当内部协议。

禁止：

- 新增用户可见 action。
- 支持 core 下动态 action。
- 让工具自动宣称 root cause。
- 改 replay plan schema，除非实现中证明不可避免；本轮默认不改。
- 改 evidence raw 文件布局。
- 改 task format。
- 改 GDB/MI parser、type sanitizer 或 hypothesis assertion 语义，除非 core smoke 暴露明确回归。
- 做多线程 core fixture 或其它不在本轮 1/3/5 范围内的功能。
- 做 unrelated cleanup 或大范围格式化。

## 实现范围

允许修改：

- `src/report/report.cpp`
  - 增加 core 专用 report 区域。
  - 汇总 core mode metadata、初始 stop/backtrace/thread/frame evidence 和 core guard errors。
- `src/workflow/session_summary.*` 或当前写 session summary 的相关代码
  - 仅当现有字段不足以支撑 report/smoke audit 时，补充最小结构化字段。
  - 优先复用已有 `core_dump` / `core_loaded` / session mode / evidence index。
- `src/replay/replay_runtime.*`
  - 仅当 core replay 的 failure/skipped/evidence 链路存在真实缺口时小修。
  - 不要改变 replay plan schema。
- `scripts/smoke_core_dump_mode.sh`
  - 扩展现有 core smoke，避免新增重复 fixture。
  - 覆盖 core report、metadata artifact audit 和 core replay semantics。
- `CMakeLists.txt`
  - 仅当新增独立 smoke 才修改；优先扩展已有 `core_dump_mode`。
- 文档：
  - `docs/agent_actions.md`
  - `docs/agent_actions.en.md`
  - `docs/evidence_model.md`
  - `docs/evidence_model.en.md`
  - 必要时 `docs/known_limitations.md`
  - `docs/ai/progress.md`
  - `docs/ai/handoff.md`
  - `docs/ai/decision.md` 仅当新增或改变项目级 decision；本轮默认不需要。

原则上不要修改：

- `examples/workflow_fixture.cpp`，除非现有 core smoke 确实无法覆盖本轮需求。
- `src/cli.cpp` 的 session lifecycle / daemon glue 架构。
- action JSON schema、response schema、task format、replay plan schema。

## 具体要求

### 1. Core dump 专用 report 区域

在最终 Markdown report 中新增或增强一个 core-first 区域，例如：

```markdown
## Core Dump Snapshot
```

当 session 是 Core Dump Mode 时，该区域应展示或链接：

- mode：`core`。
- executable path。
- working directory。
- core dump path。
- core loaded：`true` / `false`。
- task problem 摘要。
- 初始 core load / stop 相关 evidence id。
- 初始 backtrace / threads / current frame / frame args / locals 等可用 evidence id。
- `ToolError` 中属于 core-mode guard rejected 的 dynamic action，至少能看出 action name 和 reason。

实现建议：

- 优先从已有 `DebugTask`、`SessionOutcome`、`session_summary.json` 输入和 `Evidence` 列表聚合。
- 如果需要解析 evidence summary，使用明确 helper / struct，不要 grep report 文本。
- 如果某项 evidence 不存在，应显示 `-` 或省略，不要伪造。
- 该区域只做审计入口，不做根因结论。

### 2. Core task metadata / artifact audit 加强

扩展 core smoke 的 hard expectations，确保 core task/session/artifact 信息稳定存在并互相一致。

必须检查：

- daemon `create` core task 返回 `mode:"core"`。
- `session_summary.json` 存在。
- `session_summary.json` 中：
  - `core_dump` 等于 task 中的 core path。
  - `core_loaded` 是 `true`。
  - evidence count 与 `evidence/index.json` 一致。
  - 如果已有 mode/session state 字段，应检查其 core 语义；如果没有，不要为了好看新增冗余字段。
- `session_snapshot.json` 存在，但 smoke 不应把它当 live restore 文件。
- `task.normalized.json` 存在，并包含 core dump path。
- `evidence/index.json` 存在。
- index 中每个 evidence 的 view/raw/summary 文件存在。
- report 文件存在。
- report 引用的 evidence id 都存在于 evidence index。
- report 的 core dump 区域包含 core dump path、core loaded 状态和至少一个 core 静态 evidence id。

如果现有 `session_summary.json` 字段不足：

- 可以补充最小字段，但必须同步 `docs/evidence_model.md` / `.en.md`。
- 不要改变已有字段含义。
- 不要把 snapshot/summary 描述成恢复文件。

### 3. Core replay 语义打磨

在 core smoke 中增加至少两个 replay plan 场景。

#### 3.1 continue_on_error mixed core replay

在 core session 中保存或使用 replay plan，包含：

1. 静态 action：例如 `backtrace` 或 `threads`，应成功。
2. 动态 action：例如 `continue` 或 `breakpoint_set`，在 core mode 下应被 guard 拒绝并产生
   `ToolError`。
3. 后续静态 action：例如 `locals` / `args_info` / `evaluate`，在 `continue_on_error` 下应继续执行并成功。

必须验证：

- replay response 中整体语义可审计。
- `ReplayStep` evidence 至少包含 success、failed、success 三类 step。
- failed step 有 `error_evidence`，对应 `ToolError` summary 明确提到 core mode 不可用或 state guard reason。
- `ReplayRun` evidence 存在。
- report 的 `Replay Execution Audit` 能展示三步状态和 error evidence。

#### 3.2 stop_on_error mixed core replay

再保存或使用一个 `stop_on_error` replay plan，包含：

1. 静态 action，成功。
2. 动态 action，core guard 拒绝并失败。
3. 后续静态 action，应被 replay runtime 标记为 `skipped`。

必须验证：

- `ReplayStep` evidence 包含 success、failed、skipped。
- skipped step 有明确 skip reason，例如前序 step 在 `stop_on_error` 下失败。
- `ReplayRun` evidence 存在，且整体 `ok:false`。
- report 的 `Replay Execution Audit` 能展示 failed/skipped 和 skip reason。

实现要求：

- replay runtime 继续依赖 typed `ActionRequest` / `ActionOutput` / `ActionResult`。
- 不要解析 action response JSON 文本来判断 `ok:false`。
- 不要改变 replay plan schema。
- core guard rejected 要继续通过 `ToolError` evidence 审计。

### 4. 文档同步

如果 report 或 core replay 行为有用户可见变化，同步更新：

- `docs/agent_actions.md`
  - Core Dump Mode 支持的静态 action。
  - Core Dump Mode 拒绝的动态 action。
  - core mode 下 replay mixed plan 的语义：静态 step 可执行，动态 step 会失败，failure policy 决定是否继续。
- `docs/agent_actions.en.md`
- `docs/evidence_model.md`
  - Core dump summary/session fields。
  - report 的 Core Dump Snapshot / core audit 区域。
  - core replay 的 `ReplayStep` / `ReplayRun` / `ToolError` evidence 链路。
- `docs/evidence_model.en.md`

如果只增强 report 展示而不新增 artifact schema，文档应明确这是 report 聚合行为，不是 raw evidence
布局改变。

### 5. Handoff 和 progress

执行结束时：

- 更新 `docs/ai/progress.md`，记录 core report、metadata audit 和 core replay semantics 覆盖。
- 覆写 `docs/ai/handoff.md`，记录：
  - 实际修改的文件。
  - 验证命令和结果。
  - 是否修改 session summary 或 evidence/report schema。
  - 是否存在未覆盖的 core 限制。
- 只有新增或改变项目级 decision 时才更新 `docs/ai/decision.md`；本轮默认不需要。

## Artifact consistency 要求

core smoke 必须继续或新增检查：

- report 文件存在。
- assets 目录存在。
- `task.normalized.json` 存在。
- `session_summary.json` 存在。
- `session_snapshot.json` 存在。
- `evidence/index.json` 存在。
- index 中每条 evidence 的 view/raw/summary 文件存在。
- report 引用的 evidence id 都存在于 evidence index。
- core static action evidence 存在。
- core dynamic guard rejected `ToolError` evidence 存在。
- core replay mixed plan 的 `ReplayStep`、`ReplayRun`、`ToolError` evidence 存在。
- `stop_on_error` replay 的 skipped step 能在 report 中看到 skip reason。

## 保持外部行为兼容

必须保持：

- Core Dump Mode 仍是静态取证模式。
- 动态 action 在 core mode 下继续被拒绝。
- CLI/daemon action JSON schema 兼容。
- response 字段含义兼容。
- replay plan schema 兼容。
- evidence raw 文件布局兼容。
- `session_snapshot.json` 和 `session_summary.json` 不作为 live GDB 恢复文件。
- report 仍是调试报告草稿，不自动宣称根因。
- `raw_mi` 仍必须显式 `risk:"advanced"`，且不能作为 on-hit action。

## 验证

至少运行：

```bash
cmake --build build
./build/gdb-agent check examples/segfault_task.md
./scripts/smoke_core_dump_mode.sh
ctest --test-dir build -R core_dump_mode --output-on-failure
ctest --test-dir build --output-on-failure
git diff --check
```

如果改动触及 replay runtime 或 report replay audit，还应运行：

```bash
./scripts/smoke_replay_setup_plan_flow.sh
./build/replay_plan_tests
```

如果当前环境没有 `gdb`：

- 仍需运行不依赖 GDB 的构建和单元测试。
- 在 `docs/ai/handoff.md` 和最终回复中明确记录 core live smoke 未运行原因。

## 完成标准

本轮完成时应满足：

- report 中存在 core-first 的 `Core Dump Snapshot` 或等价区域。
- core report 区域能展示 core path、core loaded 状态和关键静态 evidence 链路。
- core smoke 检查 task/session/evidence/report artifact 一致性。
- core replay mixed plan 覆盖 `continue_on_error` 下 success / failed / success。
- core replay mixed plan 覆盖 `stop_on_error` 下 success / failed / skipped。
- failed dynamic replay step 能追踪到 core guard `ToolError` evidence。
- report 的 replay audit 能展示 core replay step 的 failed/skipped/error evidence/skip reason。
- docs 说明 core mode 下 replay mixed plan 语义。
- 未改变 core mode 静态取证边界。
- 未改变 action JSON schema、CLI 语法、replay plan schema、evidence raw 文件布局或 task format。
- `docs/ai/progress.md` 和 `docs/ai/handoff.md` 已记录实际完成内容、验证结果和遗留限制。
- 如无新增项目级决策，不更新 `docs/ai/decision.md`。
- 本轮相关改动已按仓库约定 commit 并 push。
