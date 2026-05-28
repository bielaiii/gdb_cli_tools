# Next CLI Task

## 目标

强化 summary 和 MI 解析能力，降低 Agent 读 GDB 输出时的噪声，同时让 raw MI 审计字段更细。

本轮聚焦四件事：

1. 更完整的 MI value parser。
2. C++ 类型 sanitizer。
3. thread/backtrace summarizer。
4. raw MI audit metadata 增强。

## 背景语义

- raw MI 始终是权威证据，必须完整保留。
- summary 是有损、低噪声视图，任何 sanitizer、归一化、截断或摘要都必须通过
  `lossy_summary` / `truncated` 等字段表达。
- 工具只负责结构化观察和证据整理，不生成根因结论。

## 范围

### 1. MI value parser

补强 MI 输出解析能力，优先支持 GDB/MI 常见 value 结构：

- const string，包括转义字符、空字符串、路径和模板类型文本。
- tuple：`{name="value",frame={...}}`
- list：`[item={...},item={...}]` 和 `["a","b"]`
- result record 中的 key/value。
- stream record 中的 console/log/target 输出。

要求：

- 不要用脆弱的简单 split 解析嵌套 tuple/list。
- 解析失败时保留 raw，并产生稳定 fallback summary。
- 为 parser 增加不依赖 GDB 的 fixture/unit smoke 测试。

### 2. C++ 类型 sanitizer

新增或强化 summary 层的类型降噪，至少覆盖：

- `std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >`
  -> `std::string`
- 常见 `std::basic_string<...>` -> `std::string`
- 常见 allocator 噪声压缩，例如 `std::allocator<T>` 在容器类型中不淹没主类型。
- 常见模板空格和 `> >` 归一化。

要求：

- 只作用于 summary/view，不改 raw。
- 不要过度改写用户类型。
- 为 sanitizer 增加 fixture 测试。

### 3. Thread / Backtrace summarizer

增强 backtrace 和 threads 的 summary 质量：

- backtrace summary 应突出：
  - frame number
  - function
  - file:line
  - signal/top frame
  - 简化后的参数和 C++ 类型
- thread summary 应突出：
  - thread id / GDB thread number
  - 当前线程标记
  - stop reason 或 top frame（可从现有 MI/console 输出中提取时）
  - 每个线程一行或低噪声结构化块

要求：

- 长路径应尽量相对 working directory 或归一化。
- 输出应稳定，便于 Agent 引用。
- summary 不能替代 raw evidence。

### 4. Raw MI 审计字段增强

细化 evidence/session 中与 raw MI 相关的审计 metadata，例如：

- command/action name。
- record sequence id。
- MI record kind：result / async / stream / prompt / unknown。
- result class 或 async class。
- stream type：console / target / log。
- token（如果存在）。
- included / related / concurrent records 的含义保持稳定。
- raw hash、raw byte count、kept summary byte count 继续保留。

要求：

- 更新 `docs/evidence_model.md` 和英文版。
- 如果 schema 字段变化，更新 session snapshot / evidence index 相关文档。
- 保持旧 evidence 基本可读，不为本轮引入无关 schema 重构。

## 不做

- 不新增调试 action。
- 不改变 raw MI 保存路径。
- 不做自动根因分析。
- 不扩展 replay/probe/hypothesis 行为。
- 不为了 macOS live debugging 做兼容；目标运行平台仍是 Linux。

## 完成标准

- MI parser、type sanitizer、backtrace/thread summarizer 有不依赖 GDB 的测试或 fixture 覆盖。
- README demo smoke 和现有 CTest 仍通过。
- 生成的 summary 明显比 raw GDB 输出更短、更稳定，但 raw 文件完整保留。
- evidence index 或 session metadata 中能看到更细的 MI audit 字段。
- 文档、progress 和 handoff 更新完整。
