# Next CLI Task

## 目标

完善 Probe on-hit policy 和命中证据闭环，让 breakpoint/watchpoint/catchpoint 命中后
自动执行的动作更可控、更可审计，并能在 report、session summary 和 evidence index 中
稳定呈现。

本轮聚焦四件事：

1. 明确 on-hit action 的执行策略和限制。
2. 强化 probe hit 与 on-hit action evidence 的关联。
3. 扩展 Linux + GDB daemon smoke，覆盖真实命中和自动取证。
4. 同步更新 action、evidence 和报告相关文档。

## 背景语义

- Probe metadata 属于运行期工具状态，`assets/probes.json` 只在 finish/report 阶段写出。
- Probe 命中必须保留当次 metadata 快照，避免之后 probe 被修改或删除导致历史证据失真。
- on-hit action 是减少重复取证成本的执行策略，不代表工具自动判断根因。
- on-hit action 只能调用已有高层 action；不要默认暴露 raw MI。
- raw evidence 仍是权威数据，summary/view 只是低噪声、有损视图。

## 范围

### 1. on-hit policy schema

为 breakpoint/watchpoint/catchpoint 的 `on_hit` metadata 补齐可控策略。建议支持：

- `actions`：命中后按顺序执行的高层 action 列表。
- `timeout_ms`：单个 on-hit action 的超时上限。
- `max_output_bytes`：单个 on-hit action 可写入 summary/view 的最大输出预算。
- `max_summary_lines`：单个 on-hit action summary 的最大行数。
- `failure_policy`：
  - `continue_on_error`
  - `stop_on_error`
- `continue_after_hit`：on-hit actions 执行完后是否自动 continue。

要求：

- 对旧格式 `on_hit` 尽量兼容；无法兼容时返回稳定 `ToolError` evidence。
- 默认值必须保守，避免命中后无限输出或反复自动 continue。
- policy 必须进入 probe metadata、hit evidence 和最终 `assets/probes.json`。
- `continue_after_hit` 需要清晰表达风险，避免 Agent 误以为命中后一定停住。

### 2. Probe hit 与 on-hit evidence 关联

增强命中证据，确保 Agent 能回答：

- 哪个 probe 命中了。
- 命中时 probe 的 kind、location/expression/event、condition、comment、purpose 是什么。
- 当次命中是第几次 hit。
- 命中后自动执行了哪些 action。
- 每个 on-hit action 是否成功、失败或被跳过。
- 每个 on-hit action 产生了哪些 evidence id。

建议新增或强化以下结构：

- `BreakpointHit` / `WatchpointHit` / `CatchpointHit` evidence 中增加：
  - `on_hit_policy`
  - `on_hit_results`
  - `on_hit_evidence_ids`
  - `on_hit_error_ids`
- 如现有 evidence 模型更适合，也可以新增 `OnHitAction` evidence kind，但不要引入无关重构。
- session summary 增加 probe hit 和 on-hit 相关计数，例如：
  - `probe_hit_count`
  - `on_hit_action_count`
  - `on_hit_error_count`

要求：

- on-hit action 失败不能吞掉；必须有 evidence 可查。
- 如果 `failure_policy: stop_on_error` 导致后续 on-hit action 未执行，应标记 skipped 并写明原因。
- on-hit action 的 evidence id 必须是本次命中新产生的 evidence，不能复用历史 id。

### 3. daemon/live smoke 覆盖真实命中

扩展 `scripts/smoke_daemon_action_flow.sh` 或新增专门 smoke，覆盖 Linux + GDB 下真实 on-hit flow：

- 创建 live session。
- 设置 breakpoint，附带 comment、purpose 和 on-hit policy。
- 触发 breakpoint hit。
- 验证 `BreakpointHit` evidence 包含 probe metadata 快照。
- 验证 on-hit action 产生独立 evidence id。
- 验证失败策略，例如一个成功 action 加一个非法/失败 action。
- 验证 `stop_on_error` 下后续 action 被 skipped。
- finish 后检查：
  - report 中有 probe hit 和 on-hit 结果。
  - `session_summary.json` 有相关计数。
  - `assets/probes.json` 有最终 probe metadata。
  - evidence index 能找到命中和 on-hit 证据。

测试要求：

- Linux + GDB 环境实际执行 live smoke。
- 缺少 GDB 或非 Linux 时按现有项目口径 skip live 部分。
- 如 policy 解析可独立测试，增加不依赖 GDB 的 fixture/unit smoke。

### 4. 文档和报告对齐

同步更新文档：

- `docs/agent_actions.md`
- `docs/agent_actions.en.md`
- `docs/evidence_model.md`
- `docs/evidence_model.en.md`

如 action payload 或 task/report 字段变化，按需更新：

- `docs/task_format.md`
- `docs/task_format.en.md`
- `docs/ai/decision.md`

报告应能稳定展示：

- probe 列表和最终 metadata。
- probe hit 记录。
- on-hit policy。
- 每个 on-hit action 的 status、evidence id 和错误信息。
- on-hit 自动 continue 的行为说明。

## 不做

- 不扩展新的 catchpoint event；本轮仍只要求已有 `catch throw` 行为不退化。
- 不新增 hypothesis assertion，除非仅为验证 on-hit evidence 归属需要最小调整。
- 不继续扩展 replay plan schema。
- 不把 on-hit 结果包装成根因判断。
- 不引入 PTY 或交互式 stdin。
- 不为了 macOS live debugging 做兼容；目标运行平台仍是 Linux。

## 完成标准

- on-hit policy 有明确 schema、默认值、错误行为和文档。
- breakpoint/watchpoint/catchpoint hit evidence 能关联 on-hit action results 和 evidence ids。
- on-hit action 的 success / failed / skipped 状态稳定输出。
- `stop_on_error`、`continue_on_error` 和 `continue_after_hit` 有测试覆盖或 smoke 覆盖。
- Linux + GDB daemon smoke 覆盖真实 probe hit 和 on-hit 自动取证。
- report、session summary、evidence index 和 `assets/probes.json` 能体现 probe/on-hit 闭环。
- `docs/agent_actions.md`、`docs/evidence_model.md` 及英文版与实现一致。
- `docs/ai/progress.md` 和 `docs/ai/handoff.md` 记录实际完成、验证结果和限制。
