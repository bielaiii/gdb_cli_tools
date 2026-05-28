# Handoff

日期：2026-05-28

## 本轮完成

- 新增 `catchpoint_set` 高层 action，本轮只支持 `{"event":"throw"}`，映射到 GDB
  `catch throw`。
- catchpoint 复用 probe store，支持 `comment`、`purpose` 和 `on_hit` metadata；
  `probe_list`/`assets/probes.json` 会展示 `kind: "catchpoint"` 和 `event: "throw"`。
- probe hit evidence 增加 `CatchpointHit`；unsupported action 和 unsupported catchpoint
  event 会记录 `ToolError` evidence。
- 新增 `scripts/smoke_daemon_action_flow.sh`，Linux + GDB 下覆盖 daemon/create/status/连续
  action/catchpoint/probe_list/非法 event/finish/shutdown，并检查 report、snapshot、
  summary、evidence index 和 probe store。
- CTest 新增 `daemon_action_flow`，macOS 会按 Linux-only 口径跳过 live 部分。
- 更新 `docs/agent_actions.md` 和 `docs/agent_actions.en.md`。
- 更新 `docs/ai/progress.md` 记录本轮状态。

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

- 当前环境是 macOS，不执行 live GDB session 作为验收；Linux + GDB 环境下仍需运行
  `scripts/smoke_daemon_action_flow.sh` 做真实 daemon/action live 验证。
- catchpoint 本轮只实现 `catch throw`，未实现 `catch catch`、`catch syscall`、
  `catch fork` 等其他事件。
- 本轮没有新增项目级设计决策，因此未更新 `docs/ai/decision.md`。
