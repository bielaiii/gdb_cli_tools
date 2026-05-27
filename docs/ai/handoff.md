# Handoff

日期：2026-05-27

## 本轮完成

- 按 `docs/ai/next_cli_task.md` 要求，为 `agent_actions`、`evidence_model` 和
  `task_format` 生成了中文版文档。
- 保留现有英文文档为 `.en.md` 文件：
  - `docs/agent_actions.en.md`
  - `docs/evidence_model.en.md`
  - `docs/task_format.en.md`
- 新增 `examples/segfault_report.md`，提供 `examples/segfault.cpp` 的定位报告示例。
- 为了让 `examples/*_report.md` 能作为示例文件进入仓库，给 `.gitignore` 增加了对应例外。
- 修复 `src/cli.cpp` 中文件 mtime 输出在 macOS libc++ 下的构建错误。

## 验证

- `cmake -S . -B build` 通过。
- `cmake --build build` 通过。
- `./build/gdb-agent check examples/segfault_task.md` 通过。

## 限制和注意事项

- 当前环境没有安装 `gdb`，因此无法运行完整的 `gdb-agent serve` 来生成 raw MI evidence、
  session snapshot 和 session summary。
- `examples/segfault_report.md` 是源码层面的示例定位报告，不伪造 GDB/MI evidence。
- 触发点要求读取的 `docs/ai/decision_log.md` 不存在；当前已有决策记录文件是
  `docs/ai/decision.md`。本轮没有新增项目级设计决策，因此未追加决策记录。
