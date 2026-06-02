# Next CLI Task

## 目标

新增一轮更丰富的工具能力回归/探索测试，用确定性的 fixture、脚本和少量单元测试主动验证
`gdb-agent` 作为 AI Agent 调试执行层的真实可用性，并暴露当前能力薄弱点、行为不稳定处和
文档/实现不一致处。

本轮不是扩展新产品功能，而是提升测试对“工具能力”的验证深度。测试应尽量模拟 Agent 会做的
真实调试动作：启动 session、设置 probe、命中断点/观察点/捕获点、执行 on-hit、保存 replay、
验证 hypothesis、加载 core、检查 evidence/report 是否能支撑推理，并把发现的问题记录到
`docs/ai/handoff.md` 和 `docs/ai/progress.md`。

## 背景

当前已有测试主要覆盖：

- `check` 的最小 README demo。
- daemon/action happy path。
- catchpoint 最小路径。
- breakpoint on-hit policy。
- replay 基础路径和 failure policy。
- hypothesis passed/failed/unknown。
- core dump mode 静态取证和动态 action guard。
- MI parser、replay plan、hypothesis assertion 的部分非 GDB 单元测试。

这些覆盖证明主路径可用，但仍偏“单一示例 + 预期路径”。下一轮应重点制造更接近真实调试的
可重复场景，让测试能回答：

1. Agent 面对不同 bug 形态时，现有 action 是否足够拿到关键证据？
2. breakpoint/watchpoint/catchpoint/on-hit 是否能可靠关联 probe metadata、命中现场和 evidence？
3. replay/hypothesis/report 是否能形成可审计的“动作 -> 观察 -> 推理材料”链路？
4. Run Mode 和 Core Dump Mode 的能力边界是否清楚，错误动作是否能稳定失败？
5. report/assets/evidence/session summary 是否足够一致，能让 Agent 低噪声地继续分析？
6. GDB/MI summary 在更真实、更嘈杂的输出下是否仍然低噪声且保留 raw audit？

## 范围

### 1. 新增工具能力 integration smoke

新增脚本，例如：

```text
scripts/smoke_capability_matrix.sh
```

并通过 CTest 运行，例如测试名：

```text
capability_matrix_flow
```

要求：

- Linux + GDB 下实际运行；非 Linux 或缺少 GDB 时按现有 smoke 口径 skip。
- 使用临时目录，不污染仓库根目录。
- 失败时打印足够上下文，包括 daemon log、关键 response、相关 report/assets 路径。
- 不依赖系统 core dump 配置。
- 不把测试产物提交进仓库。

该 smoke 应围绕“工具能力矩阵”组织，而不是围绕 parser/input 边界组织。优先覆盖以下场景。

#### Fixture 程序

如果现有 `examples/segfault.cpp` 不足以覆盖能力矩阵，允许新增小型 C++ fixture，并接入 CMake。
fixture 应保持短小、确定、可解释，例如：

- segfault fixture：稳定崩溃，便于 backtrace/locals/args/evaluate/hypothesis。
- watchpoint fixture：修改全局或堆对象字段，便于 watchpoint 命中和 `WatchpointHit` evidence。
- exception fixture：抛出 C++ exception，便于 `catchpoint_set event:"throw"` 和 `CatchpointHit`。
- stdin/env/output fixture：读取 stdin/env 并写 stdout/stderr，便于验证非 PTY I/O 和
  `InferiorOutput` evidence。
- multithread fixture：创建少量线程并在某线程崩溃，便于验证 `threads`、thread summary 和
  crash 现场表达。

不要为了 fixture 做复杂业务逻辑；fixture 的目的只是稳定触发工具能力。

#### Agent-facing action 能力

至少覆盖：

- `backtrace`、`threads`、`frame_select`、`args_info`、`locals`、`registers`、`evaluate`。
- `breakpoint_set` 命中真实代码位置，并检查 `BreakpointHit` evidence。
- `watchpoint_set` 在 fixture 中真实命中；如果当前实现或 GDB 条件限制导致不稳定，应记录为薄弱点。
- `catchpoint_set event:"throw"` 在 exception fixture 中真实命中，并检查 `CatchpointHit` evidence。
- `probe_list` 能展示 breakpoint/watchpoint/catchpoint 的 comment、purpose、condition、hit count。
- `probe_enable` / `probe_disable` / `probe_delete` 对后续命中和 `probe_list` 的影响符合预期。
- on-hit policy 覆盖：
  - 成功 action。
  - unsupported action。
  - `continue_on_error`。
  - `stop_on_error`。
  - `continue_after_hit:true`。
  - 禁止 `raw_mi` 作为 on-hit action。
- `raw_mi` 只在显式 `risk:"advanced"` 时可用，并产生可审计 raw evidence。

#### Artifact consistency

在 finish 后检查：

- `report.md` 存在。
- `task.normalized.json` 存在。
- `session_snapshot.json` 存在。
- `session_summary.json` 存在。
- `evidence/index.json` 存在。
- index 中引用的 evidence view/raw/summary 文件存在。
- `session_summary.json` 中的计数与 evidence index 中相应 kind 的数量大体一致。
- report 中出现的关键 evidence id 能在 evidence index 中找到。
- `probes.json` 只在 finish/report 阶段作为最终 artifact 出现；不要假设它是 live 恢复状态。
- 对每个 fixture/session，确认 evidence chain 足以支持 Agent 继续推理：
  - action response 有 evidence id。
  - evidence index 有对应 kind/title。
  - view/summary/raw 文件可读。
  - report 中能看到关键 session summary 和 probe/hypothesis/replay 信息。

如果发现当前实现不满足某项一致性，不要在测试里硬编码错误期望来掩盖问题；应让测试失败或记录为明确 weakness。

#### Replay / cross-session boundaries

- 保存包含多个 action 的 replay plan，其中至少包含一个会失败的 step。
- 分别测试 `continue_on_error` 和 `stop_on_error` 的行为。
- 用不同 task fingerprint 的 session 尝试 replay：
  - 默认应拒绝并记录 `ToolError`。
  - `force` 时应执行并记录 `ReplayWarning`。
- 检查 replay run/step evidence 和 `session_summary.json` 计数。
- 检查新 session 产生新的 evidence id，不复用旧 session 的 evidence id。

#### Hypothesis workflow 能力

- 在真实停点上创建 hypothesis。
- 用 `hypothesis_check` 对真实表达式执行：
  - passed check。
  - failed check。
  - unknown check。
- 检查每个 check 的 observed/status/evidence/error_evidence 是否进入：
  - action response。
  - `assets/hypotheses/index.json`。
  - 单个 hypothesis Markdown。
  - 最终 report。
- 用 `hypothesis_conclude` 写入 Agent inference/final conclusion，并确认 report 不把工具观察误写成工具自动根因结论。

#### Core mode boundaries

- core mode 下除 `run` / `continue` 外，也覆盖：
  - `breakpoint_set`
  - `watchpoint_set`
  - `catchpoint_set`
  - `probe_enable`
  - `probe_disable`
  - `probe_delete`
- 确认这些动作全部被 state guard 拒绝并产生 `ToolError` evidence。
- 确认静态 action 仍可用。

#### Session lifecycle / state guard

作为辅助覆盖，不作为本轮主线：

- 对不存在的 session 调用 `status`、`action`、`finish`，确认返回稳定错误。
- 对已 `finish` 或 `close` 的 session 再调用 action，确认不会访问悬空状态。
- 重复 `finish` 或重复 `close` 的行为应稳定：要么幂等成功，要么结构化失败，但不能崩溃或挂住。
- 在 stopped/exited/core 等不同状态下调用不合适 action，确认 `ToolError` evidence 和 response 一致。

### 2. 增加工具能力相关的非 GDB 单元测试

新增或扩展 `tests/` 下的单元测试，优先覆盖不需要 live GDB、但直接影响工具能力解释的逻辑。
不要把本轮重点放在 Markdown task parser 的穷举边界；parser 测试只在发现会影响工具能力时补。

优先覆盖：

- MI parser/sanitizer/summarizer：
  - nested tuple/list。
  - escaped quotes。
  - stream records。
  - backtrace/thread raw MI 中的真实噪声。
  - STL 类型 sanitizer，例如 vector/map/unique_ptr/shared_ptr 的噪声压缩；如果当前支持不足，先用测试暴露。
- replay result / evidence accounting：
  - stop_on_error 后 skipped step 的结构。
  - continue_on_error 的后续 step 继续执行。
  - task fingerprint mismatch / force warning 的结构化表达。
- hypothesis assertion：
  - 空 observed。
  - expected 缺失。
  - equals/not_equals 的 string 边界。
  - 为未来 numeric assertion 记录缺口，但不要在本轮实现 numeric 比较，除非测试暴露出简单一致性 bug。
- report/evidence helper：
  - 如果有可抽取的纯函数，验证 evidence id 引用、summary 计数、kind 聚合等不依赖 GDB 的逻辑。

要求：

- 新测试接入 CTest。
- 单元测试不依赖 GDB。
- 测试名称清晰，失败消息能定位具体能力缺口。

### 3. 生成“薄弱点清单”

本轮执行结束后，在 `docs/ai/handoff.md` 里新增专门小节：

```text
## 发现的薄弱点
```

每个条目写清楚：

- 现象。
- 复现测试或命令。
- 影响范围。
- 建议后续处理方式。
- 是否已在本轮修复。

如果某个测试为了避免阻塞暂时只记录 warning，也要说明为什么没有让它 hard fail。

同时更新 `docs/ai/progress.md`：

- 记录新增测试覆盖。
- 把仍未解决的薄弱点放入对应 Phase 的“仍需关注”。

### 4. 必要时做最小修复

如果新增测试发现的是明显实现 bug，允许在本轮做最小修复，但必须满足：

- 修复范围只服务于新增测试暴露的问题。
- 不引入新产品功能。
- 不做 unrelated refactor。
- 修复后同步更新相关文档：
  - action 行为变化更新 `docs/agent_actions.md` 和英文版。
  - task 字段变化更新 `docs/task_format.md` 和英文版。
  - evidence/schema/artifact 变化更新 `docs/evidence_model.md` 和英文版。
  - 项目级语义变化更新 `docs/ai/decision.md`。

如果问题较大，不要强行修；保留 failing/xfail 说明或记录为 weakness，并在 handoff 里给出下一步建议。

## 不做

- 不新增面向 Agent 的产品 action，除非是为了修复现有 action 的一致性 bug。
- 不扩展 catchpoint 的产品能力到新 event。
- 不实现 numeric hypothesis assertion，除非只是修复已有 equals/not_equals 的明显 bug。
- 不重构 daemon 架构。
- 不引入 PTY 或交互式 stdin。
- 不为了 macOS live GDB 做兼容；目标平台仍是 Linux。
- 不把测试脚本变成 Agent 正常使用入口；Agent 正常使用的接口仍是 CLI/daemon action。

## 完成标准

- 新增至少一个更丰富的 integration smoke，并接入 CTest。
- 新增或扩展至少一组非 GDB 单元测试，并接入 CTest。
- 新测试能覆盖多个错误路径、状态边界和 artifact 一致性检查。
- Linux + GDB 环境下运行：

```bash
cmake --build build
ctest --test-dir build --output-on-failure
./build/gdb-agent check examples/segfault_task.md
git diff --check
```

- 如果有测试因环境缺失 skip，必须在 handoff 中说明。
- `docs/ai/progress.md` 记录新增覆盖和剩余薄弱点。
- `docs/ai/handoff.md` 记录实际完成、验证结果、发现的薄弱点和未修限制。
- 按 Execution mode 约定提交并推送本轮相关改动。
