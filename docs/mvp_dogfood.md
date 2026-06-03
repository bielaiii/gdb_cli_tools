# MVP Dogfood: Segfault Example

本文件记录如何用当前 MVP 对仓库内最小崩溃示例完成一轮 dogfood。它不提交 generated assets，
避免不同 GDB 版本造成仓库噪声。

## 示例目标

使用：

```text
examples/segfault_task.md
examples/segfault.cpp
```

示例程序中，`main` 创建空指针 `Session *session = nullptr`，随后传入调用链，最终在
`read_session_value` 中访问 `session->value` 并触发崩溃。

## 推荐命令序列

构建并校验 task：

```bash
cmake -S . -B build
cmake --build build
./build/gdb-agent check examples/segfault_task.md
```

启动 daemon：

```bash
./build/gdb-agent daemon --socket /tmp/gdb-agent.sock
```

创建 session：

```bash
./build/gdb-agent create examples/segfault_task.md \
  --socket /tmp/gdb-agent.sock \
  --session S1 \
  --out report.md \
  --assets report.assets
```

执行第一轮证据动作：

```bash
./build/gdb-agent action S1 '{"action":"backtrace"}' --socket /tmp/gdb-agent.sock
./build/gdb-agent action S1 '{"action":"threads"}' --socket /tmp/gdb-agent.sock
./build/gdb-agent action S1 '{"action":"frame_select","frame":0}' --socket /tmp/gdb-agent.sock
./build/gdb-agent action S1 '{"action":"locals"}' --socket /tmp/gdb-agent.sock
./build/gdb-agent action S1 '{"action":"evaluate","expression":"session"}' --socket /tmp/gdb-agent.sock
```

记录 hypothesis：

```bash
./build/gdb-agent action S1 \
  '{"action":"hypothesis_create","id":"H-null-session","title":"session is null before dereference","description":"Check whether the crashing frame receives a null session pointer."}' \
  --socket /tmp/gdb-agent.sock
./build/gdb-agent action S1 \
  '{"action":"hypothesis_check","hypothesis":"H-null-session","description":"session argument is null","expression":"session","assertion":"is_null"}' \
  --socket /tmp/gdb-agent.sock
./build/gdb-agent action S1 \
  '{"action":"hypothesis_conclude","hypothesis":"H-null-session","inference":"Backtrace reaches read_session_value, and the session expression is null in the crashing frame.","conclusion":"Agent concludes this example crashes by dereferencing a null Session pointer."}' \
  --socket /tmp/gdb-agent.sock
```

Finish：

```bash
./build/gdb-agent finish S1 --socket /tmp/gdb-agent.sock --out report.md
```

Shutdown daemon when done:

```bash
./build/gdb-agent shutdown --socket /tmp/gdb-agent.sock
```

## Evidence 链路

关键位置：

```text
report.md
report.assets/evidence/index.json
report.assets/evidence/*.md
report.assets/evidence/summary/*.summary.txt
report.assets/evidence/raw/*.mi.txt
report.assets/hypotheses/index.json
report.assets/logs/session.mi.raw.log
report.assets/session_summary.json
report.assets/session_snapshot.json
```

建议阅读顺序：

1. `report.md`：快速扫 Task、Session Summary、Evidence Summary、Hypotheses 和 Limitations。
2. `report.assets/evidence/index.json`：确认 evidence id、kind、raw/view/summary 路径和 raw hash。
3. `report.assets/evidence/*.md`：阅读 human-readable view。
4. `report.assets/evidence/summary/*.summary.txt`：快速读取低噪声 summary。
5. `report.assets/evidence/raw/*.mi.txt`：审计关键证据或解析歧义。
6. `report.assets/hypotheses/index.json`：查看 tool check status、observed、evidence 和 error evidence。

## 预期观察

工具观察通常包括：

- Inferior stop reason 是 crash 或 signal stop。
- Backtrace 显示崩溃路径进入 `read_session_value`。
- `evaluate session` 或 hypothesis check 显示当前 frame 中 `session` 为空。
- Evidence index 中每条 evidence 都有 raw、summary、view 和 raw hash。
- Hypothesis check 的 `status` 只是 assertion result，不是根因结论。

## Agent 推理边界

工具可以记录：

- GDB 停止信息。
- frame、thread、locals、expression 输出。
- evidence id、raw hash 和 summary。
- hypothesis check status。

Agent 负责推理：

- 哪个调用路径导致空指针被传入。
- 这是否是示例问题的根因。
- 是否需要继续检查调用方、输入、状态机或更早的 mutation。
- 最终结论如何表述。

在这个示例中，合理的 Agent conclusion 可以是：

```text
The evidence shows the crashing frame dereferences a null Session pointer. The tool observed the
null value and preserved raw GDB evidence; the conclusion that this is the example's crash cause is
the Agent's inference.
```

## 不提交 Generated Assets

Dogfood 可以在本地生成 `report.md` 和 `report.assets/`，但本仓库不要求提交这些 generated assets。
原因：

- GDB 版本、路径和系统环境会影响 raw MI。
- Evidence raw hash 会随环境变化。
- 文档记录流程即可，回归由 smoke/CTest 负责。

## 常见失败处理

- 如果 `create` 失败，先运行 `./build/gdb-agent check examples/segfault_task.md`。
- 如果 live session 无法启动，确认 Linux 环境中 `gdb` 可用。
- 如果 action 返回 `ok:false`，优先看 response 的 `error`、`evidence` 和 `command_evidence`。
- 如果 summary 过短或有歧义，打开对应 raw MI。
