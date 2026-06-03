# MVP Quickstart for Agents

本 playbook 面向新的 AI Agent。目标是用当前 MVP 完成一轮最小调试流程，并明确哪些内容是工具
观察，哪些内容必须由 Agent 自己推理。

## 1. 编写 Task File

最小 task file 使用 Markdown 三级标题：

```markdown
### problem

Program crashes with SIGSEGV.

### executable

build/segfault

### working directory

.

### args

### core dump
```

字段说明见 `docs/task_format.md`。注意：

- `args` 按 shell-like argv 解析，不要简单按空格理解。
- `stdin` 为空时默认 `/dev/null`。
- `core dump` 为空时是 Run Mode；非空时是 Core Dump Mode。

仓库内示例：

```text
examples/segfault_task.md
```

## 2. 构建并 Check

```bash
cmake -S . -B build
cmake --build build
./build/gdb-agent check examples/segfault_task.md
```

`check` 只校验 task 和路径，不启动 inferior，不需要 GDB live session。

## 3. 启动 Daemon

多轮 Agent 调试建议使用 daemon：

```bash
./build/gdb-agent daemon --socket /tmp/gdb-agent.sock
```

在另一个 shell 或后续 CLI 调用中创建 session：

```bash
./build/gdb-agent create examples/segfault_task.md \
  --socket /tmp/gdb-agent.sock \
  --session S1 \
  --out report.md \
  --assets report.assets
```

`create` 会启动 live session。Run Mode 会运行 inferior 直到 stop event、timeout interrupt 或退出；
Core Dump Mode 会加载 executable + core dump 并进入静态取证状态。

## 4. 第一轮建议动作

先获取低风险上下文：

```bash
./build/gdb-agent action S1 '{"action":"backtrace"}' --socket /tmp/gdb-agent.sock
./build/gdb-agent action S1 '{"action":"threads"}' --socket /tmp/gdb-agent.sock
./build/gdb-agent action S1 '{"action":"frame_select","frame":0}' --socket /tmp/gdb-agent.sock
./build/gdb-agent action S1 '{"action":"locals"}' --socket /tmp/gdb-agent.sock
```

按证据继续检查表达式：

```bash
./build/gdb-agent action S1 '{"action":"evaluate","expression":"session"}' --socket /tmp/gdb-agent.sock
```

常见第一轮策略：

- crash 后先看 `backtrace` 和当前 frame。
- 多线程问题先看 `threads`，必要时 `frame_select`。
- 指针/状态字段用 `evaluate` 或 hypothesis check。
- 如果 action 返回 `ok:false`，先看 `error`、`evidence` 和 `command_evidence`。

## 5. 理解 Action Response

常见 response 字段：

- `ok`：action 是否被工具成功执行。
- `action`：执行的高层 action 名称。
- `evidence`：本 action 新产生的 evidence id，常用于 report 和后续推理引用。
- `command_evidence`：底层 GDB command 的原始 evidence id，仅在 GDB command error 等路径出现。
- `error`：工具或 GDB 拒绝时的短错误。

`ok:false` 不等于定位失败。它通常表示该动作在当前状态不可用、输入缺字段、GDB 拒绝命令或超时。
这些失败本身也会被记录为 `ToolError` evidence，供 Agent 决定下一步。

## 6. 查看 Evidence

Assets 中的 evidence 入口：

```text
report.assets/evidence/index.json
report.assets/evidence/E0002.backtrace.md
report.assets/evidence/summary/E0002.backtrace.summary.txt
report.assets/evidence/raw/E0002.backtrace.mi.txt
```

使用原则：

- Raw 是审计来源。
- Summary 是有损、低噪声视图，适合快速阅读，但不能替代 raw。
- Markdown view 是 human-readable 入口。
- Report 引用 evidence id，Agent 的推理也应引用 evidence id。
- `session_snapshot.json` 和 `session_summary.json` 是报告 artifacts，不是 live GDB 恢复文件。

什么情况下打开 raw MI：

- summary 被截断。
- GDB 输出解析看起来有歧义。
- action 返回 `ok:false` 且短错误不足以判断。
- 需要审计 ToolError 或 command evidence。
- 最终结论依赖关键地址、寄存器、frame 或对象状态。

## 7. 使用 Hypothesis Workflow

Hypothesis workflow 用于把 Agent 的猜想、检查和结论分开记录。

创建 hypothesis：

```bash
./build/gdb-agent action S1 \
  '{"action":"hypothesis_create","id":"H-null-session","title":"session is null before dereference"}' \
  --socket /tmp/gdb-agent.sock
```

执行 check：

```bash
./build/gdb-agent action S1 \
  '{"action":"hypothesis_check","hypothesis":"H-null-session","description":"session argument is null","expression":"session","assertion":"is_null"}' \
  --socket /tmp/gdb-agent.sock
```

记录 Agent 推理和结论：

```bash
./build/gdb-agent action S1 \
  '{"action":"hypothesis_conclude","hypothesis":"H-null-session","inference":"The check shows session is null in the crashing frame.","conclusion":"Agent concludes the crash path dereferences a null session."}' \
  --socket /tmp/gdb-agent.sock
```

重要边界：

- `hypothesis_check` 的 `passed`/`failed`/`unknown` 是工具级 assertion 结果。
- `unknown` 表示工具无法判断该 check，不支持也不反驳 hypothesis。
- 最终根因结论必须写在 Agent inference/conclusion 中。

## 8. 使用 Replay 和 Probe

保存并重放高层 action：

```bash
./build/gdb-agent save-action S1 action.json --name repro-checks --failure-policy stop_on_error --socket /tmp/gdb-agent.sock
./build/gdb-agent replay S1 repro-checks --socket /tmp/gdb-agent.sock
```

设置带 metadata 的 breakpoint：

```bash
./build/gdb-agent action S1 \
  '{"action":"breakpoint_set","location":"examples/segfault.cpp:14","comment":"stop before dereference","purpose":"verify null session"}' \
  --socket /tmp/gdb-agent.sock
```

Probe metadata 是工具运行态；最终 `report.assets/probes.json` 是 finish-time artifact，不是恢复文件。

## 9. Finish

当 Agent 已经完成本轮定位，调用 finish：

```bash
./build/gdb-agent finish S1 \
  --socket /tmp/gdb-agent.sock \
  --out report.md \
  --agent-inference inference.md \
  --final-conclusion conclusion.md
```

`finish` 会写 report、session summary、snapshot、evidence index，并关闭该 session。`close` 只关闭
session，不表示定位完成，也不等价于写最终报告。

## 10. 最小判断流程

一轮最小 Agent 调试流程可以按下面顺序执行：

1. `check` task。
2. `create` session。
3. `backtrace` / `threads` / `locals` / `evaluate`。
4. 根据 evidence 提出 hypothesis。
5. `hypothesis_check` 记录工具观察。
6. Agent 解释 evidence，并写 inference/conclusion。
7. `finish` 生成 report。

工具负责证据和执行；Agent 负责最终判断。
