# Next CLI Task

## 目标

修复上一轮 edge-case / capability matrix 测试暴露出的工具可靠性薄弱点，让 Agent 面对失败动作、
GDB error、watchpoint stop 和 probe 删除状态时，能从 response、evidence、summary/report 中得到
一致、可审计的信号。

本轮聚焦产品行为一致性修复，不新增新的调试能力。优先解决 `docs/ai/handoff.md` 和
`docs/ai/progress.md` 已记录的 weakness：

1. action validation failure 没有统一写 `ToolError` evidence。
2. `raw_mi` 缺少 `risk:"advanced"` 时没有 `ToolError` evidence。
3. CLI inline JSON 参数过长时会先走 `fs::exists` 并触发 `File name too long`。
4. `frame_select` / `evaluate` 遇到 GDB `result_class=error` 时仍可能返回 `ok:true`。
5. `probe_delete` 后 `probe_list` 仍返回 deleted probe metadata，语义不清。
6. watchpoint 能让 inferior 停止，但缺少 `WatchpointHit` evidence 和 on-hit 归属。

## 背景

当前项目已完成：

- live session、Run Mode、Core Dump Mode。
- evidence store、raw/summary/view/index、session summary/snapshot/report。
- replay store、probe/on-hit policy、hypothesis workflow。
- edge-case smoke 和 capability matrix smoke。

上一轮测试不是缺少覆盖，而是已经把几个“Agent 会被误导”的点暴露出来：

- 有些失败只在 response 里出现，没有 evidence，最终 report 无法审计。
- 有些 GDB error 被包装成 `ok:true`，Agent 可能误判动作成功。
- watchpoint stop 有状态信号，但没有 probe metadata 归属链路。
- `probe_list` 对 deleted probe 的展示会让 Agent 分不清 live probe 和历史 probe。

本轮应把这些错误路径收敛成稳定契约。

## 范围

### 1. 统一 action validation failure 的 `ToolError` evidence

梳理 CLI/daemon action handling 中“已经有 session，但 action payload 校验失败”的路径。

至少覆盖：

- 缺少 `action` 字段。
- `evaluate` 缺少 `expression`。
- `breakpoint_set` 缺少 `location`。
- `watchpoint_set` 缺少 `expression`。
- `raw_mi` 缺少 `risk:"advanced"`。
- `raw_mi` on-hit 禁止路径。
- replay 文件不存在或 replay validation 明确失败时，如果已有 session，应保证 response/evidence 一致。

要求：

- 返回 `ok:false`。
- response 包含：
  - `action`，如果能确定。
  - `error`。
  - `evidence`。
- 写入 `ToolError` evidence。
- `evidence/index.json`、view/summary/raw 文件完整。
- report/session summary 可以反映这些错误 evidence。

注意：

- 非法 JSON 如果在 CLI client 本地解析阶段就失败，可能没有 session context。本轮不强制把它写入 session
  evidence，但应尽量让 CLI 输出稳定 JSON 或至少避免崩溃/异常噪声。
- 不要把工具 validation failure 解释成 Agent 结论。

### 2. 修复 inline JSON_OR_FILE 参数判定

当前长 inline action JSON 作为 CLI 参数传入时，client 可能先调用 `fs::exists(arg)`，导致
`filesystem error: File name too long`。

要求：

- 调整 JSON_OR_FILE 判定逻辑。
- 如果参数看起来是 JSON object/array，例如 trim 后以 `{` 或 `[` 开头，应优先当作 inline JSON 文本，
  不做 filesystem lookup。
- 文件路径仍然可用。
- 不引入新 CLI 产品形态；本轮只修复现有参数判定。
- 为长 inline JSON 增加回归测试，至少在 smoke 中覆盖一个足够长的 on-hit policy 或 replay action。

### 3. 将 GDB command error 映射为结构化 action failure

修复 `frame_select` 和 `evaluate` 等 action 在底层 GDB 返回 `result_class=error` 时仍返回 `ok:true`
的问题。

至少覆盖：

- `frame_select` 使用负数或明显越界 frame。
- `evaluate` 使用不存在 symbol 或非法表达式。

要求：

- action response 返回 `ok:false`。
- response 包含 `action`、`error`、`evidence`。
- 原 command evidence 仍保留 raw MI。
- 额外 `ToolError` evidence 或等价错误 evidence 应能让 Agent 不打开 raw 也知道动作失败。
- 不破坏正常 `frame_select` / `evaluate` 成功路径。
- 同步更新 `docs/agent_actions.md` / `.en.md` 中相关失败语义。
- 如 evidence schema 或 report 表达变化，更新 `docs/evidence_model.md` / `.en.md`。

### 4. 明确并修复 `probe_delete` / `probe_list` 语义

当前 `probe_delete` 后 `probe_list` 仍返回 deleted probe metadata。需要选择并实现一个清晰语义。

推荐语义：

- `probe_list` 默认只返回 active/live probes。
- deleted probe 作为历史信息保留在 internal state 或 finish artifact 中，但必须标记 `deleted:true`。
- 如果当前 action API 不支持 include-deleted，本轮不要新增复杂筛选参数；默认避免让 Agent 把 deleted probe
  当成 live probe。

要求：

- `probe_delete` 后再次 `probe_list` 不应把 deleted probe 表示为可用 live probe。
- `assets/probes.json` 如果仍包含 deleted 历史项，必须明确 `deleted` 字段。
- report 对 deleted probe 的展示必须不误导。
- 同步更新 `docs/agent_actions.md` / `.en.md` 和 `docs/evidence_model.md` / `.en.md`。
- 扩展 `scripts/smoke_edge_cases.sh` 或 `scripts/smoke_capability_matrix.sh` 覆盖删除后行为。

### 5. 增强 watchpoint stop 归属

上一轮 capability fixture 已确认 `watchpoint_set` 能让 inferior 停止，并返回
`stop_reason:"watchpoint-trigger"`，但没有 `WatchpointHit` evidence，也没有执行 watchpoint on-hit action。

本轮目标是做最小可用增强：

- 当 GDB/MI stop record 能提供 watchpoint/breakpoint number 时，关联到对应 `ProbeState`。
- 如果 stop record 缺少 probe number，尝试从当前 watchpoint probe state、raw MI stop reason 或
  `-break-list` 结果做保守关联。
- 如果仍不能唯一归属，至少写入一个降级 `WatchpointHit` 或 `ToolError`/`SessionEvent` evidence，说明
  watchpoint-trigger 已发生但无法唯一关联 probe。

要求：

- 真实 watchpoint stop 后 evidence index 能看到 watchpoint 相关 evidence。
- 能唯一关联时，`WatchpointHit` evidence 携带：
  - probe number。
  - expression。
  - condition/comment/purpose。
  - hit_count。
  - stop reason。
  - on-hit result/evidence ids。
- 能唯一关联时执行 watchpoint on-hit policy，语义与 breakpoint/catchpoint on-hit 一致。
- 不能唯一关联时，不要伪造确定性 probe number；记录降级 evidence，并在 handoff 中说明限制。
- 扩展 capability matrix smoke，把 watchpoint evidence/on-hit 从 warning 尽量提升为 hard expectation。

### 6. 测试更新

更新现有测试，不要新增过多重复脚本。

至少更新：

- `scripts/smoke_edge_cases.sh`
- `scripts/smoke_capability_matrix.sh`

按需要更新或新增不依赖 GDB 的单元测试：

- action validation helper。
- JSON_OR_FILE 判定 helper。
- probe list filtering / deleted state helper。
- watchpoint stop attribution helper。

要求：

- 测试应验证修复后的行为，而不是继续把 weakness 当 warning。
- 如果某项 watchpoint 归属因为 GDB 输出缺失不能稳定 hard fail，需要明确保留 warning，并在
  `docs/ai/handoff.md` 说明原因。

## 文档同步

按实际行为更新：

- `docs/agent_actions.md`
- `docs/agent_actions.en.md`
- `docs/evidence_model.md`
- `docs/evidence_model.en.md`

如果修改了项目级语义，更新：

- `docs/ai/decision.md`

任务结束时必须更新：

- `docs/ai/progress.md`
- `docs/ai/handoff.md`

## 不做

- 不新增新的 Agent-facing action。
- 不扩展新的 catchpoint event。
- 不实现 numeric hypothesis assertion。
- 不重构 daemon/session 架构。
- 不引入 PTY 或交互式 stdin。
- 不把工具变成自动根因分析器。
- 不为了 macOS live GDB 做兼容；目标运行平台仍是 Linux。
- 不做 unrelated cleanup。

## 完成标准

- action validation failure 在有 session context 时稳定写 `ToolError` evidence。
- `raw_mi` 缺少 `risk:"advanced"` 的拒绝路径有 evidence。
- 长 inline JSON action 不再触发 `File name too long`。
- `frame_select` / `evaluate` 的 GDB error 映射为 `ok:false` 或明确结构化失败。
- `probe_delete` 后 `probe_list` 不误导 Agent。
- watchpoint stop 至少有 watchpoint 相关 evidence；若能唯一归属，则有 `WatchpointHit` 和 on-hit 链路。
- Linux + GDB 环境运行：

```bash
cmake --build build
ctest --test-dir build --output-on-failure
./build/gdb-agent check examples/segfault_task.md
git diff --check
```

- `docs/ai/progress.md` 记录实际修复和剩余限制。
- `docs/ai/handoff.md` 记录完成内容、验证结果、仍未解决的 weakness。
- 按 Execution mode 约定提交并推送本轮相关改动。
