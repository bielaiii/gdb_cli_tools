# AGENTS.md

本仓库是 `gdb-agent`，一个面向 AI Agent 的 GDB/MI 调试执行层和证据管理工具。

后续 Agent 开始修改前，先阅读：

- `final_feature.md`
- `design.md`
- `docs/ai/current_goal.md`
- `docs/ai/decision.md`
- `docs/ai/progress.md`
- `docs/ai/handoff.md`（如果存在）
- `docs/ai/next_cli_task.md`（当执行新一轮任务流时）

`docs/ai/decision_log.md` 当前不存在；项目决策记录使用 `docs/ai/decision.md`。

## 项目定位

- AI Agent 负责分析问题、提出假设、选择调试动作、解释证据并给出最终结论。
- 本工具负责稳定执行 GDB 操作、管理 live session、保存原始 evidence、生成低噪声
  summary、维护 replay/probe/hypothesis 状态并输出报告草稿。
- 工具不要自动宣称根因；最终判断必须来自 Agent 的 inference/conclusion。

## 核心约束

- 使用 C++20 和 CMake。
- GDB 接入只使用 GDB/MI，不使用 PTY。
- 默认暴露高层 action，不要求 Agent 直接输入 MI。
- `raw_mi` 只能作为高级 escape hatch，调用时必须显式标记 `risk: "advanced"`。
- 原始 evidence 必须保留；summary 是有损、低噪声视图，不能替代 raw。
- `session_snapshot.json` 和 `session_summary.json` 不是 live GDB 会话恢复文件。
- 重启后的恢复方式是 replay 高层 action，不是恢复旧 GDB 进程。
- inferior stdin 非交互式，默认 `/dev/null`；stdout/stderr 进入 assets 并作为 evidence 捕获。
- 不要把 unrelated refactor 和功能变更混在一起。

## 文档约定

- 中文文档是默认入口：
  - `docs/agent_actions.md`
  - `docs/evidence_model.md`
  - `docs/task_format.md`
- 英文版保留为：
  - `docs/agent_actions.en.md`
  - `docs/evidence_model.en.md`
  - `docs/task_format.en.md`
- 新增或修改 action 时，同步更新 `docs/agent_actions.md`。
- 修改 task 字段时，同步更新 `docs/task_format.md`。
- 修改 evidence schema、summary、raw 文件布局时，同步更新 `docs/evidence_model.md`。
- 影响项目目标、范围或核心决策时，同步更新 `docs/ai/current_goal.md` 和
  `docs/ai/decision.md`。
- 进度变化明显时，同步更新 `docs/ai/progress.md`。
- 每轮任务结束时更新 `docs/ai/handoff.md`，记录实际完成的工作、验证结果和限制。

## 常用命令

```bash
cmake -S . -B build
cmake --build build
./build/gdb-agent check examples/segfault_task.md
./build/gdb-agent serve examples/segfault_task.md --out report.md --assets report.assets
```

Daemon flow:

```bash
./build/gdb-agent daemon --socket /tmp/gdb-agent.sock
./build/gdb-agent create examples/segfault_task.md --socket /tmp/gdb-agent.sock --session S1 --out report.md --assets report.assets
./build/gdb-agent action S1 '{"action":"backtrace"}' --socket /tmp/gdb-agent.sock
./build/gdb-agent finish S1 --socket /tmp/gdb-agent.sock --out report.md
```

当前开发环境可能没有安装 `gdb`。`check` 不需要 GDB；`serve`、daemon live session、
raw MI evidence 和完整 report/assets 生成需要可用的 GDB。

## 代码结构

- `src/cli.cpp`: CLI、daemon flow、action dispatch、replay/probe/hypothesis glue。
- `src/task/`: Markdown task file 解析和校验。
- `src/gdb/`: GDB process、MI session、MI utility。
- `src/evidence/`: evidence store、summary/raw/view/index 写入。
- `src/workflow/`: crash workflow、session outcome、light/core evidence collection。
- `src/report/`: Markdown report 输出。
- `src/common/`: JSON 和字符串工具。
- `docs/`: 面向用户和 Agent 的使用文档。
- `docs/ai/`: 当前目标、设计决策、进度和交接记录。
- `examples/`: 最小示例、replay 示例和示例定位报告。

## 验证建议

至少运行：

```bash
cmake --build build
./build/gdb-agent check examples/segfault_task.md
```

如果改动涉及 GDB 行为、evidence、report、replay、probe 或 hypothesis，应额外运行示例
session，并检查生成的 report 和 assets 目录。若当前机器没有 `gdb`，在 handoff 中明确记录
未运行完整 live session 的原因。

## 触发点

当用户说：

```text
开始新一轮任务流
```

这句话意味着：

1. 先阅读：
   - `final_feature.md`
   - `design.md`
   - `docs/ai/current_goal.md`
   - `docs/ai/decision.md`
   - `docs/ai/progress.md`
   - `docs/ai/handoff.md`（如果存在）
   - `docs/ai/next_cli_task.md`

2. 只执行 `docs/ai/next_cli_task.md` 中指定的任务。

3. 不要做下面的事：
   - implement future suggestions
   - expand scope
   - perform unrelated cleanup
   - change the stage goal unless explicitly requested
   - commit 或 push，除非用户明确要求

4. 任务结束之后：
   - 如果代码改变了，执行 build/test。
   - 更新 `docs/ai/progress.md`。
   - 如果产生新的项目级决策，追加或更新 `docs/ai/decision.md`。
   - 覆写 `docs/ai/handoff.md`，补充实际完成的工作、验证结果和遗留限制。
   - 更新progress.md
   - 停止运行，等待用户下一步指令。
