# Next CLI Task: remove remaining string/Json internal transports

## 目标

下一轮集中清理当前 action/replay/on-hit 路径中仍残留的 **字符串或 JSON AST 作为内部传输协议** 的实现。

上一轮已经引入 `ActionKind`、`ActionRequest`、`ActionResult`，并让 replay/on-hit 的 success/failure
判断不再解析 response 文本。但代码里仍有多个内部路径把 action、response 或结构化扩展字段保存为
`std::string`、`Json` 或 `std::vector<Json>` 后再 parse/dump。下一轮要把这些内部传输改成 typed
C++ struct / enum / vector，或结构化 KV。这里的目标不是消灭业务字符串，也不是把天然适合 KV 的
半开放结果/metadata 硬拆成大量 action-specific struct。

本轮仍是行为保持型重构，不新增用户可见功能，不改变 CLI 命令、action JSON schema、response JSON
字段、evidence schema、replay plan schema、report schema 或 raw evidence 文件布局。

## 当前发现的残留问题

### 1. `ActionRequest` / `ActionResult` 仍有 JSON 容器

当前 `src/cli/action.hpp` 里：

- `ActionRequest` 仍保留 `Json on_hit` 和 `Json saved_action`。
- `ActionResult` 仍保留 `Json fields` 和 `std::vector<Json> prelude_responses`。
- `action_result_line` 会把 `prelude_responses` 和最终 result dump 成多行 response。

这些是“类似字符串的数据结构”：虽然不是 JSON 文本，但仍把内部结构化状态交给通用 JSON AST 承载。

下一轮要求：

- `ActionRequest` 不再持有 raw `Json` payload。
- `ActionResult` 不再持有 generic `Json fields` / `prelude_responses`。
- action-specific request 字段用 typed payload 表达。
- response 的半开放扩展字段可以保留结构化 KV，但 KV value 必须是结构化类型，不能是 JSON 文本或
  generic `Json` AST。
- 多行 response 兼容性用 typed `ActionEmission` / `ActionOutput` 之类的结构表达，最后只在 CLI/daemon 边界 dump 成 JSON。

### 2. `handle_action_line_legacy` 仍是字符串/ostream 内部协议

当前 `src/cli.cpp` 仍有：

- `handle_action_line_legacy(..., const std::string &line, bool &finished, std::ostream &out)`。
- 外层 `handle_action_line` 先 parse JSON 获取 fallback，再调用 legacy handler。
- legacy handler 内部仍直接 parse `line`、写 `ostream`、拼 response JSON 字符串。
- 外层再把 legacy output parse 成 `ActionResult` / prelude responses。

下一轮要求：

- 删除 `handle_action_line_legacy`。
- 新增真正的 `dispatch_action(ActionContext&, const ActionRequest&) -> ActionResult`。
- 每个 action handler 接收 typed request payload，返回 typed result。
- handler 内部不写 `ostream`，不拼完整 JSON response，不把 response 文本交回上层。
- `serve` / daemon action 只在最外层输出 `ActionOutput` dump 后的 JSON 行。

### 3. On-hit policy 仍用 JSON 文本保存 action

当前 `ProbeState::OnHitPolicy` 中：

- `std::vector<std::string> actions` 保存 `dump_json(item)` 后的 action JSON。
- `parse_on_hit_action` 把 `Json item` dump 成字符串保存。
- `action_name_from_text` 通过 `parse_json(action_text)` 取 action 名。
- `add_timeout_to_on_hit_action` 通过 parse/dump 修改 timeout/deadline。
- `run_on_hit_actions` 把 `action_text` 传回 `handle_action_line`。
- `continue_after_hit` 通过 `ostringstream` 构造 `{"action":"continue",...}`。

下一轮要求：

- `OnHitPolicy::actions` 改为 `std::vector<ActionRequest>` 或更窄的 `std::vector<OnHitActionRequest>`。
- on-hit parsing 边界只在 action JSON 输入处发生一次。
- timeout/deadline 注入通过 typed payload 修改，不允许 parse/dump action JSON。
- `continue_after_hit` 构造 typed continue request，不允许构造 JSON 字符串。
- `OnHitAction` evidence 中仍可 dump action JSON 作为审计 artifact，但必须从 typed request 在 artifact 边界生成。

### 4. Replay runtime 仍以 action JSON 字符串传 step

当前 replay 路径中：

- `load_replay_jsonl_actions` 返回 `std::vector<std::string>`。
- `write_replay_plan` 接收 `std::vector<std::string> actions`，内部逐条 `parse_json(actions[i])`。
- `replay_action_text(..., const std::string &line, ...)` 仍以 action JSON 文本作为执行输入。
- legacy single-action replay 仍通过 `dump_json(plan)` 传给 replay execution。

下一轮要求：

- 引入 typed replay runtime step，例如 `ReplayActionStep`：
  - `step_id`
  - `name`
  - `enabled`
  - `failure_policy`
  - `ActionRequest action`
  - 可选 `Json original_action_for_artifact` 或等价审计边界数据
- JSONL / structured replay plan 文件读取后立即 parse 成 typed replay step。
- replay 执行层只接收 typed step，不接收 action JSON string。
- replay plan 写出仍是 JSON 文件格式，但写文件时从 typed step dump artifact JSON，不把 JSON string 作为内部状态。
- `write_replay_plan` 不再以 `std::vector<std::string>` 作为内部 API；如果必须保留兼容入口，只能作为边界 adapter，并立即转换为 typed steps。

### 5. Action response 多行输出仍通过 JSON AST 传递

当前为兼容 on-hit/replay 子 action 多行输出，`ActionResult` 使用 `std::vector<Json> prelude_responses`。

下一轮要求：

- 引入 typed output container，例如：
  - `ActionOutput { std::vector<ActionResult> prelude; ActionResult final; }`
  - 或 `ActionEmission { ActionResult result; std::vector<ActionResult> child_results; }`
- 子 action response 在内部仍是 typed result，不是 `Json`。
- 最终多行 JSON 只在 CLI/daemon boundary dump。
- `prelude_responses` 删除。

## 允许保留的字符串

不要把“业务值是字符串”和“内部协议是字符串”混在一起。以下字符串可以保留：

- GDB expression、location、raw MI command、catchpoint selector。
- 文件路径、session id、evidence id、hypothesis id/title/description。
- error message、debug text、human-readable summary、report Markdown。
- GDB/MI raw lines、inferior stdout/stderr、evidence raw/summary/view 内容。
- 外部协议和 artifact 格式：CLI/daemon JSON、replay plan/JSONL 文件、evidence payload、
  session files、report。

本轮要移除的是：

- action JSON 文本作为内部 action 请求。
- response JSON 文本作为内部 action 结果。
- generic `Json` AST 作为内部 request/result 传输字段；天然半开放 response metadata 可以使用结构化 KV。
- `std::vector<std::string>` 保存待执行 action。
- parse/dump JSON 只为了在内部模块间传递 action 或 result。

## 实现要求

### 1. 类型化 ActionRequest payload

- 为各 action 增加明确 payload struct。
- `ActionRequest` 用 `std::variant` 或等价 typed union 持有 payload。
- 不再用一个大 struct 加所有 action 的通用字段作为主要模型。
- `parse_action_request(const Json&)` 是唯一 action JSON 输入 adapter。
- 旧 action JSON schema 保持兼容。

### 2. 结构化 ActionResult payload

- 对固定语义字段使用 `ActionResult` 的明确成员，例如 `ok`、`action`、`error`、`evidence`。
- 对天然半开放的 response 扩展字段，使用结构化 KV，例如 `ResultField` / `ResultObject` / `ResultArray`。
- 删除 `Json fields`。
- 删除 `std::vector<Json> prelude_responses`。
- `ActionResult -> Json` 的转换集中在输出 adapter。
- response 字段名和 smoke 断言必须保持兼容。

### 3. 删除 legacy string handler

- 删除 `handle_action_line_legacy`。
- 删除 handler 内部直接写 response JSON 的路径。
- `handle_action_line` 可以保留名字，但签名应是 typed boundary：
  - 边界：`handle_action_json(..., const Json&) -> ActionOutput`
  - 内部：`dispatch_action(ActionContext&, const ActionRequest&) -> ActionOutput`
- 不允许 action handler 接收 `std::string line` 作为 action 输入。

### 4. On-hit typed actions

- `OnHitPolicy::actions` 改成 typed request vector。
- `parse_on_hit_policy` 解析 JSON 后立即产出 typed on-hit action。
- `raw_mi` 继续禁止作为 on-hit action。
- timeout/deadline 注入通过 typed payload 完成。
- wrapper evidence 中的 action/response JSON 只能在 evidence 写入边界生成。

### 5. Replay typed steps

- 引入 `ReplayActionStep` / `ReplayActionPlan` runtime structs。
- replay JSONL 和 replay plan 读取后立即转换为 typed runtime plan。
- replay execution 不接收 action JSON string。
- replay failure policy、skipped step、warning/error evidence 语义保持不变。
- replay artifact 文件仍保持 `gdb-agent-replay-plan-v1` schema。

### 6. JSON builder 边界

- 保留 JSON dump helper，但只能用于：
  - CLI/daemon response 输出。
  - replay plan/session/evidence/report artifact 写入。
  - client request 构造，因为这是 daemon wire boundary。
- 禁止在内部 action/probe/replay/session runtime API 中用 `Json` 或 JSON string 表示 request/result。

## 可写范围

允许修改：

- `src/cli/action.hpp`
- `src/cli/action.cpp`
- `src/cli.cpp`
- `src/replay/replay_plan.hpp`
- `src/replay/replay_plan.cpp`
- `CMakeLists.txt`，仅当需要新增 runtime 文件。
- 新增 `src/cli/*.hpp` / `src/cli/*.cpp`
- 新增 `src/replay/*.hpp` / `src/replay/*.cpp`
- 新增或修改 `src/workflow/probe_runtime.*`，如果拆 on-hit/probe runtime 时需要。
- `docs/ai/progress.md`
- `docs/ai/handoff.md`
- `docs/ai/decision.md`，仅当新增或改变项目级决策。

原则上不要修改：

- `docs/agent_actions.md` / `.en.md`，除非发现文档和实际行为不一致。
- `docs/evidence_model.md` / `.en.md`，除非 evidence 字段语义发生变化。
- `docs/task_format.md` / `.en.md`。
- report 内容。
- sanitizer、MI parser、hypothesis assertion 行为。

禁止：

- 修改外部 CLI 命令语法。
- 修改用户可见 action JSON schema。
- 修改 response JSON 字段语义。
- 修改 replay plan schema。
- 修改 evidence raw 文件布局。
- 引入 protobuf、IDL/codegen 或第三方 JSON 库。
- 为了“无字符串”删除业务字符串字段，例如 expression、location、error、evidence id。

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

- daemon `action` 仍返回兼容的单行或多行 JSON response。
- on-hit action 的子 action JSON 行仍按现有 smoke 预期出现。
- replay `continue_on_error` / `stop_on_error` / skipped step 行为不变。
- save-action 写出的 JSONL 和 structured replay plan 仍与现有 schema 兼容。
- `ReplayStep` / `ReplayRun` / `OnHitAction` evidence 字段语义不变。
- ToolError、command_evidence、probe hit、hypothesis index、session summary、report/assets 引用保持兼容。

## 完成标准

本轮完成时应满足：

- `ActionRequest` 不再持有 raw `Json` payload。
- `ActionResult` 不再持有 `Json fields` 或 `std::vector<Json> prelude_responses`。
- 删除 `handle_action_line_legacy`。
- on-hit policy 不再用 `std::vector<std::string>` 保存 action JSON。
- on-hit 执行不再 parse/dump action JSON 来修改 timeout/deadline。
- replay runtime 不再以 `std::string line` / action JSON string 作为执行输入。
- replay plan writer 的内部 API 不再接收 `std::vector<std::string>` action JSON。
- JSON/string 只出现在明确的外部边界、artifact 写入或业务字符串字段中。
- 外部行为与现有 CTest/smoke 完全兼容。
