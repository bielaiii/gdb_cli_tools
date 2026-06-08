# Next CLI Task

## 目标

本轮集中完善 **MI parser / summary / sanitizer**，并补充丰富、不依赖 GDB 的测试用例。

这是 Phase 5「深度摘要和高级 MI」的一轮重点任务。目标不是引入新的 Agent-facing action，
而是让已有 evidence summary 更稳定、更低噪声、更适合 AI Agent 使用，同时保证 raw evidence
和 session MI log 仍然是审计来源。

本轮可以比普通小任务更完整，但仍要保持边界清楚：聚焦 `src/gdb/mi_utils.*`、
`src/common/string_utils.*`、`src/evidence/evidence_store.*` 中与 MI 解析、summary 和 sanitizer
直接相关的逻辑，以及对应测试和文档。不要顺手重构 CLI、daemon、replay、probe 或 hypothesis。

## 背景

当前已有能力：

- `parse_mi_value` 支持递归解析 string、tuple、list 和 bare value。
- `audit_mi_records` 能分类 result、async、stream 和 prompt，并记录 token/class/stream type。
- `summarize_mi_records` 能输出基础 `result:done` / `async:stopped` 摘要。
- `sanitize_output` 已覆盖：
  - `std::string` spelling 归一化
  - allocator/default_delete 噪声压缩
  - `std::map` / `std::unordered_map` 常见模板压缩
  - `std::pair<const K, V>` / `std::pair<K const, V>` key const 压缩
  - working directory path 相对化
- `EvidenceStore` 已对 backtrace/thread 做简单 human summary，并保留 raw/summary/view/index。
- `tests/mi_summary_tests.cpp` 已有基础覆盖。

当前短板：

- MI parser 对边界格式覆盖还不够丰富。
- MI record summary 对真实 GDB result/async payload 的字段选择不够 Agent-friendly。
- sanitizer 主要靠若干 regex，复杂 STL/nested template 仍容易残留噪声。
- backtrace/thread summarizer 对真实 GDB 输出变体覆盖有限。
- 测试用例数量偏少，没有形成覆盖矩阵。

## 范围

### 1. 强化 MI value parser

完善 `src/gdb/mi_utils.cpp` / `.hpp` 中的 MI value parser。

至少覆盖并测试：

- tuple：
  - 空 tuple：`{}`
  - 多字段 tuple
  - nested tuple
  - 字段名包含 `-` 和 `_`
- list：
  - 空 list：`[]`
  - value list：`["a","b"]`
  - result list：`[frame={...},frame={...}]`
  - 混合 list：`["x",{name="y"},item={value="z"}]`
  - nested list
- string：
  - escaped quote：`\"`
  - escaped backslash：`\\`
  - newline/tab/carriage return：`\n`、`\t`、`\r`
  - empty string
  - strings containing braces/brackets/commas that must not be parsed structurally
- bare value：
  - `true` / `false`
  - `0x0`
  - `<optimized out>`
  - `<unavailable>`
  - bare text with spaces until delimiter
- invalid input：
  - unterminated string
  - missing `=`
  - missing closing brace/list
  - trailing garbage
  - empty input

要求：

- invalid input 必须稳定返回 `std::nullopt`，不能 throw。
- parser 不需要成为完整 GDB/MI 规范实现，但要比当前更稳健。
- 不改变 raw evidence 保存逻辑。

### 2. 改进 MI record audit 和 summary

增强 `audit_mi_records` 和 `summarize_mi_records`，让 summary 对 Agent 更有用。

至少覆盖并测试：

- record kind：
  - result：`^done`、`^running`、`^error`
  - exec async：`*stopped`、`*running`
  - notify async：`=thread-created`、`=thread-exited`、`=breakpoint-modified`
  - stream：`~`、`@`、`&`
  - prompt：`(gdb)`
  - unknown line
- token：
  - numeric token 被记录
  - 无 token 时为空字符串
  - malformed prefix 不误判为 token
- result/error payload summary：
  - `msg`
  - `value`
  - `frame`
  - `bkpt`
  - `wpt`
  - `reason`
  - `thread-id`
  - `stopped-threads`
- async stopped summary：
  - breakpoint hit
  - watchpoint trigger
  - signal received
  - exited normally
  - exited with code
- stream summary：
  - console/target/log stream 的 payload 应能被 decoded_streams 保留
  - `summarize_mi_records` 可以继续只摘要 result/async，但 tests 要确认 stream 不被误分类

建议输出口径：

- 保持低噪声单行 summary。
- 不要把巨大 payload 完整塞入 summary；需要有字段数量、深度或长度限制。
- 对 `^error,msg="..."` 明确展示 error message。
- 对 `*stopped,reason="..."` 明确展示 reason、frame func/file/line、thread id 等关键字段。

### 3. 强化 C++ sanitizer

增强 `sanitize_output`，优先做对 AI token 成本最有帮助且风险低的归一化。

至少覆盖并测试：

- string：
  - `std::__cxx11::basic_string<...>` -> `std::string`
  - `std::basic_string<...>` -> `std::string`
  - spacing 变体
- vector/list/deque/set：
  - 去掉常见 allocator 噪声
  - 保留核心类型参数
- map/unordered_map：
  - 去掉 comparator/hash/equality/allocator 噪声
  - 覆盖 key 为 `std::string`、value 为用户类型或智能指针的常见形式
- smart pointer：
  - `std::unique_ptr<T, std::default_delete<T>>` -> `std::unique_ptr<T>`
  - `std::shared_ptr<T>` / `std::weak_ptr<T>` spacing 归一化
- optional/variant/tuple/pair：
  - spacing 归一化
  - pair const-key 噪声继续稳定压缩
- paths：
  - working directory 下路径相对化
  - `build/../src/file.cpp` 这类路径尽量 normalize 到稳定形式
  - 不要错误改写不在 working directory 下的系统路径
- whitespace/template closers：
  - 归一化 `< T >`、`,T`、`> >`
  - 避免生成难读的 `std::map<int,std::string>`；优先 `std::map<int, std::string>`

要求：

- sanitizer 是低噪声视图，不是完整 demangler；不要承诺完整 C++ 类型语义解析。
- raw evidence 必须保持不变。
- 如果 regex 方案开始变脆，可以增加小型 template string scanner/helper，但不要引入大型依赖。

### 4. 改进 backtrace/thread summarizer

增强 `src/evidence/evidence_store.cpp` 中 backtrace/thread summary 的稳定性。

至少覆盖并测试：

- backtrace：
  - 普通 frame：`#0 func(args) at file:line`
  - frame 没有 file/line
  - frame 使用 `from /lib/...so`
  - `??`
  - `inlined` 或 GDB 输出中包含额外注释时不崩溃
  - `thread apply all bt` 中包含 `Thread N ...` header 时，summary 保留线程边界
  - frame truncation 行为稳定
- threads：
  - current thread `*`
  - normal thread
  - thread name
  - LWP id
  - stopped/running frame
  - header line 不进入 summary
  - truncation 行为稳定

要求：

- summary 应尽量输出 frame number、function、file:line 或 shared library 来源。
- 如果无法解析，回退保留原 line 的低噪声版本，而不是丢失重要信息。
- 不要让 summary 误导 Agent：无法提取的字段可以省略，不要猜。

### 5. 丰富测试用例

重点扩展 `tests/mi_summary_tests.cpp`。如果单文件过大，可以拆分为多个测试文件并更新
`CMakeLists.txt`，例如：

- `tests/mi_parser_tests.cpp`
- `tests/mi_record_summary_tests.cpp`
- `tests/sanitizer_tests.cpp`
- `tests/evidence_summary_tests.cpp`

但如果拆分会造成 CMake 噪声过多，可以继续保留单个测试文件，内部按 section 函数组织。

测试要求：

- 不依赖系统 GDB。
- 不依赖当前机器路径，使用临时目录或固定 `/tmp/project` 字符串。
- 覆盖成功路径和失败路径。
- 每个新增 parser/sanitizer/summarizer 行为都要有断言。
- 测试失败信息要足够定位问题。
- 保留现有测试语义，不删除已有覆盖。

建议测试结构：

```cpp
static void test_mi_value_parser();
static void test_mi_record_audit();
static void test_mi_record_summary();
static void test_cpp_sanitizer();
static void test_backtrace_summary();
static void test_thread_summary();
static void test_evidence_store_integration();
```

### 6. 文档同步

根据实际变更同步更新：

- `docs/evidence_model.md`
- `docs/evidence_model.en.md`
- `docs/known_limitations.md`
- `docs/ai/progress.md`
- `docs/ai/handoff.md`

文档口径：

- Summary 是更强的有损低噪声视图，不能替代 raw evidence。
- Sanitizer 仍不是完整 C++ demangler。
- MI parser 覆盖增强，但 raw MI 是最终审计来源。
- 如果新增测试 target，在 README 或 MVP acceptance 中确认回归入口仍然准确。

仅当产生新的项目级决策时才更新：

- `docs/ai/decision.md`

预计本轮不需要新增 decision，因为这属于既有 D004 raw-first summary 设计下的增强。

## 可写范围

允许修改：

- `src/gdb/mi_utils.cpp`
- `src/gdb/mi_utils.hpp`
- `src/common/string_utils.cpp`
- `src/common/string_utils.hpp`
- `src/evidence/evidence_store.cpp`
- `src/evidence/evidence_store.hpp`，仅当 summary helper 接口确有必要调整
- `tests/mi_summary_tests.cpp`
- 其他新增 `tests/*_tests.cpp`，如确实需要拆分
- `CMakeLists.txt`，仅当新增测试 target 时
- `docs/evidence_model.md`
- `docs/evidence_model.en.md`
- `docs/known_limitations.md`
- `docs/mvp_acceptance.md`，仅当测试入口变化
- `README.md`，仅当测试入口变化
- `docs/ai/progress.md`
- `docs/ai/handoff.md`
- `docs/ai/decision.md`，仅当产生项目级决策

不要修改：

- `src/cli.cpp`，除非发现必须适配已有 summary API 的小改动
- daemon/session/replay/probe/hypothesis 行为
- action schema
- evidence raw 文件布局
- report 大结构
- `docs/ai/current_goal.md`
- `docs/ai/next_cli_task.md`，除非执行中发现任务口径必须修正

## 验证

至少运行：

```bash
cmake --build build
./build/mi_summary_tests
./build/gdb-agent check examples/segfault_task.md
git diff --check
```

如果新增了测试 target，也要运行对应测试二进制，例如：

```bash
./build/mi_parser_tests
./build/mi_record_summary_tests
./build/sanitizer_tests
./build/evidence_summary_tests
```

如果当前环境支持，也运行：

```bash
ctest --test-dir build --output-on-failure
```

本轮主体不依赖 GDB；如果 Linux + GDB smoke 未运行，要在 `docs/ai/handoff.md` 和最终回复中说明。

提交前确认：

```bash
git status --short
```

## 不做

- 不新增 Agent-facing action。
- 不扩展 catchpoint event。
- 不扩展 hypothesis assertion。
- 不改 replay/probe/on-hit 语义。
- 不改变 raw evidence 保存原则。
- 不把 summary 当作 raw 的替代品。
- 不引入完整 C++ demangler 或大型第三方依赖。
- 不做 `src/cli.cpp` 大重构。
- 不做全仓格式化。

## 提交要求

这是一轮完整 Execution task。完成后需要：

1. 更新 `docs/ai/progress.md` 和 `docs/ai/handoff.md`。
2. 只 stage 本轮相关文件。
3. 创建一次 git commit。
4. 推送当前分支到 upstream；如果没有 upstream，推送到当前分支的同名远程分支。
5. 如果 commit 或 push 因网络、认证、权限或远程配置失败，在 `docs/ai/handoff.md` 和最终回复中记录原因。
