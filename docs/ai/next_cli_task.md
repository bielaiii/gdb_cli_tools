# Next CLI Task

## 目标

把 README 中的最小 demo 进一步固化为可自动验证的 smoke test 或示例输出检查。

## 范围

- 基于 `examples/segfault_task.md` 和 `examples/segfault.cpp`。
- 优先覆盖不依赖 GDB 的路径：
  - `cmake -S . -B build`
  - `cmake --build build`
  - `./build/gdb-agent check examples/segfault_task.md`
- 如果当前环境安装了 `gdb`，再补充 live session/report/assets 的端到端验证。

## 不做

- 不扩展 action 集合。
- 不改 evidence schema。
- 不重写 README 的项目定位。
- 不引入与 demo 验证无关的重构。

## 完成标准

- README 中的 demo 命令和实际 CLI 行为一致。
- 最小验证步骤可以被后续 Agent 一眼复现。
- 如果无法运行 GDB live session，要在 handoff 中明确记录原因。
