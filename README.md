# gdb-agent

`gdb-agent` 是一个面向 AI Agent 的 GDB/MI 调试执行层和证据管理工具。它的目标不是替
AI 自动宣称根因，而是把调试过程变成可执行、可重复、可审计的证据流。

AI Agent 负责提出假设、选择动作、解释证据并给出最终结论；`gdb-agent` 负责稳定执行
GDB 操作、管理 live session、保存原始 evidence、生成低噪声 summary，并输出报告草稿。

## 项目目标

- 用 Markdown task file 描述被调试程序、参数、工作目录和 core dump。
- 通过 GDB/MI 管理 live debugging session，不使用 PTY。
- 向 Agent 暴露高层 action，避免默认直接输入底层 MI 命令。
- 保存 raw evidence，同时生成面向 Agent 的低噪声 summary。
- 支持 replay、breakpoint/watchpoint metadata、on-hit action 和 hypothesis workflow。
- 在 finish 时生成 Markdown report、session snapshot、session summary 和 evidence index。

## 当前功能

- CMake + C++20 构建。
- Markdown task file 解析和校验。
- Run Mode 和最小 Core Dump Mode。
- `check`、`serve`、daemon/create/action/status/finish/close/shutdown CLI flow。
- 高层 action，包括：
  - `backtrace`
  - `locals`
  - `registers`
  - `threads`
  - `args_info`
  - `frame_select`
  - `evaluate`
  - `breakpoint_set`
  - `watchpoint_set`
  - `probe_list`
  - `probe_enable`
  - `probe_disable`
  - `probe_delete`
  - `continue`
  - `run`
  - `save_action`
  - `replay`
  - `hypothesis_create`
  - `hypothesis_check`
  - `hypothesis_conclude`
  - `raw_mi`
  - `finish_session`
- Evidence store，包含 Markdown view、raw MI、summary、index 和 raw hash。
- Replay JSONL 与结构化 replay plan。
- 示例程序、示例 task 和示例定位报告。

## 构建

```bash
cmake -S . -B build
cmake --build build
```

## 最小 Demo

仓库自带一个会崩溃的最小程序：

```text
examples/segfault.cpp
```

它的 task file 是：

```text
examples/segfault_task.md
```

先构建，然后校验 task file：

```bash
cmake -S . -B build
cmake --build build
./build/gdb-agent check examples/segfault_task.md
```

预期输出类似：

```text
ok
executable: .../build/segfault
working directory: .../gdb_cli_tools
args:
argv:
stdin: /dev/null
run timeout ms: 30000
```

也可以运行 smoke test，一次性完成构建并校验上述输出：

```bash
./scripts/smoke_segfault_demo.sh
```

已经按 `cmake -S . -B build` 配置并构建后，也可以通过 CTest 复跑同一个输出检查：

```bash
ctest --test-dir build --output-on-failure
```

这个 demo 足够小：`main` 创建 `Session *session = nullptr`，然后一路传入
`read_session_value`，最终访问 `session->value`。对应的源码层面定位报告在：

```text
examples/segfault_report.md
```

## 生成调试报告

如果当前机器安装了 `gdb`，可以运行 live session 并生成 report/assets：

```bash
./build/gdb-agent serve examples/segfault_task.md \
  --out report.md \
  --assets report.assets
```

生成的 report 会引用 evidence id，并把 raw MI、summary、session log、snapshot 和 index
放入 assets 目录。Summary 是有损视图，审计时应回到 raw evidence。

## Daemon Flow

面向多轮 Agent 调试时，可以用 daemon 保持同一个 live session：

```bash
./build/gdb-agent daemon --socket /tmp/gdb-agent.sock
./build/gdb-agent create examples/segfault_task.md \
  --socket /tmp/gdb-agent.sock \
  --session S1 \
  --out report.md \
  --assets report.assets
./build/gdb-agent action S1 '{"action":"backtrace"}' --socket /tmp/gdb-agent.sock
./build/gdb-agent finish S1 --socket /tmp/gdb-agent.sock --out report.md
```

`finish` 表示 Agent 已经完成当前定位工作并要求写最终报告。`close` 只关闭 session，不等价于
问题已经定位完成。

## 文档入口

- `docs/task_format.md`：task file 格式。
- `docs/agent_actions.md`：Agent 可调用 action。
- `docs/evidence_model.md`：evidence、raw、summary、snapshot 和 session log 的关系。
- `examples/segfault_report.md`：最小示例定位报告。
- `docs/ai/`：当前目标、设计决策、进度和交接记录。

英文版文档保留为 `docs/*.en.md`。

## 设计边界

- 目标运行平台是 Linux；macOS 可用于非 live debugging 的构建和文档校验。
- 不支持 PTY。
- 不支持交互式 inferior stdin；默认 stdin 是 `/dev/null`。
- `session_snapshot.json` 和 `session_summary.json` 不是 live GDB 会话恢复文件。
- 重启后的恢复方式是 replay 高层 action，不是恢复旧 GDB 进程。
- `raw_mi` 是高级 escape hatch，必须显式标记风险。
- 工具记录观察和证据；最终根因判断属于 AI Agent。
