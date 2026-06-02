# Handoff

日期：2026-06-02

## 本轮完成

- 完成一轮代码审查，并修复两个会影响 Agent 判断或真实 fixture 能力的处理问题。
- 修复 task `env` 传递的边界问题：
  - `GdbSession::initialize` 现在使用 `-gdb-set environment KEY value` 形式。
  - 这样符合 GDB `set environment KEY <rest-of-line>` 语义，避免带空格的 env value 被错误保留引号或拆错。
  - 扩展 `scripts/smoke_capability_matrix.sh`，I/O fixture 现在验证
    `MATRIX_ENV=fixture env spaced` 能真实传给 inferior。
- 修复 `run` / `continue` 的 GDB control command error 语义：
  - 如果底层 GDB 返回 `result_class=error`，action 现在返回 `ok:false`。
  - response 包含错误 `evidence`。
  - 原始 command output 仍作为 `StopEvent` evidence 保留，并通过 `command_evidence` 关联。
  - run deadline / continue deadline 仍保留为 inferior 运行超时/工具中断语义，不当作 GDB command failure。
- 同步更新：
  - `docs/agent_actions.md`
  - `docs/agent_actions.en.md`
  - `docs/evidence_model.md`
  - `docs/evidence_model.en.md`
  - `docs/ai/progress.md`
- 仓库根目录未发现 `.clang-format` 文件；本轮没有进行全仓 clang-format，只保持改动块与现有代码风格一致。

## 验证

- `cmake --build build`
- `./scripts/smoke_capability_matrix.sh`
- `ctest --test-dir build --output-on-failure`
- `./build/gdb-agent check examples/segfault_task.md`
- `git diff --check`

当前 Linux 环境安装了 GDB，因此 `daemon_action_flow`、`core_dump_mode`、`edge_case_flow` 和
`capability_matrix_flow` 都实际执行了 Linux + GDB smoke，而不是 skip。

## Code Review Findings

1. 现象：task env value 带空格时，使用 MI quote 会让 inferior 收到带引号的实际值，例如
   `env="fixture env spaced"`。
   影响范围：Agent 依赖 task `env` 复现实验环境时，带空格或需要 rest-of-line 语义的 value 会不准确。
   处理结果：已修复，并用 capability matrix smoke 覆盖。

2. 现象：`run` / `continue` 如果被 GDB control command 层拒绝，原代码仍可能继续输出
   `ok:true` 的 stop response。
   影响范围：Agent 可能误判运行/继续执行成功，需要打开 raw MI 才能发现 GDB 拒绝。
   处理结果：已修复为结构化失败，并保留 raw command evidence。

## 限制和注意事项

- 本轮没有新增 Agent-facing action，未扩展 catchpoint event，未实现 numeric hypothesis assertion。
- 本轮没有修改 `.clang-format`，因为仓库中未找到该文件。后续如果需要统一风格，可以先新增项目级
  `.clang-format`，再做单独的格式化提交，避免把功能修复和大规模格式噪声混在一起。
- 非法 JSON 在 CLI client 本地解析阶段失败时仍没有 session context，因此不会写入 session evidence；
  这是上一轮 handoff 已记录的已知限制。
