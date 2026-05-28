# Handoff

日期：2026-05-29

## 本轮完成

- 新增递归 MI value parser，覆盖 const string、tuple、list、result payload 和 stream record
  分类。
- 增强 C++ summary sanitizer，覆盖常见 `std::basic_string`/`std::__cxx11::basic_string`
  到 `std::string` 的归一化、allocator 噪声压缩和模板空格归一化。
- 增强 backtrace/thread summary，输出更稳定的 frame/function/file:line 和线程行。
- Evidence index 和 Markdown view 增加 raw MI audit metadata，包括 sequence、token、
  record kind、result/async class、stream type。
- 新增不依赖 GDB 的 `mi_summary_tests` fixture，并接入 CTest。
- 更新 `docs/evidence_model.md`、`docs/evidence_model.en.md` 和 `docs/ai/progress.md`。

## 验证

- `cmake -S . -B build` 通过。
- `cmake --build build` 通过。
- `./build/mi_summary_tests` 通过。
- `./build/gdb-agent check examples/segfault_task.md` 通过。
- `./scripts/smoke_segfault_demo.sh` 通过。
- `./scripts/smoke_daemon_action_flow.sh` 在当前 macOS 环境按平台口径输出
  `skip: daemon/action live smoke requires Linux + GDB` 并返回成功。
- `ctest --test-dir build --output-on-failure` 通过，包含 `segfault_demo_check`、
  `daemon_action_flow` 和 `mi_summary_tests`。

## 限制和注意事项

- 仓库约定文件名是 `docs/ai/next_cli_task.md`，不是复数 `next_cli_tasks.md`。
- 当前环境是 macOS，不执行 live GDB session 作为验收；Linux + GDB 环境下仍需用真实 raw
  输出继续校准 summary。
- 本轮修改 evidence index/view metadata，已同步更新 evidence model 中英文文档。
- 本轮没有新增项目级设计决策，因此未更新 `docs/ai/decision.md`。
