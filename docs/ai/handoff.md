# Handoff

日期：2026-06-02

## 本轮完成

- 完成 `docs/ai/next_cli_task.md` 指定的 capability matrix regression / exploration testing 任务。
- 新增 `examples/capability_fixture.cpp`，并接入 CMake 为 `capability_fixture`：
  - `probe` mode：SIGTRAP 后触发 watchpoint、两个 breakpoint、C++ throw/catch。
  - `io` mode：读取 stdin/env，写 stdout/stderr。
  - `thread-crash` mode：工作线程稳定 SIGSEGV。
  - `core` mode：用于 GDB batch 生成确定性 core file。
- 新增 `scripts/smoke_capability_matrix.sh`，并接入 CTest 为 `capability_matrix_flow`。
- `capability_matrix_flow` 在 Linux + GDB 下实际覆盖：
  - `backtrace`、`threads`、`frame_select`、`args_info`、`locals`、`registers`、`evaluate`。
  - 真实 breakpoint 命中和 `BreakpointHit` evidence。
  - 真实 watchpoint 停止路径，并记录当前 watchpoint hit evidence/on-hit 归属薄弱点。
  - `catchpoint_set event:"throw"` 真实命中和 `CatchpointHit` evidence。
  - `probe_list` 的 kind、comment、purpose、condition、hit_count metadata。
  - on-hit 成功 action、unsupported action、`continue_on_error`、`stop_on_error` skipped evidence、
    `continue_after_hit:true` wrapper evidence。
  - `raw_mi` 显式 `risk:"advanced"` 成功路径，以及缺少 risk 的拒绝路径。
  - hypothesis create/check/conclude，覆盖 passed、failed、unknown、observed、error evidence、
    hypotheses index、单个 hypothesis Markdown 和最终 report 聚合。
  - replay continue/stop failure policy、fingerprint mismatch 默认拒绝、force warning、
    replay run/step evidence 和 session summary 计数。
  - inferior stdin/env/stdout/stderr evidence。
  - thread crash 现场取证。
  - fixture core dump mode 静态 action 和动态 action/probe guard。
  - 多个 session finish 后 artifact consistency：report、task.normalized、session snapshot、
    session summary、evidence index、view/raw/summary 文件引用和 report evidence id 引用。
- 修复 task `env` 未实际传给 inferior 的问题：`GdbSession::initialize` 现在用未整体 quote 的
  `-gdb-set environment KEY=value`，避免 GDB 把带引号字符串当作错误变量名。
- 扩展 `tests/mi_summary_tests.cpp`：
  - escaped quotes。
  - nested list/tuple。
  - target/log stream record audit。
  - MI result summary。
  - vector allocator 和 unique_ptr default_delete sanitizer。
- 扩展 `tests/hypothesis_assertion_tests.cpp`：
  - 空 observed 对非 `none` assertion 返回 `unknown`。
  - equals/not_equals 比较前同时 trim observed 和 expected。
- 本轮未新增面向 Agent 的 action，未扩展 catchpoint event，未实现 numeric hypothesis assertion。
- `docs/ai/next_cli_task.md` 是本轮 execution task 输入记录，保留在本轮提交范围内。

## 验证

- `cmake --build build`
- `./build/mi_summary_tests`
- `./build/hypothesis_assertion_tests`
- `./build/replay_plan_tests`
- `./scripts/smoke_capability_matrix.sh`
- `ctest --test-dir build --output-on-failure`
- `./build/gdb-agent check examples/segfault_task.md`
- `git diff --check`

当前 Linux 环境安装了 GDB，因此 `daemon_action_flow`、`core_dump_mode`、`edge_case_flow` 和
`capability_matrix_flow` 都实际执行了 Linux + GDB smoke，而不是 skip。

## 发现的薄弱点

1. 现象：`raw_mi` 缺少 `risk:"advanced"` 时返回 `ok:false`，但没有 `ToolError` evidence。
   复现测试或命令：`scripts/smoke_capability_matrix.sh` 中
   `{"action":"raw_mi","command":"-gdb-version"}`。
   影响范围：Agent 能从 response 看到拒绝原因，但 session artifacts/report 中没有该拒绝事件。
   建议后续处理方式：为 `raw_mi` validation failure 统一写 `ToolError` evidence，并在 response
   返回 `evidence`。
   是否已在本轮修复：否；本轮只记录为 warning，避免扩大到 action validation 统一重构。

2. 现象：超长 inline action JSON 作为 CLI 参数传入时，client 先调用 `fs::exists(arg)`，
   可能触发 `filesystem error: File name too long`，还没进入 daemon action handling。
   复现测试或命令：直接执行较长的
   `gdb-agent action P1 '{"action":"watchpoint_set", ... 长 on_hit policy ...}' --socket ...`。
   影响范围：长 action payload 不能稳定作为 inline 参数使用，也无法写入 session-level
   `ToolError` evidence。
   建议后续处理方式：调整 `read_text_file_arg`，例如先判断字符串是否像 JSON object/array，再决定是否
   做 filesystem lookup；或者为 CLI 明确区分 `--json` 和 `--file`。
   是否已在本轮修复：否；测试脚本改为通过临时 JSON 文件发送长 action，记录该 CLI 弱点。

3. 现象：capability fixture 中 `watchpoint_set` 能让 inferior 停止，response 返回
   `stop_reason:"watchpoint-trigger"`，但没有 `WatchpointHit` evidence，也没有执行 watchpoint
   on-hit action。
   复现测试或命令：`scripts/smoke_capability_matrix.sh` 中 P1 session 的
   `watchpoint_set g_watch_value` 后第一次 `continue`。
   影响范围：Agent 知道 GDB 因 watchpoint 停止，但无法从 evidence index/report 中获得与
   probe metadata 绑定的 `WatchpointHit` 和 on-hit 链路。
   建议后续处理方式：增强 watchpoint stop 归属逻辑；当 GDB/MI stop record 缺少 breakpoint number
   时，尝试从 raw MI、`-break-list` 或当前 probe state 关联 watchpoint，至少记录降级
   `WatchpointHit`/`ToolError` evidence。
   是否已在本轮修复：否；本轮将其作为 capability matrix 暴露的非阻塞 weakness。

## 限制和注意事项

- 本轮修复了 env 传递、hypothesis assertion 空 observed/trim 边界和一个 sanitizer 噪声点；这些都是
  新测试暴露的最小一致性修复。
- capability matrix 仍没有覆盖 `probe_enable` / `probe_disable` / `probe_delete` 对后续命中的完整影响；
  相关边界已有 `edge_case_flow` 覆盖删除后 metadata 暴露问题，但后续仍应补更强的能力测试。
- watchpoint 的 stop/on-hit 归属没有作为 hard fail，是因为当前 GDB/MI stop response 在该 fixture 下
  缺少 probe number；脚本 hard fail watchpoint 停止能力，warning 记录 evidence 归属缺口。
