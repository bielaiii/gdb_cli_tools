# Handoff

日期：2026-06-09

## 本轮完成

- 按 `docs/ai/next_cli_task.md` 执行 capability matrix hardening 任务，聚焦新增 catchpoint event、
  真实 Linux + GDB smoke 和文档同步。
- 更新 `src/cli.cpp`：
  - `catchpoint_set` 新增 `event:"syscall"`，支持 `catch syscall`。
  - `event:"syscall"` 支持 `name` 或 `syscall` selector，映射到 `catch syscall <selector>`。
  - selector 支持字符串 syscall 名或整数 id；字符串只允许字母、数字和下划线。
  - 新增 `event:"fork"` / `"vfork"` / `"exec"`，映射到对应 GDB catch command。
  - Probe metadata 新增 `selector` 字段，并写入 action response、`probe_list`、
    finish-time `assets/probes.json` 和 `CatchpointHit` evidence。
- 更新 `examples/capability_fixture.cpp`：
  - 新增 Linux-only `syscall`、`fork` 和 `exec` 模式，用于真实触发新增 catchpoint。
  - 非 Linux 下保留可构建 fallback。
- 新增 `scripts/smoke_catchpoint_matrix.sh` 并接入 CTest `catchpoint_matrix_flow`：
  - 使用 `--replay-before-run` 在 initial run 前设置 catchpoint。
  - 真实覆盖 generic syscall、`write` syscall selector、fork 和 exec 命中。
  - 验证 `probe_list` metadata、`CatchpointHit` evidence、`assets/probes.json`、
    `session_summary.json`、report evidence id 引用和 raw/view/summary 文件存在。
  - 覆盖 unsupported event、非法 syscall selector 和 `syscall` selector 字段别名。
- 更新 `scripts/smoke_capability_matrix.sh`，Core Dump Mode guard 覆盖新增动态 catchpoint event。
- 更新 `scripts/smoke_daemon_action_flow.sh`，旧 unsupported catchpoint 负例从 `syscall` 改为
  `not-real`。
- 同步更新：
  - `docs/agent_actions.md`
  - `docs/agent_actions.en.md`
  - `docs/evidence_model.md`
  - `docs/evidence_model.en.md`
  - `docs/known_limitations.md`
  - `docs/mvp_acceptance.md`
  - `docs/ai/progress.md`

## 验证

- `cmake --build build`
- `scripts/smoke_catchpoint_matrix.sh`
- `scripts/smoke_daemon_action_flow.sh`
- `scripts/smoke_capability_matrix.sh`
- `./build/gdb-agent check examples/segfault_task.md`
- `git diff --check`
- `ctest --test-dir build --output-on-failure`
  - 当前 Linux 环境有 GDB，完整 CTest 已运行。
  - 结果：10/10 tests passed。

## 限制和注意事项

- 本轮实现了 `vfork` action 支持，但没有把真实 `vfork` hit 放进稳定 smoke；当前真实 hit smoke
  覆盖 syscall、指定 `write` syscall、fork 和 exec。
- `catch syscall` 在真实 GDB stop record 中可能出现 `syscall-entry` 等 stop reason，不一定是
  `breakpoint-hit`；probe attribution 仍通过 GDB 返回的 catchpoint number 关联到
  `CatchpointHit` evidence。
- 某些 Linux/GDB/target 组合可能不支持特定 catch command；工具会返回 `ok:false`，保留
  `ToolError` 和原始 command evidence，不伪装成功。
- 本轮没有改变 raw evidence 优先原则、session snapshot 语义、daemon 协议或 Core Dump Mode
  静态取证边界。
- 本轮未新增项目级 decision，因此未修改 `docs/ai/decision.md`。
