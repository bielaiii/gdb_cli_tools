# Handoff

日期：2026-05-29

## 本轮完成

- 收敛 Probe Store 运行期/持久化语义：运行期以内存 `ProbeState` 为权威状态，
  `assets/probes.json` 只在 finish/report 写出阶段生成。
- 停止在 `breakpoint_set`、`watchpoint_set`、`catchpoint_set`、probe enable/disable/delete、
  probe hit 和 `probe_list` 路径中同步写 `assets/probes.json`。
- daemon `finish`、`finish_session` 和 `serve` stdin EOF/finish 路径都会在写 session/report
  文件前统一写最终 probe snapshot。
- `probe_list` 继续返回当前内存 metadata；probe hit evidence 继续携带 number、kind、
  location/expression/event、condition、comment、purpose、hit_count 等上下文。
- 更新 `docs/evidence_model.md`、`docs/evidence_model.en.md`、`docs/agent_actions.md`、
  `docs/agent_actions.en.md`、`docs/ai/decision.md` 和 `docs/ai/progress.md`。

## 验证

- `cmake -S . -B build` 通过。
- `cmake --build build` 通过。
- `./build/gdb-agent check examples/segfault_task.md` 通过。
- `./scripts/smoke_segfault_demo.sh` 通过。
- `./scripts/smoke_daemon_action_flow.sh` 在当前 macOS 环境按平台口径输出
  `skip: daemon/action live smoke requires Linux + GDB` 并返回成功。
- `ctest --test-dir build --output-on-failure` 通过，包含 `segfault_demo_check` 和
  `daemon_action_flow`。

## 限制和注意事项

- 仓库约定文件名是 `docs/ai/next_cli_task.md`，不是复数 `next_cli_tasks.md`。
- 当前环境是 macOS，不执行 live GDB session 作为验收；Linux + GDB 环境下仍需运行
  `scripts/smoke_daemon_action_flow.sh`，确认 daemon/action live flow 和 finish-time
  `probes.json` 内容。
- 本轮更新了既有设计决策 D007 的措辞，未新增新的决策编号。
