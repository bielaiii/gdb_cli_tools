# GDB Agent 工具修订设计方案

## 1. 设计定位

本工具是一个面向 AI Agent 的 GDB 交互、会话管理与证据管理工具。它提供稳定的调试执行层，帮助 AI Agent 持续提出假设、执行检查、收集证据、更新判断，直到 AI Agent 明确确认问题已经定位完成。

工具不承担“自动根因分析器”的角色。它负责可控执行、状态维护、证据保存、结构化摘要和报告草稿；AI Agent 负责分析、提出假设、选择下一步动作和给出最终结论。

职责边界：

| 角色 | 职责 |
| --- | --- |
| AI Agent | 分析问题、提出假设、选择调试动作、判断结论、确认问题是否定位完成 |
| 本工具 | 管理 GDB 会话、执行高层调试动作、保存原始证据、生成结构化摘要、维护可重放操作、生成报告草稿 |
| GDB | 调试可执行文件或 core dump，执行底层调试命令 |

核心要求：

1. 放弃 PTY，只使用 GDB/MI 和非交互式 I/O。
2. 使用 C++20 coroutine，耗时操作异步完成。
3. 使用 CMake 构建。
4. 支持 Run 模式和 Core Dump 模式。
5. 支持持续对话式 session，直到 AI Agent 确认问题定位完毕。
6. 支持应用程序重复启动，并能保存特定操作，在重启后一次性重放。
7. 对 AI 暴露高层动作接口，默认不要求 AI 直接输入 MI 命令。
8. 支持断点、观察点、条件断点和命中后的自动动作。
9. 保存原始证据，并生成面向 AI 的低噪声结构化摘要。
10. 支持“假设验证”工作流，但工具结论和 AI 结论必须分离。
11. 提供清晰的数据结构和动作文档，供 AI Agent 使用前理解。

## 2. 总体设计结论

新版方案采用“Interactive Session Only + 高层动作 API + 证据优先”的架构。

唯一对外运行形态：

| 形态 | 说明 |
| --- | --- |
| Interactive Session Mode | 启动长驻本地 session，AI 多轮发送高层动作，直到确认完成 |

不再提供独立 Batch Mode。原因是本项目的目标不是一次性生成调试报告，而是让 AI Agent 与 GDB 持续协作定位问题。一次性默认取证仍然有价值，但它应作为 interactive session 启动后的 `initial_actions` 或 `replay_plan` 执行，而不是成为另一套产品模式。

两种调试对象：

| 模式 | 说明 |
| --- | --- |
| Run Mode | 启动 executable，等待崩溃、断点、观察点、超时中断或正常退出 |
| Core Dump Mode | 加载 executable 和 core dump，执行静态取证 |

默认对 AI 暴露的是高层动作，例如 `backtrace`、`evaluate`、`breakpoint_set`、`continue`、`hypothesis_check`。底层 GDB/MI 命令只作为高级 escape hatch，必须显式标记为 `raw_mi`。

## 3. 任务文件格式

AI 使用工具前提供一个 Markdown 任务文件。

### 3.1 基础字段

```markdown
### problem

当前服务偶发 segfault，怀疑异步回调访问已经释放的对象。

### executable

/home/xiang/project/build/server

### working directory

/home/xiang/project

### args

--config conf/dev.yaml --port 8080

### core dump

/home/xiang/project/core.12345
```

字段说明：

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `problem` | 是 | 问题描述，可以是多行 |
| `executable` | 是 | 被调试程序路径 |
| `working directory` | 是 | 目标程序工作目录 |
| `args` | 否 | 目标程序参数，允许为空 |
| `core dump` | 否 | core 文件路径，允许为空 |

模式判定：

| 条件 | 调试模式 |
| --- | --- |
| `core dump` 为空 | Run Mode |
| `core dump` 非空 | Core Dump Mode |

### 3.2 可选 I/O 字段

因为工具明确放弃 PTY，需要把 inferior I/O 设计清楚。

```markdown
### stdin

/home/xiang/project/input.txt

### env

ASAN_OPTIONS=abort_on_error=1
LOG_LEVEL=debug

### run timeout

30000
```

可选字段：

| 字段 | 说明 |
| --- | --- |
| `stdin` | 目标程序 stdin 输入文件。为空时默认 `/dev/null` |
| `env` | 目标程序环境变量，一行一个 `KEY=value` |
| `run timeout` | 目标程序运行 deadline，单位毫秒 |

MVP I/O 约束：

1. 默认 stdin 使用 `/dev/null`。
2. 支持从文件提供 stdin。
3. 不支持交互式 stdin。
4. inferior stdout/stderr 必须作为 `InferiorOutput` evidence 保存。
5. 报告必须记录 I/O 配置，并说明非 PTY 可能影响 `isatty` 相关行为。

### 3.3 Args 解析

`args` 必须按 shell-like argv 解析，不允许简单按空格切分。

```markdown
### args

--name "hello world" --path /tmp/a\ b
```

解析为：

```json
["--name", "hello world", "--path", "/tmp/a b"]
```

后续可以支持 Markdown argv 列表：

```markdown
### args

- --name
- hello world
- --path
- /tmp/a b
```

## 4. 架构概览

```text
+------------------------------------------------+
| CLI / Local Daemon / Agent Interface            |
+------------------------------------------------+
                     |
+------------------------------------------------+
| High-level Action API                           |
| backtrace / evaluate / breakpoint_set / ...     |
+------------------------------------------------+
                     |
+------------------------------------------------+
| Session Manager                                 |
| interactive live session                         |
+------------------------------------------------+
        |                |                 |
+---------------+ +--------------+ +---------------+
| Workflow      | | Probe Store   | | Replay Store  |
| Engine        | | bp/wp/catch   | | saved actions |
+---------------+ +--------------+ +---------------+
        |
+------------------------------------------------+
| GDB/MI Session                                  |
| state machine / MI dispatcher / event router     |
+------------------------------------------------+
        |
+------------------------------------------------+
| Evidence Store + Session Log                     |
+------------------------------------------------+
        |
+------------------------------------------------+
| Sanitizer + Summarizer + Report Builder          |
+------------------------------------------------+
        |
+------------------------------------------------+
| gdb --interpreter=mi2 --quiet                    |
+------------------------------------------------+
```

关键原则：

1. AI 默认调用高层动作，不直接写 MI。
2. GDB/MI 复杂度封装在工具内部。
3. 活 session 与历史 snapshot 明确区分。
4. 证据归属使用精确 record id，而不只依赖连续 range。
5. 重启重放使用 Replay Store，而不是试图从 JSON 恢复 GDB 内部状态。

## 5. Session 生命周期

### 5.1 Session 类型

```cpp
enum class DebugTargetMode {
    Run,
    CoreDump
};
```

本工具只保留 Interactive Session Mode：

1. 启动本地 daemon 或长驻进程。
2. 创建 live session。
3. AI 多轮提交 high-level action。
4. 工具返回结构化结果和 evidence id。
5. AI 判断是否继续。
6. AI 发送 `finish_session` 后生成最终报告并关闭 session。

启动时可以配置 `initial_actions`，用于自动执行一组低风险默认取证动作。这不是 Batch Mode，而是 live session 的启动阶段；执行完后 session 仍保持打开，AI 可以继续追问、设置断点、重启、重放或验证假设。

### 5.2 Live Session 与 Snapshot

必须区分：

| 名称 | 含义 |
| --- | --- |
| `live session` | 仍有 GDB 子进程和 inferior 状态，可以继续调试 |
| `session_snapshot.json` | 历史状态快照，不代表活 GDB 进程 |
| `session_summary.json` | 报告用摘要，不可用于恢复进程 |
| `replay_plan.json` | 可重放动作列表，用于新 session 启动后重新执行 |

禁止把 `session.json` 设计成“可恢复活 GDB 会话”。GDB 的 frame、thread、inferior 内存、临时断点等状态不能可靠从 JSON 恢复。

### 5.3 推荐 CLI

```bash
gdb-agent check task.md
gdb-agent serve task.md --session S1 --assets report.assets/
gdb-agent action S1 action.json
gdb-agent save-action S1 action.json --name reproduce_checks
gdb-agent replay S1 reproduce_checks
gdb-agent finish S1 --out report.md
gdb-agent close S1
```

说明：

1. `serve` 创建 live session，并保持 GDB 子进程存活。
2. `action` 对 live session 发送高层动作。
3. `finish` 表示 AI Agent 已确认当前定位工作完成，工具生成最终报告。
4. `close` 只关闭 session，不等价于问题定位完成。
5. `check` 只做任务文件和路径校验，不启动 inferior。

## 6. 重启与重放设计

feature2 要求“允许 AI 可以重复启动应用程序，并允许保存特定操作，重启程序之后可以一次性执行特定命令”。

这里的“恢复”不是恢复旧 GDB 进程，而是在新 session 中重放一组高层动作。

### 6.1 Replay Action

```cpp
struct ReplayAction {
    std::string id;
    std::string name;
    std::string action_json;
    bool enabled = true;
    std::vector<std::string> tags;
};

struct ReplayPlan {
    std::string id;
    std::string name;
    std::vector<ReplayAction> actions;
};
```

文件示例：

```json
{
  "id": "replay-001",
  "name": "stale_fd_checks",
  "actions": [
    {
      "id": "a1",
      "name": "break_on_epoll_handler",
      "enabled": true,
      "action": {
        "action": "breakpoint_set",
        "location": "src/server.cpp:188",
        "condition": "fd == 42",
        "comment": "验证 fd=42 的 epoll event"
      }
    },
    {
      "id": "a2",
      "name": "run_until_break",
      "enabled": true,
      "action": {
        "action": "run",
        "deadline_ms": 30000
      }
    }
  ]
}
```

### 6.2 重放规则

1. Replay 只保存高层 action，不保存 raw MI 命令作为默认形式。
2. 每个 action 在新 session 中重新编译为 MI 命令。
3. 重放失败时记录 `ToolError` evidence，并继续或停止由 action policy 决定。
4. 重放结果产生新的 evidence id，不能复用旧 evidence id。
5. 报告必须记录 replay plan 名称、版本和每步结果。

## 7. Session 状态机

```cpp
enum class SessionState {
    Created,
    Starting,
    Ready,
    Loading,
    Stopped,
    Running,
    Interrupting,
    Exited,
    Error,
    Finishing,
    Closed
};
```

状态说明：

| 状态 | 说明 |
| --- | --- |
| `Created` | session 已创建，GDB 未启动 |
| `Starting` | 正在启动 GDB |
| `Ready` | GDB 可接受设置或加载命令 |
| `Loading` | 正在加载 executable 或 core |
| `Stopped` | inferior 处于停止态，可以取证 |
| `Running` | inferior 正在运行，只允许 interrupt、状态查询等有限动作 |
| `Interrupting` | 工具正在中断运行中的 inferior |
| `Exited` | inferior 已退出 |
| `Error` | 不可恢复错误 |
| `Finishing` | 正在生成最终报告 |
| `Closed` | GDB 已关闭 |

状态约束：

1. `backtrace`、`locals`、`evaluate` 默认要求 `Stopped` 或 Core Dump 模式。
2. `continue`、`step`、`next` 要求 `Stopped`。
3. `interrupt` 要求 `Running`。
4. `finish_session` 可以在 `Stopped`、`Exited`、`Error` 状态执行。
5. 自动策略只能在 `Stopped` 或 Core Dump 模式执行。

## 8. 高层 Action API

AI 默认通过高层 action 与工具交互。工具负责状态校验、参数校验、quoting、MI 编译、证据命名和摘要类型选择。

### 8.1 通用请求 Envelope

```json
{
  "request_id": "req-001",
  "action": "backtrace",
  "params": {
    "full": false,
    "thread": "current"
  },
  "timeout_ms": 3000,
  "max_output_bytes": 1048576,
  "max_lines": 2000,
  "capture_evidence": true
}
```

通用响应：

```json
{
  "ok": true,
  "version": "0.1.0",
  "session_id": "S1",
  "state": "stopped",
  "action": "backtrace",
  "status": "done",
  "result": {
    "summary": {}
  },
  "evidence": [
    {
      "id": "E0007",
      "kind": "GdbCommand",
      "title": "Backtrace",
      "summary_file": "report.assets/evidence/E0007.summary.json",
      "raw_file": "report.assets/evidence/E0007.raw.txt"
    }
  ],
  "warnings": [],
  "errors": []
}
```

### 8.2 推荐动作表

这是 AI Agent 首选使用的动作表。

| Action | 状态要求 | 说明 |
| --- | --- | --- |
| `run` | `Ready` 或 `Stopped` 后重启 | 启动 inferior |
| `continue` | `Stopped` | 继续运行 |
| `interrupt` | `Running` | 中断正在运行的 inferior |
| `backtrace` | `Stopped` 或 Core | 获取 backtrace |
| `threads` | 任意加载后状态 | 获取线程列表 |
| `frame_select` | `Stopped` 或 Core | 选择 frame |
| `locals` | `Stopped` 或 Core | 获取局部变量 |
| `args_info` | `Stopped` 或 Core | 获取当前 frame 参数 |
| `registers` | `Stopped` 或 Core | 获取寄存器 |
| `evaluate` | `Stopped` 或 Core | 求值表达式 |
| `breakpoint_set` | `Ready`、`Stopped` | 设置断点 |
| `watchpoint_set` | `Stopped` | 设置观察点 |
| `probe_delete` | 已加载状态 | 删除 probe |
| `probe_enable` | 已加载状态 | 启用 probe |
| `probe_disable` | 已加载状态 | 禁用 probe |
| `save_action` | 任意 live session | 保存一个可重放动作 |
| `replay` | `Ready`、`Stopped` | 重放保存的动作 |
| `hypothesis_create` | 任意 live session | 创建假设 |
| `hypothesis_check` | 通常要求 `Stopped` | 执行假设检查 |
| `finish_session` | `Stopped`、`Exited`、`Error` | AI 确认完成，生成最终报告 |
| `raw_mi` | 高级模式 | 逃生通道，不作为默认推荐 |

### 8.3 Action 示例

backtrace：

```json
{
  "action": "backtrace",
  "params": {
    "full": false,
    "thread": "current",
    "limit": 64
  }
}
```

evaluate：

```json
{
  "action": "evaluate",
  "params": {
    "expression": "fd_state_table[fd]",
    "thread": "current",
    "frame": 0
  },
  "timeout_ms": 3000
}
```

条件断点：

```json
{
  "action": "breakpoint_set",
  "params": {
    "location": "src/server.cpp:188",
    "condition": "fd == 42",
    "comment": "验证 fd=42 的事件是否来自过期 epoll event",
    "purpose": "hypothesis_check",
    "on_hit": [
      {
        "action": "evaluate",
        "params": {
          "expression": "fd_state_table[fd]"
        }
      },
      {
        "action": "backtrace",
        "params": {
          "full": false
        }
      }
    ]
  }
}
```

raw MI escape hatch：

```json
{
  "action": "raw_mi",
  "params": {
    "command": "-interpreter-exec console \"maintenance info sections\"",
    "requires": ["stopped"],
    "risk": "advanced"
  },
  "timeout_ms": 5000
}
```

raw MI 规则：

1. 默认文档不推荐 AI 使用。
2. 必须显式标记 `risk: advanced`。
3. 必须记录原始命令到 evidence 和报告。
4. 工具仍要执行状态校验和 timeout。

## 9. GDB/MI 内部模型

高层 action 最终会被编译为 GDB/MI 命令。该层不直接暴露给 AI 作为默认接口。

### 9.1 MI Record

```cpp
enum class MiRecordKind {
    Result,
    ExecAsync,
    StatusAsync,
    NotifyAsync,
    ConsoleStream,
    TargetStream,
    LogStream,
    Prompt,
    Unknown
};

struct MiRecord {
    uint64_t sequence;
    std::chrono::system_clock::time_point timestamp;
    MiRecordKind kind;
    std::optional<uint64_t> token;
    std::string raw;
    std::string klass;
    std::map<std::string, std::string> fields;
};
```

### 9.2 命令分类

```cpp
enum class MiCommandKind {
    Immediate,
    ExecutionControl
};
```

Immediate 命令等待 token result，例如 `^done` 或 `^error`。

ExecutionControl 命令不能只等待 `^running`，必须等待后续事件：

1. `*stopped`
2. `=thread-group-exited`
3. GDB 进程退出
4. run deadline 到达后 interrupt 的结果

### 9.3 MI Dispatcher 规则

1. 每条 MI 输出进入全局 session log。
2. 每条命令分配 token。
3. 有 token 的 result record 唤醒对应 request。
4. 无 token 的 async record 交给 event router。
5. target stream 与 inferior output 分开建模。
6. console stream 不直接按连续 range 归属，必须通过当前 request 和 record 分类归属。

## 10. Timeout 与 Cancellation

必须区分 command timeout 和 run deadline。

### 10.1 Command Timeout

Command timeout 表示 GDB 对某条命令没有按时返回结果。

处理：

1. 标记该 action 为 `CommandTimeout`。
2. 记录 `ToolError` evidence。
3. 如果 session 仍可用，继续后续动作。
4. 如果 GDB 无响应，进入 `Error` 并关闭。

### 10.2 Run Deadline

Run deadline 表示 inferior 运行超过 AI 或任务指定时间，但 GDB 本身可能正常。

处理：

1. 状态从 `Running` 进入 `Interrupting`。
2. 发送 `-exec-interrupt`。
3. 等待 `*stopped`。
4. 如果 interrupt 成功，状态变为 `Stopped`，stop reason 记为 `interrupted_by_tool_deadline`。
5. 执行 timeout 相关取证，例如 threads、backtrace。
6. 如果 interrupt 失败，再终止 GDB。

报告规则：

1. run deadline 到期不是程序错误。
2. 报告只能写“在限定时间内未复现目标停止事件”。
3. 如果 interrupt 后发现死锁迹象，必须以 evidence 引用方式表达。

## 11. Inferior I/O 设计

由于放弃 PTY，I/O 行为必须明确。

### 11.1 默认策略

| 流 | MVP 策略 |
| --- | --- |
| stdin | 默认 `/dev/null`，可指定文件 |
| stdout | 捕获为 `InferiorOutput` evidence |
| stderr | 捕获为 `InferiorOutput` evidence |

实现方式可以二选一：

1. 使用 GDB inferior tty / inferior-io 重定向到工具创建的 pipe 或文件。
2. 使用 GDB target stream 捕获 inferior 输出，并在 event router 中分类。

MVP 建议选择更稳定的文件重定向：

1. stdin 来自 `/dev/null` 或用户指定文件。
2. stdout/stderr 重定向到 assets 目录下的文件。
3. 工具异步 tail 这些文件或在停止后读取。

示例文件：

```text
report.assets/inferior/stdout.log
report.assets/inferior/stderr.log
```

### 11.2 限制

1. 不支持交互式输入。
2. 无换行输出需要按 byte buffer 捕获，不能只依赖 read line。
3. 如果目标程序等待 stdin，工具只能通过 run deadline 和 interrupt 观察，不能自动判断业务语义。
4. 报告必须记录 `stdin`、`stdout`、`stderr` 配置。

## 12. Evidence 设计

Evidence 是可信报告的基础。

### 12.1 Evidence Kind

```cpp
enum class EvidenceKind {
    GdbCommand,
    StopEvent,
    BreakpointHit,
    WatchpointHit,
    InferiorOutput,
    SessionEvent,
    EnvironmentInfo,
    ReplayStep,
    ToolError
};
```

### 12.2 Evidence 结构

```cpp
struct Evidence {
    std::string id;
    EvidenceKind kind;
    std::string title;
    std::string action_name;
    std::string purpose;

    std::vector<uint64_t> included_records;
    std::vector<uint64_t> related_records;
    std::vector<uint64_t> concurrent_records;

    std::filesystem::path raw_file;
    std::string raw_sha256;
    std::filesystem::path sanitized_file;
    std::filesystem::path summary_file;
    bool lossy_summary;

    size_t raw_bytes;
    size_t kept_bytes;
    bool truncated;

    std::map<std::string, std::string> structured;
    std::chrono::system_clock::time_point captured_at;
};
```

### 12.3 归属规则

1. `included_records` 是明确属于该 evidence 的记录。
2. `related_records` 是与该 evidence 有关系但不是直接输出的记录，例如触发该命令的 stop event。
3. `concurrent_records` 是命令窗口期间发生的其他 async event，不能混入 raw evidence。
4. session log 保存全量顺序记录。
5. evidence raw 文件只保存明确归属的 included records。
6. 无法归属的 async event 单独生成 `SessionEvent` evidence。

这解决了只用 sequence range 过宽的问题。

### 12.4 文件布局

```text
report.assets/
  session_snapshot.json
  session_summary.json
  session.mi.raw.log
  task.normalized.json
  replay/
    stale_fd_checks.json
  inferior/
    stdout.log
    stderr.log
  evidence/
    E0001.gdb-version.raw.txt
    E0001.gdb-version.summary.json
    E0002.stop-event.raw.txt
    E0002.stop-event.summary.json
```

## 13. Sanitizer 与 Summarizer

### 13.1 Sanitizer

Sanitizer 只做确定性降噪，不做推理。

规则：

1. C++ 常见类型简化：
   - `std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >`
   - 简化为 `std::string`
2. 工作目录下路径转为相对路径。
3. 规整路径中的 `..` 和重复 `/`。
4. 去掉 GDB prompt、banner 和重复空白。
5. 超长模板参数可以折叠，但 raw 永远保留。
6. 所有截断必须记录原始字节数、保留字节数和截断原因。

### 13.2 Summarizer

Summarizer 只做结构化摘录，不做根因判断。

backtrace summary 示例：

```json
{
  "type": "backtrace",
  "crash_frame": {
    "index": 0,
    "function": "Session::on_read",
    "location": "src/session.cpp:142"
  },
  "user_frames": [
    {
      "index": 0,
      "function": "Session::on_read",
      "location": "src/session.cpp:142"
    }
  ],
  "library_frame_count": 8,
  "truncated": false
}
```

watchpoint summary 示例：

```json
{
  "type": "watchpoint",
  "expression": "fd_state_table[fd].generation",
  "old_value": "2",
  "new_value": "3",
  "thread_id": "4"
}
```

## 14. Probe 管理层

Probe 包括断点、观察点和 catchpoint。

### 14.1 Probe 结构

```cpp
enum class ProbeKind {
    Breakpoint,
    Watchpoint,
    Catchpoint
};

enum class ProbePurpose {
    Investigation,
    HypothesisCheck,
    Memo,
    AutoStrategy
};

struct ProbeAction {
    std::string action_json;
    std::chrono::milliseconds timeout;
    size_t max_output_bytes;
    size_t max_lines;
    bool capture_evidence = true;
};

struct Probe {
    std::string id;
    ProbeKind kind;
    ProbePurpose purpose;
    std::string location_or_expression;
    std::string condition;
    std::string comment;
    std::optional<std::string> hypothesis_id;

    std::optional<int> gdb_number;
    bool enabled = true;
    bool pending = false;
    uint64_t hit_count = 0;
    std::optional<std::string> last_hit_evidence_id;

    std::vector<ProbeAction> on_hit_actions;
};
```

### 14.2 命中动作规则

1. 命中后先记录 `BreakpointHit` 或 `WatchpointHit` evidence。
2. 每个 `on_hit_actions` 是高层 action，不是默认 raw MI。
3. 每个动作必须有 timeout、max bytes 和 max lines。
4. 动作失败记录 `ToolError` evidence，不中断其他取证，除非 action policy 指定停止。
5. 如果命中时 session 状态不满足动作要求，跳过该动作并记录 warning。

## 15. 自动策略

自动策略用于停止事件后的默认取证。

### 15.1 策略分层

| 档位 | 说明 |
| --- | --- |
| light | 默认执行，低输出、低风险 |
| deep | 显式请求，输出大、耗时长 |

### 15.2 初始化限制

为了避免输出爆炸：

```text
-gdb-set pagination off
-gdb-set confirm off
-gdb-set print pretty on
-gdb-set print object on
-gdb-set print elements 200
-gdb-set print max-depth 4
-gdb-set print repeats 10
```

### 15.3 Light 策略

SIGSEGV / SIGABRT：

1. `backtrace full=false limit=64`
2. `args_info`
3. `locals simple=true`
4. `registers`

断点命中：

1. `frame_info`
2. `args_info`
3. `locals simple=true`
4. probe 的 `on_hit_actions`

run deadline 中断：

1. `threads`
2. `backtrace thread=all full=false limit=128`

### 15.4 Deep 策略

Deep 策略需要 AI 显式请求：

1. `backtrace full=true thread=all`
2. `locals simple=false`
3. 大对象 evaluate
4. shared library 和 section 详情

每个 deep 动作必须设置更严格的输出上限和更长 timeout。

## 16. Hypothesis 工作流

工具帮助 AI 收集证据，但不把 AI 的推断伪装成工具事实。

### 16.1 拆分状态

```cpp
enum class EvidenceCollectionStatus {
    Draft,
    Running,
    EvidenceCollected,
    Failed
};

enum class AgentConclusionStatus {
    Unset,
    Supported,
    Refuted,
    Inconclusive
};

enum class CheckAssertionStatus {
    NotRun,
    Passed,
    Failed,
    Error,
    NotApplicable
};
```

### 16.2 Hypothesis 结构

```cpp
struct HypothesisCheck {
    std::string id;
    std::string description;
    std::string action_json;
    std::optional<int> thread_id;
    std::optional<int> frame_id;
    bool requires_stopped = true;

    std::string assertion_kind; // none, contains, equals, non_null, changed 等
    std::string expected;
    CheckAssertionStatus assertion_status;

    std::optional<std::string> evidence_id;
};

struct Hypothesis {
    std::string id;
    std::string title;
    std::string description;

    EvidenceCollectionStatus evidence_status;
    AgentConclusionStatus agent_conclusion;

    std::vector<HypothesisCheck> checks;
    std::vector<std::string> evidence_ids;
    std::string agent_inference;
};
```

### 16.3 报告规则

1. check 层面的 assertion pass/fail 是工具事实。
2. hypothesis 的 supported/refuted 默认来自 AI Agent 的 conclusion。
3. 如果 AI 未给结论，报告写 `AgentConclusionStatus: Unset`。
4. 报告必须分区展示：
   - Tool Observations
   - Tool Assertion Results
   - Agent Inference
   - Final Agent Conclusion

## 17. Core Dump 模式

Core Dump 是核心能力，MVP 必须支持最小闭环。

### 17.1 MVP Core 流程

1. 加载 executable。
2. 加载 core dump。
3. 执行最小证据：
   - `backtrace`
   - `threads`
   - `registers`
   - `info files`
4. 生成 report。

### 17.2 完整 Core 流程

1. `info files`
2. `info sharedlibrary`
3. `info threads`
4. `thread apply all bt`
5. 当前线程 `bt full`
6. 当前 frame `info args`
7. 当前 frame `info locals`
8. `info registers`

### 17.3 风险检查

报告必须记录：

1. executable 是否存在。
2. core dump 是否存在。
3. debug symbol 是否完整。
4. shared library symbol 是否缺失。
5. build-id 是否可能不匹配。
6. executable 是否 strip。

## 18. 报告设计

最终报告是 Markdown，同时保留 JSON session summary。

### 18.1 报告结构

```markdown
# GDB Verification Report

## Task

## Environment

## Session Lifecycle

## I/O Configuration

## Replay Plans

## Stop Events

## Tool Observations

## Tool Assertion Results

## Agent Inference

## Final Agent Conclusion

## Evidence Summary

## Limitations

## Raw Evidence Index
```

### 18.2 报告规则

1. `Tool Observations` 只能写工具实际观察到的事实。
2. `Agent Inference` 只能写 AI Agent 的推断。
3. `Final Agent Conclusion` 需要 AI 通过 `finish_session` 或报告参数提供。
4. 每条关键 observation 必须引用 evidence id。
5. 报告必须说明截断、超时、pretty-printer 错误、缺失符号。
6. 报告必须说明非 PTY 和 stdin 配置限制。
7. run deadline 到期不能写成程序错误。

## 19. 环境与安全边界

报告和 session summary 必须记录：

1. GDB version。
2. tool version。
3. OS、kernel、architecture。
4. executable path、mtime、size。
5. core dump path、mtime、size。
6. working directory。
7. stdin/stdout/stderr 配置。
8. 环境变量白名单。

安全边界：

1. 本工具会执行本地程序和 GDB 命令，应仅在受信任 workspace 使用。
2. `check` 或 `serve --dry-run` 应展示将要执行的 executable、args、working directory、stdin、env。
3. 可选支持 `--workspace-root`，限制路径逃逸。
4. raw MI 和 console escape hatch 必须记录到 evidence。

## 20. C++20 异步方案

推荐使用 `asio` 或 `boost::asio` 的 C++20 coroutine 能力，避免把项目重心变成自研异步框架。

最小异步能力：

```cpp
template <class T>
using Task = asio::awaitable<T>;

class AsyncGdbProcess {
public:
    Task<void> start();
    Task<void> write_stdin(std::string_view data);
    Task<ReadChunk> read_stdout();
    Task<ReadChunk> read_stderr();
    Task<int> wait_exit();
    Task<void> terminate();
};
```

必须支持：

1. stdout/stderr 异步读取，包含无换行 partial output。
2. stdin 异步写入到 GDB。
3. timer。
4. cancellation。
5. 子进程退出检测。
6. inferior stdout/stderr 文件或 pipe 异步采集。

## 21. 工程结构

```text
gdb_cli_tools/
  CMakeLists.txt
  docs/
    agent_actions.md
    task_format.md
    evidence_model.md
  src/
    main.cpp
    cli/
      cli.hpp
      cli.cpp
    agent/
      action.hpp
      action_parser.cpp
      action_compiler.cpp
      action_dispatcher.cpp
    task/
      debug_task.hpp
      markdown_task_parser.cpp
      argv_parser.cpp
    session/
      session_manager.hpp
      live_session.cpp
      session_snapshot.cpp
      replay_store.cpp
    async/
      async_gdb_process.hpp
      timeout.cpp
    gdb/
      gdb_mi_session.hpp
      gdb_mi_session.cpp
      mi_parser.cpp
      mi_record.hpp
      event_router.cpp
    workflow/
      auto_strategy.cpp
      probe_store.cpp
      hypothesis.cpp
    evidence/
      evidence.hpp
      evidence_store.cpp
      sanitizer.cpp
      summarizer.cpp
    report/
      report_builder.cpp
    util/
      sha256.cpp
      json.cpp
  tests/
    task_parser_tests.cpp
    argv_parser_tests.cpp
    action_parser_tests.cpp
    action_compiler_tests.cpp
    mi_parser_tests.cpp
    evidence_store_tests.cpp
    replay_store_tests.cpp
```

## 22. 分阶段计划

### Phase 1: Interactive MVP，包含 Run 和 Core

目标：形成最小可用的持续会话闭环，而不是一次性报告闭环。

范围：

1. Markdown task parser。
2. shell-like args parser。
3. stdin/env/run timeout 字段。
4. GDB/MI 启动和初始化。
5. Session 状态机。
6. `serve`、`action`、`finish`、`close` 命令。
7. live session registry。
8. 高层 Action API。
9. Run Mode 正确处理 `^running` 与 `*stopped`。
10. Core Dump Mode 最小闭环。
11. run deadline 和 interrupt 语义。
12. light 自动证据采集。
13. Evidence Store，包含 included/related/concurrent records。
14. raw MI session log。
15. Markdown 报告、session_snapshot.json 和 session_summary.json。

### Phase 2: Replay Store

目标：支持重复启动和一次性重放特定操作。

范围：

1. `save-action`。
2. replay_plan.json。
3. `replay`。
4. replay step evidence。

### Phase 3: Probe 与自动命中动作

目标：支持 AI 条件断点和观察点。

范围：

1. breakpoint/watchpoint/catchpoint。
2. condition。
3. comment/purpose。
4. on_hit 高层动作。
5. hit evidence 关联。

### Phase 4: Hypothesis Workflow

目标：系统化执行假设检查。

范围：

1. hypothesis create。
2. hypothesis check。
3. check assertion。
4. tool observation 与 agent conclusion 分离。
5. 报告 hypothesis 区域。

### Phase 5: 深度摘要与高级 MI

目标：提升复杂 C++ 项目的可读性和覆盖面。

范围：

1. 更完整 MI value parser。
2. C++ 类型 sanitizer 增强。
3. backtrace/thread summarizer 增强。
4. raw_mi 高级审计。

## 23. 验收条件

本方案满足 `feature2.md` 和 `revise1.md` 的关键条件：

1. 明确支持持续性 session，直到 AI Agent 确认问题定位完成。
2. 只保留 Interactive Session Mode，不再提供独立 Batch Mode。
3. 不再把 `session.json` 当作可恢复活会话，改为 snapshot/summary/replay plan。
4. 默认对 AI 暴露高层 action，避免要求 AI 直接输入 MI 命令。
5. raw MI 仅作为高级 escape hatch。
6. Evidence 使用 included/related/concurrent records，避免连续 range 归属过宽。
7. 明确非 PTY 下 inferior stdin/stdout/stderr 策略。
8. 区分 command timeout 和 run deadline。
9. 自动策略有 timeout、max bytes、max lines 和 ToolError evidence。
10. Hypothesis 拆分工具取证状态与 AI 结论状态。
11. Phase 1 就包含 live session、高层 action、Run 和最小 Core Dump 模式。
12. 支持保存动作和重启后重放动作。

## 24. 总结

修订后的设计把项目核心收敛为“面向 AI Agent 的长会话 GDB 执行与证据系统”。AI 不再直接面对 GDB/MI 的复杂细节，而是通过稳定的高层 action 调试程序。工具内部负责状态机、MI 事件、证据归属、I/O 限制、超时中断、自动策略和报告草稿。

这个方案从第一阶段就围绕 Interactive Session 落地：先把 live session、Run/Core、高层 action 和可信证据闭环做稳，再继续演进 Replay Store、Probe 自动动作和 Hypothesis Workflow，满足持续对话式定位问题的目标。
