# Handoff

日期：2026-05-31

## 本轮完成

- 完成 `docs/ai/next_cli_task.md` 指定的 Probe on-hit policy 和命中证据闭环任务。
- `on_hit` metadata 现在支持 policy object：
  - `actions`
  - `timeout_ms`
  - `max_output_bytes`
  - `max_summary_lines`
  - `failure_policy`
  - `continue_after_hit`
- 旧格式 `on_hit: [{"action":"backtrace"}]` 仍兼容读取，按默认 policy 执行。
- `breakpoint_set`、`watchpoint_set` 和 `catchpoint_set` 会把 on-hit policy 保存到
  `ProbeState` 与 finish-time `assets/probes.json`。
- probe hit evidence（`BreakpointHit`、`WatchpointHit`、`CatchpointHit`）现在包含：
  - `on_hit_policy`
  - `on_hit_results`
  - `on_hit_evidence_ids`
  - `on_hit_error_ids`
- 每个自动 on-hit action 会写入独立 `OnHitAction` evidence，记录 status、action evidence ids、
  error evidence、skip reason 和按 policy 预算截断的 response 摘要。
- 支持 `continue_on_error` / `stop_on_error`；`stop_on_error` 会把后续 action 记录为
  `skipped`。
- 支持 `continue_after_hit:true`，执行结果作为 `continue_after_hit` on-hit result 记录。
- `session_summary.json` 新增：
  - `probe_hit_count`
  - `on_hit_action_count`
  - `on_hit_error_count`
- 报告 Probes 区域会列出 probe hit 与 on-hit evidence。
- 扩展 `scripts/smoke_daemon_action_flow.sh`，Linux + GDB 下覆盖真实 breakpoint hit、
  on-hit 成功/失败/skipped、`continue_after_hit`、summary/report/probes/evidence index。
- 同步更新：
  - `docs/agent_actions.md`
  - `docs/agent_actions.en.md`
  - `docs/evidence_model.md`
  - `docs/evidence_model.en.md`
  - `docs/ai/decision.md`
  - `docs/ai/progress.md`

## 验证

- `cmake --build build`
- `ctest --test-dir build --output-on-failure`
- `./build/gdb-agent check examples/segfault_task.md`
- `git diff --check`

当前 Linux 环境安装了 GDB，因此 `daemon_action_flow` 已实际执行 live daemon smoke，
包括本轮新增的 on-hit policy 命中路径，而不是 skip。

## 限制和注意事项

- 本轮没有扩展新的 catchpoint event；仍只支持已有 `catch throw`。
- 本轮没有新增 hypothesis assertion，也没有继续扩展 replay plan schema。
- `max_output_bytes` 和 `max_summary_lines` 目前限制 `OnHitAction` wrapper evidence 中的
  response 摘要；底层 action 自己产生的 raw/summary evidence 仍按 evidence store 的全局规则保留。
- `continue_after_hit:true` 会让 inferior 自动继续运行到下一个 stop event；默认值仍为
  `false`。
- 当前工作区仍存在用户侧 `AGENTS.md` 修改，本轮未触碰，也不应纳入本轮提交。
