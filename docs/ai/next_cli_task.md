# Next CLI Task

## 目标

执行一轮 Agent 友好能力增强，重点降低 AI Agent 阅读 evidence、判断 hypothesis 和使用 report 时的 token 成本与歧义。

本轮聚焦三件事：

1. 扩展 `hypothesis_check` 的 assertion 能力，优先支持常用 numeric 比较。
2. 增强低噪声 summary / sanitizer，让 GDB 输出里的 C++ 噪声更少、更稳定。
3. 改进 report 中 hypothesis、ToolError 和 command evidence 的展示，让 Agent 不必频繁打开 raw MI 才能判断下一步。

这不是大规模架构重构任务，也不是格式整理任务。格式问题交给 `.clang-format` 或后续单独格式化任务处理。

## 背景

当前 MVP 主链路已经基本完整：

- Markdown task file、Run Mode、Core Dump Mode。
- daemon/create/action/status/finish/close/shutdown flow。
- evidence raw/summary/view/index、session summary/snapshot/report。
- replay store、probe metadata/on-hit policy、hypothesis workflow。
- Linux + GDB smoke 和不依赖 GDB 的单元测试。

`docs/ai/progress.md` 当前把 “深度摘要和高级 MI” 标为 Early，并把以下内容列为后续建议：

- 用真实 Linux GDB raw 输出继续校准 MI parser、类型 sanitizer 和 backtrace/thread summary。
- 按真实调试需求继续扩展 hypothesis assertion，例如 numeric 比较。
- 继续优化 report 中 hypothesis 聚合、长 observed 展示、错误 evidence 和跳转体验。

本轮应优先补这些直接影响 Agent 使用体验的能力。

## 范围

### 1. 扩展 hypothesis numeric assertion

在现有 `src/workflow/hypothesis.cpp` / `.hpp` 的 assertion helper 上扩展小而稳定的 numeric 比较。

优先支持以下 assertion：

- `greater_than`
- `less_than`
- `greater_equal`
- `less_equal`

可按实现情况增加但不要过度扩展：

- `equals_number`
- `not_equals_number`

语义要求：

- `observed` 来自 `hypothesis_check` 新产生的 evidence summary，仍然是低噪声、有损视图。
- numeric parser 应能从常见 GDB print 输出中提取数字，例如：
  - `$1 = 42`
  - `42`
  - `$2 = -7`
  - `$3 = 0x10`
  - `value = 17`
- `expected` 必须存在并能解析为数字；否则返回 `unknown`，并由现有路径写 `ToolError` evidence。
- `observed` 为空、无法解析数字或包含多个明显冲突数字时，返回 `unknown`，不要猜。
- 支持十进制和 `0x` 十六进制；是否支持浮点数由实现难度决定，若不支持需在文档和 handoff 中明确。
- 不要把 numeric assertion 的 pass/fail 解释成 hypothesis 被支持或反驳；仍然只是工具级 check result。

测试要求：

- 扩展 `hypothesis_assertion_tests`，覆盖 pass、fail、unknown。
- 覆盖十进制、负数、十六进制、缺少 expected、observed 无数字、observed 多数字歧义。
- 如果 smoke 中已有合适停点，可在 `scripts/smoke_capability_matrix.sh` 中增加一个真实 `hypothesis_check`
  numeric assertion；避免新增重复脚本。

### 2. 增强 summary / sanitizer

检查并增强当前 MI summary 和 C++ 类型 sanitizer，目标是减少 Agent 看到的噪声，而不是追求完整 demangler。

优先增强：

- 常见 STL 容器类型压缩：
  - `std::vector<T, std::allocator<T>>` -> `std::vector<T>`
  - `std::map<K, V, ..., std::allocator<...>>` -> `std::map<K, V>`
  - `std::unordered_map<K, V, ..., std::allocator<...>>` -> `std::unordered_map<K, V>`
- 常见智能指针噪声压缩：
  - `std::unique_ptr<T, std::default_delete<T>>` -> `std::unique_ptr<T>`
  - `std::shared_ptr<T>` 保持稳定。
- 常见路径归一化：
  - 对 repo 内路径或 working directory 下路径，summary/view 中尽量展示相对路径。
  - 不要改变 raw evidence。
- backtrace/thread summary 中优先保留 Agent 判断最需要的信息：
  - frame number
  - function
  - file:line
  - 当前线程标记
  - stop reason

约束：

- raw MI 和 raw evidence 必须完整保留。
- summary 是有损视图，相关 `lossy_summary` / `truncated` 标记不能被破坏。
- 不要引入重型依赖或完整 C++ demangling 系统。
- 不要为了 summary 美化改变 action result 的 machine-readable 字段语义。

测试要求：

- 扩展 `mi_summary_tests`，覆盖新增 sanitizer 规则和路径归一化。
- 如果新增路径归一化 helper，尽量用不依赖 GDB 的单元测试覆盖。

### 3. 改进 report 的 Agent 可读性

在不改变 report 基本结构的前提下，增强 Agent 快速定位关键信号的能力。

优先改进：

- Hypotheses 区域：
  - 展示 assertion、expected、observed 摘要、status、error evidence。
  - 对 `unknown` 和 ToolError 链路更明确。
  - 长 observed 应截断展示，并保留 evidence id 供 Agent 回看。
- Errors / ToolError 展示：
  - report 中应能快速看到失败 action、错误信息、error evidence id。
  - 如果存在 `command_evidence`，应展示它，方便 Agent 关联原始 GDB command output。
- Replay / Probe / On-hit 相关区域：
  - 如果本轮触及 report 聚合逻辑，可以顺手让失败 step、on-hit error 和 degraded watchpoint evidence
    更容易被扫到。
  - 不要大规模重排 report，保持当前用户和 smoke 预期稳定。

测试要求：

- 优先扩展现有 smoke 对 report 内容的 grep 断言。
- 不要让测试过度依赖完整自然语言句子；断言关键字段、evidence id、status 和 action name 即可。

## 工作方式

1. 先阅读：
   - `final_feature.md`
   - `design.md`
   - `docs/ai/current_goal.md`
   - `docs/ai/decision.md`
   - `docs/ai/progress.md`
   - `docs/ai/handoff.md`
   - 本文件
2. 快速定位相关代码：
   - `src/workflow/hypothesis.cpp`
   - `tests/hypothesis_assertion_tests.cpp`
   - `src/gdb/mi_utils.cpp`
   - `tests/mi_summary_tests.cpp`
   - `src/report/report.cpp`
   - `scripts/smoke_capability_matrix.sh`
   - 相关 docs
3. 优先实现 numeric assertion，因为它是清晰的 Agent-facing 能力。
4. 再做低风险 sanitizer/report 增强；如果发现范围过大，选择最有价值的小集合完成，不要展开成大重构。
5. 修改 action、evidence、summary 或 report 语义时，同步更新文档。

## 可写范围

允许修改：

- `src/workflow/hypothesis.cpp`
- `src/workflow/hypothesis.hpp`
- `tests/hypothesis_assertion_tests.cpp`
- `src/gdb/mi_utils.cpp`
- `src/gdb/mi_utils.hpp`
- `tests/mi_summary_tests.cpp`
- `src/report/report.cpp`
- 现有 smoke scripts，优先 `scripts/smoke_capability_matrix.sh`
- 与实际行为变化对应的文档：
  - `docs/agent_actions.md`
  - `docs/agent_actions.en.md`
  - `docs/evidence_model.md`
  - `docs/evidence_model.en.md`
- 任务结束记录：
  - `docs/ai/progress.md`
  - `docs/ai/handoff.md`
  - `docs/ai/decision.md`，仅当产生新的项目级决策时更新。

如确实需要，也可小范围修改调用上述 helper 的邻近源码。不要做 unrelated cleanup。

不要修改：

- 与本轮 Agent 友好能力无关的模块。
- `docs/ai/next_cli_task.md`，除非用户明确要求重新规划下一轮任务。
- 纯格式文件或大规模格式化输出。

## 不做

- 不新增新的调试 action，除非 numeric assertion 需要文档列出新 assertion 名称；`hypothesis_check`
  仍是原 action。
- 不扩展 catchpoint event。
- 不引入 PTY 或交互式 stdin。
- 不实现完整 C++ demangler。
- 不把工具变成自动根因分析器。
- 不大规模重构 `src/cli.cpp` 或 daemon/session 架构。
- 不为了 macOS live GDB 做兼容；目标运行平台仍是 Linux。
- 不做全仓 clang-format。

## 文档同步

按实际行为更新：

- `docs/agent_actions.md`
- `docs/agent_actions.en.md`
- `docs/evidence_model.md`
- `docs/evidence_model.en.md`

文档中需要明确：

- 新增 numeric assertion 的名称和语义。
- 解析失败、缺少 expected、observed 歧义时返回 `unknown`。
- `observed` 仍来自有损 summary，不是 raw，也不是 Agent 结论。
- report 中新增或调整的错误/evidence 链路展示。

任务结束时必须更新：

- `docs/ai/progress.md`
- `docs/ai/handoff.md`

## 验证要求

至少运行：

```bash
cmake --build build
./build/hypothesis_assertion_tests
./build/mi_summary_tests
./build/gdb-agent check examples/segfault_task.md
git diff --check
```

如果修改了 smoke 或 report 聚合逻辑，并且当前 Linux 环境有 GDB，还应运行：

```bash
ctest --test-dir build --output-on-failure
```

如果当前环境没有 GDB，必须在 `docs/ai/handoff.md` 和最终回复中说明哪些 live smoke 未运行。

## 完成标准

- `hypothesis_check` 支持至少 `greater_than`、`less_than`、`greater_equal`、`less_equal`。
- numeric assertion 对解析失败和歧义情况稳定返回 `unknown`，并保留 ToolError 链路。
- `hypothesis_assertion_tests` 覆盖新增 numeric assertion。
- summary / sanitizer 至少完成一组高价值 C++ 噪声压缩或路径归一化增强，并有测试。
- report 对 hypothesis unknown / ToolError / command evidence 的展示更利于 Agent 扫描。
- 相关中英文文档同步。
- `docs/ai/progress.md` 和 `docs/ai/handoff.md` 记录实际完成内容、验证结果和限制。
- 按 Execution mode 约定 stage 本轮相关文件，创建一次 commit，并推送当前分支。
