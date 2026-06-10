# Next CLI Task: cli.cpp typed action refactor

## 目标

下一轮集中做 **`src/cli.cpp` 拆分与类型化内部 action 重构**。

这是行为保持型重构，不新增用户可见功能。目标是把当前 `cli.cpp` 中混在一起的 CLI、
daemon、session lifecycle、action dispatch、probe/on-hit、replay、hypothesis glue 拆开，
并把内部 action 流程从“JSON/string/ostream 驱动”收敛为 typed C++ struct / enum 驱动。

核心原则：

1. JSON 只作为 CLI/daemon 输入输出边界、replay plan 文件和 evidence/report 审计 artifact 的格式。
2. 内部模块不要把 JSON 文本、response 字符串或 `ostream` 当作控制流协议。
3. action 输入在边界解析成 typed request；handler 返回 typed result；最后统一 dump 成现有 JSON response。
4. 不引入 protobuf，不引入第三方 JSON 库；普通 C++ `struct` / `enum class` 足够。
5. 外部 CLI 命令、action JSON schema、response JSON 字段、evidence schema、report schema 和 replay plan schema 必须保持兼容。

## 背景

当前 `src/cli.cpp` 约 3,500 行，主要职责包括：

- CLI 参数解析和顶层命令路由。
- `check` / `serve`。
- Unix socket daemon 和 client command JSON 构造。
- live session 启动、initial replay、Run/Core mode 初始化、finish 写 report/assets。
- `handle_action_line` action dispatch。
- breakpoint/watchpoint/catchpoint metadata 和 on-hit action。
- hypothesis 文件/index 写入。
- replay JSONL / structured plan 执行。
- response JSON 字符串拼接和 `ostream` 输出。

这使后续扩 action、修 failure semantics 或调整 replay/on-hit 行为时很容易误改无关路径。

本轮重构的关键不是更换外部协议，而是收紧内部边界：

- 业务字段仍可以是字符串，例如 GDB expression、location、raw MI command、error message、
  evidence id、debug text 和 human-readable summary。
- 但序列化 JSON 文本、拼接后的 response 文本、dump 后再 parse 的 action/response，不应作为内部模块协议。
- replay 和 on-hit 判断 action 是否失败时，应直接看 typed `ActionResult.ok`，不要解析 response 文本里的 `ok:false`。

## 任务分类

### 1. 类型化 action 边界

新增内部 action 类型系统。

建议结构：

- `ActionKind`
  - 覆盖当前支持的 action：`backtrace`、`locals`、`registers`、`threads`、`args_info`、
    `frame_select`、`evaluate`、`breakpoint_set`、`watchpoint_set`、`catchpoint_set`、
    `probe_list`、`probe_enable`、`probe_disable`、`probe_delete`、`run`、`continue`、
    `save_action`、`replay`、`hypothesis_create`、`hypothesis_check`、`hypothesis_conclude`、
    `raw_mi`、`finish_session`。
- `ActionRequest`
  - 包含 `ActionKind kind`。
  - 包含各 action 的 typed payload。可使用 `std::variant`，也可以先用一个聚合 struct 加 optional payload，
    但不要把 raw `Json` 作为 handler 的主要输入。
- `ActionResult`
  - 至少包含 `bool ok`、`ActionKind kind`、`bool finished`、`std::string error`、
    `std::string evidence_id`、`std::string command_evidence_id`。
  - 需要表达 action-specific 字段时，用 typed result payload 或一个结构化 fields 容器。
  - 允许最终序列化时转换为 `Json`，但 handler 内部不应拼接 response JSON 字符串。

要求：

- `Json -> ActionRequest` 只发生在 action 输入边界。
- `ActionResult -> Json -> dump_json` 只发生在 CLI/daemon 输出边界。
- handler 不接收 raw JSON，不直接写 `ostream`，不返回拼接好的 response 文本。

### 2. Action parser / dispatcher

抽出 action parser 和 dispatcher。

要求：

- `parse_action_request(const Json&) -> ActionRequest`
  - 负责字段读取、默认值和 validation。
  - 对缺少必需字段、非法 on-hit policy、非法 catchpoint selector、`raw_mi` 缺少
    `risk:"advanced"` 等情况生成稳定 typed validation failure。
- `dispatch_action(ActionContext&, const ActionRequest&) -> ActionResult`
  - 负责 state guard 和分发。
  - action handler 只处理 typed payload。
- `finish_session` / `finish` 通过 `ActionResult.finished = true` 表达结束请求。

注意：

- 当前 response 字段名和错误语义必须保持兼容。
- validation failure 仍要写 `ToolError` evidence，并在 response 中返回 `evidence`。
- GDB command failure 仍要通过 `command_evidence` 关联原始 command evidence。

### 3. Probe / on-hit runtime

从 `cli.cpp` 中抽出 probe runtime。

覆盖：

- `ProbeState`。
- on-hit policy parsing 和 validation。
- breakpoint/watchpoint/catchpoint metadata 管理。
- probe hit attribution。
- `BreakpointHit` / `WatchpointHit` / `CatchpointHit` evidence 写入。
- `OnHitAction` wrapper evidence 写入。
- finish-time `assets/probes.json` 写出。

要求：

- 运行期仍以内存 `ProbeState` 为权威状态。
- `assets/probes.json` 仍只在 finish/report 阶段写出，不作为 live GDB 恢复文件。
- on-hit action 必须走 typed `ActionRequest` / `ActionResult`。
- on-hit success/failure 直接看 `ActionResult.ok`。
- `raw_mi` 继续禁止作为 on-hit action。
- 现有 evidence kind、字段和 session summary 计数语义保持兼容。

### 4. Replay runtime

从 `cli.cpp` 中抽出 replay runtime。

要求：

- `src/replay/replay_plan.*` 继续负责 replay plan schema、fingerprint 和 validation。
- replay plan / JSONL 文件仍是 JSON 持久化格式。
- load replay step 时，把 action JSON parse 成 `ActionRequest`。
- 执行 replay step 时调用 typed dispatcher，得到 `ActionResult`。
- step success/failure 直接使用 `ActionResult.ok`，不要从 response 文本反解析。
- `continue_on_error` / `stop_on_error` / skipped step 行为保持不变。
- `ReplayStep` / `ReplayRun` / `ReplayWarning` / replay `ToolError` evidence 字段语义保持兼容。

注意：

- replay response 仍要输出当前兼容的 JSON 字段，例如 `steps`、`run_evidence`、`error_evidence`、
  `warning_evidence`、`task_metadata_match`。
- 允许 replay evidence 中保留 action 的 JSON 表示作为审计 artifact，但这必须来自 typed request 的边界/审计 dump，
  不能作为内部控制流。

### 5. Session runtime

抽出 live session 生命周期，消除 `serve` 和 daemon `create/finish` 的重复逻辑。

覆盖：

- validate task。
- 创建 assets。
- 创建和初始化 `GdbSession`。
- 设置 session id。
- 设置 inferior stdout/stderr assets path。
- collect environment info。
- optional `--replay-before-run`。
- Run Mode initial run。
- Core Dump Mode load core 和静态取证。
- stop follow-up collection。
- finish 时 flush inferior output、写 probe snapshot、写 session files、写 report、shutdown。

要求：

- `serve` 和 daemon flow 复用同一套 session runtime。
- `finish` 表示定位流程完成；`close` 仍只关闭 session。
- `session_snapshot.json` / `session_summary.json` 语义不变。
- Core Dump Mode state guard 语义不变。

### 6. Daemon / client split

从 `cli.cpp` 中抽出 daemon 和 client command glue。

覆盖：

- Unix socket listen/connect/read/write。
- daemon request handling。
- daemon session map。
- client subcommand request construction：`create`、`action`、`save-action`、`replay`、`finish`、
  `close`、`status`、`list`、`shutdown`。

要求：

- 外部 CLI 用法完全兼容。
- daemon action response 仍是单行 JSON。
- client 仍支持 inline JSON 或 JSON file。
- `src/cli.cpp` 最终只保留顶层 `run_cli` 路由、`check` / `serve` / daemon/client 调用 glue 和少量 shared CLI parsing。

### 7. JSON / string 使用边界

本轮必须明确代码层面的边界。

允许：

- CLI/daemon 输入输出 JSON。
- replay plan / JSONL 作为持久化文件格式。
- evidence payload、session files、report artifacts 使用 JSON/Markdown/text。
- 业务值使用字符串，例如 expression、location、GDB command、error、evidence id、hypothesis title、
  debug text、summary。

禁止：

- handler 通过拼接 JSON 字符串返回 response。
- handler 直接写 `ostream`。
- replay/on-hit 通过解析 response 文本判断 action 成功失败。
- 模块间把 dump 后的 action/response JSON 文本当作主要内部协议。
- 引入 protobuf 或大型第三方序列化依赖。

## 建议文件组织

可以按实际实现微调，但建议不要把新代码继续堆回 `src/cli.cpp`。

建议新增：

- `src/cli/action.hpp`
- `src/cli/action.cpp`
- `src/cli/session_runtime.hpp`
- `src/cli/session_runtime.cpp`
- `src/cli/daemon.hpp`
- `src/cli/daemon.cpp`
- `src/workflow/probe_runtime.hpp`
- `src/workflow/probe_runtime.cpp`
- `src/replay/replay_runtime.hpp`
- `src/replay/replay_runtime.cpp`

也可以新增小型 helper：

- `src/cli/json_response.hpp`
- `src/cli/json_response.cpp`

要求：

- `src/cli.hpp` 继续只暴露 `int run_cli(int argc, char **argv);`。
- 不把内部 runtime 类型暴露成项目公共 API。
- CMake 中只把新增 `.cpp` 加到 `gdb-agent` target；除非新增单元测试需要，否则不要扩大测试 target 依赖。

## 可写范围

允许修改：

- `src/cli.cpp`
- `src/cli.hpp`，仅当确有必要；原则上保持只暴露 `run_cli`
- `CMakeLists.txt`
- 新增 `src/cli/*.hpp`
- 新增 `src/cli/*.cpp`
- 新增或修改 `src/workflow/probe_runtime.*`
- 新增或修改 `src/replay/replay_runtime.*`
- `docs/ai/progress.md`
- `docs/ai/handoff.md`
- `docs/ai/decision.md`，仅当实现过程中需要记录项目级决策；当前已知决策可记录为：
  内部 action 流程使用 typed structs，JSON/string 只作为边界格式

原则上不要修改：

- action JSON schema 文档，除非发现现有文档与实际行为不一致。
- evidence model 文档，除非 evidence 字段语义意外需要说明。
- report 输出内容。
- replay plan schema。
- task format。
- sanitizer、MI parser、hypothesis assertion 行为。

禁止：

- 修改外部 CLI 命令语法。
- 修改用户可见 action schema。
- 修改 evidence raw 文件布局。
- 修改 replay plan schema。
- 引入 protobuf、IDL/codegen 或第三方 JSON 库。
- 顺手做 unrelated refactor 或全仓格式化。

## 验证

至少运行：

```bash
cmake --build build
./build/gdb-agent check examples/segfault_task.md
./build/task_parser_tests
./build/replay_plan_tests
./build/hypothesis_assertion_tests
./build/mi_summary_tests
./build/type_sanitizer_tests
ctest --test-dir build --output-on-failure
git diff --check
```

重点确认：

- daemon `action` 仍返回单行 JSON。
- `finish_session` 在 `serve` 和 daemon flow 下仍正确结束并写 report/assets。
- replay `continue_on_error` / `stop_on_error` / skipped step 行为不变。
- on-hit action 成功失败判断不再依赖文本 response。
- validation failure、state guard failure、GDB command failure 仍写 `ToolError` evidence。
- report、session summary、snapshot、evidence index、probe snapshot 和 hypothesis index 引用保持一致。

如果当前环境缺少 GDB 或 live smoke 被平台跳过，必须在 `docs/ai/handoff.md` 和最终回复中说明：

- 已运行哪些测试。
- 哪些 live smoke 未实际运行。
- 原因是环境限制、GDB 版本差异，还是实现遗留。

## 完成标准

本轮完成时应满足：

- `src/cli.cpp` 不再承担全部 action/probe/replay/daemon/session runtime 职责。
- action handler 内部不直接写 `ostream`。
- action handler 不返回拼接 JSON response 字符串。
- action handler 主要接收 typed request，返回 typed result。
- replay/on-hit 不再解析 response 文本判断成功失败。
- 外部 CLI/action/response/evidence/report/replay 行为与现有 smoke 兼容。
- `docs/ai/progress.md` 记录本轮实际完成内容。
- `docs/ai/handoff.md` 覆写为本轮交接，包含验证结果和遗留限制。
- 本轮相关文件被单独 staged、commit，并 push 到当前分支 upstream；如果 commit/push 失败，
  按 AGENTS.md 要求记录原因。

## 当前轮次限制

如果本文件只是由 planning/documentation 步骤更新，则本步骤只允许修改
`docs/ai/next_cli_task.md`，不修改源码、不运行 build/test、不更新 progress/handoff、不 commit/push。
真正执行上述重构时，必须由后续“开始新一轮任务流”或等价的明确执行指令触发。
