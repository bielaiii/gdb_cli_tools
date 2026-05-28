# Handoff

日期：2026-05-29

## 本轮完成

- 根据最新要求，更新 `docs/ai/next_cli_task.md`。
- 下一轮任务改为强化 summary 和 MI 解析能力，包含：
  - 更完整的 MI value parser。
  - C++ 类型 sanitizer。
  - thread/backtrace summarizer。
  - raw MI audit metadata 增强。
- 更新 `docs/ai/progress.md` 记录下一轮任务口径。

## 验证

- 本轮只更新任务和进度文档，没有改 C++ 代码；未重新运行 build/test。

## 限制和注意事项

- 仓库约定文件名是 `docs/ai/next_cli_task.md`，不是复数 `next_cli_tasks.md`。
- 下一轮如果修改 evidence index/session metadata schema，需要同步更新
  `docs/evidence_model.md` 和 `docs/evidence_model.en.md`。
- 下一轮应优先增加不依赖 GDB 的 parser/sanitizer/summarizer fixture 测试。
- 本轮没有新增项目级设计决策，因此未更新 `docs/ai/decision.md`。
