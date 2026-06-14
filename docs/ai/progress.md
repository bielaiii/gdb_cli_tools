# Progress

本文件是面向后续 Agent 的项目进度索引。它不是完整 changelog，而是帮助快速判断：
当前代码已经覆盖了哪些设计点，下一步应该补哪里。

## 已有基础

- CMake/C++20 项目结构已建立。
- 主程序为 `gdb-agent`。
- 示例程序 `examples/segfault.cpp` 和示例 task 已存在。
- 文档已有：
  - `docs/task_format.md`
  - `docs/agent_actions.md`
  - `docs/evidence_model.md`
- 设计输入已有：
  - `final_feature.md`
  - `design.md`

## 当前已实现或已有入口

- Markdown task file 解析，包含 problem、executable、working directory、args、stdin、
  env、run timeout、core dump。
- shell-like argv 解析。
- GDB/MI process/session 基础封装。
- Run Mode 初始运行和 Core Dump Mode 最小取证。
- daemon/create/action/list/status/finish/close/shutdown 形式的 live session 调用。
- stdin 非交互输入文件，stdout/stderr 重定向并作为 inferior output 取证。
- evidence store，包含 raw、summary、view、index、raw hash、record attribution 字段。
- session raw MI log、session snapshot 和 session summary。
- Markdown report 输出。
- 高层 action：
  - `backtrace`
  - `locals`
  - `registers`
  - `threads`
  - `args_info`
  - `frame_select`
  - `evaluate`
  - `breakpoint_set`
  - `watchpoint_set`
  - `catchpoint_set`（`event: "throw"` / `"catch"` / `"syscall"` / `"fork"` / `"vfork"` / `"exec"`）
  - `probe_list`
  - `probe_enable`
  - `probe_disable`
  - `probe_delete`
  - `continue`
  - `run`
  - `save_action`
  - `replay`
  - `hypothesis_create`
  - `hypothesis_check`
  - `hypothesis_conclude`
  - `raw_mi`
  - `finish_session`
- Replay JSONL 和结构化 replay plan 输出。
- `--replay-before-run`。
- breakpoint/watchpoint metadata、condition、comment、purpose、on-hit action。
- catchpoint metadata、可选 syscall selector、on-hit action 和 `CatchpointHit` evidence。
- hypothesis 记录文件和 `hypotheses/index.json`。
- action state guard，非法状态下记录 `ToolError` evidence。
- CTest 覆盖 README demo check；daemon/action live flow 有 Linux + GDB smoke 脚本，
  macOS 会按平台口径跳过 live 部分。

## 2026-05-27 本轮更新

- `docs/agent_actions.md`、`docs/evidence_model.md`、`docs/task_format.md` 已改为中文版默认入口。
- 原英文版文档保留为 `docs/agent_actions.en.md`、`docs/evidence_model.en.md`、
  `docs/task_format.en.md`。
- 新增 `examples/segfault_report.md`，作为 `examples/segfault.cpp` 的源码层面定位报告。
  当前环境没有安装 `gdb`，因此没有生成包含 raw MI evidence 的完整工具报告。
- 修复了 macOS libc++ 下 `filesystem` 时间戳 `rep` 为 `__int128` 时无法直接写入
  ostream 的构建问题。

## 2026-05-28 本轮更新

- 新增根目录 `README.md`，介绍项目目标、当前功能、构建方式、最小 demo、daemon flow 和
  设计边界。
- 更新 `docs/ai/next_cli_task.md`，下一轮任务聚焦把 README demo 固化为可自动验证的
  smoke test 或示例输出检查。

## 2026-05-28 本轮更新（demo smoke）

- 新增 `scripts/smoke_segfault_demo.sh`，一键执行 README 最小 demo 的 configure、build 和
  `gdb-agent check` 输出校验。
- 在 `CMakeLists.txt` 中启用 CTest，并新增 `segfault_demo_check`，用于在已构建的
  `build/` 目录上复跑同一个输出检查。
- README 的最小 Demo 章节已补充 smoke script 和 CTest 复现命令。

## 2026-05-28 任务口径更新

- 明确项目目标运行平台只支持 Linux；macOS 上 live GDB session 失败不作为阻塞项。
- 下一轮任务更新为：
  - 完成 daemon + action flow 的系统化回归测试。
  - 新增最小 catchpoint 支持，本轮只支持 `catch throw`。

## 2026-05-28 本轮更新（daemon/catchpoint）

- 新增高层 action `catchpoint_set`，本轮只接受 `{"event":"throw"}`，映射到 GDB
  `catch throw`。
- catchpoint 复用 probe store，支持 `comment`、`purpose` 和 `on_hit` metadata；
  `probe_list` 和最终 `assets/probes.json` 会展示 `kind: "catchpoint"` 与 `event: "throw"`。
- probe 命中记录已支持 `CatchpointHit` evidence；unsupported action 和不支持的
  catchpoint event 会记录 `ToolError` evidence。
- 新增 `scripts/smoke_daemon_action_flow.sh` 和 CTest `daemon_action_flow`，Linux + GDB 下覆盖
  daemon/create/status/连续 action/catchpoint/probe_list/非法 event/finish/shutdown 以及
  report、snapshot、summary、evidence index、probe store 生成；macOS 按平台口径 skip。

## 2026-05-29 任务口径更新

- 下一轮任务更新为收敛 Probe Store 语义：
  - 运行期以内存 `ProbeState` 为权威状态。
  - `assets/probes.json` 只在 finish/report 阶段统一写出。
  - probe hit evidence 保留必要 metadata 快照，便于异常退出后解释命中上下文。

## 2026-05-29 本轮更新（probe store）

- 收敛 Probe Store 持久化语义：运行期只维护内存 `ProbeState`，不再在
  `breakpoint_set`、`watchpoint_set`、`catchpoint_set`、probe enable/disable/delete、
  probe hit 或 `probe_list` 时同步写 `assets/probes.json`。
- `finish`、daemon `finish` 和 `finish_session` 路径会在写 report/session files 前统一从
  `ProbeState` 写出最终 `assets/probes.json`。
- `probe_list` 仍返回当前内存 metadata，并可记录 metadata evidence；probe hit evidence
  继续携带 number、kind、location/expression/event、condition、comment、purpose 和
  hit_count 等上下文。
- 更新 `docs/evidence_model.md`、`docs/evidence_model.en.md`、`docs/agent_actions.md`、
  `docs/agent_actions.en.md` 和 `docs/ai/decision.md`，明确 `probes.json` 是 finish-time
  report artifact，不是恢复文件。

## 2026-05-29 任务口径更新（summary/MI）

- 下一轮任务更新为强化 summary 和 MI 解析能力：
  - 更完整的 MI value parser。
  - C++ 类型 sanitizer。
  - thread/backtrace summarizer。
  - raw MI audit metadata 增强。

## 2026-05-29 本轮更新（summary/MI）

- 新增递归 MI value parser，支持 const string、tuple、list、result payload 和 stream record
  分类；解析失败时仍保留 raw，并回落到稳定 raw/decoded summary。
- 增强 C++ summary sanitizer，覆盖常见 `std::basic_string`/`std::__cxx11::basic_string`
  到 `std::string` 的归一化、allocator 噪声压缩、模板空格和 `> >` 归一化。
- 增强 backtrace/thread summary：backtrace 摘要会提取 frame/function/file:line，threads
  摘要按当前线程和普通线程生成低噪声行。
- Evidence index 和 Markdown view 增加 raw MI audit metadata：record sequence、token、
  record kind、result/async class、stream type。
- 新增 `mi_summary_tests`，覆盖 MI parser、type sanitizer、backtrace/thread summarizer 和
  raw MI audit，不依赖 GDB。

## 2026-05-31 本轮更新（replay store）

- 强化结构化 replay plan：继续使用 `gdb-agent-replay-plan-v1`，新增
  `schema_version`、plan tags、source session id、created_at、task metadata、task
  fingerprint 和 plan-level `failure_policy`。
- `save_action` / `save-action` 支持 `failure_policy`；`replay` / CLI `replay` 支持
  `failure_policy` override 和 `force`。
- replay 执行现在会返回结构化 step result，包含 step index、action name、status、
  failure policy、ReplayStep evidence、action evidence、error evidence 和 skip reason。
- 每次 replay 会额外记录 `ReplayRun` evidence，保存整体 replay result 快照，方便报告引用。
- 支持 `continue_on_error` 和 `stop_on_error`；step-level policy 可覆盖 plan-level policy。
  `stop_on_error` 触发后，后续 step 会记录为 skipped。
- replay 前校验 schema 和 task fingerprint；task mismatch 默认拒绝并记录 `ToolError`，
  force replay 会执行并记录 `ReplayWarning`。
- `session_summary.json` 新增 `replay_step_count` 和 `replay_warning_count`。
- 新增 `replay_plan_tests` 覆盖 schema、fingerprint、force mismatch、legacy plan 和
  failure policy；扩展 `scripts/smoke_daemon_action_flow.sh` 覆盖重启后 replay flow。

## 2026-05-31 本轮更新（probe on-hit policy）

- 将 probe `on_hit` 从旧 action 数组扩展为 policy object，并保留旧数组格式兼容。
  新 policy 支持 `actions`、`timeout_ms`、`max_output_bytes`、`max_summary_lines`、
  `failure_policy` 和 `continue_after_hit`。
- `breakpoint_set`、`watchpoint_set` 和 `catchpoint_set` 会解析并保存 on-hit policy；
  `raw_mi` 不能作为 on-hit action。
- probe hit evidence 现在包含 `on_hit_policy`、`on_hit_results`、`on_hit_evidence_ids` 和
  `on_hit_error_ids`。每个自动 action 会写入独立 `OnHitAction` evidence。
- 支持 on-hit `continue_on_error` / `stop_on_error`；`stop_on_error` 会把后续 action 记录为
  `skipped`。`continue_after_hit:true` 会追加自动 continue，并作为 `continue_after_hit`
  result 记录。
- `session_summary.json` 新增 `probe_hit_count`、`on_hit_action_count` 和
  `on_hit_error_count`。
- 报告的 Probes 区域会列出 probe hit 与 on-hit evidence，便于从 report 跳转到对应 summary。
- 扩展 `scripts/smoke_daemon_action_flow.sh`，Linux + GDB 下覆盖真实 breakpoint hit、
  on-hit 成功/失败/skipped、`continue_after_hit`、session summary、report、probes.json 和
  evidence index。

## 2026-06-01 本轮更新（hypothesis workflow）

- `hypothesis_check` 现在返回并持久化结构化 check result：hypothesis、check id、
  description、expression、assertion、expected、observed、status、evidence 和
  error evidence。
- `assets/hypotheses/index.json` 现在是机器可读的 hypothesis 入口，每个 check 包含
  `observed`、`status` 和 `error_evidence`；单个 hypothesis Markdown 同步展示相同链路。
- assertion 逻辑抽到 `src/workflow/hypothesis.cpp`，新增不依赖 GDB 的
  `hypothesis_assertion_tests`。
- 保留并覆盖已有 `none`、`contains`、`not_contains`、`is_null`、`non_null`，新增
  `equals` 和 `not_equals`。
- 未知 assertion 或缺少必需 `expected` 的 assertion 稳定返回 `status:"unknown"`，并记录
  `ToolError` evidence，不把 unknown 解释为支持或反驳 hypothesis。
- 最终 report 的 Hypotheses 区域现在从 `assets/hypotheses/index.json` 聚合 hypothesis、
  checks、observed、evidence id、Agent inference 和 final agent conclusion；index 缺失或
  解析失败时降级为列出单个 Markdown 文件。
- 扩展 Linux + GDB daemon smoke，覆盖 `hypothesis_create`、passed/failed/unknown
  `hypothesis_check`、`hypothesis_conclude`、finish report 和 hypotheses index 字段。

## 2026-06-01 本轮更新（core dump mode）

- 新增 `scripts/smoke_core_dump_mode.sh` 和 CTest `core_dump_mode`。
- smoke 使用 GDB batch 在 `read_session_value` 断点处 `generate-core-file`，避免依赖系统
  `core_pattern` 或 shell core dump 限制。
- daemon `create` core task 现在返回 `mode:"core"`，便于 Agent 直接判断 session 类型。
- `session_summary.json` 新增 `core_dump` 和 `core_loaded` 字段。
- Core Dump Mode 下 `run`、`continue`、`breakpoint_set`、`watchpoint_set`、`catchpoint_set`
  和 probe enable/disable/delete 会被 state guard 拒绝，并写 `ToolError` evidence。
- core smoke 覆盖 `check`、daemon create/status、`backtrace`、`threads`、`frame_select`、
  `args_info`、`locals`、`evaluate`、core-mode guard、finish report、session summary、
  snapshot 和 evidence index。
- 同步更新 task format、agent actions 和 evidence model 中英文文档。

## 2026-06-01 本轮更新（edge-case test hardening）

- 新增 `scripts/smoke_edge_cases.sh` 和 CTest `edge_case_flow`，Linux + GDB 下覆盖更复杂的
  daemon/session/error/replay/core/artifact 边界。
- `edge_case_flow` 覆盖：
  - 不存在 session 的 `status`、`action`、`finish`、`close`。
  - 非法 action JSON、缺少 action、缺少 expression/location/risk 等错误输入。
  - negative `frame_select`、非法 `evaluate` 表达式、on-hit raw MI 拒绝、watchpoint 设置失败。
  - `probe_delete` 后 `probe_list` 的 deleted metadata 暴露情况。
  - 包含失败 step 的 replay plan、不同 task fingerprint 下默认拒绝和 `--force` warning。
  - finish 后 action / 重复 finish / 重复 close 的稳定失败行为。
  - finish 后 report、task.normalized、snapshot、summary、evidence index 和 evidence 文件引用一致性。
  - Core Dump Mode 下静态 action 可用，动态 action/probe 操作被 state guard 拒绝并写
    `ToolError` evidence。
- 新增不依赖 GDB 的 `task_parser_tests`，覆盖 required section、shell-like args quoting/
  escaping、空字符串 arg、env 行、stdin/core dump path、run timeout 和 JSON 基础错误。
- 本轮未改产品行为，只将发现的边界弱点记录到 handoff，作为后续修复输入。

## 2026-06-02 本轮更新（capability matrix）

- 新增 `examples/capability_fixture.cpp` 和 CMake target `capability_fixture`，用一个确定性 fixture
  覆盖 probe、stdin/env/output、thread crash 和 core dump 生成场景。
- 新增 `scripts/smoke_capability_matrix.sh` 和 CTest `capability_matrix_flow`，Linux + GDB 下覆盖：
  - 真实 `watchpoint_set` 停止、两个真实 `breakpoint_set` 命中、`catchpoint_set event:"throw"`
    / `event:"catch"` 命中和 `probe_list` metadata。
  - on-hit 成功 action、unsupported action、`continue_on_error`、`stop_on_error` skipped evidence
    和 `continue_after_hit:true` wrapper evidence。
  - `raw_mi` 显式 `risk:"advanced"` 成功路径，以及缺少 risk 的稳定拒绝路径。
  - 真实停点上的 hypothesis create/check/conclude，覆盖 passed、failed、unknown、observed、
    error evidence、hypotheses index 和 report 聚合。
  - replay continue/stop failure policy、task fingerprint mismatch 默认拒绝、force warning、
    replay step/run evidence 和 session summary 计数。
  - inferior stdin/env/stdout/stderr 的 evidence 链路。
  - multithread crash 下的 backtrace、threads、frame_select、args_info、locals、registers、evaluate。
  - fixture core dump mode 下静态 action 可用，动态 action/probe 操作被 state guard 拒绝并写
    `ToolError` evidence。
  - 多个 session finish 后 report、task.normalized、snapshot、summary、evidence index 和
    view/raw/summary 文件引用一致性。
- 修复 `GdbSession::initialize` 中 task `env` 未实际传给 inferior 的问题：GDB `set environment`
  不能把整个 `KEY=value` 作为 quoted argument。
- 扩展 `mi_summary_tests`，覆盖 escaped MI string、nested list/tuple、target/log stream audit、
  MI result summary、vector allocator 和 unique_ptr default_delete sanitizer。
- 扩展 `hypothesis_assertion_tests`，覆盖空 observed 返回 `unknown`、equals/not_equals 的 expected
  trim 边界。
- `hypothesis` assertion 逻辑现在对非 `none` assertion 的空 observed 稳定返回 `unknown`，并且
  equals/not_equals 比较前会同时 trim observed 和 expected。

## 2026-06-02 本轮更新（failure semantics / watchpoint attribution）

- action validation failure 在 live session 上会稳定写 `ToolError` evidence，并在 response 中返回
  `evidence`；覆盖缺少 `action`、缺少 `expression`/`location`/`number`、`raw_mi` 缺少
  `risk:"advanced"`、replay 文件或 plan 校验失败等路径。
- `frame_select` 和 `evaluate` 遇到 GDB `result_class=error` 或 timeout 时返回 `ok:false`，
  同时保留原始 `GdbCommand` evidence，并用 `command_evidence` 关联到错误 response。
- 修复 CLI `JSON_OR_FILE` 判定：trim 后以 `{` 或 `[` 开头的参数优先作为 inline JSON，
  不再先调用 `fs::exists`，避免超长 action JSON 触发 `File name too long`。
- `probe_list` 默认只返回 active/live probe；`probe_delete` 后的历史 probe 仍可写入最终
  `assets/probes.json`，但标记 `deleted:true`。
- 增强 watchpoint stop 归属：优先读取 MI stop record 中的 watchpoint number；缺编号时，
  只有当前存在唯一 active watchpoint 才保守归属并执行 on-hit。真实 capability matrix 中
  `WatchpointHit`、watchpoint on-hit 和 session summary 计数已提升为 hard expectation。
- 更新 `scripts/smoke_edge_cases.sh`、`scripts/smoke_capability_matrix.sh` 和
  `scripts/smoke_core_dump_mode.sh`，把旧 weakness 改为修复后行为断言；core smoke 对真实
  GDB command error 改为要求结构化失败而不是误报成功。
- 同步更新 `docs/agent_actions.md` / `.en.md`、`docs/evidence_model.md` / `.en.md` 和
  `docs/ai/decision.md`。

## 2026-06-02 本轮更新（code review follow-up）

- 代码审查发现 task `env` 传递仍有一个边界问题：`GdbSession::initialize` 使用
  `-gdb-set environment KEY=value` 只覆盖了简单值，带空格 value 的真实 inferior 环境需要使用
  `set environment KEY <rest-of-line>` 语义。
- 修复 `GdbSession::initialize`，改为 `-gdb-set environment KEY value`，并扩展
  `scripts/smoke_capability_matrix.sh` 的 I/O fixture，验证 `MATRIX_ENV=fixture env spaced`
  能真实传给 inferior。
- 代码审查还发现 `run` / `continue` 底层 GDB control command 若返回 `result_class=error`，原实现仍会
  继续输出 `ok:true` stop response。现在这两类 GDB 拒绝路径会返回 `ok:false`，写
  `ToolError` evidence，并通过 `command_evidence` 指向原始 `StopEvent` command evidence。
- 同步更新 `docs/agent_actions.md` / `.en.md` 和 `docs/evidence_model.md` / `.en.md`，明确
  `run` / `continue` 的 GDB error 与 run deadline 语义分离。
- 仓库根目录未发现 `.clang-format`，因此本轮没有做全仓 clang-format，只保持改动块与现有风格一致。

## Phase 1: Live Session 和证据闭环

状态：Mostly Done

已经覆盖 live session、task format、Run Mode、Core Dump Mode smoke、run deadline、
light evidence、evidence store、session log、report、snapshot 和 summary。

仍需关注：

- 对更多 stop reason 的状态转换做回归测试。
- 继续用更多真实 core dump 和不同 GDB 输出版本验证 Core Dump Mode 兼容性。
- 让错误消息和 report 对 Agent 更稳定。
- 非法 JSON 在 CLI client 本地解析阶段失败时仍没有 session context，因此不会写入 session
  evidence；当前只要求 CLI 输出稳定错误而不是崩溃。

## Phase 2: Replay Store

状态：Implemented

已经支持 `save_action`、JSONL、结构化 replay plan、`replay`、failure policy、task
fingerprint 校验、force replay warning、replay run evidence 和 replay step evidence。

仍需关注：

- 给 replay plan 增加更完整的 tags 使用约定和适用 task metadata 展示。
- 扩展 replay 失败策略到更多 action 组合和真实项目 fixture。
- 增加跨进程/跨 assets 目录 replay 的更多报告聚合展示。

## Phase 3: Probe 和自动命中动作

状态：Mostly Done

已经支持 breakpoint/watchpoint、condition、comment、purpose、on-hit action、active-only
probe list 和 probe hit evidence；已有最小 `catch throw` / `catch catch` catchpoint。on-hit 已有 policy
schema、failure policy、自动 continue 行为记录、`OnHitAction` evidence 和 Linux + GDB live
smoke 覆盖。watchpoint stop 现在能在 MI 提供编号或当前唯一 active watchpoint 时归属到
`WatchpointHit`，并执行 watchpoint on-hit policy。

仍需关注：

- catchpoint 仍只支持 `catch throw` 和 `catch catch`，其他 catchpoint 类型尚未实现。
- on-hit policy 目前只限制 `OnHitAction` wrapper evidence 的 response 摘要预算；底层 action
  evidence 仍按 evidence store 的全局规则保留。
- 还可以增加更多 watchpoint/catchpoint 命中 fixture，覆盖多 watchpoint 且 GDB stop record
  缺编号时的降级归属 evidence。

## Phase 4: Hypothesis Workflow

状态：Mostly Done

已经有 create/check/conclude、结构化 check result、assertion、evidence 关联、Markdown
记录、机器可读 index 和 report 聚合。

仍需关注：

- assertion 类型已覆盖基础字符串/null、整数比较/range 和地址判断；后续可按真实需求增加
  float、changed 或跨 check 历史比较。
- report 中 hypothesis 聚合已有基础视图，后续可继续优化长 observed 的展示和跳转体验。

## Phase 5: 深度摘要和高级 MI

状态：In Progress

已有 MI value parser、基础 C++ 类型 sanitizer、backtrace/thread summary 和 raw MI audit
metadata。MI record summary 已能提取 result/async payload 中的 `msg`、`value`、`reason`、
`thread-id`、`stopped-threads`、`frame`、`bkpt` 和 `wpt` 等关键信号。C++ sanitizer 已增加轻量
STL template scanner，覆盖常见 vector/list/deque/set/map/unordered_map、smart pointer、
optional/variant/tuple/pair 和工作目录路径归一化。`raw_mi` 已作为受限高级 escape hatch。

仍需关注：

- MI parser 还可以继续覆盖更多 GDB/MI 边界格式。
- C++ 类型 sanitizer 已覆盖 vector allocator 和 unique_ptr default_delete 的基础压缩；仍需更多
  STL 容器、智能指针组合和用户类型 fixture。
- thread/backtrace summarizer 需要在 Linux + GDB raw 输出上继续校准。
- raw MI 的风险说明和跨 evidence 相关记录归因还可以更细。

## 2026-06-03 本轮更新（代码质量 review）

- 完成一轮面向性能开销、架构职责边界和行为风险的 code review，重点查看：
  - `src/cli.cpp` action dispatch、state guard、replay/probe/hypothesis glue。
  - `src/gdb/` MI command/control command 收发。
  - `src/evidence/` raw/summary/view/index 写入。
  - `src/workflow/` 静态/轻量取证入口。
  - `src/report/` report 聚合。
  - Linux + GDB smoke scripts。
- 修复静态 GDB-backed action 的失败语义：
  - `backtrace`、`locals`、`args_info`、`registers`、`threads` 和 `hypothesis_check`
    现在如果底层 GDB console command 返回 `result_class=error` 或命令 timeout，会返回
    `ok:false`。
  - response 保留错误 `evidence`，并通过 `command_evidence` 指向原始 `GdbCommand`
    evidence，避免 Agent 把 GDB 拒绝或无栈 core 的 `No stack.` 误判为成功取证。
  - `hypothesis_check` 的表达式 GDB command 失败时不再写入成功/失败/unknown check result。
- 新增 `collect_console_with_result`，保留原 `collect_console` 兼容默认取证流程，同时让 action
  dispatch 能检查 `CommandResult`。
- 扩展 `scripts/smoke_edge_cases.sh` 覆盖 `hypothesis_check` missing symbol 的
  `ok:false` + `command_evidence` 路径。
- 调整 `scripts/smoke_core_dump_mode.sh` 和 `scripts/smoke_capability_matrix.sh` 中 core 静态
  action 断言：core fixture 可能没有可用 stack，静态 action 可以失败，但失败必须保留
  `command_evidence`。
- 同步更新 `docs/agent_actions.md`、`docs/agent_actions.en.md`、`docs/evidence_model.md` 和
  `docs/evidence_model.en.md` 的失败语义。
- 本轮 review 确认的后续维护建议：
  - `src/cli.cpp` 仍是主要职责集中点，后续可渐进拆出 action handlers、probe/replay/hypothesis
    store helpers 和 daemon request handling。
  - `EvidenceStore::add*` 每新增一条 evidence 都全量重写 `evidence/index.json`，当前 smoke 规模可接受；
    若真实 session 产生大量 evidence，建议后续改为可审计的 append/journal 或 finish-time compact
    index 策略。

## 2026-06-03 本轮更新（Agent 友好能力增强）

- 扩展 `hypothesis_check` assertion：
  - 新增 `greater_than`、`less_than`、`greater_equal`、`less_equal`。
  - 同时新增 `equals_number`、`not_equals_number`。
  - numeric parser 支持十进制、负数和 `0x` 十六进制整数，并跳过 GDB value-history 前缀
    （例如 `$1 = 42` 中的 `$1`）。
  - observed/expected 无法解析整数、缺少 expected 或存在多个不同整数时稳定返回
    `status:"unknown"`，继续通过既有路径记录 `ToolError` evidence。
  - 当前不支持浮点数。
- 增强 summary sanitizer：
  - 在已有 `std::string`、vector allocator 和 unique_ptr default_delete 降噪基础上，增加常见
    `std::map<K, V, std::less<K>, std::allocator<std::pair<...>>>` 到 `std::map<K, V>` 的压缩。
  - 增加常见 `std::unordered_map<K, V, std::hash<K>, std::equal_to<K>, std::allocator<std::pair<...>>>`
    到 `std::unordered_map<K, V>` 的压缩。
  - 继续保持 raw evidence 不变，只影响 summary/view。
- 改进 report 的 Agent 可读性：
  - Hypotheses 表格新增 observed summary 列。
  - `unknown` 或带 `error_evidence` 的 check 会在 Hypotheses 区域额外列为
    `Checks needing attention`。
  - report 新增 `Tool Errors` 区域，按 evidence id 汇总 action、error、summary，并在存在
    `command_evidence` 时展示原始 GDB command evidence 链路。
- 扩展测试：
  - `hypothesis_assertion_tests` 覆盖 numeric assertion pass/fail/unknown、负数、十六进制、
    缺少 expected、无数字和多数字歧义。
  - `mi_summary_tests` 覆盖 map/unordered_map 降噪和路径相对化。
  - `scripts/smoke_capability_matrix.sh` 增加真实 `greater_than` hypothesis check，并断言 report 中
    `Tool Errors`、`Checks needing attention`、`Observed Summary` 和 numeric assertion 展示。
- 同步更新 `docs/agent_actions.md`、`docs/agent_actions.en.md`、`docs/evidence_model.md` 和
  `docs/evidence_model.en.md`。

## 2026-06-03 本轮更新（advanced hypothesis assertions）

- 继续扩展 `hypothesis_check` assertion：
  - 新增 `between`，`expected` 使用 `LOW..HIGH`，闭区间判断，边界支持十进制、负数和 `0x`
    十六进制整数。
  - 新增 `address_non_null`，从 observed 中解析唯一 `0x...` 地址，非零时 `passed`。
  - 新增 `address_equals`，从 observed 和 expected 中各解析唯一 `0x...` 地址，相等时
    `passed`。
  - `between` 的 expected 缺失、边界格式无效、LOW 大于 HIGH、无法解析整数，以及 address
    assertion 无法解析地址或存在多个不同地址时，稳定返回 `status:"unknown"` 并沿用
    `ToolError` / `error_evidence` 链路。
- 继续校准低噪声 summary：
  - 在已有 `std::string`、allocator/default_delete、map/unordered_map 降噪基础上，增加
    `std::pair<const K, V>` 和 `std::pair<K const, V>` key const 噪声压缩。
  - raw evidence、session MI log 和 raw 文件布局不变。
- 改进 report：
  - Hypotheses 的 `Checks needing attention` 会对带 `error_evidence` 的 check 反查并展示
    对应 `ToolError` summary，方便 Agent 直接看到 unknown 原因。
  - Limitations 明确提醒 hypothesis observed value 是有损 summary，最终结论前应检查 linked raw
    evidence。
- 扩展测试和 smoke：
  - `hypothesis_assertion_tests` 覆盖 `between`、`address_non_null`、`address_equals` 的
    pass/fail/unknown、负数、十六进制、缺少 expected、边界无效和多值歧义。
  - `mi_summary_tests` 覆盖 `std::pair<const K, V>` / `std::pair<K const, V>` 降噪。
  - `scripts/smoke_capability_matrix.sh` 增加真实 `between` hypothesis check，并断言 report 中
    `between` 和 `unknown assertion: numeric_greater_than` 的 error summary 可见。
- 同步更新 `docs/agent_actions.md`、`docs/agent_actions.en.md`、`docs/evidence_model.md` 和
  `docs/evidence_model.en.md`。

## 2026-06-03 本轮更新（task scope refinement verification）

- 本轮 `docs/ai/next_cli_task.md` 将 advanced hypothesis assertions 任务口径收窄为：
  - 只聚焦 `between` / address assertion 和 summary/sanitizer。
  - 不把 report 改进作为主目标。
- 核对当前 HEAD，上一轮提交 `2297e19 Add advanced hypothesis assertions` 已覆盖该收窄任务的完成标准：
  - `between`、`address_non_null`、`address_equals` 已实现并有单元测试和 smoke 覆盖。
  - `std::pair<const K, V>` / `std::pair<K const, V>` key const 噪声压缩已实现并有
    `mi_summary_tests` 覆盖。
  - 末尾建议已从“numeric 比较”更新为 float、changed 或跨 check 历史比较。
- 本轮没有新增源码行为变更，只验证当前实现与更新后的任务口径一致，并保留本轮任务记录。

## 2026-06-03 本轮更新（report scope verification）

- 本轮 `docs/ai/next_cli_task.md` 再次将 report 小幅增强纳入 advanced hypothesis assertions 范围：
  - Hypotheses unknown check 展示 error summary。
  - Tool Errors 稳定展示 action、error、ToolError evidence id 和 `command_evidence`。
  - Limitations 提醒 hypothesis observed 来自有损 summary。
- 核对当前 HEAD，上一轮提交 `2297e19 Add advanced hypothesis assertions` 已覆盖这些 report 信号：
  - `src/report/report.cpp` 的 `Checks needing attention` 会反查并展示 error summary。
  - `Tool Errors` 表格包含 Evidence、Action、Error、Command Evidence 和 Summary。
  - `scripts/smoke_capability_matrix.sh` 已断言 `## Tool Errors`、`Checks needing attention`、
    `between` 和 `unknown assertion: numeric_greater_than`。
- 本轮没有新增源码行为变更，只验证当前实现与更新后的任务口径一致，并保留本轮任务记录。

## 2026-06-03 本轮更新（MVP documentation convergence）

- 进入 MVP 收敛文档阶段，本轮未新增调试功能、未修改源码行为。
- README 增加 MVP 文档入口，并补齐当前 action 列表中的 `catchpoint_set`。
- 新增 `docs/mvp_acceptance.md`：
  - 明确 Linux 目标平台、构建/check/daemon flow、Run Mode、Core Dump Mode、evidence、report、
    replay/probe/hypothesis 和 CTest 回归入口的 MVP 验收条件。
- 新增 `docs/mvp_quickstart.md`：
  - 面向 Agent 记录从 task file、check、daemon create、第一轮 action、hypothesis workflow 到
    finish report 的最小 playbook。
- 新增 `docs/known_limitations.md`：
  - 集中说明 Linux/GDB、非 PTY、非交互 stdin、snapshot/replay、Core Dump Mode、raw MI、
    summary/report、hypothesis 和 assertion 支持范围等限制。
- 新增 `docs/mvp_dogfood.md`：
  - 记录如何使用 `examples/segfault_task.md` 完成一轮最小 dogfood，区分工具观察和 Agent inference，
    并明确不提交 generated assets。
- 当前过期的“numeric 比较”后续建议已修正；整数 numeric assertion 已完成，后续 assertion 扩展不阻塞
  MVP 收敛。

## 2026-06-08 本轮更新（catch catch）

- 扩展 `catchpoint_set`，在既有 `event:"throw"` / `catch throw` 基础上新增
  `event:"catch"` / `catch catch`。
- `event:"catch"` 复用现有 catchpoint probe metadata、`CatchpointHit` evidence、
  on-hit policy 和 Core Dump Mode state guard，不新增 evidence schema。
- `scripts/smoke_capability_matrix.sh` 增加 `catch catch` 覆盖，验证 action response、
  `probe_list` metadata、`CatchpointHit` evidence、`assets/probes.json` 和
  `session_summary.json` probe hit 计数。
- 同步更新 `docs/agent_actions.md`、`docs/agent_actions.en.md`、`docs/known_limitations.md`
  和 `docs/mvp_acceptance.md`。

## 2026-06-08 本轮更新（MI summary hardening）

- 强化 MI record audit：malformed non-numeric token prefix 不再被误判为带 token 的 MI record。
- 强化 MI record summary：对 result/async payload 提取 `msg`、`value`、`reason`、
  `thread-id`、`stopped-threads`、`frame`、`bkpt` 和 `wpt` 等低噪声字段。
- 扩展 sanitizer：新增轻量 `std::...<...>` template scanner，压缩常见 STL 容器、
  map/unordered_map、smart pointer、optional/variant/tuple/pair 噪声，并规范工作目录下
  `build/../src/file.cpp` 这类路径。
- 改进 backtrace/thread summary：保留 `thread apply all bt` 的 thread boundary，识别
  `from /lib/...so` shared-library 来源，并保持 frame/thread truncation 稳定。
- 大幅扩展 `tests/mi_summary_tests.cpp`，覆盖 MI value parser 成功/失败路径、record audit、
  payload summary、sanitizer、backtrace/thread summary 和 EvidenceStore 集成；测试不依赖 GDB。
- 同步更新 `docs/evidence_model.md`、`docs/evidence_model.en.md` 和
  `docs/known_limitations.md`。

## 2026-06-09 本轮更新（catchpoint matrix hardening）

- 扩展 `catchpoint_set`：
  - 新增 `event:"syscall"`，映射到 GDB `catch syscall`。
  - `event:"syscall"` 支持 `name` 或 `syscall` selector，映射到 `catch syscall <selector>`。
  - 新增 `event:"fork"` / `"vfork"` / `"exec"`，分别映射到对应 GDB catch command。
  - syscall selector 支持字符串名或整数 id；字符串只允许字母、数字和下划线，避免拼接任意
    GDB console command。
- Probe metadata 新增 `selector` 字段；action response、`probe_list`、finish-time
  `assets/probes.json` 和 `CatchpointHit` evidence 都会记录 catchpoint `event` 与 selector。
- 扩展 `examples/capability_fixture.cpp`，新增 `syscall`、`fork` 和 `exec` 模式，稳定触发
  新增 catchpoint event；非 Linux 构建用条件编译保持可构建。
- 新增 `scripts/smoke_catchpoint_matrix.sh` 和 CTest `catchpoint_matrix_flow`：
  - 使用 `--replay-before-run` 在 initial run 前设置 catchpoint。
  - Linux + GDB 下真实覆盖 generic syscall、`write` syscall selector、fork 和 exec 命中。
  - 验证 `probe_list` metadata、`CatchpointHit` evidence、`assets/probes.json`、
    `session_summary.json`、report evidence 引用和 raw/view/summary 文件存在。
  - 覆盖 unsupported event、非法 syscall selector 和 `syscall` selector 字段别名。
- 扩展 `scripts/smoke_capability_matrix.sh` 的 Core Dump Mode guard，确认新增动态 catchpoint event
  在 core mode 下继续被拒绝并写 `ToolError` evidence。
- 更新 `scripts/smoke_daemon_action_flow.sh`，旧 unsupported catchpoint 负例从 `syscall` 改为
  `not-real`，避免与新增能力冲突。
- 同步更新 `docs/agent_actions.md`、`docs/agent_actions.en.md`、`docs/evidence_model.md`、
  `docs/evidence_model.en.md`、`docs/known_limitations.md` 和 `docs/mvp_acceptance.md`。
- 本轮未新增项目级 decision；属于既有高层 action、raw evidence 优先和 probe metadata 决策下的能力扩展。

## 2026-06-09 本轮更新（type sanitizer hardening）

- 收敛 C++ type sanitizer 的策略压缩语义：
  - 默认 `std::allocator<T>`、`std::less<T>`、`std::hash<T>`、`std::equal_to<T>` 和
    `std::default_delete<T>` 只在对应 STL 容器 / `unique_ptr` 上下文中压缩。
  - 自定义 deleter、allocator、comparator、hash 和 equality 类型默认保留，不再通过全局 regex
    误删。
  - map/unordered_map/set/vector/list/deque/unique_ptr 遇到不认识的策略类型时保留完整参数。
- 增强 sanitizer 支持：
  - `std::basic_string_view<char, std::char_traits<char>>` -> `std::string_view`。
  - `std::array<T, N>`、`std::function<R(Args...)>`、`std::ratio<N, D>`、
    `std::chrono::duration<Rep, Period>` 和
    `std::chrono::time_point<Clock, Duration>` 的 spacing / ratio 噪声归一化。
  - template arg splitter 现在识别函数类型括号，避免把 `std::function<int(A, B)>` 的参数逗号误判为
    template 分隔符。
- 新增 `tests/type_sanitizer_tests.cpp` 和 CMake target / CTest `type_sanitizer_tests`，覆盖默认策略压缩、
  自定义策略保留、新类型支持、nested 组合和真实 GDB 输出抽取出的代表性字符串。
- 新增 `examples/type_sanitizer_fixture.cpp`，用真实类型字段覆盖默认 STL 策略、自定义策略、
  `std::string_view`、`std::array`、`std::function`、`std::chrono::duration` 和
  `std::chrono::time_point`。
- 新增 `scripts/smoke_type_sanitizer.sh` 和 CTest `type_sanitizer_flow`，Linux + GDB 下通过
  `raw_mi` / `ptype` 抓真实类型输出，并验证 summary 中默认策略被降噪、自定义策略仍可见，以及
  finish 后 report/evidence index 文件引用一致。
- 同步更新 `README.md`、`docs/evidence_model.md`、`docs/evidence_model.en.md`、
  `docs/known_limitations.md` 和 `docs/mvp_acceptance.md`。
- 本轮未新增项目级 decision；raw evidence、session log 和 raw 文件布局不变，sanitizer 仍只是有损
  summary/view 增强。

## 2026-06-09 本轮更新（performance review follow-up）

- 根据 code review 结果优化字符串处理性能：
  - `Json` object lookup 改用 `std::less<>` 和 `std::string_view` key，避免大量字符串字面量
    lookup 构造临时 `std::string`。
  - `parse_json` / 内部 parser 改用 `std::string_view` 输入，number token 改用
    `std::from_chars`，避免 `substr` 后再 `std::stod` 的临时 string。
  - type sanitizer 的 template arg splitter 改为拆出 trimmed `std::string_view`，渲染和默认策略
    判断改为 view/append/分段比较，减少中间 vector/string/ostringstream 分配。
  - 删除 map/unordered_map/pair 的旧 regex 降噪路径；剩余 sanitizer regex 改为 static。
- 补强测试：
  - `type_sanitizer_tests` 增加默认策略压缩的精确输出断言。
  - `task_parser_tests` 覆盖非 null-terminated `std::string_view` JSON 输入和 key/fallback lookup。
  - `smoke_type_sanitizer.sh` 增加默认策略噪声的负向断言。
- 本轮未新增项目级 decision；不改变 action schema、evidence schema 或 raw evidence 保存原则。

## 2026-06-10 本轮更新（typed action boundary）

- 新增 `src/cli/action.hpp` / `src/cli/action.cpp`，引入内部 `ActionKind`、
  `ActionRequest` 和 `ActionResult`：
  - `ActionRequest` 在 action 输入边界从 JSON 中解析 action 名、常用 payload、timeout、
    replay/on-hit 相关字段等 typed 字段。
  - `ActionResult` 统一承载 action 执行结果、`ok`、`finished`、error、evidence id、
    command evidence id、结构化扩展字段和多行 prelude response。
  - action response 的最终 JSON dump 集中到 `action_result_to_json` /
    `action_result_line`，外部 CLI/daemon 输出字段保持兼容。
- `handle_action_line` 现在对外返回 `ActionResult`，调用方不再直接把 `ostream` 传入 action
  handler 边界；`serve` 和 daemon action 边界统一通过 `action_result_line` 输出 JSON。
- replay step 执行改为直接消费 `ActionResult.ok`、`ActionResult.error` 和 evidence 字段判断成功/
  失败，不再通过解析 response 文本中的 `ok:false` 控制 replay failure policy。
- on-hit action 执行改为直接消费 `ActionResult.ok`、`ActionResult.error` 和 evidence 字段判断成功/
  失败，不再通过解析 response 文本控制 on-hit `continue_on_error` / `stop_on_error`。
- 为保持既有 smoke 和外部输出兼容，`ActionResult` 保留结构化 `prelude_responses`，用于在
  on-hit/replay 等场景按原顺序输出子 action JSON 行，再输出主 action JSON 行。
- 新增项目级决策 D012，明确内部 action 流程使用 typed structs，JSON/string 只作为 CLI/daemon
  边界、replay plan 和 evidence/report artifact 格式；不引入 protobuf。
- 本轮未改变用户可见 action schema、response 字段语义、evidence schema、replay plan schema、
  report schema 或 raw evidence 文件布局。
- 说明：本轮完成了 typed action boundary 和 replay/on-hit 去 response 文本反解析；更深层的
  probe runtime、session runtime、daemon/client 物理拆分仍可在后续小步重构中继续推进。

## 2026-06-10 本轮更新（remove remaining action string transports）

- 继续落实 D012，将 action/on-hit/replay 的内部传输从 JSON/string/ostream 协议推进到 typed
  request/result：
  - `ActionRequest` 改为 `std::variant` typed payload，不再持有 raw `Json on_hit` 或
    `Json saved_action`。
  - `ActionResult` 删除 generic `Json fields` 和 `std::vector<Json> prelude_responses`；
    固定字段使用明确成员，半开放 response metadata 使用 `ResultField` / `ResultObject` /
    `ResultArray` 结构化 KV。
  - 新增 `ActionOutput { prelude, final }` 表达 on-hit/replay 的多行兼容输出，最终只在
    CLI/daemon 边界 dump JSON 行。
- 删除旧 `handle_action_line_legacy` / `handle_action_line` 字符串 handler 和 response 文本
  反解析 wrapper；action 分发改为 `dispatch_action(..., const ActionRequest&) -> ActionOutput`。
- on-hit policy 运行期改为保存 typed `ActionRequest`，执行时直接调用 typed dispatcher；
  timeout/deadline 注入通过 typed payload 的 default 标记完成，不再 parse/dump action JSON。
  `continue_after_hit` 也改为构造 typed continue request。
- replay runtime 引入 typed `ReplayActionStep`，structured plan、JSONL 和 legacy single-action
  文件读取后立即转换为 typed action；step 执行不再接收 action JSON string。
- `write_replay_plan` 内部 API 改为接收 `std::vector<ActionRequest>`，写文件时才把 typed action
  dump 成现有 `gdb-agent-replay-plan-v1` JSON schema；`replay_plan_tests` 同步使用 typed
  action 调用 writer。
- 保持外部行为兼容：CLI/daemon action JSON schema、response 字段、evidence schema、replay
  plan schema、report schema 和 raw evidence 文件布局未改。
- 同步更新 D012，明确请求侧已知 payload 使用 typed struct；结果侧天然半开放 metadata 可以保留
  结构化 KV，但不能用 JSON 文本或 generic `Json` AST 当内部协议。

## 2026-06-10 本轮更新（MI summary live fixture）

- 将 `docs/ai/next_cli_task.md` 更新为真实 Linux + GDB MI summary hardening 任务。
- 新增 `examples/mi_summary_fixture.cpp` 和 CMake target `mi_summary_fixture`：
  - fixture 先用 `SIGTRAP` 提供 create 后的稳定初始停点。
  - 启动 worker thread，便于真实 `info threads` 覆盖当前线程和普通线程。
  - 提供 noinline 调用链 `mi_summary_entry` -> `mi_summary_middle` -> `mi_summary_leaf` ->
    `mi_summary_observe_here`，便于 backtrace summary 校准。
  - 在用户函数停点暴露 STL/template 局部和参数，用于验证 summary 不回退到长模板噪声。
- 新增 `scripts/smoke_mi_summary_live.sh` 和 CTest `mi_summary_live_flow`：
  - Linux + GDB 下真实覆盖 daemon create、breakpoint_set、continue、backtrace、threads、
    locals、frame_select 和 raw_mi。
  - 断言 backtrace summary 包含稳定函数调用链和 `examples/mi_summary_fixture.cpp` 相对路径，
    且不包含仓库工作目录绝对路径。
  - 断言 threads summary 保留当前线程标记、普通线程、LWP/thread identity 和 stopped frame。
  - 断言 raw_mi result summary 包含 `result:done` 和 `value=`，并检查 evidence Markdown view
    中的 Raw MI Audit 表含 `result` / `stream` / `async` 等真实 record kind。
  - 断言真实 GDB `ptype` summary 中 `std::vector<std::string>` 和 `std::map<std::string, int>`
    保持低噪声，不出现 `std::__cxx11::basic_string<char, std::char_traits<char>` 或 `> >` 回归。
- 本轮没有改变 summary/sanitizer 行为实现；新增真实 fixture 后现有 summary 逻辑已经通过该
  Linux + GDB live 回归。
- 本轮未新增项目级 decision；不改变 action schema、response schema、evidence raw 文件布局、
  report schema 或 replay plan schema。

## 2026-06-10 本轮更新（ActionContext / action dispatch split）

- 执行 `docs/ai/next_cli_task.md` 中的行为保持型架构重构任务：抽出 action runtime context，并将
  action dispatch 从 `src/cli.cpp` 拆到独立模块。
- 新增 `src/cli/action_context.hpp`：
  - 将运行期 `ProbeState` 从 `src/cli.cpp` 移到共享 header。
  - 新增 `ActionContext`，集中借用 `GdbSession`、`DebugTask`、`SessionOutcome` 和 `ProbeState`。
- 新增 `src/cli/action_dispatch.hpp` / `src/cli/action_dispatch.cpp`：
  - 公开 `dispatch_action(ActionContext&, const ActionRequest&)`。
  - 公开 `handle_action_request` / `handle_action_json` typed boundary。
  - 将原 `src/cli.cpp` 中的 action dispatch switch 迁移到新模块。
  - 同步迁移 action dispatch 直接依赖的 on-hit/replay/hypothesis/probe helper，避免
    `src/cli.cpp` 继续承载完整 action runtime。
- `src/cli.cpp` 现在保留 CLI/daemon/session/report 顶层 orchestration；创建 action 时构造
  `ActionContext` 再调用 action dispatch 模块。
- 更新 `CMakeLists.txt`，将 `src/cli/action_dispatch.cpp` 接入 `gdb-agent` target。
- 保持外部行为兼容：未改变 CLI 命令、action JSON schema、response JSON 字段、evidence schema、
  replay plan schema、report schema 或 raw evidence 文件布局。
- 本轮未新增项目级 decision；属于 D012 typed action boundary 下的物理模块拆分。

## 2026-06-10 本轮更新（probe/replay/hypothesis runtime split）

- 执行 `docs/ai/next_cli_task.md` 中的 1-3 架构拆分任务，保持行为和外部 schema 不变：
  - 新增 `src/workflow/probe_runtime.hpp` / `src/workflow/probe_runtime.cpp`。
  - 新增 `src/replay/replay_runtime.hpp` / `src/replay/replay_runtime.cpp`。
  - 新增 `src/workflow/hypothesis_store.hpp` / `src/workflow/hypothesis_store.cpp`。
- `ProbeState` 不再定义在 `src/cli/action_context.hpp`：
  - probe metadata、hit attribution、probe snapshot、on-hit action 执行和 probe list result
    生成移入 `probe_runtime`。
  - `ProbeState` 继续保存 probe runtime 状态，并通过 typed `ActionRequest` /
    `ActionOutput` 执行 on-hit action；没有引入内部 JSON/string action 协议。
- replay 执行从 `src/cli/action_dispatch.cpp` 移入 `replay_runtime`：
  - JSONL/plan 读取、step execution、failure policy、ReplayStep/ReplayRun evidence 和最终
    replay result 构造集中到 replay runtime。
  - `save_action` 仍复用现有 replay plan schema，`replay_runtime` 提供
    `rebuild_replay_plan_from_jsonl`。
- hypothesis markdown/index 持久化从 action dispatch 移入 `hypothesis_store`：
  - `HypothesisStore` 保存 hypothesis records 和 id counter。
  - `hypothesis_file_for`、`write_hypothesis_index`、append markdown helper 集中在 store 模块。
  - action dispatch 只保留 action payload 校验、GDB evidence collection 和对 store API 的调用。
- `src/cli/action_dispatch.cpp` 现在聚焦 typed action dispatch switch，不再承载完整 probe/on-hit
  runtime、replay execution runtime 或 hypothesis store persistence。
- 更新 `CMakeLists.txt`，将三个新 runtime/store 源文件接入 `gdb-agent` target。
- 保持外部行为兼容：未改变用户可见 action schema、response schema、evidence schema/raw 文件布局、
  replay plan schema、report schema、CLI 语法、GDB/MI parser、type sanitizer 或 hypothesis
  assertion 语义。
- 本轮未新增项目级 decision；属于 D012 typed action boundary 下的物理模块拆分。

## 2026-06-13 本轮更新（real workflow smoke）

- 执行 `docs/ai/next_cli_task.md` 中的真实多步骤 workflow smoke 任务，聚焦 core/replay/probe/on-hit
  evidence 链路，不新增用户可见 action，不改变外部 schema。
- 新增 `examples/workflow_fixture.cpp` 和 CMake target `workflow_fixture`：
  - `live` mode 先用 `SIGTRAP` 提供稳定 create 停点，再依次触发 watchpoint、两个普通
    breakpoint、一个 `continue_after_hit` breakpoint 和 `write` syscall catchpoint。
  - `core` mode 在 `workflow_core_capture` 暴露稳定 frame、global value 和 node 指针，供 GDB
    batch 生成 core 后执行静态取证。
- 新增 `scripts/smoke_real_workflow_flow.sh` 和 CTest `real_workflow_flow`：
  - Linux + GDB 下覆盖 daemon create、带 metadata 的 breakpoint/watchpoint/catchpoint、
    on-hit success/failure/skipped、`continue_after_hit`、静态取证 action 和 hypothesis
    create/check/conclude。
  - live flow 保存 `stop_on_error` replay plan；第二个 session 使用同一 task 跨 session replay，
    覆盖成功 step、失败 step 和 skipped step，并检查 `ReplayStep` / `ReplayRun` /
    `ToolError` evidence。
  - core flow 使用 GDB `generate-core-file` 生成 fixture core，覆盖 core session 静态 action 和
    dynamic action/probe guard rejected 的 `ToolError` evidence。
  - smoke 统一检查 report、`task.normalized.json`、`session_summary.json`、
    `session_snapshot.json`、`evidence/index.json`、raw/summary/view 文件引用、`probes.json`
    deleted metadata、hypotheses index 和 report evidence id 引用一致性。
- 本轮没有修改 action JSON schema、response schema、replay plan schema、evidence raw 文件布局、
  snapshot/session summary 既有字段含义、GDB/MI parser、type sanitizer 或 hypothesis assertion
  语义。
- 本轮未新增项目级 decision；属于既有 core/replay/probe/hypothesis 能力的真实组合 workflow
  回归覆盖。

## 2026-06-13 本轮更新（replay setup plan/report audit）

- 执行 `docs/ai/next_cli_task.md` 中的 replay setup plan 和 report auditability hardening 任务，
  聚焦 `save-action`、`--replay-before-run`、probe setup、replay 失败策略、mismatch warning
  和报告可审计性。
- `watchpoint_set` 状态守卫已补齐：live session 的 ready/stopped/exited state 均可设置
  watchpoint，和 breakpoint/catchpoint 保持一致；core mode 仍拒绝动态 probe action。
  这使 setup replay plan 能在第一次 `run` 前安装 breakpoint、watchpoint 和 catchpoint。
- 扩展 `examples/workflow_fixture.cpp` 的 live path，新增稳定 C++ throw/catchpoint 事件，用于
  replay setup plan smoke 覆盖 catchpoint hit，避免 syscall catchpoint 在程序启动阶段产生过多
  环境相关 stop。
- 新增 `scripts/smoke_replay_setup_plan_flow.sh` 和 CTest `replay_setup_plan_flow`：
  - session RS1 通过 `save-action` 生成 probe setup plan，确认结构化 plan 保留 schema、tags、
    source session、task fingerprint 和高层 action。
  - session RS2 使用 `--replay-before-run` 在初始运行前安装 breakpoint/watchpoint/catchpoint，
    并真实命中三类 probe，产生 `BreakpointHit`、`WatchpointHit`、`CatchpointHit` 和
    `OnHitAction` evidence。
  - session RS3 replay `stop_on_error` plan，覆盖成功 step、失败 step、skipped step、
    `ReplayStep`、`ReplayRun` 和 `ToolError` evidence。
  - session RS4 覆盖 task fingerprint mismatch 默认拒绝和 `--force` warning，确认
    `ReplayWarning`、`ToolError`、`replay_warning_count` 和报告 warning table。
  - smoke 统一检查 report、assets、session summary、snapshot、evidence index、raw/summary/view
    文件和 report evidence id 引用一致性。
- 增强最终报告：
  - `Replay Plans` 从结构化 plan 文件列出 plan file、name、tags、source session、failure
    policy 和 task fingerprint。
  - 新增 `Replay Execution Audit` 区域，从结构化 `ReplayRun`、`ReplayStep` 和 `ReplayWarning`
    evidence 生成 run、step 和 warning 表；不从 action response 文本反解析 replay 结果。
- 更新 `docs/agent_actions.md` / `.en.md` 和 `docs/evidence_model.md` / `.en.md`，记录
  before-run setup plan、watchpoint ready-state 支持和 replay audit report 行为。
- 本轮没有修改 replay plan schema、action JSON schema、response schema、evidence raw 文件布局、
  task format、GDB/MI parser、type sanitizer 或 hypothesis assertion 语义。
- 本轮未新增项目级 decision；属于既有 replay/probe evidence 模型和 D012 typed action boundary
  下的真实工作流与报告审计增强。

## 2026-06-13 本轮更新（core dump report/replay audit）

- 执行 `docs/ai/next_cli_task.md` 中的 Core Dump Mode report、metadata audit 和 replay semantics
  任务，不新增用户可见 action，不改变 Core Dump Mode 静态取证边界。
- 修复 core 加载命令：
  - 原 `-target-select core "<path>"` 在当前 GDB/MI 环境会把引号当成路径字符，导致 absolute
    core path 被错误解析为 working directory 下的带引号相对路径。
  - 现在通过 MI `-interpreter-exec console "core-file <path>"` 加载 core，保留 GDB/MI 接入方式。
  - core 加载失败时不再把 `stop_reason` 标成 `core_loaded`；只在 GDB 成功加载 core 后采集静态
    core evidence。
- 增强最终报告：
  - Core Dump Mode 下新增 `Core Dump Snapshot` 区域。
  - 汇总 mode、executable、working directory、core dump path、`core_loaded` 状态、session state、
    stop reason、problem 摘要。
  - 汇总 `Core load`、core files/libraries、threads、backtraces、frame args、locals、registers 等
    静态 evidence id。
  - 汇总 core-mode guard rejected 的 `ToolError` evidence，显示 action 和 reason。
- 扩展 `scripts/smoke_core_dump_mode.sh`：
  - 继续用 GDB batch `generate-core-file` 生成真实 core。
  - 检查 `session_summary.json` 中 `core_dump` 与 task core path 一致、`core_loaded:true`、
    evidence count 与 index 一致。
  - 检查 `task.normalized.json`、`session_snapshot.json`、`evidence/index.json` 和 raw/summary/view
    文件引用一致性。
  - 检查 report 的 `Core Dump Snapshot` 包含 core path、loaded 状态、静态 evidence 和 core
    guard rejection。
  - 新增 core mixed replay 覆盖：
    - `continue_on_error`：静态 `backtrace` success、动态 `continue` failed、后续 `args_info`
      success。
    - `stop_on_error`：静态 `backtrace` success、动态 `breakpoint_set` failed、后续 `locals`
      skipped。
    - 检查 `ReplayStep`、`ReplayRun`、`ToolError` evidence 和 report `Replay Execution Audit`
      中的 failed/skipped/error evidence/skip reason。
- 更新 `docs/agent_actions.md` / `.en.md` 和 `docs/evidence_model.md` / `.en.md`，记录
  Core Dump Snapshot、core mixed replay 语义和 core guard evidence 链路。
- 本轮没有修改 action JSON schema、CLI 语法、replay plan schema、task format、evidence raw
  文件布局、snapshot/session summary 既有字段含义、GDB/MI parser、type sanitizer 或 hypothesis
  assertion 语义。
- 本轮未新增项目级 decision；属于 D011 Core Dump Mode 静态取证边界内的审计和回归增强。

## 2026-06-14 本轮更新（session executor boundary）

- 执行 `docs/ai/next_cli_task.md` 中的 session concurrency and operation executor 任务，先建立
  session 串行执行边界，不实现高层 record action，也不新增 action 组二进制持久化。
- 新增 `src/cli/session_executor.hpp` / `src/cli/session_executor.cpp`：
  - 提供 `SessionOperationExecutor` 和 `execute_session_operation` 作为 typed action execution
    boundary。
  - 通过 `SessionOperationOrigin` 标记 direct、replay step 和 on-hit action 调用来源，为后续
    record append hook 预留位置。
  - 本轮实现是同步 executor，没有引入 OS thread 或后台 worker。
- `GdbSession` 新增 per-session `std::recursive_mutex` operation lock：
  - executor 执行 action 时持锁，确保同一 session 的 action 语义串行。
  - 使用 recursive mutex 允许 `run` / `continue` 命中 probe 后在同一线程中执行 on-hit 子 action。
- 普通 action、replay step 和 on-hit action 已统一经过 executor 或共享底层 execution entry：
  - `handle_action_request` 现在通过 `execute_session_operation`。
  - replay step 通过 `execute_session_operation(... ReplayStep)` 执行。
  - on-hit action 和 `continue_after_hit` 通过 `execute_session_operation(... OnHitAction)` 执行。
- 保持外部行为兼容：未修改 action JSON schema、response 字段语义、replay plan schema、task
  format、evidence raw 文件布局、report schema、Core Dump Mode 静态边界或 raw MI risk 语义。
- 本轮新增项目级 decision：
  - D013：高层 action record 是 replay 的运行期来源，后续默认内存保存，并提供持久化接口。
  - D014：先建立 session 串行执行边界，再实现 record。

## 建议的下一步

1. 继续收集不同 GDB 版本和真实项目 core 的 raw 输出，扩展 MI summary fixture 覆盖面。
2. 视目标环境稳定性，为 `vfork` 增加真实 hit smoke，或记录不同 GDB/target 组合的 catchpoint
   capability 差异。
3. 按真实调试需求继续扩展 hypothesis assertion，例如 float、changed 或跨 check 历史比较。
4. 增加更多真实项目 core/replay/probe fixture，覆盖 core dump 兼容性、失败策略、
   watchpoint/catchpoint on-hit 和跨 assets 目录报告展示。
5. 后续若继续架构收敛，可把 session lifecycle / daemon client glue 从 `src/cli.cpp` 继续拆出；
   本轮未触碰该范围。
