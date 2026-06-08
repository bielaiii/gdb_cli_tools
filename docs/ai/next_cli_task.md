# Next CLI Task

## 目标

本轮扩展 `catchpoint_set` 的最小能力：在已有 `event:"throw"` 的基础上，新增支持
`event:"catch"`，映射到 GDB 的 `catch catch`。

这是一个小而完整的 Execution task，预期足够支撑约半小时工作：需要修改 action dispatch、
probe metadata、smoke 测试和文档，但不牵涉架构重构或大范围 report/evidence schema 变更。

## 背景

当前项目已经有最小 catchpoint 支持：

- `catchpoint_set` 只接受 `{"event":"throw"}`。
- 内部映射到 `catch throw`。
- probe store 会保存 `kind:"catchpoint"`、`event:"throw"` 和 `location:"catch throw"`。
- `probe_list`、`assets/probes.json`、`CatchpointHit` evidence 和 on-hit policy 已能处理
  catchpoint。
- `examples/capability_fixture.cpp` 的 probe 模式已经包含：
  - 抛出 `std::runtime_error`
  - 在 `catch (const std::exception &ex)` 中捕获并打印

因此新增 `event:"catch"` 应该优先复用现有 catchpoint/probe/on-hit 机制，而不是引入新的
probe 类型或改 evidence schema。

## 范围

### 1. 扩展 `catchpoint_set` action

修改 `src/cli.cpp` 中 `catchpoint_set` 的处理逻辑：

- 接受 `event:"throw"` 和 `event:"catch"`。
- `event:"throw"` 继续映射到 GDB console command `catch throw`。
- `event:"catch"` 映射到 GDB console command `catch catch`。
- response 中继续返回：
  - `ok`
  - `action:"catchpoint_set"`
  - `catchpoint`
  - `event`
  - `evidence`
- probe metadata 中：
  - `kind` 仍为 `catchpoint`
  - `event` 保存原始 event，即 `throw` 或 `catch`
  - `location` 保存对应 GDB 命令文本，例如 `catch catch`
- 保持 unsupported event 的稳定失败语义：
  - 返回 `ok:false`
  - `error:"unsupported catchpoint event"`
  - 写入 `ToolError` evidence

不要新增 raw MI 入口，不要改变 `raw_mi` 风险约束。

### 2. 保持 Core Dump Mode guard

Core Dump Mode 下 `catchpoint_set` 仍然必须被 state guard 拒绝。

本轮不要改变 Core Dump Mode 语义，也不要尝试在 core 文件里支持 catchpoint。

### 3. 扩展 smoke 覆盖

优先修改已有 Linux + GDB smoke，而不是新增全新大脚本。

建议更新：

- `scripts/smoke_capability_matrix.sh`

覆盖点：

- 在 capability fixture 的 probe flow 中设置 `{"action":"catchpoint_set","event":"catch"}`。
- 验证 response 包含：
  - `"action":"catchpoint_set"`
  - `"event":"catch"`
  - `"ok":true`
- 继续运行到对应 catchpoint hit。
- 验证至少一个 artifact 能看到 `event:"catch"`：
  - action response
  - `probe_list`
  - final `assets/probes.json`
  - evidence index / report 中的 `CatchpointHit`

如果在当前 fixture 的执行顺序中同时设置 `catch throw` 和 `catch catch` 会让 stop sequence
复杂化，可以只在 capability matrix 中新增一个单独 session 或小段 flow，专门验证
`catch catch`。保持脚本可读，不要为了复用而让流程变脆。

可选更新：

- `scripts/smoke_daemon_action_flow.sh`

如果改动很小，可以补一条 `event:"catch"` 的 action response / `probe_list` 检查；但不要让
daemon smoke 变成主要复杂度来源。

### 4. 更新文档

同步更新中文默认文档，英文版保持一致：

- `docs/agent_actions.md`
- `docs/agent_actions.en.md`
- `docs/known_limitations.md`
- `docs/mvp_acceptance.md`，如果其中仍写死“只支持 catch throw”
- `README.md`，如果当前功能列表或限制里写死“只支持 catch throw”

文档口径：

- `catchpoint_set` 当前支持 C++ exception 的：
  - `event:"throw"` -> `catch throw`
  - `event:"catch"` -> `catch catch`
- 其他 catchpoint event 仍未实现。
- Unsupported event 仍稳定返回 `ToolError` evidence。

如果修改了 `docs/evidence_model.md`，只做必要的小同步；本轮不改变 evidence schema。

### 5. 更新 AI 记录

任务结束时更新：

- `docs/ai/progress.md`
- `docs/ai/handoff.md`

仅当产生新的项目级决策时才更新：

- `docs/ai/decision.md`

预计本轮不需要新增项目级 decision，因为这是 D007 Probe metadata 和现有 catchpoint 设计下的
小扩展。

不要修改：

- `docs/ai/current_goal.md`
- `docs/ai/next_cli_task.md`，除非执行中发现任务描述必须修正

## 验证

至少运行：

```bash
cmake --build build
./build/gdb-agent check examples/segfault_task.md
```

如果当前机器是 Linux 且安装了 GDB，额外运行：

```bash
ctest --test-dir build --output-on-failure
```

或者至少运行被修改覆盖到的 smoke：

```bash
./scripts/smoke_capability_matrix.sh
```

如果当前机器没有 GDB，或不是 Linux，必须在 `docs/ai/handoff.md` 和最终回复里明确说明
live smoke 未完整运行的原因。

提交前运行：

```bash
git diff --check
git status --short
```

## 不做

- 不支持其他 catchpoint event，例如 syscall、load、unload、fork、exec、signal。
- 不重构 `src/cli.cpp`。
- 不改变 evidence schema。
- 不改变 Core Dump Mode 静态取证边界。
- 不新增 PTY 或交互式 stdin。
- 不扩展 hypothesis assertion。
- 不改 summary/sanitizer。
- 不做全仓格式化。

## 提交要求

这是一轮完整 Execution task。完成后需要：

1. 只 stage 本轮相关文件。
2. 创建一次 git commit。
3. 推送当前分支到 upstream；如果没有 upstream，推送到当前分支的同名远程分支。
4. 如果 commit/push 因网络、认证、权限或远程配置失败，在 `docs/ai/handoff.md` 和最终回复中记录原因。
