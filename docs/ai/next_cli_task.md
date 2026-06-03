# Next CLI Task

## 目标

执行一轮面向代码质量的 code review，重点审查当前实现中是否存在不必要的性能开销、过度复杂的控制流、边界不清的模块职责或会妨碍后续演进的架构问题。

本轮不是格式审查。缩进、空格、换行、局部排版等纯格式问题默认忽略；格式统一交给项目级 `.clang-format` 或后续单独格式化任务处理。除非格式问题直接造成可读性误判、宏/模板解析风险或实际 bug，否则不要把它列为 review finding。

## 背景

当前项目已经完成 MVP 的主要能力：

- Markdown task file 解析和校验。
- GDB/MI live session、Run Mode 和 Core Dump Mode。
- daemon/create/action/status/finish/close/shutdown flow。
- evidence raw/summary/view/index、session summary/snapshot/report。
- replay store、probe metadata/on-hit policy、hypothesis workflow。
- 多组 Linux + GDB smoke，包括 daemon flow、core dump、edge cases 和 capability matrix。

上一轮已修复若干会误导 Agent 的行为问题，包括 task env 传递和 run/continue command error 语义。本轮应从“功能已经能跑”切换到“实现是否足够轻、边界是否足够清楚、后续扩展是否容易”的审查视角。

## 审查重点

### 1. 不必要的性能开销

重点检查 hot path 和高频 action 路径：

- daemon action dispatch。
- `GdbSession` MI command send/receive 和 stop event handling。
- evidence 写入、index 更新、summary/view/raw 文件生成。
- MI parsing、summary sanitizer、backtrace/thread summarizer。
- replay/probe/hypothesis 相关状态读写。
- report/session snapshot/session summary 生成。

优先寻找：

- 每个 action 都重复全量读写大文件或全量重建 JSON 的路径。
- 可以轻量缓存却反复扫描目录、反复读取 evidence index、反复解析 task/replay/probe/hypothesis 状态的路径。
- 大对象、JSON、字符串、MI record 或 evidence payload 的不必要拷贝。
- 同步文件 I/O 放在明显可避免的 action fast path 上。
- 为生成低噪声 summary 反复处理 raw 全量文本，而不是只处理当前 record 或当前 evidence。
- GDB round trip 明显多余、可以合并或可以从已有 stop record/probe state 推导的情况。
- 没有预算控制的输出聚合、字符串拼接或 report 生成。

注意：

- 不要为了“看起来更快”做没有证据的微优化。
- 如果性能问题只在极大数据量下出现，请说明触发条件和可能影响，不要把它夸大成当前 blocker。
- 对外契约、evidence 可审计性和 raw 保留优先于过度压缩 I/O。

### 2. 架构和职责边界

重点检查模块边界是否符合项目设计：

- `src/cli.cpp` 是否承担了过多 session/action/evidence/replay/probe/hypothesis 业务逻辑。
- `src/gdb/` 是否只处理 GDB/MI process、session state、MI utility，而不是混入 report 或 Agent 推理语义。
- `src/evidence/` 是否保持 evidence store 职责清晰，不把 summary 当 raw 替代。
- `src/workflow/` 是否适合作为 crash/session outcome/light/core evidence collection 的承载层。
- replay/probe/hypothesis 状态是否保持“高层 action 可重放、live state 与 finish artifact 分离”的设计。
- Core Dump Mode 和 Run Mode 的 state guard 是否集中、清晰、可扩展。
- 工具观察、action result、ToolError evidence 和 Agent conclusion 是否仍保持分离。

优先寻找：

- 一个函数同时做 CLI 参数解析、业务校验、GDB 调用、evidence 写入和 report 拼装。
- action validation 分散在多个地方，导致 response/evidence 语义不一致。
- session state、probe state、replay state、hypothesis state 的所有权不清。
- 为了当前 smoke test 临时拼接出来、后续难以维护的特殊路径。
- 可以通过小型 helper 或局部模块化降低重复和错误概率的地方。
- 与 `design.md` / `docs/ai/decision.md` 明确决策冲突的实现。

注意：

- 不要提出大规模重写 daemon/session 架构，除非能说明具体风险和渐进迁移方案。
- 不要把“可以更优雅”作为 finding；finding 必须指向实际风险、维护成本或未来功能阻塞。
- 如果发现架构问题但本轮不宜修复，应在 handoff 中写清建议拆分的后续任务。

### 3. 行为风险和测试缺口

性能和架构审查过程中，如果发现会影响 Agent 判断的行为风险，也应记录：

- 失败 action 是否可能返回过于乐观的 response。
- evidence 是否可能丢 raw、错链 evidence id、错标 lossy/truncated。
- report/session summary 是否可能遗漏关键错误或 probe/hypothesis/replay 状态。
- smoke test 是否覆盖了实现中的关键分支，还是只覆盖了“正好能过”的路径。

本轮不以扩展测试矩阵为主要目标，但如果发现一个小测试能锁住高价值问题，可以补充。

## 工作方式

1. 先阅读本任务和项目入口文档：
   - `final_feature.md`
   - `design.md`
   - `docs/ai/current_goal.md`
   - `docs/ai/decision.md`
   - `docs/ai/progress.md`
   - `docs/ai/handoff.md`
2. 使用 `rg` / `rg --files` 快速建立代码结构视图。
3. 优先审查高风险文件：
   - `src/cli.cpp`
   - `src/gdb/`
   - `src/evidence/`
   - `src/workflow/`
   - `src/report/`
   - 相关 tests 和 smoke scripts
4. 形成 findings 时，按严重程度排序，并标明：
   - 文件和行号。
   - 现象。
   - 影响。
   - 建议处理方式。
5. 如果发现高置信、低风险、范围小的问题，可以在本轮直接修复。
6. 如果问题需要较大重构，先不要展开实现；把它作为后续任务建议写入 `docs/ai/handoff.md`。

## 可写范围

本轮允许修改：

- 为修复高置信 review finding 所需的源码文件。
- 为锁住修复所需的最小测试或 smoke script。
- 与实际行为变化对应的文档：
  - `docs/agent_actions.md`
  - `docs/agent_actions.en.md`
  - `docs/evidence_model.md`
  - `docs/evidence_model.en.md`
  - `docs/task_format.md`
  - `docs/task_format.en.md`
- 任务结束记录：
  - `docs/ai/progress.md`
  - `docs/ai/handoff.md`
  - `docs/ai/decision.md`，仅当发现并确认新的项目级设计决策时更新。

不要修改：

- 与 review finding 无关的源码。
- 纯格式文件或大规模格式化输出。
- `docs/ai/next_cli_task.md`，除非用户明确要求重新规划下一轮任务。

## 不做

- 不做纯格式审查；格式问题交给 `.clang-format`。
- 不进行全仓 clang-format。
- 不做 unrelated cleanup。
- 不新增 Agent-facing action。
- 不扩展 catchpoint event。
- 不实现 numeric hypothesis assertion。
- 不引入 PTY 或交互式 stdin。
- 不把工具变成自动根因分析器。
- 不为了 macOS live GDB 做兼容；目标运行平台仍是 Linux。

## 输出要求

在 `docs/ai/handoff.md` 中记录本轮 code review 结果：

- 如果发现问题，列出 findings，按严重程度排序。
- 如果直接修复了问题，记录修复内容、验证结果和剩余风险。
- 如果只发现需要后续较大重构的问题，记录建议拆分方式。
- 如果没有发现值得处理的问题，也要明确说明审查范围和残余风险。

在 `docs/ai/progress.md` 中追加本轮进度摘要：

- 审查范围。
- 已修复的问题或确认没有直接修复项。
- 后续建议。

## 验证要求

如果本轮修改了源码或测试，至少运行：

```bash
cmake --build build
ctest --test-dir build --output-on-failure
./build/gdb-agent check examples/segfault_task.md
git diff --check
```

如果只更新 review 记录而没有代码变更：

- 不要求运行完整 build/test。
- 仍应运行 `git diff --check`，确保文档改动没有明显 whitespace 问题。

如果发现性能或架构问题但暂不修复：

- 不要伪造验证结果。
- 在 `docs/ai/handoff.md` 说明没有执行对应验证的原因。

## 完成标准

- 完成一轮聚焦性能开销和架构设计的 code review。
- findings 不包含纯格式问题，除非格式直接导致实际风险。
- 高置信、低风险的问题已修复或明确记录为后续任务。
- `docs/ai/progress.md` 和 `docs/ai/handoff.md` 已更新。
- 如有代码或测试改动，已按要求完成 build/test。
- 按 Execution mode 约定 stage 本轮相关文件，创建一次 commit，并推送当前分支。
