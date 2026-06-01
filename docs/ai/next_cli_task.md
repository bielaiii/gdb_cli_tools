# Next CLI Task

## 目标

强化 Hypothesis Workflow 的结构化检查结果和报告聚合，让 Agent 能清楚追踪：

1. 当前有哪些 hypothesis。
2. 每个 hypothesis 执行了哪些 check。
3. 每个 check 的 expression、assertion、expected、observed、status 和 evidence id 是什么。
4. 工具观察、assertion result、Agent inference 和 final conclusion 如何分离。
5. finish report、hypotheses index 和单个 hypothesis Markdown 能稳定呈现这条证据链。

本轮目标不是让工具自动判断根因，而是把“提出假设 -> 执行检查 -> 记录证据 -> Agent
更新判断”这条链路变得更可审计、更结构化。

## 背景语义

- 工具只记录观察、检查结果和 evidence id；最终根因判断仍必须来自 Agent。
- Hypothesis workflow 已有 `hypothesis_create`、`hypothesis_check`、`hypothesis_conclude`、
  Markdown 记录和 `assets/hypotheses/index.json`。
- 当前 hypothesis check 的 assertion 类型较少，result 结构较薄，report 中 hypothesis 区域
  也只是列文件，缺少聚合视图。
- raw evidence 仍是权威数据；hypothesis check 的 observed/summary 是低噪声视图，不能替代 raw。

## 范围

### 1. 结构化 hypothesis check result

强化 `hypothesis_check` 的输出和持久化记录。建议 result 至少包含：

- `hypothesis`
- `check_id`
- `description`
- `expression`
- `assertion`
- `expected`
- `observed`
- `status`
  - `passed`
  - `failed`
  - `unknown`
- `evidence`
- `error_evidence`（如有）

要求：

- `observed` 应来自本次新产生的 evidence summary 或稳定提取结果，不能复用历史值。
- assertion 不可识别或无法判断时，应稳定返回 `unknown` 或 `ok:false`，并记录 `ToolError`
  evidence；选择哪种语义要在文档中说清楚。
- 不要把 check result 包装成根因判断。

### 2. Assertion 能力小步扩展

在不扩大范围的前提下，扩展 assertion。建议保留已有：

- `none`
- `contains`
- `not_contains`
- `is_null`
- `non_null`

并新增一小组稳定、容易测试的 assertion，例如：

- `equals`
- `not_equals`

或者新增 numeric 比较：

- `gt`
- `gte`
- `lt`
- `lte`

二选一即可，不要一次性做太多。

要求：

- assertion 逻辑应能独立测试，不依赖 GDB。
- 文档要说明 assertion 是对 observed/summary 的工具级检查，不等于根因结论。
- 对空 expected、不可解析 observed、未知 assertion 要有稳定行为。

### 3. Hypothesis evidence 和 index 强化

视现有模型选择最小改动：

- 可以新增 `HypothesisCheck` evidence kind；或
- 强化现有 hypothesis Markdown/index，让每个 check 记录完整结构。

无论选择哪种方式，都要确保 Agent 能从 artifacts 中回答：

- hypothesis 的标题和描述是什么。
- 每个 check 执行了什么表达式。
- check 的 assertion 和 expected 是什么。
- 工具观察到的 observed 是什么。
- check 是 passed、failed 还是 unknown。
- 对应 evidence id 是哪个。
- Agent 的 inference/conclusion 是否存在，且与工具观察分开。

`assets/hypotheses/index.json` 应成为机器可读入口，不只是一份松散列表。

### 4. Report 聚合 Hypotheses

扩展最终 Markdown report 的 Hypotheses 区域，稳定展示：

- hypothesis id、title、tool_status。
- 每个 check 的 description、expression、assertion、expected、observed、status 和 evidence id。
- Agent inference。
- final agent conclusion。
- hypothesis index 和单个 Markdown 文件路径。

要求：

- 报告要避免暗示工具自动宣称根因。
- 如果没有 hypothesis，报告保持当前简洁行为。
- 如果 hypothesis 文件或 index 缺失，报告应稳定降级，而不是崩溃。

### 5. 测试和 smoke

至少增加不依赖 GDB 的 assertion/unit 测试，覆盖：

- 已有 assertion。
- 本轮新增 assertion。
- unknown/invalid assertion 或不可判断输入。

如改动 action/result/report 行为，应扩展 Linux + GDB daemon smoke，覆盖：

- `hypothesis_create`
- `hypothesis_check`
- 至少一个 passed check。
- 至少一个 failed 或 unknown check。
- `hypothesis_conclude`
- `finish`
- report 中 Hypotheses 聚合内容。
- `assets/hypotheses/index.json` 的结构化字段。

测试要求：

- Linux + GDB 环境实际执行 live smoke。
- 缺少 GDB 或非 Linux 时按现有项目口径 skip live 部分。
- 不依赖 GDB 的 assertion 测试应在普通 CTest 中稳定运行。

### 6. 文档同步

同步更新：

- `docs/agent_actions.md`
- `docs/agent_actions.en.md`
- `docs/evidence_model.md`
- `docs/evidence_model.en.md`

如引入新的项目级决策或调整 hypothesis 语义，更新：

- `docs/ai/decision.md`

任务结束时更新：

- `docs/ai/progress.md`
- `docs/ai/handoff.md`

## 不做

- 不让工具自动宣称根因。
- 不新增 replay plan schema。
- 不扩展新的 catchpoint event。
- 不继续大改 MI parser，除非 hypothesis observed 提取确实需要一个很小的 helper。
- 不引入 PTY 或交互式 stdin。
- 不为了 macOS live debugging 做兼容；目标运行平台仍是 Linux。
- 不做大型 report 重构；只补 Hypotheses 区域需要的最小聚合。

## 完成标准

- `hypothesis_check` 输出和 `assets/hypotheses/index.json` 有结构化 check result。
- assertion 行为有明确文档和不依赖 GDB 的测试覆盖。
- report 的 Hypotheses 区域能聚合 hypothesis、checks、evidence、Agent inference 和 final
  conclusion。
- 工具观察、assertion result 和 Agent 结论保持清晰分离。
- Linux + GDB daemon smoke 覆盖 create/check/conclude/finish/report 的 hypothesis flow，
  或明确说明未覆盖原因。
- `docs/agent_actions.md`、`docs/evidence_model.md` 及英文版与实现一致。
- `docs/ai/progress.md` 和 `docs/ai/handoff.md` 记录实际完成、验证结果和限制。
