# Next CLI Task

## 目标

新增一轮更丰富的回归/探索测试，用确定性的脚本和单元测试主动暴露当前项目的薄弱点、
边界不完善处和文档/实现不一致处。

本轮不是扩展新产品功能，而是提升测试的“找问题能力”。测试应覆盖现有核心能力在错误输入、
状态切换、跨 session、artifact 一致性和 GDB/MI 输出边界下的行为，并把发现的问题记录到
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

这些覆盖证明主路径可用，但仍偏“预期路径”。下一轮应重点制造更复杂但可重复的场景，让测试能回答：

1. 状态机在非法动作、重复动作、关闭 session、finish 后 action 等场景是否稳定？
2. 错误输入是否都能产生结构化 `ToolError`，而不是崩溃、挂住或输出不可解析文本？
3. report/assets/evidence/session summary 之间是否一致？
4. replay/probe/hypothesis/core/run mode 的边界是否清楚？
5. GDB/MI summary 在更真实、更嘈杂的输出下是否仍然低噪声且可审计？

## 范围

### 1. 新增一个 edge-case integration smoke

新增脚本，例如：

```text
scripts/smoke_edge_cases.sh
```

并通过 CTest 运行，例如测试名：

```text
edge_case_flow
```

要求：

- Linux + GDB 下实际运行；非 Linux 或缺少 GDB 时按现有 smoke 口径 skip。
- 使用临时目录，不污染仓库根目录。
- 失败时打印足够上下文，包括 daemon log、关键 response、相关 report/assets 路径。
- 不依赖系统 core dump 配置。
- 不把测试产物提交进仓库。

该 smoke 至少覆盖以下类别中的大部分，优先选择最容易暴露问题的场景：

#### Session lifecycle / state guard

- 对不存在的 session 调用 `status`、`action`、`finish`，确认返回稳定错误。
- 对已 `finish` 或 `close` 的 session 再调用 action，确认不会访问悬空状态。
- 重复 `finish` 或重复 `close` 的行为应稳定：要么幂等成功，要么结构化失败，但不能崩溃或挂住。
- 在 running/stopped/exited/core 等不同状态下调用不合适 action，确认 `ToolError` evidence 和 response 一致。

#### Invalid action payloads

- action JSON 非法或缺少 `action` 字段。
- `breakpoint_set` 缺少 location。
- `watchpoint_set` 缺少 expression。
- `frame_select` 使用负数或明显越界 frame。
- `evaluate` 缺少 expression 或表达式非法。
- `raw_mi` 缺少 `risk:"advanced"`。
- `raw_mi` 使用不允许或危险的 payload 时应稳定拒绝或记录风险。
- `replay` 指向不存在的 plan 文件。
- `hypothesis_check` 指向不存在的 hypothesis。
- `hypothesis_check` 缺少 expression 或 assertion 参数。

要求：

- 每个错误路径返回 `ok:false` 或明确的 `status:"unknown"`，具体按现有语义。
- 错误路径应尽量写入 `ToolError` evidence。
- response 中至少包含 action/error/evidence 或等价字段，便于 Agent 判断下一步。

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

如果发现当前实现不满足某项一致性，不要在测试里硬编码错误期望来掩盖问题；应让测试失败或记录为明确 weakness。

#### Replay / cross-session boundaries

- 保存包含多个 action 的 replay plan，其中至少包含一个会失败的 step。
- 分别测试 `continue_on_error` 和 `stop_on_error` 的行为。
- 用不同 task fingerprint 的 session 尝试 replay：
  - 默认应拒绝并记录 `ToolError`。
  - `force` 时应执行并记录 `ReplayWarning`。
- 检查 replay run/step evidence 和 `session_summary.json` 计数。
- 检查新 session 产生新的 evidence id，不复用旧 session 的 evidence id。

#### Probe / on-hit boundaries

- watchpoint 命中或至少 watchpoint 设置失败路径。
- disabled probe 不应触发 on-hit。
- 删除 probe 后 `probe_list` 不应继续返回该 probe。
- on-hit 中包含 unsupported action 时，`continue_on_error` 和 `stop_on_error` 的 skipped/error 记录应稳定。
- `raw_mi` 不允许作为 on-hit action。

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

### 2. 增加非 GDB 单元测试

新增或扩展 `tests/` 下的单元测试，优先覆盖不需要 live GDB 的薄弱逻辑：

- task parser 边界：
  - 缺少 required section。
  - 空 executable。
  - shell-like args quoting/escaping。
  - env 行格式。
  - stdin/core dump path 字段。
- JSON/action parsing 边界：
  - 非法 JSON。
  - 缺少 action。
  - 参数类型错误。
- MI parser/sanitizer 边界：
  - nested tuple/list。
  - escaped quotes。
  - stream records。
  - STL 类型 sanitizer，例如 vector/map/unique_ptr/shared_ptr 的噪声压缩，如果当前支持不足，先用测试暴露。
- hypothesis assertion 边界：
  - 空 observed。
  - expected 缺失。
  - equals/not_equals 的 string 边界。
  - 为未来 numeric assertion 记录缺口，但不要在本轮实现 numeric 比较，除非测试暴露出简单一致性 bug。

要求：

- 新测试接入 CTest。
- 单元测试不依赖 GDB。
- 测试名称清晰，失败消息能定位具体 case。

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
