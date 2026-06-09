# Next CLI Task

## 目标

本轮进入一轮偏长的 **capability matrix hardening**：用真实 Linux + GDB flow
扩展 catchpoint/probe 能力，校准真实 GDB 输出下的 evidence/report/artifact 链路，并补齐
Agent 长会话排查时最容易依赖的一组回归覆盖。

这轮任务可以做得比普通小任务更久、更完整，但必须仍然围绕一个闭环：

1. 扩展 `catchpoint_set` 支持更多实用 catchpoint event。
2. 扩展 `examples/capability_fixture.cpp` 和 Linux smoke，让真实 GDB 停点覆盖更多 stop reason。
3. 确认每个新增路径都会产生可审计 raw evidence、低噪声 summary、probe metadata、session summary
   和 report 引用。
4. 同步更新 Agent-facing 文档、MVP acceptance、known limitations、progress 和 handoff。

不要把本轮做成大规模架构重构。可以在现有 `src/cli.cpp` / `src/gdb` / `src/evidence`
结构内做必要的小型 helper，但不要拆 action dispatcher、不要重写 daemon 协议、不要引入大型依赖。

## 背景

当前已有能力：

- Run Mode / Core Dump Mode / daemon live session 已有 smoke 和 CTest 覆盖。
- `catchpoint_set` 当前支持：
  - `event:"throw"` -> GDB `catch throw`
  - `event:"catch"` -> GDB `catch catch`
- breakpoint/watchpoint/catchpoint 都复用 probe metadata、on-hit policy、probe hit evidence、
  `probe_list` 和 finish-time `assets/probes.json`。
- `scripts/smoke_capability_matrix.sh` 已覆盖真实 breakpoint/watchpoint、throw/catch catchpoint、
  raw MI、hypothesis、replay、stdin/env/stdout/stderr、thread crash 和 core dump flow。
- summary/sanitizer 已增强，但仍需要更多真实 GDB 输出校准。

当前短板：

- catchpoint 仍只覆盖 C++ exception throw/catch，缺少 syscall、fork/exec 等常见调试事件。
- capability fixture 对不同 stop reason 的真实 GDB/MI 输出覆盖还可以更系统。
- report / evidence index / probes / session summary 的一致性检查可以更深入，尤其是新增 stop reason
  对应的 evidence kind、probe kind/event、hit count 和 report 引用。
- `docs/known_limitations.md` 里“其他 catchpoint event 尚未实现”的限制需要随着实现更新。

## 范围

### 1. 扩展 catchpoint_set event

在现有 `catchpoint_set` 基础上，新增一组稳定、Linux + GDB 下常见且适合自动 smoke 的 event。

优先实现：

- `event:"syscall"`，默认映射到 `catch syscall`。
- `event:"syscall"` + `name` 或 `syscall` 字段，映射到指定 syscall，例如 `catch syscall write`。
- `event:"fork"`，映射到 `catch fork`。
- `event:"vfork"`，映射到 `catch vfork`。
- `event:"exec"`，映射到 `catch exec`。

字段设计建议：

```json
{"action":"catchpoint_set","event":"syscall","name":"write"}
{"action":"catchpoint_set","event":"syscall","syscall":"write"}
{"action":"catchpoint_set","event":"fork"}
{"action":"catchpoint_set","event":"vfork"}
{"action":"catchpoint_set","event":"exec"}
```

要求：

- 保持 `event:"throw"` 和 `event:"catch"` 兼容不变。
- `syscall` 的 selector 字段优先接受字符串；如果当前代码风格更适合，也可以接受数字 syscall id。
- 对 syscall selector 做保守校验：不能为空，不能包含 shell/control 字符，不允许拼接任意 GDB 命令。
- unsupported event 或非法 selector 必须返回 `ok:false`，写 `ToolError` evidence。
- Core Dump Mode 下新增 catchpoint event 必须继续被 state guard 拒绝，并写 `ToolError` evidence。
- action response、probe metadata、probe hit evidence、`probe_list` 和 `assets/probes.json`
  都要能看出 catchpoint 的 `event` 和可选 selector。
- 如果 GDB 在某些环境不支持某个 catchpoint command，不要伪装成功：返回 `ok:false`，
  并通过 command evidence / ToolError 保留 GDB 原始拒绝信息。

### 2. 扩展 capability fixture

扩展 `examples/capability_fixture.cpp`，新增确定性的模式，用于触发新增 catchpoint。

建议新增模式：

- `syscall`：执行一次或多次稳定 syscall，优先选择 `write`，因为 fixture 已有 stdout/stderr。
- `fork`：执行 `fork()`，父进程等待子进程退出，子进程立即 `_exit(0)`。
- `exec`：执行一个可控、低风险的 `exec` 路径。优先考虑：
  - `fork()` 子进程中 `execl("/bin/true", "true", nullptr)`；
  - 或执行当前 fixture 自身的一个 `exec-child` 模式，避免依赖复杂外部程序。
- `vfork`：如果实现成本和稳定性可控则覆盖；如果环境差异太大，可以实现 action 支持但在 smoke
  中降级为 best-effort，并在 handoff 解释。

要求：

- fixture 只用于测试，不要引入复杂业务逻辑。
- Linux-only 代码可用 `#ifdef __linux__` 隔离；非 Linux 构建不能失败。
- 不要让 fork/exec 测试遗留后台进程。
- 输出尽量短、稳定，避免污染 smoke 断言。

### 3. 扩展真实 Linux smoke

重点扩展 `scripts/smoke_capability_matrix.sh`。如果单个脚本过长且难维护，可以新增
`scripts/smoke_catchpoint_matrix.sh` 并在 `CMakeLists.txt` 加入 CTest。

必须覆盖：

- `catchpoint_set event:"syscall"` 成功或稳定失败路径。
- `catchpoint_set event:"syscall"` + selector，例如 `write`。
- `catchpoint_set event:"fork"`。
- `catchpoint_set event:"exec"`。
- unsupported event 稳定 `ok:false` + `ToolError` evidence。
- 非法 syscall selector 稳定 `ok:false` + `ToolError` evidence。
- Core Dump Mode 下新增动态 catchpoint event 被 state guard 拒绝。
- `probe_list` 中新增 catchpoint 的 metadata：
  - `kind:"catchpoint"`
  - `event`
  - selector/name/syscall（如果有）
  - comment/purpose
  - hit count
- finish 后：
  - `assets/probes.json` 含新增 catchpoint metadata。
  - `assets/session_summary.json` 的 probe hit count / evidence count 与 index 一致。
  - `assets/evidence/index.json` 中相关 raw/view/summary 文件存在。
  - report 引用的 evidence id 都能在 index 中找到。

建议覆盖但可按环境稳定性取舍：

- 验证 `CatchpointHit` evidence 对新增 event 能归属到对应 probe。
- 验证新增 catchpoint 的 on-hit policy 能执行至少一个低风险 action，例如 `threads` 或 `backtrace`。
- 验证 `continue_after_hit:true` 在 syscall catchpoint 上不会造成无限停住；如果 syscall 太频繁，
  选择更稳定的 event 或限制动作序列。

注意：

- `catch syscall` 可能在同一次 syscall 的 entry/return 都停住；smoke 断言不要假设只命中一次。
- `fork` / `exec` 可能改变 inferior/process 状态；测试要用独立 session，避免影响现有 probe flow。
- 如果某个 GDB/平台组合返回“不支持”，脚本可以将该 event 记录为 skip/best-effort，但必须仍覆盖
  action 的稳定失败语义和 evidence 链路。不要让测试随机失败。

### 4. 校准 evidence / summary / report

新增 catchpoint event 后，检查并必要时改进：

- `CatchpointHit` evidence summary 是否能表达 event 和 selector。
- `probe_list` response 是否足够让 Agent 区分不同 catchpoint。
- `assets/probes.json` 是否保留新增 event/selector/comment/purpose/deleted/hit_count。
- `session_summary.json` 是否继续准确统计：
  - `evidence_count`
  - `probe_hit_count`
  - `on_hit_action_count`
  - `on_hit_error_count`
  - `tool_error_count`
- Markdown report 的 Probes / Tool Errors / Limitations 区域是否仍然引用正确 evidence。
- raw MI、session log、raw evidence 文件仍然保留，summary 只作为有损视图。

不要新增“自动根因”或让工具替 Agent 下结论。

### 5. 文档同步

根据实际实现同步更新：

- `docs/agent_actions.md`
- `docs/agent_actions.en.md`
- `docs/known_limitations.md`
- `docs/mvp_acceptance.md`
- `docs/evidence_model.md`，仅当 evidence/report 字段或语义发生变化
- `docs/evidence_model.en.md`，同上
- `README.md`，仅当 quick demo / action 列表明显过期
- `docs/ai/progress.md`
- `docs/ai/handoff.md`

文档口径：

- 新增 catchpoint event 是高层 action 能力扩展，不改变 raw evidence 优先原则。
- syscall/fork/exec/vfork 是 Linux + GDB live session 能力；Core Dump Mode 仍是静态取证。
- 如果某个 event 在部分 GDB 环境中不支持，应说明工具会返回结构化失败和 evidence，而不是保证所有
  GDB 版本都可用。

仅当本轮引入新的项目级设计决策时才更新：

- `docs/ai/decision.md`

预计本轮大概率不需要新增 decision；它属于既有 D003/D004/D007/D011 下的能力扩展。

## 可写范围

允许修改：

- `src/cli.cpp`
- `src/gdb/gdb_session.cpp`
- `src/gdb/gdb_session.hpp`，仅当 catchpoint command helper 需要调整
- `src/gdb/mi_utils.cpp`
- `src/gdb/mi_utils.hpp`，仅当真实 GDB 输出需要 parser/summary 小修
- `src/evidence/evidence_store.cpp`
- `src/evidence/evidence_store.hpp`，仅当 catchpoint evidence 字段/summary 确有必要调整
- `src/report/report.cpp`
- `src/report/report.hpp`，仅当 report 聚合需展示新增信号
- `examples/capability_fixture.cpp`
- `scripts/smoke_capability_matrix.sh`
- 新增 `scripts/smoke_catchpoint_matrix.sh`，如确实比塞进现有脚本更清楚
- `CMakeLists.txt`，仅当新增测试 target 或 CTest
- `tests/*`，如需要新增不依赖 GDB 的 selector/parser/helper 单元测试
- `docs/agent_actions.md`
- `docs/agent_actions.en.md`
- `docs/known_limitations.md`
- `docs/mvp_acceptance.md`
- `docs/evidence_model.md` / `.en.md`，仅当 evidence 语义变化
- `README.md`，仅当 action/demo 列表需要同步
- `docs/ai/progress.md`
- `docs/ai/handoff.md`
- `docs/ai/decision.md`，仅当新增项目级 decision

不要修改：

- `docs/ai/current_goal.md`，除非执行中发现阶段目标必须调整。
- `docs/ai/next_cli_task.md`，除非执行中发现任务口径本身必须修正。
- 与本轮无关的格式化、重命名或全仓重构。

## 验证

至少运行：

```bash
cmake --build build
./build/gdb-agent check examples/segfault_task.md
ctest --test-dir build --output-on-failure
git diff --check
```

如果新增了单独测试 target 或脚本，也要直接运行它，例如：

```bash
./build/<new_test_target>
scripts/smoke_catchpoint_matrix.sh
```

Linux + GDB 环境下，必须确认新增 catchpoint flow 的 smoke 实际运行，而不是只被 skip。
如果当前机器缺少 GDB、权限不足、`catch syscall`/`catch fork`/`catch exec` 在该 GDB 版本不支持，
必须在 `docs/ai/handoff.md` 和最终回复中记录：

- 哪些验证已运行。
- 哪些验证被 skip 或降级为 best-effort。
- 原因是工具 bug、环境限制，还是 GDB capability 差异。

## 完成标准

本轮完成时应满足：

- `catchpoint_set` 至少新增 `syscall`、`fork`、`exec` 三类 event 的 action 支持或稳定失败语义。
- `throw` / `catch` 既有行为和文档不回退。
- 新增 event 的 selector 校验不会让 Agent 拼接任意 GDB command。
- capability fixture 有真实触发新增 event 的模式。
- Linux smoke 覆盖新增 event 的 set/list/hit/finish/report/artifact 链路；环境不支持的路径有稳定 skip
  或稳定 `ok:false` 断言。
- Core Dump Mode guard 覆盖新增动态 catchpoint event。
- 文档同步说明新增 event、限制和 evidence/report 行为。
- `docs/ai/progress.md` 记录本轮实际完成内容。
- `docs/ai/handoff.md` 覆写为本轮交接，包含验证结果和遗留限制。
- 本轮相关文件被单独 staged、commit，并 push 到当前分支 upstream；如果 commit/push 失败，按
  AGENTS.md 要求记录原因。

## 禁止事项

- 不实现 PTY。
- 不支持交互式 inferior stdin。
- 不把 snapshot 设计成 live session 恢复文件。
- 不把工具输出改成自动根因结论。
- 不新增独立 Batch Mode。
- 不把 `raw_mi` 放宽为普通 action。
- 不为了本轮 catchpoint 扩展重写 replay/probe/hypothesis/report 的整体架构。
- 不提交 generated report/assets 临时目录，除非明确是仓库中已有的示例 artifact。
