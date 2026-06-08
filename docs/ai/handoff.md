# Handoff

日期：2026-06-08

## 本轮完成

- 按 `docs/ai/next_cli_task.md` 执行 Phase 5 MI parser / summary / sanitizer hardening 任务。
- 更新 `src/gdb/mi_utils.cpp`：
  - malformed non-numeric prefix 不再被误判为 MI token。
  - `summarize_mi_records` 会从 result/async payload 中提取 `msg`、`value`、`reason`、
    `thread-id`、`stopped-threads`、`frame`、`bkpt` 和 `wpt` 等低噪声字段。
- 更新 `src/common/string_utils.cpp`：
  - 新增轻量 `std::...<...>` template scanner，压缩常见 STL 容器、map/unordered_map、
    smart pointer、optional/variant/tuple/pair 噪声。
  - 工作目录路径会先 lexically normalize，再相对化，覆盖 `build/../src/file.cpp` 这类路径。
- 更新 `src/evidence/evidence_store.cpp`：
  - backtrace summary 保留 `thread apply all bt` 的 thread boundary。
  - frame summary 支持 `from /lib/...so` shared-library 来源。
  - thread summary 继续跳过 header，并保持 truncation 行为稳定。
- 重写扩展 `tests/mi_summary_tests.cpp`，按 parser、record audit、record summary、sanitizer、
  EvidenceStore integration 分段覆盖，不依赖系统 GDB。
- 同步更新：
  - `docs/evidence_model.md`
  - `docs/evidence_model.en.md`
  - `docs/known_limitations.md`
  - `docs/ai/progress.md`

## 验证

- `cmake --build build`
- `./build/mi_summary_tests`
- `./build/gdb-agent check examples/segfault_task.md`
- `git diff --check`
- `ctest --test-dir build --output-on-failure`
  - 当前 Linux 环境有 GDB，完整 CTest 已运行。
  - 结果：9/9 tests passed。

## 限制和注意事项

- 本轮没有新增 Agent-facing action。
- 本轮没有改变 raw evidence 保存原则、evidence raw 文件布局或 action schema。
- Sanitizer 仍不是完整 C++ demangler；新增 scanner 只覆盖常见 STL summary 降噪。
- MI parser/summary 覆盖增强后，raw MI 仍是最终审计来源。
- 本轮未新增项目级 decision，因此未修改 `docs/ai/decision.md`。
