# Handoff

日期：2026-06-03

## 本轮完成

- 按 `docs/ai/next_cli_task.md` 执行 advanced hypothesis assertions、summary/sanitizer 校准和
  report 可读性增强。
- 扩展 `hypothesis_check` assertion：
  - 新增 `between`，`expected` 使用 `LOW..HIGH`，闭区间判断，支持十进制、负数和 `0x`
    十六进制整数。
  - 新增 `address_non_null`，从 observed 中解析唯一 `0x...` 地址，非零时 `passed`。
  - 新增 `address_equals`，从 observed 和 expected 中各解析唯一 `0x...` 地址，相等时
    `passed`。
  - 对缺少 expected、无效区间、LOW 大于 HIGH、无法解析整数/地址、多值歧义等情况稳定返回
    `status:"unknown"`，并沿用 `ToolError` / `error_evidence` 链路。
- 增强 summary sanitizer：
  - 新增 `std::pair<const K, V>` 和 `std::pair<K const, V>` key const 噪声压缩。
  - raw evidence、raw MI、session log 和 raw 文件布局未改变。
- 改进 report：
  - Hypotheses 的 `Checks needing attention` 现在会对带 `error_evidence` 的 check 展示对应
    `ToolError` summary。
  - Limitations 明确提示 hypothesis observed value 是有损 summary，最终结论前应检查 linked raw
    evidence。
- 扩展 smoke 和测试：
  - `hypothesis_assertion_tests` 覆盖 `between`、`address_non_null`、`address_equals` 的
    pass/fail/unknown、负数、十六进制、无效区间、缺少 expected 和多值歧义。
  - `mi_summary_tests` 覆盖 pair key const 降噪。
  - `scripts/smoke_capability_matrix.sh` 增加真实 `between` hypothesis check，并断言 report 中
    `between` 和 unknown assertion error summary 可见。
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

当前 Linux 环境安装了 GDB，完整 CTest 9/9 通过；其中 `daemon_action_flow`、`core_dump_mode`、
`edge_case_flow` 和 `capability_matrix_flow` 都实际执行了 Linux + GDB smoke。

## 限制和注意事项

- 仍未实现浮点 assertion、`changed` 或跨 check 历史比较。
- `between` 和已有 numeric assertion 只支持整数；遇到多个不同整数会返回 `unknown`，避免从有损
  summary 中猜测。
- address assertion 只解析 `0x...` 十六进制地址；无法解析或存在多个不同地址时返回 `unknown`。
- 本轮没有扩展 catchpoint event，没有引入 PTY 或交互式 stdin，也没有修改 raw evidence/schema 布局。
