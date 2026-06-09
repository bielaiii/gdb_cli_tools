# Handoff

日期：2026-06-09

## 本轮完成

- 按 `docs/ai/next_cli_task.md` 执行 type sanitizer hardening 任务，聚焦 C++ 类型 summary 降噪、
  自定义策略保留、真实 GDB 输出 fixture/smoke 和文档同步。
- 更新 `src/common/string_utils.cpp`：
  - template arg splitter 增加函数类型括号深度识别，避免把 `std::function<int(A, B)>`
    中的逗号误判为 template 分隔符。
  - 默认 `std::allocator<T>`、`std::less<T>`、`std::hash<T>`、`std::equal_to<T>` 和
    `std::default_delete<T>` 只在对应 STL 容器 / `unique_ptr` 上下文中压缩。
  - 自定义 deleter、allocator、comparator、hash 和 equality 类型默认保留。
  - 增加 `std::string_view`、`std::array`、`std::function`、`std::ratio`、
    `std::chrono::duration` 和 `std::chrono::time_point` 的低噪声展示支持。
  - 移除会全局删除 allocator/default_delete 的旧 regex 路径。
- 新增 `tests/type_sanitizer_tests.cpp`：
  - 覆盖默认策略压缩、自定义策略保留、新类型支持、nested 组合和真实 GDB 输出抽取出的代表性字符串。
- 新增 `examples/type_sanitizer_fixture.cpp`：
  - fixture 暴露默认 STL 策略、自定义策略、`string_view`、`array`、`function`、`duration` 和
    `time_point` 字段，用于真实 `ptype` 输出校准。
- 新增 `scripts/smoke_type_sanitizer.sh` 并接入 CTest `type_sanitizer_flow`：
  - 通过 daemon live session 和 `raw_mi` / `ptype` 抓取真实 GDB 类型输出。
  - 验证默认策略在 summary 中被降噪，自定义策略仍可见。
  - 验证 finish 后 report、session summary、evidence index 和 raw/view/summary 文件引用一致。
- CMake 新增 target：
  - `type_sanitizer_fixture`
  - `type_sanitizer_tests`
- 同步更新：
  - `README.md`
  - `docs/evidence_model.md`
  - `docs/evidence_model.en.md`
  - `docs/known_limitations.md`
  - `docs/mvp_acceptance.md`
  - `docs/ai/progress.md`

## 验证

- `cmake --build build`
- `./build/type_sanitizer_tests`
- `./build/mi_summary_tests`
- `scripts/smoke_type_sanitizer.sh`
- `./build/gdb-agent check examples/segfault_task.md`
- `git diff --check`
- `ctest --test-dir build --output-on-failure`
  - 当前 Linux 环境有 GDB，完整 CTest 已运行。
  - 结果：12/12 tests passed。

## 限制和注意事项

- Sanitizer 仍不是完整 C++ demangler，只做已测试的低噪声 summary 归一化。
- `std::chrono::duration` 不会被自动改写成 `milliseconds` 等别名；当前保留
  `std::chrono::duration<Rep, Period>` 的核心语义。
- 自定义策略类型默认保留，即使这让 summary 更长；这是为了避免删除可能影响根因判断的调试线索。
- 本轮没有改变 raw evidence 保存原则、session MI log、evidence raw 文件布局或 action schema。
- 本轮未新增项目级 decision，因此未修改 `docs/ai/decision.md`。
