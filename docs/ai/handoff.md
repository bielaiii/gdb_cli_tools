# Handoff

日期：2026-05-31

## 本轮完成

- 将 `docs/ai/next_cli_task.md` 从已完成的 Replay Store 收尾任务更新为下一轮规划：
  完善 Probe on-hit policy 和命中证据闭环。
- 新任务明确聚焦：
  - on-hit action policy schema。
  - probe hit 与 on-hit action evidence 关联。
  - Linux + GDB daemon/live smoke 覆盖真实命中。
  - action、evidence 和 report 文档同步。
- 明确下一轮不扩展新的 catchpoint event、不新增 hypothesis assertion、不继续扩展 replay
  plan schema，除非这些改动是 on-hit evidence 归属所必需的最小调整。

## 验证

- 本轮只修改任务规划和交接文档，未改代码。
- 未运行 build 或测试。
- 已用 `git diff --check` 检查文档 diff 格式。

## 限制和注意事项

- Probe/on-hit 功能本轮尚未实现；下一轮应只执行 `docs/ai/next_cli_task.md` 中指定的
  probe on-hit policy 和命中证据闭环任务。
- 下一轮如改动 probe action payload、evidence schema、report 字段或 session summary 字段，
  需要同步更新 `docs/agent_actions.md`、`docs/evidence_model.md` 及英文版；必要时更新
  `docs/ai/decision.md`。
- 当前工作区存在用户侧 `AGENTS.md` 修改，本轮未触碰也不应混入本轮提交。
