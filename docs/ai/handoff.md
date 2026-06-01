# Handoff

日期：2026-06-01

## 本轮完成

- 完成 `docs/ai/next_cli_task.md` 指定的 edge-case regression / exploration testing 任务。
- 新增 `scripts/smoke_edge_cases.sh`，并接入 CTest 为 `edge_case_flow`。
- `edge_case_flow` 在 Linux + GDB 下实际覆盖：
  - 不存在 session 的 `status`、`action`、`finish`、`close`。
  - 非法 action JSON、缺少 action、缺少 expression/location/risk 等错误输入。
  - negative `frame_select` 和非法 `evaluate` 表达式。
  - on-hit raw MI 拒绝。
  - watchpoint 设置失败路径。
  - `probe_delete` 后 `probe_list` 的表现。
  - 包含失败 step 的 replay plan。
  - 不同 task fingerprint 下 replay 默认拒绝，以及 `--force` 下 `ReplayWarning`。
  - finish 后 action、重复 finish、重复 close 的稳定失败行为。
  - finish 后 report、task.normalized、session snapshot、session summary、evidence index 和
    evidence view/raw/summary 文件引用一致性。
  - Core Dump Mode 下静态 action 可用，动态 action/probe 操作被 state guard 拒绝并产生
    `ToolError` evidence。
- 新增 `tests/task_parser_tests.cpp`，并接入 CTest 为 `task_parser_tests`。
- `task_parser_tests` 不依赖 GDB，覆盖：
  - required section 缺失。
  - shell-like args quoting/escaping。
  - 空字符串 argv token。
  - env 行格式和含 `=` 的 env value。
  - stdin/core dump 相对路径解析。
  - run timeout 校验。
  - 基础 JSON parse/type 边界。
- 本轮没有修改面向 Agent 的产品 action、schema 或文档化行为；主要变更是新增测试和记录薄弱点。
- `docs/ai/next_cli_task.md` 是本轮 execution task 记录，纳入本轮提交。

## 验证

- `cmake --build build`
- `./scripts/smoke_edge_cases.sh`
- `ctest --test-dir build --output-on-failure`

当前 Linux 环境安装了 GDB，因此 `daemon_action_flow`、`core_dump_mode` 和 `edge_case_flow`
都实际执行了 Linux + GDB smoke，而不是 skip。

## 发现的薄弱点

1. 现象：非法 action JSON 在 CLI client 解析阶段被拒绝，没有进入 daemon action handling，
   因此不会产生 `ToolError` evidence。
   复现测试或命令：`scripts/smoke_edge_cases.sh` 中 `gdb-agent action S1 '{not-json'`。
   影响范围：Agent 通过 CLI 传入非法 JSON 时只能看到进程级错误，不能在 session artifacts 中审计。
   建议后续处理方式：考虑让 daemon/client 对 action parse error 统一返回 JSON，并在 session 存在时写
   `ToolError` evidence。
   是否已在本轮修复：否；本轮记录为测试发现的 weakness。

2. 现象：缺少 `action` 字段时返回 `ok:false`，但没有 `ToolError` evidence。
   复现测试或命令：`scripts/smoke_edge_cases.sh` 中 `gdb-agent action S1 '{}'`。
   影响范围：错误路径不会进入 evidence index，报告无法审计该输入错误。
   建议后续处理方式：在 `handle_action_line` 解析到 missing action 时写 `ToolError` evidence。
   是否已在本轮修复：否。

3. 现象：`evaluate` 缺少 expression、`breakpoint_set` 缺少 location、`watchpoint_set`
   缺少 expression、`raw_mi` 缺少 `risk:"advanced"` 时返回 `ok:false`，但没有
   `ToolError` evidence。
   复现测试或命令：`scripts/smoke_edge_cases.sh` 对应 missing payload cases。
   影响范围：常见错误输入不能从 artifacts 中完整追踪。
   建议后续处理方式：为 action-level validation failure 统一封装 `ToolError` evidence 和
   response `evidence` 字段。
   是否已在本轮修复：否。

4. 现象：`frame_select` 使用负数时当前返回 `ok:true`，GDB 错误只保留在 command evidence 中。
   复现测试或命令：`scripts/smoke_edge_cases.sh` 中 `{"action":"frame_select","frame":-1}`。
   影响范围：Agent 可能误以为 frame 切换成功。
   建议后续处理方式：检查 `CommandResult::result_class`，对 GDB `error` 映射为 `ok:false`
   并记录 `ToolError` 或明确 action evidence。
   是否已在本轮修复：否。

5. 现象：`evaluate` 使用非法表达式时当前返回 `ok:true`，GDB 错误只保留在 command evidence 中。
   复现测试或命令：`scripts/smoke_edge_cases.sh` 中
   `{"action":"evaluate","expression":"definitely_missing_symbol"}`。
   影响范围：Agent 可能误判 evaluate 成功，需要额外打开 evidence 才能看到 GDB error。
   建议后续处理方式：同 `frame_select`，将 GDB `result_class=error` 映射为结构化 failure。
   是否已在本轮修复：否。

6. 现象：`probe_delete` 后 `probe_list` 仍返回该 probe metadata，只是内部标记为 deleted。
   复现测试或命令：`scripts/smoke_edge_cases.sh` 中 breakpoint set/delete/probe_list flow。
   影响范围：Agent 可能把 deleted probe 当作仍可命中的 live probe。
   建议后续处理方式：明确 `probe_list` 是否展示 deleted 历史项；如果不展示，应默认过滤；
   如果展示，应在 docs/report 中显式说明 deleted 语义。
   是否已在本轮修复：否。

## 限制和注意事项

- 本轮没有把上述 weakness 转成 hard fail；按任务要求，测试对结构稳定性 hard fail，对已发现但
  尚未修复的行为记录 warning/weakness，避免把错误期望固化为通过条件。
- 本轮没有新增产品 action、catchpoint event、numeric hypothesis assertion、PTY 或 daemon 架构重构。
- `edge_case_flow` 依赖 Linux、GDB 和 `python3`；缺少任一条件时按 smoke 口径 skip。
