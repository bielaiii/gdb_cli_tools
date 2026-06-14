# Next CLI Task: session concurrency and operation executor

## 目标

下一轮进入 Execution mode，聚焦 **先整理 session 多操作并发逻辑和串行执行边界**。

本轮只做执行模型重构，不实现高层 record action，不实现 action 组二进制持久化。record/replay 的后续设计依赖
本轮建立的执行边界，但本轮不要提前扩展用户可见 record 功能。

完成后，代码应具备一个清晰的 session 操作模型：

1. Agent/daemon/CLI 请求可以被接收和解析。
2. 同一个 GDB live session 的 action 执行必须串行化。
3. 普通 action、replay step 和 on-hit action 应尽量复用同一个 typed execution boundary。
4. 不能让多个调用路径同时向同一个 GDB/MI pipe 写命令。
5. 后续 record 能在这个 boundary 上记录“被 session 接受的高层 action 顺序”。

## 当前背景

当前已具备：

- daemon/create/action/list/status/finish/close/shutdown live session flow。
- typed `ActionRequest` / `ActionOutput` / `ActionResult` 基础。
- replay runtime 已能按 plan step 顺序执行高层 action，并依赖 typed result 判断 success/failure。
- on-hit action 已能在 probe 命中后执行高层 action。
- replay、on-hit、hypothesis、probe runtime 已从 `src/cli.cpp` 中拆出一部分，但 action dispatch 仍是主要集中点。

当前缺口：

- 普通 action、replay step、on-hit action 的执行入口仍不够统一。
- session 操作对象边界不够明确，后续加 record 时容易出现旁路记录、乱序或重复记录。
- GDB/MI command 串行化主要依赖当前调用路径的事实顺序，还没有显式 executor/queue 抽象。
- 如果未来引入多线程或 coroutine worker，没有清晰位置保证同一 session 的 GDB/MI 写操作互斥且有序。

## 必须遵守的设计约束

遵守 `docs/ai/decision.md`：

- D002：GDB 接入使用 GDB/MI，不使用 PTY。
- D003：AI 默认使用高层 action。
- D004：raw evidence 是权威，summary/report 是有损入口。
- D005：`session_snapshot.json` / `session_summary.json` 不是 live session 恢复文件。
- D006：replay 保存并重放高层 action；新 session 产生新的 evidence id。
- D012：内部 action 流程使用 typed structs，JSON/string 只作为边界格式。
- D014：先建立 session 串行执行边界，再实现 record。

禁止：

- 本轮不新增 `record_start` / `record_stop` / `record_status` 等用户可见 action。
- 本轮不实现 GDB `record full`、reverse debugging 或 process record。
- 不让多个线程或调用路径同时向同一个 GDB/MI session 写命令。
- 不用 shared mutable state + 大量 mutex 替代 per-session queue/actor 语义。
- 不解析 action response JSON 文本来驱动 replay、on-hit 或后续 record 控制流。
- 不改变 replay plan schema、task format、evidence raw 文件布局。
- 不改变 Core Dump Mode 静态/动态 action 边界。
- 不做 unrelated cleanup 或大范围格式化。

## 设计方向

推荐模型：

```text
Agent/CLI/daemon request boundary
        |
        v
typed ActionRequest
        |
        v
SessionOperationExecutor
        |
        +-- state guard
        +-- optional future record hook
        +-- action handler execution
        +-- replay/on-hit sub-action execution through same boundary
        +-- evidence/result attribution
        |
        v
typed ActionOutput / ActionResult
```

本轮不强制引入 OS thread。可以先实现同步 executor/queue abstraction，把边界立住；后续再把 executor
挂到 coroutine 或 dedicated worker thread。若本轮确实引入线程，必须是 per-session queue/actor 模型，
不能多个线程直接共享写 `GdbSession` 的 MI pipe。

## 实现范围

允许修改：

- `src/cli/action_dispatch.*`
  - 拆出或收敛 action execution entry。
  - 保持 external response 兼容。
- 可新增 `src/cli/session_executor.*` 或 `src/workflow/session_executor.*`
  - 名称按现有目录职责选择。
  - 表达 session 级操作执行边界。
- `src/replay/replay_runtime.*`
  - replay step 应调用新的 shared execution function / executor entry，而不是复制 action 判断逻辑。
  - 保持 `ReplayStep` / `ReplayRun` evidence 语义不变。
- `src/workflow/probe_runtime.*`
  - on-hit action 应调用新的 shared execution function / executor entry。
  - 如果完全迁移风险过大，可先留下小范围 adapter，但 handoff 必须记录剩余差距。
- `src/cli/action.*`
  - 仅在 executor 需要更清晰 typed payload/result helper 时修改。
- `tests/`
  - 增加不依赖 GDB 的 executor/order/state guard 单元测试。
- `scripts/`
  - 必要时扩展现有 Linux + GDB smoke，验证 replay/on-hit 行为未回归。
- `CMakeLists.txt`
  - 添加新增测试。
- 文档：
  - `docs/ai/progress.md`
  - `docs/ai/handoff.md`
  - `docs/ai/decision.md` 仅当本轮发现需要新增或改变项目级 decision。

原则上不要修改：

- `docs/agent_actions.md` / `.en.md`，除非外部 action 行为或语义变化。
- `docs/evidence_model.md` / `.en.md`，除非 evidence/session artifact schema 变化。
- replay plan JSON schema。
- report 格式。

## 具体要求

### 1. 建立 session operation executor

新增或抽出一个明确的执行入口，至少能表达：

- 输入：typed `ActionRequest`、session/task/outcome/probe context。
- 输出：typed `ActionOutput`。
- 同一 session 内 action 语义串行。
- state guard 在执行前稳定生效。
- action handler 不直接依赖 serialized response text。

该 executor 可以先是同步函数/类，但要避免继续把普通 action、replay step、on-hit action 分散成互不相关的控制流。

### 2. 普通 action 迁移

daemon/CLI `action` 路径应通过新的 executor 执行。

要求：

- 现有 action response JSON 兼容。
- 现有 state guard error 行为兼容。
- 现有 evidence id 和 action result 字段尽量不变。
- `raw_mi` 仍要求 `risk:"advanced"`。

### 3. Replay step 迁移

replay runtime 中每个 step 应通过新的 executor 或共享 execution function 执行。

要求：

- `continue_on_error` / `stop_on_error` 语义不变。
- `ReplayStep` 的 `success` / `failed` / `skipped` 语义不变。
- action 返回 `ok:false` 时仍能记录 `ToolError` 并链接 `error_evidence`。
- task fingerprint mismatch / force warning 语义不变。
- 不通过解析 action response JSON 文本判断 step 成败。

### 4. On-hit action 迁移

probe on-hit action 应通过新的 executor 或共享 execution function 执行。

要求：

- on-hit `continue_on_error` / `stop_on_error` 语义不变。
- `OnHitAction` evidence 语义不变。
- `continue_after_hit` 行为不变。
- `raw_mi` 仍不能作为 on-hit action。
- 若 on-hit 因循环依赖或上下文差异无法完全迁移，本轮至少抽出共同底层 execution function，并在
  `docs/ai/handoff.md` 记录剩余差距。

### 5. 为后续 record 预留 hook

本轮不实现 record action，但 executor 设计要能放置未来 hook：

```text
action accepted by session
        |
        +-- future record append intent
        |
        v
execute action
        |
        +-- future record observed metadata
```

不要在本轮写入 record 文件或新增 record state。只需保证代码结构有清晰位置接入。

### 6. 测试

新增不依赖 GDB 的测试，优先覆盖：

- executor 能接收 typed `ActionRequest` 并返回 typed `ActionOutput`。
- state guard failure 不执行 handler。
- replay step 使用 typed result 判断 success/failure。
- `stop_on_error` 后续 step 仍记录 skipped。
- on-hit action 使用 typed result 判断 success/failure。
- 不需要解析 response JSON 文本即可完成控制流。

继续运行现有 smoke，确保没有行为回归。

如果本轮新增 executor 但难以构造完全不依赖 GDB 的测试，可以至少通过现有 `replay_plan_tests`、
`mi_summary_tests`、daemon smoke 和 capability smoke 覆盖，并在 handoff 中说明测试缺口。

## 保持外部行为兼容

必须保持：

- 现有 CLI/daemon action schema 兼容。
- 现有 response 字段含义兼容。
- 现有 `save_action` / `save-action` 仍可用。
- 现有 JSON plan 和 JSONL replay 仍可读取。
- 现有 replay failure policy、task fingerprint、force warning 语义兼容。
- 现有 on-hit action 行为兼容。
- 现有 report replay audit 继续工作。
- `raw_mi` 仍必须显式 `risk:"advanced"`。
- Core Dump Mode 仍是静态取证模式。

## 文档和交接

本轮结束时：

- 更新 `docs/ai/progress.md`，记录 executor / concurrency boundary 的完成情况。
- 覆写 `docs/ai/handoff.md`，记录：
  - 实际修改的文件。
  - 普通 action、replay step、on-hit action 是否都已迁移。
  - 是否引入 OS thread；如果没有，说明本轮建立的是同步 executor/queue boundary。
  - 验证命令和结果。
  - 剩余限制，尤其是未来 record hook 的位置和未迁移路径。
- 只有新增或改变项目级 decision 时才更新 `docs/ai/decision.md`；D014 已提供本轮设计依据。

## 验证

至少运行：

```bash
cmake --build build
./build/gdb-agent check examples/segfault_task.md
./build/replay_plan_tests
ctest --test-dir build --output-on-failure
git diff --check
```

如果改动触及 on-hit/probe/replay runtime，应额外运行：

```bash
./scripts/smoke_replay_setup_plan_flow.sh
./scripts/smoke_daemon_action_flow.sh
```

如果当前环境没有 `gdb`：

- 仍需运行不依赖 GDB 的构建和单元测试。
- 在 `docs/ai/handoff.md` 和最终回复中明确记录 live smoke 未运行原因。

## 完成标准

本轮完成时应满足：

- 代码中存在清晰 session operation executor 或等价边界。
- 同一 session 的 GDB/MI command 仍保持串行。
- 普通 action 已通过该边界执行。
- replay step 已通过该边界或共享底层 execution function 执行。
- on-hit action 已通过该边界或共享底层 execution function 执行；若未完全迁移，handoff 明确记录原因。
- replay/on-hit 不依赖 response JSON 文本判断控制流。
- 现有 replay、on-hit、core mode、hypothesis、evidence/report 行为无回归。
- 为后续 record append intent / observed metadata 留出明确接入位置。
- `docs/ai/progress.md` 和 `docs/ai/handoff.md` 已记录实际完成内容、验证结果和遗留限制。
- 本轮相关改动已按仓库约定 commit 并 push。
