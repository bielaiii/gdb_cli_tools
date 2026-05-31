# Handoff

日期：2026-05-31

## 本轮完成

- 完成 Replay Store 收尾任务：
  - 结构化 replay plan 继续使用 `gdb-agent-replay-plan-v1`，新增 `schema_version`、tags、
    source session id、created_at、task metadata、task fingerprint 和 plan-level
    `failure_policy`。
  - 支持 `continue_on_error` 与 `stop_on_error`，step-level `failure_policy` 可覆盖 plan-level
    policy。
  - replay result 现在按 step 输出 index、action name、status、failure policy、ReplayStep
    evidence、action evidence、error evidence 和 skip reason。
  - 每次 replay 会记录 `ReplayRun` evidence，保存 plan/schema/force/match/warning/error 和
    step result 整体快照。
  - task fingerprint mismatch 默认拒绝 replay 并记录 `ToolError`；显式 `force` 时允许执行，
    并记录 `ReplayWarning`。
- 新增 `src/replay/replay_plan.*`，将 replay plan 写入、fingerprint 和校验逻辑拆出为可测试模块。
- `save_action` / `save-action` 支持 `failure_policy`；`replay` / CLI `replay` 支持
  `failure_policy` override 和 `force`。
- `session_summary.json` 新增 `replay_step_count` 和 `replay_warning_count`。
- 新增 `tests/replay_plan_tests.cpp`，覆盖 schema、task fingerprint、force mismatch、
  legacy plan 和 failure policy。
- 扩展 `scripts/smoke_daemon_action_flow.sh`，覆盖保存 plan、finish 后新建 session、按文件
  replay、检查 ReplayStep evidence、session summary 和 report。
- 同步更新：
  - `docs/agent_actions.md`
  - `docs/agent_actions.en.md`
  - `docs/evidence_model.md`
  - `docs/evidence_model.en.md`
  - `docs/ai/decision.md`
  - `docs/ai/progress.md`

## 验证

- `cmake -S . -B build`
- `cmake --build build`
- `ctest --test-dir build --output-on-failure`

当前 Linux 环境安装了 GDB，因此 `daemon_action_flow` 已实际执行 live daemon + restart replay
smoke，而不是 skip。

## 限制和注意事项

- 本轮没有扩展 probe、hypothesis、catchpoint 类型或 MI summary 行为。
- replay plan task fingerprint 使用当前实现的稳定 FNV-1a 64-bit 摘要，目标是低成本识别明显
  task mismatch，不是安全用途的密码学哈希。
- 旧 JSONL 和缺少 task metadata/fingerprint 的旧结构化 plan 会尽量兼容读取并产生 warning；
  未知 schema 或 schema version 会稳定拒绝。
- 已创建并推送提交：
  - `81954ca Complete replay store hardening`
  - `git push` 已将 `main` 从 `097c0d8` 推进到 `81954ca`。
