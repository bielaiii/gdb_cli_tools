# Handoff

日期：2026-06-03

## 本轮完成

- 按 `docs/ai/next_cli_task.md` 执行一轮代码质量 review，重点审查性能开销、架构职责边界和会影响
  Agent 判断的行为风险。
- 审查范围包括：
  - `src/cli.cpp` action dispatch、state guard、run/continue、replay/probe/hypothesis glue。
  - `src/gdb/` MI command/control command 收发和 timeout 语义。
  - `src/evidence/` raw/summary/view/index 写入。
  - `src/workflow/` 静态/轻量取证入口。
  - `src/report/` report 聚合。
  - 相关 Linux + GDB smoke scripts。
- 修复一个高置信行为问题：
  - `backtrace`、`locals`、`args_info`、`registers`、`threads` 和 `hypothesis_check`
    之前通过 `collect_console` 保存 raw evidence 后直接返回 `ok:true`。
  - 如果底层 GDB console command 返回 `result_class=error` 或命令 timeout，Agent 会看到成功响应，
    必须打开 raw MI 才能发现 GDB 拒绝。例如 core fixture 中 `bt` 返回 `No stack.` 时会被包装成成功
    backtrace。
  - 现在这些 action 会返回 `ok:false`，写 `ToolError` evidence，并通过 `command_evidence` 关联原始
    `GdbCommand` evidence。
  - `hypothesis_check` 的表达式 GDB command 失败时不再写入 check result，避免把命令失败误写成
    assertion passed/failed/unknown。
- 新增 `collect_console_with_result`，让 action dispatch 能检查 `CommandResult`；原
  `collect_console` 保持兼容默认 light/core evidence collection。
- 更新 smoke：
  - `scripts/smoke_edge_cases.sh` 新增 `hypothesis_check` missing symbol 的结构化失败覆盖。
  - `scripts/smoke_core_dump_mode.sh` 和 `scripts/smoke_capability_matrix.sh` 调整 core 静态 action
    断言：GDB 有时可能对 generated core 返回 `No stack.`，允许 action 失败，但失败必须带
    `command_evidence`。
- 同步更新：
  - `docs/agent_actions.md`
  - `docs/agent_actions.en.md`
  - `docs/evidence_model.md`
  - `docs/evidence_model.en.md`
  - `docs/ai/progress.md`

## 验证

- `cmake --build build`
- `ctest --test-dir build --output-on-failure`
- `./build/gdb-agent check examples/segfault_task.md`
- `git diff --check`

当前 Linux 环境安装了 GDB，因此 CTest 中的 `daemon_action_flow`、`core_dump_mode`、
`edge_case_flow` 和 `capability_matrix_flow` 都实际执行了 Linux + GDB smoke，而不是 skip。

## Code Review Findings

1. 现象：静态 GDB-backed action 通过 `collect_console` 保存 command evidence 后未检查
   `CommandResult`，导致 `backtrace`、`locals`、`args_info`、`registers`、`threads` 和
   `hypothesis_check` 在 GDB 返回 `result_class=error` 或 timeout 时仍可能返回 `ok:true`。
   影响范围：Agent 可能把 GDB 拒绝、无栈 core、缺失 symbol 或命令超时误判为成功取证；
   `hypothesis_check` 还可能把表达式求值失败记录成 assertion 结果。
   处理结果：已修复为结构化失败，并用 edge/core/capability smoke 覆盖。

2. 现象：`src/cli.cpp` 仍承担 CLI/daemon request handling、action validation、GDB 调用、
   evidence 写入、replay/probe/hypothesis 状态维护和 session artifact 写出等大量职责。
   影响范围：当前功能可运行，但后续新增 action 或扩展 replay/probe/hypothesis 时容易造成重复校验和
   response/evidence 语义漂移。
   处理结果：本轮未展开大规模重构；建议后续渐进拆出 action handlers、probe/replay/hypothesis store
   helpers 和 daemon request handling，避免一次性重写。

3. 现象：`EvidenceStore::add` / `add_text` 每新增一条 evidence 都全量重写
   `assets/evidence/index.json`。
   影响范围：当前 smoke 规模可接受；如果真实长 session 产生大量 evidence，会出现 O(N^2) index 写入
   开销和更多同步文件 I/O。
   处理结果：本轮不为微优化改变 evidence 可审计性。建议后续在有真实规模数据后评估 append/journal
   或 finish-time compact index，同时保留中途审计需求。

## 限制和注意事项

- 本轮没有新增 Agent-facing action，未扩展 catchpoint event，未实现 numeric hypothesis assertion。
- 默认 light/core evidence collection 仍使用兼容 `collect_console`，本轮只改变高层 action response
  语义；默认取证阶段不会因为某个静态命令失败而中断整轮 session。
- 非法 JSON 在 CLI client 本地解析阶段失败时仍没有 session context，因此不会写入 session evidence；
  这是既有已知限制。
