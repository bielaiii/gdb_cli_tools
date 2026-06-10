# Handoff

日期：2026-06-10

## 本轮完成

- 按用户要求将 `docs/ai/next_cli_task.md` 更新为
  `MI summary hardening with real Linux GDB fixtures` 任务，并执行该任务。
- 新增 `examples/mi_summary_fixture.cpp`：
  - 程序启动后先 `SIGTRAP`，让 daemon `create` 后稳定停住。
  - 启动 worker thread，便于真实 `info threads` 覆盖当前线程和普通线程。
  - 提供 noinline 调用链 `mi_summary_entry` -> `mi_summary_middle` -> `mi_summary_leaf` ->
    `mi_summary_observe_here`，用于 backtrace summary 断言。
  - 在稳定用户函数停点保留 STL/template 数据结构，用于验证真实 GDB 输出下的 summary 降噪。
- 更新 `CMakeLists.txt`：
  - 新增 `mi_summary_fixture` target。
  - 新增 CTest `mi_summary_live_flow`。
- 新增 `scripts/smoke_mi_summary_live.sh`：
  - Linux + GDB 下启动 daemon，创建 session，设置 breakpoint，continue 到稳定停点。
  - 执行并校验 `backtrace`、`threads`、`locals`、`frame_select` 和 `raw_mi`。
  - 从 response evidence id 反查 `assets/evidence/index.json`，校验 summary/view/raw 文件存在。
  - 校验 backtrace summary 的调用链、相对路径和绝对路径降噪。
  - 校验 threads summary 的当前线程、普通线程、LWP/thread identity 和 stopped frame。
  - 校验 raw MI result summary 和 evidence Markdown view 中的 Raw MI Audit record kind。
  - 校验真实 `ptype` 输出中的 STL 类型保持低噪声，不出现长 `std::__cxx11::basic_string...`
    和 `> >` spacing 回归。
- 更新 `docs/ai/progress.md`，记录本轮实际完成内容。

## 验证

- `cmake --build build`
- `scripts/smoke_mi_summary_live.sh`
- `./build/gdb-agent check examples/segfault_task.md`
- `./build/mi_summary_tests`
- `./build/type_sanitizer_tests`
- `./build/task_parser_tests`
- `ctest --test-dir build --output-on-failure`
  - 结果：13/13 tests passed。
  - 新增 `mi_summary_live_flow` 在当前 Linux + GDB 环境实际运行并通过。
- `git diff --check`

## 完成标准审计

- `docs/ai/next_cli_task.md` 已替换为本轮 MI summary live fixture 任务。
- 新增 `mi_summary_fixture` target。
- 新增 `scripts/smoke_mi_summary_live.sh`。
- 新增 CTest `mi_summary_live_flow`。
- live smoke 覆盖 backtrace、threads、locals/frame_select、raw_mi 和 raw audit metadata。
- 本轮没有改变用户可见 action JSON schema、response 字段语义、evidence schema、replay plan
  schema、report schema 或 raw evidence 文件布局。
- 本轮没有修改 summary/sanitizer 行为实现；现有实现已通过新增真实 Linux + GDB fixture 回归。

## 限制和注意事项

- 当前新增 fixture 覆盖的是本机 Linux + 当前 GDB 组合；不同 GDB 版本和真实项目 core 的输出差异
  仍可在后续继续补充 fixture。
- 本轮没有扩展 hypothesis assertion、没有新增 action、没有拆分 `src/cli.cpp`。
- 本轮未新增项目级 decision，因此没有更新 `docs/ai/decision.md`。
