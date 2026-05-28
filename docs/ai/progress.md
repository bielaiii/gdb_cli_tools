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
  `probe_list` 和 `assets/probes.json` 会展示 `kind: "catchpoint"` 与 `event: "throw"`。
- probe 命中记录已支持 `CatchpointHit` evidence；unsupported action 和不支持的
  catchpoint event 会记录 `ToolError` evidence。
- 新增 `scripts/smoke_daemon_action_flow.sh` 和 CTest `daemon_action_flow`，Linux + GDB 下覆盖
  daemon/create/status/连续 action/catchpoint/probe_list/非法 event/finish/shutdown 以及
  report、snapshot、summary、evidence index、probe store 生成；macOS 按平台口径 skip。

## Phase 1: Live Session 和证据闭环

状态：Mostly Done

已经覆盖 live session、task format、Run Mode、最小 Core Dump Mode、run deadline、
light evidence、evidence store、session log、report、snapshot 和 summary。

仍需关注：

- 对更多 stop reason 的状态转换做回归测试。
- 验证 Core Dump Mode 在不同 GDB 输出版本下的兼容性。
- 让错误消息和 report 对 Agent 更稳定。

## Phase 2: Replay Store

状态：Implemented, Needs Hardening

已经支持 `save_action`、JSONL、结构化 replay plan、`replay` 和 replay step evidence。

仍需关注：

- 明确 replay 失败策略是否始终继续，还是允许 per-step policy。
- 给 replay plan 增加版本、tags、适用 task metadata 的校验。
- 增加重启后 replay 的端到端示例和测试。

## Phase 3: Probe 和自动命中动作

状态：Partially Done

已经支持 breakpoint/watchpoint、condition、comment、purpose、on-hit action、probe list 和
probe hit evidence；已有最小 `catch throw` catchpoint。

仍需关注：

- catchpoint 仍只支持 `catch throw`，其他 catchpoint 类型尚未实现。
- on-hit action 的超时、最大输出、最大行数等策略需要更完整。
- Probe Store 的持久化恢复语义需要进一步明确。

## Phase 4: Hypothesis Workflow

状态：Partially Done

已经有 create/check/conclude、assertion、evidence 关联、Markdown 记录和 index。

仍需关注：

- assertion 类型还比较少。
- hypothesis check 结果需要更强的结构化表达。
- 报告中 hypothesis 区域可以进一步聚合工具观察和 AI 结论。

## Phase 5: 深度摘要和高级 MI

状态：Early

已有基础 summary 和 backtrace summary。`raw_mi` 已作为受限高级 escape hatch。

仍需关注：

- 更完整的 MI value parser。
- 更好的 C++ 类型 sanitizer。
- 更强的 backtrace/thread summarizer。
- raw MI 的审计字段和风险说明可以更细。

## 建议的下一步

1. 在 Linux + GDB 环境跑 `scripts/smoke_daemon_action_flow.sh`，确认 live daemon flow 真实通过。
2. 补 catchpoint 其他事件和 on-hit policy 限制。
3. 强化 replay plan schema 与失败策略。
4. 扩展 hypothesis assertion 与报告聚合。
5. 继续把 `docs/agent_actions.md` 和代码中的 action 行为保持同步。
