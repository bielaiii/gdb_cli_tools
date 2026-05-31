# Next CLI Task

## 目标

把 Replay Store 做完整、做稳定，让 Agent 可以可靠保存一组高层 action，并在重启
或新 session 中一次性重放，且 replay 的失败行为、适用范围和证据归属都可审计。

本轮聚焦四件事：

1. 明确 replay 失败策略，并让行为可配置、可记录。
2. 强化 `gdb-agent-replay-plan-v1` schema 和 task metadata 校验。
3. 增加重启后 replay 的端到端示例和自动化测试。
4. 同步更新 action、evidence 和 task 相关文档。

## 背景语义

- Replay Store 只保存高层 action，不保存或恢复 GDB 内部 live 状态。
- replay 在新 session 中重新执行 action，并产生新的 evidence id；不能复用旧 evidence id。
- replay 失败必须留下 `ToolError` 或 replay step evidence，方便 Agent 判断下一步。
- `session_snapshot.json` 和 `session_summary.json` 不是恢复文件；重启恢复只能依赖 replay 高层 action。
- 工具不基于 replay 自动宣称根因，只负责执行和证据整理。

## 范围

### 1. Replay 失败策略

补齐 replay 执行时的失败策略语义：

- 支持 plan-level failure policy，例如：
  - `continue_on_error`
  - `stop_on_error`
- 支持 step-level override；未指定时继承 plan-level policy。
- 为兼容旧 replay plan，明确旧计划的默认策略，并在文档中写清楚。
- 每个 replay step 都要记录：
  - step index
  - action name
  - action payload 摘要
  - status：success / failed / skipped
  - failure policy
  - evidence id 或 error evidence id
  - 如果因为前一步失败而跳过，记录 skip reason。

要求：

- 不吞掉失败，不只在 stdout/stderr 中展示错误。
- `stop_on_error` 停止后，后续 step 应标记为 skipped 或在 replay result 中明确未执行。
- replay result 要便于 Agent 直接判断哪些检查成功、哪些失败、是否需要新假设。

### 2. Replay plan schema 和 metadata 校验

强化结构化 replay plan，继续使用 `gdb-agent-replay-plan-v1`，但补齐必要 metadata：

- schema version。
- plan name。
- optional tags。
- source session id。
- created_at。
- task metadata，例如 executable、working directory、args、core dump、problem 摘要。
- task fingerprint，用于判断 replay plan 是否可能应用到了不同调试目标。
- action list，每步保留高层 action payload 和 step metadata。

校验要求：

- replay 时检查 schema version。
- replay 时检查 task metadata / fingerprint。
- 如果当前 task 与 replay plan 不匹配，默认拒绝执行，除非用户显式传入 force 选项。
- force replay 时必须在 result 和 evidence 中记录 mismatch warning。
- 旧 JSONL 或旧结构化 plan 应尽量保持可读；如果不能完整校验，要给出稳定错误信息。

### 3. 重启后 replay 端到端示例和测试

增加一个最小端到端回归，覆盖：

- 创建 session。
- 执行若干高层 action。
- 保存 replay plan。
- 关闭或重启 session。
- 新建 session 后 replay 同一 plan。
- 检查新 replay 产生新的 evidence id。
- 检查 replay result、session summary、evidence index 和 report 中能看到 replay step 归属。

测试建议：

- 不依赖 GDB 的部分用 fixture/unit smoke 覆盖 schema、metadata 校验和 failure policy。
- Linux + GDB 环境下增加或扩展 daemon/action smoke，覆盖真实重启 replay flow。
- macOS 或缺少 GDB 的环境按既有项目口径 skip live 部分，但 schema/policy 测试仍应运行。

### 4. 文档和报告对齐

同步更新文档：

- `docs/agent_actions.md`
- `docs/agent_actions.en.md`
- `docs/evidence_model.md`
- `docs/evidence_model.en.md`
- 如 task 字段或 replay 调用参数变化，更新 `docs/task_format.md` 和英文版。

报告和 evidence 要能说明：

- replay plan 名称和 schema version。
- replay 是否 force 执行。
- task metadata 是否匹配。
- 每个 replay step 的执行结果和 evidence id。
- replay 失败、跳过和 warning 的区别。

## 不做

- 不恢复旧 GDB 进程或旧 live session。
- 不把 `session_snapshot.json` 当作 replay 输入。
- 不新增 probe、hypothesis 或 summary/MI 解析功能，除非 replay evidence 归属确实需要。
- 不扩展 catchpoint 类型。
- 不做自动根因分析。
- 不为了 macOS live debugging 做兼容；目标运行平台仍是 Linux。

## 完成标准

- replay 失败策略有明确实现、测试和文档。
- replay plan schema 包含版本、tags、task metadata / fingerprint，并在 replay 前校验。
- task mismatch 默认拒绝 replay，force replay 有明确 warning 和 evidence 记录。
- 重启后 replay 有端到端示例或 smoke test；Linux + GDB 下覆盖真实 daemon flow。
- 旧 replay plan 的兼容或拒绝行为稳定、可解释。
- `docs/agent_actions.md`、`docs/evidence_model.md` 及英文版与实现一致。
- `docs/ai/progress.md` 和 `docs/ai/handoff.md` 记录实际完成、验证结果和限制。
