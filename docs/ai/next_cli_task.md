# Next CLI Task

## 目标

继续增强 Agent 友好能力，但不要重复上一轮已经完成的整数 numeric assertion。本轮聚焦：

1. 扩展更贴近真实调试场景的 hypothesis assertion。
2. 用真实 Linux + GDB 输出继续校准 summary / sanitizer / report。
3. 修正当前进度记录中已经过期的后续建议，让后续 Agent 不再把“整数 numeric 比较”当作未完成项。

本轮不是格式整理任务，也不是架构重构任务。格式问题交给 `.clang-format` 或后续单独格式化任务处理。

## 背景

上一轮 Agent 友好能力增强已经完成：

- `hypothesis_check` 新增整数 numeric assertion：
  - `greater_than`
  - `less_than`
  - `greater_equal`
  - `less_equal`
  - `equals_number`
  - `not_equals_number`
- numeric parser 支持十进制、负数和 `0x` 十六进制整数。
- 对无法解析、缺少 expected、多个不同整数歧义等情况稳定返回 `unknown`。
- summary sanitizer 已增加常见 `std::map` / `std::unordered_map` 降噪。
- report 已增加 `Observed Summary`、`Checks needing attention` 和 `Tool Errors` 区域。

当前仍有价值的 Agent 友好增强是：让 hypothesis 更能表达真实调试判断，并继续让 summary/report 在真实 GDB 输出上更省 token、更少歧义。

## 范围

### 1. 扩展进阶 hypothesis assertion

在现有 `src/workflow/hypothesis.cpp` / `.hpp` 上继续扩展 assertion。优先选择高价值、小范围、语义稳定的断言。

优先实现：

- `between`
  - `expected` 推荐格式：`LOW..HIGH`，例如 `0..10`、`-5..5`、`0x10..0x20`。
  - 默认包含边界：`LOW <= observed <= HIGH`。
  - 如果 `LOW > HIGH`，返回 `unknown`。
- `address_non_null`
  - 判断 observed 中的地址是否非零。
  - 支持常见 GDB 输出：`0x0`、`0x0000000000000000`、`0x7ffff...`、`ptr = 0x...`。
- `address_equals`
  - expected 是一个地址，支持 `0x0` 和非零十六进制地址。
  - 用于验证两个指针、generation pointer 或 sentinel address 是否一致。

可按实现情况增加，但不要强行扩展：

- `changed`
  - 只有当当前 hypothesis store 已经有同一 hypothesis/check 的历史 observed 可安全比较时才做。
  - 如果没有清晰历史语义，本轮不要实现，避免制造伪确定性。
- 浮点比较
  - 只有在 parser 和文档能清楚处理精度、NaN、inf 和比较 epsilon 时才做。
  - 否则保留为后续任务，并在 handoff 说明。

语义要求：

- assertion 仍然只表达工具级观察，不代表 hypothesis 被支持或反驳。
- `observed` 仍来自有损 summary，不是 raw evidence。
- 解析失败、缺少 expected、expected 格式错误、observed 歧义时返回 `unknown`。
- unknown 路径继续通过现有 `ToolError` / `error_evidence` 链路记录原因。
- 不要把多个不同地址或多个不同数字强行选一个；有歧义就 unknown。

测试要求：

- 扩展 `hypothesis_assertion_tests`。
- 覆盖：
  - `between` pass/fail/unknown。
  - 十进制、负数、十六进制 range。
  - `LOW > HIGH`。
  - `address_non_null` 的 zero/non-zero/unknown。
  - `address_equals` 的 equal/not equal/expected 缺失/歧义。
- 如果 smoke fixture 中已有合适停点，扩展 `scripts/smoke_capability_matrix.sh` 做至少一个真实
  `between` 或 address assertion。

### 2. 校准真实 GDB summary / sanitizer

基于现有 Linux + GDB smoke 输出，继续增强 summary 的 Agent 可读性。不要追求完整 demangler，只做高价值、低风险规则。

优先检查：

- backtrace summary 是否仍暴露过多模板、allocator、长绝对路径。
- threads summary 是否能稳定标出当前线程、stop reason 和关键 frame。
- GDB command error summary 是否足够短，并能帮助 Agent 判断失败原因。
- Core Dump Mode 下无栈、缺符号、缺 debug info 时的 summary 是否清楚。

可实现的增强方向：

- 对 `std::pair<const K, V>` 做更稳定压缩，辅助 map/unordered_map summary。
- 继续压缩 `std::optional<T>`、`std::variant<...>`、`std::function<...>` 等常见类型噪声。
- 对 repo 内路径或 working directory 下路径做更稳定的相对化。
- 对 command error summary 增加一行短原因提取，避免 Agent 只看到大段 MI。

约束：

- raw evidence 和 session MI log 必须完整保留。
- summary/view 可以更短，但必须保持 `lossy_summary` / `truncated` 语义正确。
- 不引入重型依赖或完整 C++ demangling 系统。
- 不改变 action response 的 machine-readable 字段语义。

测试要求：

- 扩展 `mi_summary_tests`，覆盖新增 sanitizer 或 summary 规则。
- 如果修改 report/smoke，尽量断言关键字段和 evidence id，不断言整段自然语言。

### 3. 改进 report 中“下一步判断”相关信号

上一轮已经新增 `Tool Errors` 和 `Checks needing attention`。本轮可以在此基础上做小幅增强：

- Hypotheses 区域：
  - 对 `unknown` check 展示 unknown reason 或 error summary，避免 Agent 只看到 unknown。
  - 长 observed 保持截断，并明确 evidence id。
- Tool Errors 区域：
  - 对 `command_evidence`、action、error 做稳定字段展示。
  - 如果同一 action 多次失败，保持按 evidence id 可追踪，不做模糊合并。
- Report 限制说明：
  - 如果 report 引用了有损 summary 做 assertion observed，应继续提醒 raw evidence 才是审计来源。

不要大规模重排 report，保持当前用户和 smoke 预期稳定。

### 4. 修正进度记录中的过期建议

`docs/ai/progress.md` 末尾的“建议的下一步”当前仍写着“继续扩展 hypothesis assertion，例如 numeric 比较”。上一轮已经完成整数 numeric 比较，因此本轮结束时应把该建议改成更准确的说法，例如：

- 继续扩展 hypothesis assertion，例如 range/address/float/change detection。

如果本轮完成了 range/address，也要把建议更新为剩余未完成的具体能力。

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
   - `src/workflow/hypothesis.hpp`
   - `tests/hypothesis_assertion_tests.cpp`
   - `src/gdb/mi_utils.cpp`
   - `src/gdb/mi_utils.hpp`
   - `tests/mi_summary_tests.cpp`
   - `src/report/report.cpp`
   - `scripts/smoke_capability_matrix.sh`
3. 优先实现 `between` 和 address assertions。
4. 再做一到两项高价值 summary/report 增强。
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

- 不新增新的调试 action；`hypothesis_check` 仍是原 action。
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

- 新增 assertion 的名称和语义。
- 解析失败、缺少 expected、observed 歧义时返回 `unknown`。
- `observed` 仍来自有损 summary，不是 raw，也不是 Agent 结论。
- 本轮新增或调整的 summary/report 行为。

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

- `hypothesis_check` 至少新增 `between`、`address_non_null` 和 `address_equals`。
- 新 assertion 对解析失败和歧义情况稳定返回 `unknown`，并保留 ToolError 链路。
- `hypothesis_assertion_tests` 覆盖新增 assertion。
- summary / sanitizer 至少完成一组高价值真实 GDB 噪声压缩或错误 summary 增强，并有测试。
- report 对 unknown reason / ToolError / command evidence 的展示保持 Agent 友好。
- `docs/ai/progress.md` 中过期的“numeric 比较”后续建议已修正为当前真实剩余能力。
- 相关中英文文档同步。
- `docs/ai/progress.md` 和 `docs/ai/handoff.md` 记录实际完成内容、验证结果和限制。
- 按 Execution mode 约定 stage 本轮相关文件，创建一次 commit，并推送当前分支。
