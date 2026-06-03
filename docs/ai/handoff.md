# Handoff

日期：2026-06-03

## 本轮完成

- 按 `docs/ai/next_cli_task.md` 执行 Agent 友好能力增强，聚焦 hypothesis numeric assertion、
  summary/sanitizer 降噪和 report 可读性。
- 扩展 `hypothesis_check` assertion：
  - 新增 `greater_than`、`less_than`、`greater_equal`、`less_equal`。
  - 同时新增 `equals_number`、`not_equals_number`。
  - 支持十进制、负数和 `0x` 十六进制整数。
  - 会跳过 GDB value-history 前缀，例如 `$1 = 42` 中的 `$1`。
  - 缺少 expected、无法解析整数、observed/expected 中存在多个不同整数时稳定返回
    `status:"unknown"`，并沿用现有 `ToolError` / `error_evidence` 链路。
- 增强 summary sanitizer：
  - 新增常见 `std::map<K, V, std::less<K>, std::allocator<std::pair<...>>>` 到
    `std::map<K, V>` 的压缩。
  - 新增常见 `std::unordered_map<K, V, std::hash<K>, std::equal_to<K>, std::allocator<std::pair<...>>>`
    到 `std::unordered_map<K, V>` 的压缩。
  - raw evidence 和 session MI log 不变。
- 改进 report：
  - Hypotheses 表格新增 `Observed Summary` 列。
  - `unknown` 或带 `error_evidence` 的 check 会额外出现在 `Checks needing attention` 列表。
  - 新增 `Tool Errors` 区域，汇总 ToolError evidence、action、error、summary，并展示
    `command_evidence` 链路。
- 扩展 smoke 和测试：
  - `hypothesis_assertion_tests` 覆盖 numeric assertion 的 pass/fail/unknown、负数、十六进制、
    缺少 expected、无数字和多数字歧义。
  - `mi_summary_tests` 覆盖 map/unordered_map 降噪和路径相对化。
  - `scripts/smoke_capability_matrix.sh` 增加真实 `greater_than` hypothesis check，并断言 report 中
    `Tool Errors`、`Checks needing attention`、`Observed Summary` 和 `greater_than` 展示。
- 同步更新：
  - `docs/agent_actions.md`
  - `docs/agent_actions.en.md`
  - `docs/evidence_model.md`
  - `docs/evidence_model.en.md`
  - `docs/ai/progress.md`

## 验证

- `cmake --build build`
- `./build/hypothesis_assertion_tests`
- `./build/mi_summary_tests`
- `./build/gdb-agent check examples/segfault_task.md`
- `git diff --check`
- `ctest --test-dir build --output-on-failure`

当前 Linux 环境安装了 GDB，因此 CTest 中的 `daemon_action_flow`、`core_dump_mode`、
`edge_case_flow` 和 `capability_matrix_flow` 都实际执行了 Linux + GDB smoke，而不是 skip。

## 限制和注意事项

- numeric assertion 当前只支持整数，不支持浮点数。
- numeric parser 是保守解析：如果 observed 或 expected 中存在多个不同整数，会返回
  `unknown`，避免从有损 summary 中猜测。
- 本轮没有新增新的调试 action，`hypothesis_check` 仍是原 action。
- 本轮没有扩展 catchpoint event，没有引入 PTY 或交互式 stdin，也没有实现完整 C++ demangler。
