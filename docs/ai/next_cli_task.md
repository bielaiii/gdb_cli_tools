# Next CLI Task

## 目标

本轮集中做 **type sanitizer hardening**：让 summary 对常见 C++ 类型更低噪声，同时避免把可能影响
根因判断的自定义类型策略错误压掉。

重点是两条：

1. `custom deleter` / `custom allocator` / `custom comparator` / `custom hash` / `custom equality`
   必须在 summary 中保留，不要被当成默认 STL 噪声删除。
2. 增加 `std::string_view`、`std::array`、`std::function`、`std::chrono::*` 的 sanitizer 支持和测试。

本轮仍然遵守 raw evidence 优先原则：sanitizer 只影响 summary/view，不改变 raw MI、session log
或 raw evidence 文件。

## 背景

当前 sanitizer 已有轻量 `std::...<...>` template scanner，覆盖常见：

- `std::string` 归一化。
- vector/list/deque/set 的默认 allocator/comparator 噪声压缩。
- map/unordered_map 的默认 comparator/hash/equality/allocator 噪声压缩。
- `std::unique_ptr<T, std::default_delete<T>>` deleter 噪声压缩。
- `std::shared_ptr` / `std::weak_ptr` / `std::optional` / `std::variant` / `std::tuple` / `std::pair`
  的基础 spacing 和 key const 降噪。
- 工作目录路径相对化。

当前短板：

- 默认 allocator/comparator/hash/deleter 可以压缩，但自定义策略可能就是 bug 线索，不能丢。
- `std::string_view`、`std::array`、`std::function`、`std::chrono::*` 还没有明确 sanitizer 规则。
- 测试主要是手写高噪声字符串，还缺一个专门的类型 fixture 来产生真实 GDB 输出。

## “从真实输出里挑高噪声类型”是什么意思

执行本轮时，不要只靠想象写 regex。应该新增或扩展一个 C++ fixture，里面放一些典型变量，然后用
真实 GDB action（例如 `locals`、`evaluate`、必要时 `raw_mi` 的 `ptype`/`whatis`）抓取实际输出。

所谓“从真实输出里挑高噪声类型”，就是：

1. 先让 GDB 输出真实类型字符串和变量展示。
2. 看哪些输出特别长、重复、影响 Agent 阅读，比如默认 allocator、`std::less<T>`、`std::hash<T>`、
   `std::default_delete<T>`、chrono ratio 展开等。
3. 只对这些确认过的噪声添加 sanitizer 规则。
4. 对自定义 allocator/comparator/hash/deleter 这类可能代表业务语义或 bug 线索的部分，明确保留。
5. 把真实输出中抽取出的代表性字符串加入不依赖 GDB 的单元测试；把完整 live flow 放进 Linux + GDB
   smoke。

换句话说：真实输出用于发现“最吵、最值得压缩”的形态；单元测试用于把这些形态固定成回归。

## 范围

### 1. 明确保留自定义策略类型

修改 `sanitize_output` / template scanner 逻辑，使它只删除明确识别为 STL 默认策略的噪声。

必须保留：

- `std::unique_ptr<Foo, CustomDeleter>` 中的 `CustomDeleter`。
- `std::vector<Foo, CustomAllocator<Foo>>` 中的 custom allocator。
- `std::set<Foo, CustomCompare>` 中的 custom comparator。
- `std::map<Key, Value, CustomCompare>` 中的 custom comparator。
- `std::unordered_map<Key, Value, CustomHash, CustomEqual>` 中的 custom hash/equality。
- allocator/comparator/hash/equality/deleter 是项目命名空间类型时，例如：
  - `project::ArenaAllocator<T>`
  - `my::TransparentLess`
  - `MyHash`
  - `MyEqual`
  - `FdCloser`

允许压缩：

- `std::allocator<T>`
- `std::less<T>`，仅当它是标准默认 comparator。
- `std::hash<T>` 和 `std::equal_to<T>`，仅当它们是标准默认 unordered 策略。
- `std::default_delete<T>`，仅当它是 `unique_ptr` 默认 deleter。

要求：

- 对 map/unordered_map/set/vector/list/deque/unique_ptr 的压缩逻辑增加“默认策略识别”判断。
- 不要因为模板参数数量超过预期就盲目只取前一两个参数。
- 对不认识的策略类型，默认保留，而不是删除。
- 输出保持可读 spacing，例如 `std::map<Key, Value, CustomCompare>`。

### 2. 增加 string_view / array / function / chrono 支持

新增 sanitizer 规则和测试覆盖：

- `std::basic_string_view<char, std::char_traits<char>>` -> `std::string_view`
- `std::string_view` spacing 归一化。
- `std::array<T, N>` spacing 归一化，保留 `N`。
- `std::function<R(Args...)>` spacing 归一化，不破坏函数签名。
- `std::chrono::duration<Rep, Period>`：
  - 保留核心语义，优先输出为 `std::chrono::duration<Rep, Period>` 的低噪声形式。
  - 可识别常见 `std::ratio<1l, 1000l>` / `std::ratio<1, 1000>` 等 spacing 噪声。
  - 不要把 duration 自动改写成 `milliseconds` 等别名，除非实现能稳定确认且测试覆盖充分。
- `std::chrono::time_point<Clock, Duration>`：
  - 保留 `Clock` 和简化后的 `Duration`。
  - 不要丢掉 custom clock 或 custom duration。

可以按实际 GDB 输出补充：

- `std::chrono::steady_clock::time_point`
- `std::chrono::system_clock::time_point`
- `std::ratio<N, D>` spacing 归一化

要求：

- 不引入大型 demangler 依赖。
- 不承诺完整 C++ 类型系统解析。
- 如果 scanner 遇到函数类型里的逗号、括号或复杂签名，不能崩溃；无法可靠简化时保留原始可读形式。

### 3. 新增或扩展类型 fixture

新增一个专门 fixture，推荐：

- `examples/type_sanitizer_fixture.cpp`

fixture 里应包含函数或局部变量，覆盖：

- 默认 allocator/comparator/hash/deleter 的 STL 类型。
- 自定义 allocator/comparator/hash/equality/deleter 的 STL 类型。
- `std::string_view`
- `std::array`
- `std::function`
- `std::chrono::duration`
- `std::chrono::time_point`
- nested 组合，例如：
  - `std::array<std::string_view, 2>`
  - `std::function<int(std::string_view)>`
  - `std::map<std::string_view, std::chrono::steady_clock::time_point>`
  - `std::unique_ptr<Foo, CustomDeleter>`

要求：

- CMake 中新增 build target。
- fixture 稳定、短小、Linux/macOS 都能编译；live GDB smoke 仍按 Linux 目标平台运行。
- 不需要生成业务行为复杂的程序，只要能让 GDB 在停点处看到这些变量。

### 4. 测试

扩展不依赖 GDB 的单元测试。可以继续放在 `tests/mi_summary_tests.cpp`，也可以新增：

- `tests/type_sanitizer_tests.cpp`

必须覆盖：

- 默认策略被压缩。
- 自定义策略被保留。
- `string_view` / `array` / `function` / `chrono` 的成功路径。
- nested 类型组合。
- spacing 稳定。
- unknown/custom 类型 fallback 保留。
- raw-ish 真实 GDB 输出中抽取的代表性字符串。

建议新增 Linux + GDB smoke：

- `scripts/smoke_type_sanitizer.sh`

smoke 目标：

- 构建并运行 type sanitizer fixture。
- 在停点处执行 `locals`、`evaluate` 或必要的 `raw_mi` `ptype`/`whatis`。
- 确认 summary/view 中默认策略被降噪。
- 确认 summary/view 中 custom deleter/allocator/comparator/hash/equality 仍可见。
- finish 后检查 evidence index、summary files 和 report 引用一致。

注意：

- 单元测试应覆盖主要 sanitizer 语义；smoke 负责确认真实 GDB 输出没有偏离预期。
- 如果不同 GDB 版本对 `ptype` 输出差异较大，smoke 断言应聚焦关键 substring，不要写脆弱的整行匹配。

### 5. 文档同步

根据实际变更同步更新：

- `docs/evidence_model.md`
- `docs/evidence_model.en.md`
- `docs/known_limitations.md`
- `docs/mvp_acceptance.md`，如果新增 smoke/CTest 入口
- `README.md`，仅当测试入口或能力列表需要同步
- `docs/ai/progress.md`
- `docs/ai/handoff.md`

文档口径：

- Sanitizer 是有损 summary 视图，不替代 raw evidence。
- 默认 STL 策略噪声可以压缩；自定义 deleter/allocator/comparator/hash/equality 必须保留。
- 新增 `string_view`、`array`、`function`、`chrono` 是低噪声展示增强，不是完整 demangler。

预计本轮不需要更新：

- `docs/ai/decision.md`

只有当执行中改变 raw-first 或 summary-lossy 的项目级决策时才更新 decision。

## 可写范围

允许修改：

- `src/common/string_utils.cpp`
- `src/common/string_utils.hpp`，仅当需要暴露小型 helper 给测试
- `tests/mi_summary_tests.cpp`
- 新增 `tests/type_sanitizer_tests.cpp`，如拆分更清楚
- `examples/type_sanitizer_fixture.cpp`
- `CMakeLists.txt`
- 新增 `scripts/smoke_type_sanitizer.sh`
- `docs/evidence_model.md`
- `docs/evidence_model.en.md`
- `docs/known_limitations.md`
- `docs/mvp_acceptance.md`，如果新增回归入口
- `README.md`，仅当测试入口或能力列表需要同步
- `docs/ai/progress.md`
- `docs/ai/handoff.md`
- `docs/ai/decision.md`，仅当新增项目级 decision

不要修改：

- `src/cli.cpp`，除非新增 smoke 必须暴露已有 action 的稳定调用方式；预计不需要。
- daemon/replay/probe/hypothesis 行为。
- evidence raw 文件布局。
- `docs/ai/current_goal.md`。
- `docs/ai/next_cli_task.md`，除非执行中发现任务口径必须修正。
- 与本轮无关的格式化、重命名或全仓重构。

## 验证

至少运行：

```bash
cmake --build build
./build/mi_summary_tests
./build/gdb-agent check examples/segfault_task.md
ctest --test-dir build --output-on-failure
git diff --check
```

如果新增测试 target 或 smoke：

```bash
./build/type_sanitizer_tests
scripts/smoke_type_sanitizer.sh
```

如果当前环境缺少 GDB 或 live smoke 因 GDB 输出差异需要降级，必须在 `docs/ai/handoff.md` 和最终回复中说明：

- 已运行哪些测试。
- 哪些 live 输出校准没有运行或只做了 best-effort。
- 原因是环境限制、GDB 版本差异，还是实现遗留。

## 完成标准

本轮完成时应满足：

- 默认 STL 噪声继续被压缩，不回退现有 `std::string`、vector/list/deque/set/map/unordered_map、
  smart pointer、optional/variant/tuple/pair 行为。
- custom deleter/allocator/comparator/hash/equality 在 summary 中保留。
- `std::string_view`、`std::array`、`std::function`、`std::chrono::duration`、
  `std::chrono::time_point` 有明确 sanitizer 规则和测试。
- 新增或扩展 fixture 能产生代表性真实 GDB 输出。
- 单元测试覆盖默认策略压缩、自定义策略保留、新类型支持和 nested 组合。
- 如果新增 smoke，CTest 或文档中有清楚入口。
- 文档同步说明 sanitizer 的能力边界和 raw evidence 优先原则。
- `docs/ai/progress.md` 记录本轮实际完成内容。
- `docs/ai/handoff.md` 覆写为本轮交接，包含验证结果和遗留限制。
- 本轮相关文件被单独 staged、commit，并 push 到当前分支 upstream；如果 commit/push 失败，按
  AGENTS.md 要求记录原因。

## 禁止事项

- 不引入完整 demangler 或大型第三方依赖。
- 不把 summary 当作 raw evidence 的替代品。
- 不删除自定义策略类型来追求短 summary。
- 不改变 action schema。
- 不改变 evidence raw 文件布局。
- 不做 `src/cli.cpp` 大重构。
- 不做全仓格式化。
