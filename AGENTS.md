# AGENTS.md

本仓库是 `gdb-agent`，一个面向 AI Agent 的 GDB/MI 调试执行层和证据管理工具。
后续 Agent 修改代码前，应先阅读 `final_feature.md`、`design.md` 和 `docs/ai/`。

## 项目定位

- AI Agent 负责分析、提出假设、选择动作和给出最终结论。
- 本工具负责稳定执行 GDB 操作、管理 live session、保存原始证据、生成摘要和报告。
- 工具不要自动宣称根因；最终判断必须来自 Agent 的 inference/conclusion。

## 核心约束

- 使用 C++20 和 CMake。
- GDB 接入使用 GDB/MI，不使用 PTY。
- 默认暴露高层 action，不要求 Agent 直接输入 MI。
- `raw_mi` 只能作为高级 escape hatch，调用时必须显式标记风险。
- 保留原始 evidence；summary 是有损、低噪声视图，不能替代 raw。
- `session_snapshot.json` 和 `session_summary.json` 不是 live GDB 会话恢复文件。
- 重启后的恢复方式是 replay 高层 action，不是恢复旧 GDB 进程。
- inferior stdin 非交互式，默认 `/dev/null`；stdout/stderr 进入 assets 并作为 evidence 捕获。

## 常用命令

```bash
cmake -S . -B build
cmake --build build
./build/gdb-agent check examples/segfault_task.md
./build/gdb-agent serve examples/segfault_task.md --out report.md --assets report.assets
```

daemon flow:

```bash
./build/gdb-agent daemon --socket /tmp/gdb-agent.sock
./build/gdb-agent create examples/segfault_task.md --socket /tmp/gdb-agent.sock --session S1 --out report.md --assets report.assets
./build/gdb-agent action S1 '{"action":"backtrace"}' --socket /tmp/gdb-agent.sock
./build/gdb-agent finish S1 --socket /tmp/gdb-agent.sock --out report.md
```

## 代码结构

- `src/cli.cpp`: CLI、daemon flow、action dispatch、replay/probe/hypothesis glue。
- `src/task/`: Markdown task file 解析和校验。
- `src/gdb/`: GDB process、MI session、MI utility。
- `src/evidence/`: evidence store、summary/raw/view/index 写入。
- `src/workflow/`: crash workflow、session outcome、light/core evidence collection。
- `src/report/`: Markdown report 输出。
- `src/common/`: JSON 和字符串工具。
- `docs/`: 面向用户和 Agent 的使用文档。
- `docs/ai/`: 当前目标、设计决策、进度记录。
- `examples/`: 最小示例和 replay 示例。

## 修改守则

- 新增或修改 action 时，同步更新 `docs/agent_actions.md`。
- 修改 task 字段时，同步更新 `docs/task_format.md`。
- 修改 evidence schema、summary、raw 文件布局时，同步更新 `docs/evidence_model.md`。
- 影响项目目标、范围或核心决策时，同步更新 `docs/ai/current_goal.md` 和
  `docs/ai/decision.md`。
- 进度变化明显时，同步更新 `docs/ai/progress.md`。
- 尽量保持 action 输出为稳定 JSON，错误也要记录 `ToolError` evidence。
- 不要把 unrelated refactor 和功能变更混在一起。

## 验证建议

至少运行：

```bash
cmake --build build
./build/gdb-agent check examples/segfault_task.md
```

如果改动涉及 GDB 行为、evidence、report、replay、probe 或 hypothesis，应额外运行示例
session，并检查生成的 report 和 assets 目录。


## 触发点

当用户说

`开始新一轮任务流`

这句话意味着:

1. 阅读下面的文件:
   - `docs/ai/current_goal.md`
   - `docs/ai/decision_log.md`
   - `docs/ai/progress.md`
   - `docs/ai/next_cli_task.md`

2. 参照下面的任务，开始执行任务:
   - `docs/ai/next_cli_task.md`

3. 不要做下面的事:
   - implement future suggestions
   - expand scope
   - perform unrelated cleanup
   - change the stage goal unless explicitly requested

4. 任务结束之后:
   - 执行 `build/test` 如果代码改变了
   - 更新文件 `docs/ai/progress.md`
   - 追加文件 `docs/ai/decision_log.md`，如果任何有新的决策诞生
   - 新键 `docs/ai/handoff.md` 并补充实际完成的工作
   - 做一次git commit & push，包括docs/ai所有改动的文件
   - 停止运行
