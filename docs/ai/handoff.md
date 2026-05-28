# Handoff

日期：2026-05-28

## 本轮完成

- 新增 `scripts/smoke_segfault_demo.sh`，用于一键执行 README 最小 demo 的 configure、
  build 和 `gdb-agent check` 输出校验。
- 在 `CMakeLists.txt` 中启用 CTest，并新增 `segfault_demo_check` 测试，在已构建的
  `build/` 目录上复跑同一个输出检查。
- 更新 `README.md` 的最小 Demo 章节，补充 smoke script 和 CTest 命令。
- 更新 `docs/ai/progress.md` 记录 demo smoke 的当前状态。

## 验证

- `cmake -S . -B build` 通过。
- `cmake --build build` 通过。
- `./build/gdb-agent check examples/segfault_task.md` 通过，输出包含 `ok`、示例
  executable、working directory、`argv:`、`stdin: /dev/null` 和 `run timeout ms: 30000`。
- `./scripts/smoke_segfault_demo.sh` 通过。
- `ctest --test-dir build --output-on-failure` 通过，`segfault_demo_check` 通过。
- 已尝试运行 live session/report/assets：
  `./build/gdb-agent serve examples/segfault_task.md --out /private/tmp/gdb-agent-smoke-report.md --assets /private/tmp/gdb-agent-smoke.assets`。
  命令生成 report/assets，但进程返回 1，session state 为 `error`，初始 run evidence 为
  `Don't know how to run.  Try "help target".`

## 限制和注意事项

- 当前机器安装了 GNU gdb 17.2，但该 GDB 显示 `--target=x86_64-apple-darwin20`，而
  `build/segfault` 是 Mach-O arm64 executable。因此本轮没有得到成功的 SIGSEGV live
  session 端到端验证。
- 本轮没有扩展 action、修改 evidence schema 或调整调试行为。
- 本轮没有新增项目级设计决策，因此未更新 `docs/ai/decision.md`。
