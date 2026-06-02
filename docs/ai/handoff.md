# Handoff

日期：2026-06-02

## 本轮完成

- 完成 `docs/ai/next_cli_task.md` 指定的 reliability consistency 修复任务。
- 统一 live session 内 action validation failure 的审计行为：
  - 缺少 `action` 字段会写 `ToolError` evidence。
  - `evaluate` 缺少 `expression`、`breakpoint_set` 缺少 `location`、`watchpoint_set` 缺少
    `expression`、probe 操作缺少 `number` 会写 `ToolError` evidence。
  - `raw_mi` 缺少 `risk:"advanced"` 或缺少 `command` 会写 `ToolError` evidence。
  - `replay` 缺少 file/name、replay 文件不存在或 replay plan validation 失败时，在已有 session
    context 下返回结构化失败并写 evidence。
- 修复 CLI `JSON_OR_FILE` 判定：
  - 参数 trim 后以 `{` 或 `[` 开头时优先作为 inline JSON，不再先调用 `fs::exists`。
  - 增加长 inline JSON action smoke 覆盖，避免 `File name too long`。
- 修复 GDB command error 语义：
  - `frame_select` 和 `evaluate` 遇到 GDB `result_class=error` 或 timeout 时返回 `ok:false`。
  - 原始命令仍写 `GdbCommand` evidence；错误 response 通过 `command_evidence` 指向该 evidence，
    并通过 `evidence` 指向 `ToolError`。
- 明确并实现 probe 删除语义：
  - `probe_list` 默认只返回 active/live probes。
  - `probe_delete` 后的历史 probe 仍可保留在 finish-time `assets/probes.json`，但标记
    `deleted:true`。
- 增强 watchpoint stop 归属：
  - 优先从 MI stop record 读取 watchpoint number。
  - 如果 GDB stop record 缺少编号，只有当前存在唯一 active watchpoint 时才保守归属并执行
    watchpoint on-hit。
  - 无法唯一归属时记录降级 `WatchpointHit` evidence，不伪造 probe number。
  - capability matrix 中真实 watchpoint stop、`WatchpointHit` evidence、watchpoint on-hit 和
    session summary 计数已改为 hard expectation。
- 更新 smoke：
  - `scripts/smoke_edge_cases.sh`
  - `scripts/smoke_capability_matrix.sh`
  - `scripts/smoke_core_dump_mode.sh`
- 同步文档：
  - `docs/agent_actions.md`
  - `docs/agent_actions.en.md`
  - `docs/evidence_model.md`
  - `docs/evidence_model.en.md`
  - `docs/ai/decision.md`
  - `docs/ai/progress.md`
- 本轮未新增 Agent-facing action，未扩展 catchpoint event，未实现 numeric hypothesis assertion。

## 验证

- `cmake --build build`
- `./scripts/smoke_edge_cases.sh`
- `./scripts/smoke_capability_matrix.sh`
- `./scripts/smoke_core_dump_mode.sh`
- `ctest --test-dir build --output-on-failure`
- `./build/gdb-agent check examples/segfault_task.md`
- `git diff --check`

当前 Linux 环境安装了 GDB，因此 `daemon_action_flow`、`core_dump_mode`、`edge_case_flow` 和
`capability_matrix_flow` 都实际执行了 Linux + GDB smoke，而不是 skip。

## 限制和注意事项

- 非法 JSON 在 CLI client 本地解析阶段失败时没有 session context，因此不会写入 session
  evidence；当前行为是 CLI 输出稳定错误并退出，不崩溃。
- watchpoint stop record 缺少 probe number 且当前存在多个 active watchpoint 时，本轮不会伪造归属；
  会记录降级 watchpoint evidence。后续如需更强归属，可以结合更完整的 raw MI/`-break-list`
  解析或增加多 watchpoint fixture。
- `frame_select` / `evaluate` 在 core dump 上可能因具体 core/GDB context 返回 `No registers.` 或
  `No symbol ...`；现在这会被正确映射为结构化失败。相关 smoke 已改为验证失败时必须有
  `ToolError` 和 `command_evidence`。
