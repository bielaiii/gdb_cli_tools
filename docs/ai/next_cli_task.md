# Next CLI Task: high-level action record

## 目标

下一轮进入 Execution mode，开始实现 **高层 action record 功能**。

record 的含义是：记录 Agent 提交并被 session 接受的高层 action intent，用于后续 replay。它不是
GDB `record full`、reverse debugging 或 process record。

本轮目标：

1. 新增最小高层 record action。
2. 默认把 record actions 保存到 session 内存中。
3. 通过上一轮新增的 `SessionOperationExecutor` 记录普通 action 的接受顺序。
4. 提供持久化 action 组接口，优先写版本化二进制 artifact。
5. 提供字符串格式化/export 接口，方便人工检查。
6. replay 能读取新 record artifact，并继续兼容旧 JSON/JSONL replay。

本轮不要做 GDB process record/reverse debugging，不要把 record 做成 snapshot 恢复，也不要改变 Core
Dump Mode 的静态/动态 action 边界。

## 当前背景

当前已具备：

- `SessionOperationExecutor` 和 per-session operation lock。
- 普通 action、replay step、on-hit action 已通过 executor 或共享执行入口。
- replay 支持一整组高层 action，并记录 `ReplayStep` / `ReplayRun` evidence。
- `save_action` / `save-action` 仍可用，会写 JSONL 并 rebuild 结构化 JSON replay plan。
- replay runtime 已不依赖 response JSON 文本判断 success/failure。
- `docs/ai/decision.md` 已有：
  - D013：高层 action record 是 replay 的运行期来源。
  - D014：先建立 session 串行执行边界，再实现 record。

当前缺口：

- 没有 `record_start` / `record_status` / `record_stop` / `record_discard` 高层 action。
- 没有 session 级内存 `RecordingState`。
- executor 还没有 record append hook。
- 没有版本化二进制 action group artifact。
- 没有人类可读 formatter/export。
- replay 还不能读取新二进制 record artifact。

## 必须遵守的设计约束

遵守 `docs/ai/decision.md`：

- D003：AI 默认使用高层 action。
- D004：raw evidence 是权威，summary/report 是有损入口。
- D005：`session_snapshot.json` / `session_summary.json` 不是 live session 恢复文件。
- D006：replay 保存并重放高层 action；新 session 产生新的 evidence id。
- D012：内部 action 流程使用 typed structs，JSON/string 只作为边界格式。
- D013：record 记录高层 action intent，默认内存保存，提供持久化接口，二进制优先，提供字符串格式化接口。
- D014：record 必须接在 session 串行执行边界上。

禁止：

- 不实现 GDB `record full`、reverse debugging 或 process record。
- 不把旧 action 的执行结果当作 replay 的新结果。
- 不把 `session_snapshot.json` / `session_summary.json` 设计成 record 恢复文件。
- 不让 record 绕过 `SessionOperationExecutor`。
- 不把 JSONL 作为新 record 抽象的唯一权威状态。
- 不解析 action response JSON 文本来驱动 record/replay/on-hit 控制流。
- 不改变 task format、Core Dump Mode 静态边界或 evidence raw 文件布局。
- 不做 unrelated cleanup 或大范围格式化。

## 用户可见 action

新增最小 action：

```json
{"action":"record_start","name":"repro","failure_policy":"stop_on_error"}
{"action":"record_status"}
{"action":"record_stop"}
{"action":"record_discard"}
```

可选但非必须：

```json
{"action":"record_pause"}
{"action":"record_resume"}
```

本轮如果实现 pause/resume 会扩大范围；优先完成 start/status/stop/discard。

行为要求：

- `record_start` 创建当前 session 的内存 recording buffer。
- 默认同一 session 只允许一个 active recording；重复 start 返回 `ok:false` 并写 `ToolError`。
- `record_status` 返回是否 active、name、step count、failure policy、artifact path/export path。
- `record_stop` 把当前内存 buffer 持久化为可 replay artifact，结束 active recording。
- `record_discard` 丢弃内存 buffer，不写最终 replay artifact。
- 如果没有 active recording，`record_stop` / `record_discard` 返回稳定 `ok:false` 并写 `ToolError`。
- inferior segfault 或 stop event 不应丢失 gdb-agent 内存中的 recording buffer。

默认不自动记录：

- `record_*`
- `replay`
- `finish_session`
- `save_action`

`raw_mi` 默认不记录。若本轮实现 opt in，必须要求显式字段，例如：

```json
{"action":"record_start","name":"repro","include_raw_mi":true}
```

并在 metadata 中标记 advanced risk。为控制范围，本轮可以先不实现 `include_raw_mi`，直接默认跳过
`raw_mi`。

## 实现范围

允许修改：

- `src/cli/action.hpp` / `src/cli/action.cpp`
  - 新增 record action kind 和 typed payload/result fields。
- `src/cli/action_dispatch.cpp`
  - 处理 `record_start` / `record_status` / `record_stop` / `record_discard`。
- `src/cli/session_executor.*`
  - 增加 record append hook。
  - hook 应在 action 被 session 接受后记录 action intent；不要记录被 state guard 拒绝的 action。
  - hook 不记录 `record_*` / `replay` / `finish_session` / `save_action` / 默认 `raw_mi`。
- 新增 `src/replay/record_store.*` 或等价模块
  - 保存 session 级 `RecordingState`。
  - 持久化二进制 action group。
  - 导出人类可读文本。
- `src/replay/replay_runtime.*`
  - 支持读取新二进制 action group artifact。
  - 继续兼容旧 JSON plan 和 JSONL replay。
- `src/cli/action_context.hpp`
  - 如需要，把 `RecordingState` 接入 `ActionContext` 或 session-scoped state。
- `src/cli.cpp`
  - daemon/live session 和 serve loop 持有 recording state。
  - finish/close 时如 active recording 未 stop，应在 handoff/report 不强制处理；本轮可返回
    status 供 Agent 自行 stop/discard。
- `tests/`
  - 新增不依赖 GDB 的 record store / binary format / formatter 测试。
- `scripts/`
  - 新增或扩展 Linux + GDB smoke，验证 record -> stop -> replay。
- `CMakeLists.txt`
  - 添加新源码和测试。
- 文档：
  - `docs/agent_actions.md`
  - `docs/agent_actions.en.md`
  - `docs/evidence_model.md`
  - `docs/evidence_model.en.md`
  - `docs/ai/progress.md`
  - `docs/ai/handoff.md`
  - `docs/ai/decision.md` 仅当实现中新增或改变项目级 decision。

原则上不要修改：

- Core Dump Mode 的 action 边界。
- GDB/MI parser、type sanitizer、hypothesis assertion 语义。
- report 格式，除非新增 record artifact audit 最小入口确实需要。
- 既有 JSON replay plan schema；新二进制 artifact 可以是新格式，旧格式继续兼容读取。

## 具体要求

### 1. RecordingState

实现 session 级内存状态，例如：

```cpp
struct RecordingState {
    bool active = false;
    std::string name;
    std::string failure_policy;
    bool include_raw_mi = false;
    std::vector<ActionRequest> actions;
    std::filesystem::path binary_path;
    std::filesystem::path export_path;
};
```

要求：

- 内存 buffer 是 record 运行期权威状态。
- action 以 typed `ActionRequest` 保存。
- 不保存旧 action response 文本作为 replay 控制流。
- 如记录 observed metadata，只能作为审计字段，replay 不得依赖旧 observed result。

### 2. Executor append hook

在 `SessionOperationExecutor` 中加入 hook：

```text
parse typed request
state guard / dispatch accepted
record append intent
execute action
optional observed metadata
```

本轮可以采用更保守顺序：在 action 成功通过 state guard 并完成执行后记录 intent，但必须避免记录 state
guard 拒绝的 action。handoff 中说明选择的语义。

必须保证：

- replay step 不自动进入 record，避免 record replay 自身造成递归/污染。
- on-hit action 是否记录要明确：
  - 推荐本轮默认记录普通 Agent action，不记录 on-hit 自动 action。
  - 如果实现记录 on-hit，必须在 action metadata 标记 origin。
  - 为控制范围，本轮默认不记录 on-hit action。
- `record_stop` 自身不记录进当前 buffer。

### 3. 二进制 action group artifact

新增版本化二进制格式。最低字段：

- magic，例如 `GDBA_REC1`
- format version
- plan name
- source session id
- created_at
- task metadata / task fingerprint
- failure policy
- step count
- typed action payload list

实现要求：

- 文件写到 `<assets>/replay/`。
- 扩展名建议 `.gar` 或 `.record`，自行选择并文档化。
- 写入使用临时文件 + atomic rename，避免半写。
- reader 必须校验 magic/version/step count，并返回稳定错误。
- 不引入 protobuf、IDL/codegen 或第三方序列化库。

二进制内部如何编码 typed action：

- 本轮可以用 length-prefixed `action_request_to_json()` 作为 payload，以二进制 envelope 提供 magic/version、
  校验和边界；这仍满足“二进制 artifact 为权威、人类文本为 export”的要求。
- 不要把 action response JSON 文本写入 payload。

### 4. 字符串格式化/export

为二进制 action group 提供人类可读 export。

最低要求：

- record stop 后生成 `<assets>/replay/<name>.json` 或 `<name>.md` 作为 export/view。
- export 包含 schema/name/source_session_id/task fingerprint/failure_policy/actions[]。
- export 明确是 human-readable view，不是运行期权威状态。
- 如果生成 JSON export，旧工具链可以继续人工检查；replay 可优先读二进制，也可以继续读 JSON。

### 5. Replay 集成

replay runtime 支持：

- 新二进制 record artifact。
- 旧结构化 JSON plan。
- 旧 JSONL action list。

要求：

- 从二进制 artifact 读取后转换为 typed `ActionRequest` list。
- 继续执行 `ReplayStep` / `ReplayRun` evidence。
- `continue_on_error` / `stop_on_error` 语义不变。
- task fingerprint mismatch 默认拒绝，`force` 时 warning，行为与 JSON plan 一致。

### 6. CLI/daemon 集成

daemon action flow 支持 record actions：

```bash
./build/gdb-agent action S1 '{"action":"record_start","name":"repro"}' --socket /tmp/gdb-agent.sock
./build/gdb-agent action S1 '{"action":"backtrace"}' --socket /tmp/gdb-agent.sock
./build/gdb-agent action S1 '{"action":"locals"}' --socket /tmp/gdb-agent.sock
./build/gdb-agent action S1 '{"action":"record_stop"}' --socket /tmp/gdb-agent.sock
./build/gdb-agent replay S1 repro --socket /tmp/gdb-agent.sock
```

如果现有 CLI `replay S1 NAME` 只查 `.json` / `.jsonl`，本轮应扩展为也查新二进制 artifact。

### 7. 测试

新增不依赖 GDB 的单元测试，至少覆盖：

- start/status/stop/discard state transition。
- 普通 action append 到内存 buffer。
- 不记录 `record_*`、`replay`、`finish_session`、`save_action`。
- `raw_mi` 默认不记录。
- 二进制 artifact round-trip。
- formatter/export 输出包含 name、source session、failure policy、task fingerprint 和 actions。
- task fingerprint mismatch 默认拒绝，force warning 与 JSON plan 语义一致。

新增或扩展 Linux + GDB smoke，至少覆盖：

- daemon create。
- `record_start`。
- 执行一组真实高层 action，例如 `backtrace`、`locals`，或 stopped session 上的 `evaluate`。
- `record_status` step count 正确。
- `record_stop` 生成二进制 artifact 和 human-readable export。
- 新 session replay 该 artifact，并产生新的 `ReplayStep` / `ReplayRun` evidence。
- finish report 的 replay audit 仍能展示 replay 执行。

如果当前环境没有 `gdb`，仍需运行不依赖 GDB 的构建和单元测试，并在 handoff 与最终回复中说明 live
smoke 未运行原因。

## 保持外部行为兼容

必须保持：

- 现有 `save_action` / `save-action` 仍可用。
- 现有 JSON plan 和 JSONL replay 仍可读取。
- 现有 replay failure policy、task fingerprint、force warning 语义兼容。
- 现有 replay report audit 继续工作。
- 现有 on-hit action 行为兼容。
- 现有 CLI/daemon action response 字段语义尽量兼容。
- `raw_mi` 仍必须显式 `risk:"advanced"`。
- Core Dump Mode 仍是静态取证模式。

## 文档和交接

执行结束时：

- 更新 `docs/agent_actions.md` / `.en.md`，说明 record action 用法。
- 更新 `docs/evidence_model.md` / `.en.md`，说明二进制 record artifact、human-readable export 和 replay
  evidence 关系。
- 更新 `docs/ai/progress.md`。
- 覆写 `docs/ai/handoff.md`，记录：
  - 实际修改的文件。
  - record append 语义：执行前还是执行后记录、哪些 action 会被排除。
  - 二进制格式概要和 export 路径。
  - replay 是否支持新 artifact。
  - 验证命令和结果。
  - 剩余限制。
- `docs/ai/decision.md` 仅当新增或改变项目级 decision；D013/D014 已覆盖本轮设计。

## 验证

至少运行：

```bash
cmake --build build
./build/gdb-agent check examples/segfault_task.md
./build/replay_plan_tests
ctest --test-dir build --output-on-failure
git diff --check
```

新增 record 测试后，也要直接运行对应测试二进制。

如果新增或扩展 Linux + GDB smoke：

```bash
./scripts/smoke_daemon_action_flow.sh
./scripts/smoke_replay_setup_plan_flow.sh
```

以及新增 smoke 脚本本身。

## 完成标准

本轮完成时应满足：

- Agent 可以使用高层 action 启动、查看、停止和丢弃 record。
- record 默认使用 session 内存 buffer 保存高层 action intent。
- 普通 Agent action 通过 executor append 到 recording buffer。
- 默认不记录 `record_*`、`replay`、`finish_session`、`save_action` 和 `raw_mi`。
- `record_stop` 能生成版本化二进制 action group artifact。
- 工具能导出/生成 human-readable action group view。
- replay 能读取新二进制 artifact 并执行一整组 action。
- 旧 JSON/JSONL replay 继续可用。
- 测试覆盖内存状态、二进制 round-trip、formatter/export、replay integration 和 live smoke。
- 文档明确 record 不是 GDB process record，也不是 snapshot 恢复。
- `docs/ai/progress.md` 和 `docs/ai/handoff.md` 已记录实际完成内容、验证结果和遗留限制。
- 本轮相关改动已按仓库约定 commit 并 push。
