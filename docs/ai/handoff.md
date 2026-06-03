# Handoff

日期：2026-06-03

## 本轮完成

- 按 `docs/ai/next_cli_task.md` 执行 MVP 文档收敛任务。
- 本轮未新增调试功能、未修改源码行为、未改测试逻辑或 build 配置。
- 更新 `README.md`：
  - 增加 MVP quickstart、MVP acceptance、known limitations 和 dogfood 文档入口。
  - 补齐当前 action 列表中的 `catchpoint_set`。
- 新增 `docs/mvp_acceptance.md`：
  - 明确 Linux 目标平台。
  - 记录构建、task check、daemon flow、Run Mode、Core Dump Mode、evidence、report、
    replay/probe/hypothesis 和 CTest 回归的 MVP 验收条件。
- 新增 `docs/mvp_quickstart.md`：
  - 面向 Agent 记录最小 task、check、daemon create、第一轮 action、evidence 查看、
    hypothesis workflow、replay/probe 和 finish 流程。
- 新增 `docs/known_limitations.md`：
  - 集中说明 Linux/GDB、非 PTY、非交互 stdin、snapshot/replay、Core Dump Mode、raw MI、
    summary/report、hypothesis 和 assertion 支持范围等限制。
- 新增 `docs/mvp_dogfood.md`：
  - 记录如何用 `examples/segfault_task.md` 完成一轮最小 dogfood。
  - 明确哪些属于工具观察，哪些属于 Agent inference。
  - 明确不提交 generated assets，避免 GDB 版本和路径差异带来仓库噪声。
- 更新 `docs/ai/progress.md`，记录本轮 MVP documentation convergence。

## 验证

- `git diff --check`

本轮是文档收敛任务，没有新增脚本、没有修改源码功能，也没有修改测试逻辑，因此未运行完整 build/test。
文档中新增的命令与 README、CTest 和现有 smoke 入口保持一致。

## 限制和注意事项

- 本轮没有新增 `scripts/mvp_acceptance.sh`；MVP 验收命令记录在 `docs/mvp_acceptance.md`。
- 仍未实现浮点 assertion、`changed` 或跨 check 历史比较。
- 仍未扩展 catchpoint event，没有引入 PTY 或交互式 stdin，也没有修改 raw evidence/schema 布局。
- 当前 `docs/ai/next_cli_task.md` 已被纳入本轮任务记录；下一轮需要先写入新的具体任务，否则会继续指向已完成范围。
