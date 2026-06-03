# Next CLI Task

## 目标

进入 MVP 收敛，本轮以补充文档为主，不新增调试功能。

核心目标是让当前 MVP 的使用路径、验收标准、边界限制和后续工作优先级变得清晰，方便后续 Agent 或用户判断：

- 这个工具当前能做什么。
- 如何用它完成一轮最小 Agent 调试流程。
- MVP 如何验收。
- 哪些限制是明确边界，不是当前 blocker。
- 哪些建议已经过期，需要从进度记录中修正。

本轮不是功能开发任务，不做架构重构，不做 report 大改，不做全仓格式化。

## 背景

当前项目已经具备 MVP 主链路：

- Markdown task file。
- Run Mode 和 Core Dump Mode。
- daemon/create/action/status/finish/close/shutdown flow。
- 高层 action、probe/on-hit、replay、hypothesis workflow。
- evidence raw/summary/view/index、session summary/snapshot/report。
- Linux + GDB smoke 和不依赖 GDB 的单元测试。

后续继续扩展 catchpoint、assertion、summary/sanitizer 或架构拆分都有价值，但当前更需要先把 MVP 收敛为可验收、可交接、可 dogfood 的状态。

## 范围

### 1. 补充 MVP 验收标准

新增或更新文档，明确 MVP 通过条件。

建议产出位置：

- 优先更新 `README.md`。
- 如内容较长，可新增 `docs/mvp_acceptance.md` 并在 README 中链接。

验收标准至少覆盖：

- Linux 目标平台。
- 构建命令：
  - `cmake -S . -B build`
  - `cmake --build build`
- task 校验：
  - `./build/gdb-agent check examples/segfault_task.md`
- daemon flow：
  - daemon start
  - create session
  - action
  - finish
  - report/assets 生成
- Run Mode 最小 crash/stop 取证。
- Core Dump Mode 最小静态取证。
- evidence 文件必须包含 raw、summary、view、index 和 raw hash。
- report 必须引用 evidence id。
- replay/probe/hypothesis 至少有可运行 smoke 或文档入口。
- Linux + GDB smoke / CTest 是 MVP 回归入口。
- 工具不自动声明根因，最终结论由 Agent 给出。

### 2. 补充 Agent 使用路径文档

补一个偏实际使用的 Agent playbook，让新 Agent 能从 task file 走到 finish report。

建议产出位置：

- `docs/mvp_quickstart.md`
- 或 README 中新增 `MVP Quickstart`，但如果 README 变得过长，优先新建文档并从 README 链接。

内容至少包括：

- 如何编写最小 task file。
- 如何运行 `check`。
- 如何启动 daemon。
- 如何 create session。
- 第一轮建议动作：
  - `backtrace`
  - `threads`
  - `frame_select`
  - `locals`
  - `evaluate`
- 如何理解 action response：
  - `ok`
  - `action`
  - `evidence`
  - `command_evidence`
  - `error`
- 如何查看 evidence：
  - raw 是审计来源。
  - summary 是有损低噪声视图。
  - Markdown view 是 human-readable 入口。
  - report 引用 evidence id。
- 如何使用 hypothesis workflow：
  - create
  - check
  - conclude
- 如何 finish。
- 什么情况下应该打开 raw MI。

### 3. 集中整理 Known Limitations

新增或更新一处集中限制说明，避免限制散落在多份文档中造成误解。

建议产出位置：

- `docs/known_limitations.md`
- README 中新增简短 Known Limitations 并链接完整文档。

至少覆盖：

- 目标运行平台只支持 Linux。
- live session 需要系统可用 GDB。
- 不支持 PTY。
- 不支持交互式 inferior stdin。
- inferior stdin 只能是 `/dev/null` 或 task 指定文件。
- `session_snapshot.json` 和 `session_summary.json` 不是 live GDB 会话恢复文件。
- 重启后的恢复方式是 replay 高层 action，不是恢复旧进程。
- Core Dump Mode 是静态取证模式，不支持动态 action/probe 操作。
- `catchpoint_set` 当前只支持 `catch throw`。
- summary 是有损视图，不能替代 raw evidence。
- `raw_mi` 是高级 escape hatch，必须显式 `risk:"advanced"`。
- report 是调试报告草稿，不是自动根因结论。
- hypothesis check 是工具级 assertion，不是最终根因判断。
- numeric assertion 当前支持整数，不支持浮点数。
- sanitizer 不是完整 C++ demangler。

### 4. 补充 Dogfood 记录

补一份轻量 dogfood 文档，记录如何用当前工具完成一轮最小示例定位。

建议产出位置：

- `docs/mvp_dogfood.md`
- 或 `examples/mvp_dogfood.md`

内容建议：

- 使用 `examples/segfault_task.md` 或 capability fixture。
- 记录推荐命令序列。
- 记录建议 action 序列。
- 说明关键 evidence/report 链路应该如何查看。
- 说明 Agent 如何从 evidence 形成最终结论。
- 说明哪些内容来自工具观察，哪些属于 Agent inference。
- 不要求提交完整 generated assets，避免 GDB 版本差异造成仓库噪声。

### 5. 可选：补 MVP 验收脚本

如果范围允许，可以新增一键验收脚本：

- `scripts/mvp_acceptance.sh`

建议执行：

```bash
cmake -S . -B build
cmake --build build
./build/gdb-agent check examples/segfault_task.md
ctest --test-dir build --output-on-failure
git diff --check
```

要求：

- 如果当前环境没有 GDB，应清楚说明 live smoke 可能 skip 或失败的原因。
- 不要让脚本隐藏失败。
- 如果新增脚本，需要在 README 或 MVP acceptance 文档中说明。

如果脚本实现会牵扯较多边界，本轮可以只补文档中的验收命令，不强制新增脚本。

### 6. 修正过期进度建议

更新 `docs/ai/progress.md` 中已经过期或容易误导的建议。

至少修正：

- “继续扩展 hypothesis assertion，例如 numeric 比较”这一类表述。

建议改成：

- 整数 numeric assertion 已完成。
- 后续可扩展 range/address/float/change detection，但不阻塞 MVP。
- MVP 收敛优先级高于继续新增 assertion。

如果文档中还有其他已完成但仍作为待办描述的内容，也一并小范围修正。

## 可写范围

允许修改：

- `README.md`
- `docs/mvp_quickstart.md`
- `docs/known_limitations.md`
- `docs/mvp_acceptance.md`
- `docs/mvp_dogfood.md`
- `scripts/mvp_acceptance.sh`，可选
- `docs/ai/progress.md`
- `docs/ai/handoff.md`
- `docs/ai/decision.md`，仅当产生新的项目级决策时更新

如需要链接现有文档，也可小范围修改：

- `docs/task_format.md`
- `docs/agent_actions.md`
- `docs/evidence_model.md`

不要修改：

- 源码功能实现。
- 测试逻辑，除非新增可选 `scripts/mvp_acceptance.sh` 需要 CMake/CTest 说明；一般不应修改测试。
- build 配置。
- 与 MVP 文档收敛无关的文件。
- `docs/ai/next_cli_task.md`，除非用户明确要求重新规划下一轮任务。

## 不做

- 不新增 Agent-facing action。
- 不扩展 catchpoint event。
- 不新增 hypothesis assertion。
- 不改 summary/sanitizer 行为。
- 不改 report 生成逻辑。
- 不重构 `src/cli.cpp` 或 daemon/session 架构。
- 不优化 evidence index 性能。
- 不引入 PTY 或交互式 stdin。
- 不为了 macOS live GDB 做兼容；目标运行平台仍是 Linux。
- 不做全仓 clang-format。

## 文档同步

本轮文档应保持一致：

- README 应指向新增的 MVP quickstart、known limitations、acceptance 或 dogfood 文档。
- 新增文档应与 `docs/ai/current_goal.md` 和 `docs/ai/decision.md` 的项目边界一致。
- 如果引用 action、task 或 evidence 语义，应与 `docs/agent_actions.md`、`docs/task_format.md`、
  `docs/evidence_model.md` 一致。
- 不要把工具输出描述成自动根因结论。

任务结束时必须更新：

- `docs/ai/progress.md`
- `docs/ai/handoff.md`

## 验证要求

文档为主时至少运行：

```bash
git diff --check
```

如果新增 `scripts/mvp_acceptance.sh`，还应运行：

```bash
./scripts/mvp_acceptance.sh
```

如果只是补充文档，不要求运行完整 build/test。但如果文档中新增或修改了命令，建议至少确认当前命令文本和已有 README/CTest 入口一致。

## 完成标准

- README 或新增文档明确 MVP 验收标准。
- 有 Agent 可执行的 MVP quickstart/playbook。
- 有集中 Known Limitations。
- 有轻量 dogfood 记录，说明一轮最小示例如何使用当前工具完成。
- `docs/ai/progress.md` 中过期的“numeric 比较”后续建议已修正。
- 未新增功能，未修改源码行为。
- `docs/ai/progress.md` 和 `docs/ai/handoff.md` 记录实际完成内容、验证结果和限制。
- 按 Execution mode 约定 stage 本轮相关文件，创建一次 commit，并推送当前分支。
