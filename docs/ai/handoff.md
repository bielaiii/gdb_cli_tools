# Handoff

日期：2026-06-09

## 本轮完成

- 根据性能 code review 结果修复字符串处理中的低效点，重点减少可避免的 `std::string`
  临时构造和 regex 重复编译。
- 更新 `src/common/json.hpp` / `src/common/json.cpp`：
  - `Json::object_value` 改为 `std::map<std::string, Json, std::less<>>`，支持异构 key lookup。
  - `Json::find`、`string_or`、`int_or`、`bool_or` 改为接收 `std::string_view`。
  - `parse_json` 和内部 `Parser` 改为接收 / 保存 `std::string_view`，避免解析入口要求
    owning `std::string`。
  - number 解析从 `std::stod(substr)` 改为 `std::from_chars`，避免为数字 token 构造临时
    string。
- 更新 `src/common/string_utils.cpp`：
  - template arg splitter 改为返回 trimmed `std::string_view`，拆分阶段不再为每个参数立即分配
    string。
  - `render_template` / `join_template_args` 改用 `std::string_view` 参数和预估 reserve 的
    string append，移除 `std::ostringstream` 和 initializer_list 到 vector 的临时复制。
  - 默认 allocator/comparator/hash/equality/deleter 判断改为分段 `string_view` 比较，不再构造
    `"std::allocator<" + value_type + ">"` 这类临时 string。
  - 删除 map / unordered_map / pair 的旧 regex 降噪路径，统一依赖 template scanner。
  - 剩余 `sanitize_output` regex 改为函数内 `static const std::regex`，避免每次调用重新编译。
- 更新测试：
  - `tests/type_sanitizer_tests.cpp` 增加默认策略压缩的精确输出断言，防止“短类型片段存在但默认噪声仍残留”的回归。
  - `tests/task_parser_tests.cpp` 增加非 null-terminated `std::string_view` JSON 输入、key lookup 和 fallback 测试。
  - `scripts/smoke_type_sanitizer.sh` 增加 summary 负向断言，确认默认 `std::less`、`std::hash`、
    `std::equal_to` 和 pair allocator 噪声不会残留。

## 验证

- `cmake --build build`
- `./build/type_sanitizer_tests`
- `./build/task_parser_tests`
- `./build/replay_plan_tests`
- `scripts/smoke_type_sanitizer.sh`
- `./build/gdb-agent check examples/segfault_task.md`
- `git diff --check`
- `ctest --test-dir build --output-on-failure`
  - 结果：12/12 tests passed。

## 限制和注意事项

- 本轮是用户在 code review 后要求直接修复的问题，不是由“开始新一轮任务流”触发的完整任务流。
  因此本轮未创建 git commit，也未 push。
- 本轮未改变 action schema、evidence schema、raw evidence 保存原则或项目级目标。
- 本轮未新增项目级 decision，因此未修改 `docs/ai/decision.md`。
