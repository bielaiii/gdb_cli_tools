# Handoff

日期：2026-06-14

## 本轮完成

- 执行 `docs/ai/next_cli_task.md` 中的 high-level action record 任务。
- 新增高层 record action：
  - `record_start`：开启 session 内存 `RecordingState`，支持 `name`、`failure_policy` 和
    `include_raw_mi`。
  - `record_status`：返回当前录制 active 状态、组名、failure policy、`include_raw_mi`、step count
    和最近 artifact 路径。
  - `record_stop`：把当前内存 action group 写到 `assets/replay/<name>.gar`，并生成
    `assets/replay/<name>.json` 人工可读 export。
  - `record_discard`：丢弃当前内存录制，不写 artifact。
- 新增 `src/replay/record_store.hpp` / `src/replay/record_store.cpp`：
  - `.gar` 是版本化二进制权威 artifact，magic 为 `GDBA_REC1`，当前 version 为 `1`。
  - artifact 保存 group name、source session id、created_at、failure policy、task metadata JSON
    和 high-level action JSON list。
  - JSON export 使用 replay plan 形状，标记 `record_artifact_schema:"gdb-agent-record-artifact-v1"`
    和 `record_artifact_authority:"binary"`，方便人工检查。
- `SessionOperationExecutor` 已接入 record append hook：
  - 只记录 direct Agent action。
  - 只在 action 返回 `ok:true` 后 append。
  - 默认排除 `record_*`、`replay`、`finish_session`、`save_action`、`raw_mi`、`Unknown`。
  - `raw_mi` 只有在 `record_start` 设置 `include_raw_mi:true` 时才会记录，原 action 仍需
    `risk:"advanced"`。
  - replay step 和 probe on-hit 子 action 不会被录入当前 direct recording。
- Replay runtime 已支持二进制 record artifact：
  - `gdb-agent replay S1 --file path/to/name.gar` 可直接执行。
  - `gdb-agent replay S1 <name>` 会优先选择 `assets/replay/<name>.gar`，不存在时继续 fallback 到
    `.json` 和 `.jsonl`。
  - `.gar` 执行时在内存转换为现有 replay plan，仍写现有 `ReplayRun` / `ReplayStep` /
    `ReplayWarning` / `ToolError` evidence。
- 新增验证覆盖：
  - `tests/record_store_tests.cpp`
  - `scripts/smoke_record_flow.sh`
  - CTest `record_store_tests`
  - CTest `record_flow`
- 更新文档：
  - `docs/agent_actions.md`
  - `docs/agent_actions.en.md`
  - `docs/evidence_model.md`
  - `docs/evidence_model.en.md`
  - `docs/ai/progress.md`
- 本轮没有更新 `docs/ai/decision.md`；D013/D014 已覆盖 record 默认内存、持久化接口、二进制优先和先建
  session executor 边界的决策。

## 验证

- `cmake --build build`
  - 结果：通过。
- `./build/gdb-agent check examples/segfault_task.md`
  - 结果：通过，输出 `ok`。
- `./build/replay_plan_tests`
  - 结果：通过，输出 `replay_plan_tests ok`。
- `./build/record_store_tests`
  - 结果：通过，输出 `record_store_tests ok`。
- `./scripts/smoke_record_flow.sh`
  - 结果：通过。
  - 覆盖 high-level record、`.gar` 二进制 artifact、JSON export 和 `.gar` replay。
- `./scripts/smoke_daemon_action_flow.sh`
  - 结果：通过。
  - 确认 daemon/action、catchpoint、on-hit policy 和 restart replay 无回归。
- `./scripts/smoke_replay_setup_plan_flow.sh`
  - 结果：通过。
  - 确认 replay setup plan、`--replay-before-run`、report audit 和 mismatch handling 无回归。
- `ctest --test-dir build --output-on-failure`
  - 结果：通过，`17/17 tests passed`。
- `git diff --check`
  - 结果：通过。

## 完成标准审计

- `record_start` / `record_status` / `record_stop` / `record_discard` 均已通过 action parser 和
  dispatch 支持。
- 默认 record state 保存在 session 内存中。
- direct Agent action 通过 executor 成功执行后 append 到 `RecordingState`。
- record 默认排除 `record_*`、`replay`、`finish_session`、`save_action`、`raw_mi`、失败 action、
  replay step 和 on-hit 子 action。
- 提供二进制持久化接口和人工可读字符串/JSON export。
- Replay 支持读取 `.gar`，并保持 JSON/JSONL 兼容。
- 用户文档和 evidence 文档已同步更新。

## 限制和注意事项

- 当前 record 持久化仅在 `record_stop` 发生；如果进程在录制中途崩溃，尚未 stop 的内存 actions 会丢失。
  这是 D013 中“默认内存保存，提供持久化接口”的当前实现形态。
- `.gar` 是本轮新增的 replay artifact 格式，不是 evidence raw/summary/view 文件；执行 replay 后仍通过
  现有 replay evidence 链路审计。
- 当前没有引入真正的后台 writer thread 或异步 WAL；后续如果要降低 host process crash 时的 action
  丢失窗口，需要单独设计。
- `record_status` 在 `record_stop` 后保留最近 artifact 路径和已清空的 step count；重新
  `record_start` 会清空这些路径。
