# Next CLI Task

## 目标

完成 daemon + action flow 的系统化回归测试，并补最小 catchpoint 能力。

## 平台口径

- 目标运行平台只支持 Linux。
- macOS 上 live GDB session 失败是允许的，不作为阻塞项。
- 不依赖 GDB 的构建、`check`、smoke test 仍可在 macOS 上运行。

## 范围

### 1. Daemon + action flow 回归测试

新增自动化测试或脚本，像 Agent 一样走完整 CLI 流程：

- `gdb-agent daemon --socket ...`
- `gdb-agent create ... --session S1`
- `gdb-agent status S1`
- `gdb-agent action S1 ...`
- `gdb-agent finish S1 --out ...`
- `gdb-agent shutdown ...`

至少覆盖：

- daemon 能启动并接受 socket 请求。
- create/status/finish/shutdown 基础生命周期。
- 连续 action 能保持同一个 live session 状态。
- action 返回稳定 JSON，包含 `ok`、`action`、必要时包含 `evidence`。
- report、`session_snapshot.json`、`session_summary.json`、`evidence/index.json` 能生成。
- 非法 action 或非法状态应返回 `ok:false`，并记录 `ToolError` evidence。

如果当前环境不是可用 Linux + GDB，测试应能优雅跳过 live session 部分，并在输出或
handoff 中说明原因。

### 2. 最小 catchpoint 支持

新增高层 action：

```json
{"action":"catchpoint_set","event":"throw"}
```

本轮只支持 `event: "throw"`，映射到 GDB：

```gdb
catch throw
```

要求：

- 支持 `comment`、`purpose`、`on_hit` metadata，尽量复用现有 probe store 结构。
- 命中时记录 `CatchpointHit` evidence，或在当前 probe hit 结构无法区分时先记录清楚的
  stop/probe evidence。
- `probe_list` 应能展示 catchpoint metadata。
- GDB 拒绝 catchpoint 时返回 `ok:false`，并记录 `ToolError` evidence。
- 更新 `docs/agent_actions.md`，必要时同步英文版 `docs/agent_actions.en.md`。

## 不做

- 不支持 `catch catch`、`catch syscall`、`catch fork` 等其他 catchpoint。
- 不扩展复杂 catchpoint 参数。
- 不重构 replay schema。
- 不实现 macOS live debugging 兼容。

## 完成标准

- Linux + GDB 环境下 daemon + action flow 回归测试通过。
- macOS 环境下非 live 部分仍能通过，live 部分可跳过且说明原因。
- `catchpoint_set` 只接受 `event: "throw"`；其他 event 有稳定错误输出和 `ToolError`
  evidence。
- 文档、progress 和 handoff 更新完整。
