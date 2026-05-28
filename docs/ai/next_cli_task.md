# Next CLI Task

## 目标

收敛 Probe Store 的运行期和持久化语义：运行中以内存 `ProbeState` 为权威状态，
`assets/probes.json` 只作为 finish-time 报告产物写出，不再作为每次 probe 变化时同步落盘的
运行时状态文件。

## 背景语义

- `ProbeState` 是 live session 运行期权威状态，保存在内存中。
- `assets/probes.json` 是从 `ProbeState` 派生出的最终报告快照。
- `probes.json` 不是 live GDB session 恢复文件，也不是运行时同步数据库。
- 跨 session 复现仍应依赖 replay 高层 action，而不是读取旧 `probes.json` 恢复 GDB 状态。

## 范围

### 1. 调整 probe 持久化时机

- 移除或停止在以下 action 中立即写 `assets/probes.json`：
  - `breakpoint_set`
  - `watchpoint_set`
  - `catchpoint_set`
  - `probe_enable`
  - `probe_disable`
  - `probe_delete`
  - probe hit 更新
- 在 `finish` / `finish_session` 写最终报告产物时，统一从内存 `ProbeState` 写出
  `assets/probes.json`。
- daemon `finish` 和 `serve` stdin EOF/finish 路径都应写出最终 probe snapshot。

### 2. 保留运行期可观察性

- `probe_list` 继续从内存 `ProbeState` 返回当前 metadata。
- `probe_list` 如需产生 evidence，可以记录当前内存 metadata 的 snapshot，但不要求写
  `assets/probes.json`。
- on-hit、hit count、enabled/deleted、last stop reason 仍由内存 `ProbeState` 维护。

### 3. 命中 evidence 保留必要上下文

- `BreakpointHit` / `WatchpointHit` / `CatchpointHit` evidence 应包含当次命中的必要 probe
  metadata 快照，例如：
  - number
  - kind
  - location 或 expression/event
  - condition
  - comment
  - purpose
  - hit_count
- 这样即使 session 异常退出，关键命中 evidence 仍能解释“为什么停在这里”。

### 4. 文档同步

- 更新 `docs/evidence_model.md`，说明 `probes.json` 是 finish-time report artifact，不是运行时
  状态同步文件或恢复文件。
- 如 action 文档中提到 probe metadata 持久化，要同步调整 `docs/agent_actions.md` 和
  `docs/agent_actions.en.md`。
- 更新 `docs/ai/progress.md` 和 `docs/ai/handoff.md`。

## 不做

- 不实现从 `probes.json` 自动恢复 probe。
- 不改变 replay schema。
- 不扩展 catchpoint 类型。
- 不重构 unrelated action。

## 完成标准

- 运行期 probe 操作不再频繁写 `assets/probes.json`。
- finish 后 `assets/probes.json` 存在，并反映最终内存 `ProbeState`。
- `probe_list` 仍能返回当前 probe metadata。
- probe hit evidence 包含足够的 probe metadata 上下文。
- 现有 smoke/CTest 在当前平台通过；Linux + GDB live 测试如无法运行，要在 handoff 记录原因。
