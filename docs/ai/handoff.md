# Handoff

日期：2026-06-08

## 本轮完成

- 按 `docs/ai/next_cli_task.md` 执行 `catchpoint_set` 最小扩展任务。
- 扩展 `src/cli.cpp` 中的 `catchpoint_set`：
  - `event:"throw"` 继续映射到 GDB console command `catch throw`。
  - 新增 `event:"catch"`，映射到 GDB console command `catch catch`。
  - probe metadata 继续使用 `kind:"catchpoint"`，并保存原始 `event` 和对应 `location`。
  - unsupported event、Core Dump Mode guard 和 `raw_mi` 风险约束保持原语义。
- 更新 `scripts/smoke_capability_matrix.sh`：
  - 在 probe flow 中设置并验证 `event:"catch"`。
  - 继续运行到 catch handler catchpoint hit。
  - 验证 `probe_list`、`CatchpointHit` evidence、`assets/probes.json` 和
    `session_summary.json` 能体现新增 catchpoint。
- 更新文档：
  - `docs/agent_actions.md`
  - `docs/agent_actions.en.md`
  - `docs/known_limitations.md`
  - `docs/mvp_acceptance.md`
- 更新 `docs/ai/progress.md`，记录本轮 `catch catch` 支持。

## 验证

- `cmake --build build`
- `./build/gdb-agent check examples/segfault_task.md`
- `ctest --test-dir build --output-on-failure`
  - 当前 Linux 环境有 `/usr/bin/gdb`。
  - 结果：9/9 tests passed。

## 限制和注意事项

- 本轮没有新增 evidence schema，也没有修改 report/schema 布局。
- Core Dump Mode 仍然拒绝 `catchpoint_set`，保持静态取证边界。
- `catchpoint_set` 当前只支持 C++ exception 的 `catch throw` 和 `catch catch`。
- 其他 catchpoint event（例如 syscall、load、unload、fork、exec、signal）仍未实现。
- 本轮未新增项目级 decision，因此未修改 `docs/ai/decision.md`。
