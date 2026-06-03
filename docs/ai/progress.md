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
  - `catchpoint_set`（仅 `event: "throw"`）
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
- 最小 catchpoint metadata、on-hit action 和 `CatchpointHit` evidence。
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
    命中和 `probe_list` metadata。
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
probe list 和 probe hit evidence；已有最小 `catch throw` catchpoint。on-hit 已有 policy
schema、failure policy、自动 continue 行为记录、`OnHitAction` evidence 和 Linux + GDB live
smoke 覆盖。watchpoint stop 现在能在 MI 提供编号或当前唯一 active watchpoint 时归属到
`WatchpointHit`，并执行 watchpoint on-hit policy。

仍需关注：

- catchpoint 仍只支持 `catch throw`，其他 catchpoint 类型尚未实现。
- on-hit policy 目前只限制 `OnHitAction` wrapper evidence 的 response 摘要预算；底层 action
  evidence 仍按 evidence store 的全局规则保留。
- 还可以增加更多 watchpoint/catchpoint 命中 fixture，覆盖多 watchpoint 且 GDB stop record
  缺编号时的降级归属 evidence。

## Phase 4: Hypothesis Workflow

状态：Mostly Done

已经有 create/check/conclude、结构化 check result、assertion、evidence 关联、Markdown
记录、机器可读 index 和 report 聚合。

仍需关注：

- assertion 类型仍然保持小集合，后续可按真实需求增加 numeric 比较。
- report 中 hypothesis 聚合已有基础视图，后续可继续优化长 observed 的展示和跳转体验。

## Phase 5: 深度摘要和高级 MI

状态：Early

已有 MI value parser、基础 C++ 类型 sanitizer、backtrace/thread summary 和 raw MI audit
metadata。`raw_mi` 已作为受限高级 escape hatch。

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

## 建议的下一步

1. 用真实 Linux GDB raw 输出继续校准 MI parser、类型 sanitizer 和 backtrace/thread summary。
2. 补 catchpoint 其他事件。
3. 按真实调试需求继续扩展 hypothesis assertion，例如 numeric 比较。
4. 增加更多真实项目 core/replay/probe fixture，覆盖 core dump 兼容性、失败策略、
   watchpoint/catchpoint on-hit 和跨 assets 目录报告展示。
