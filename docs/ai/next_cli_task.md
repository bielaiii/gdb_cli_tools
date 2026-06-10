# Next CLI Task: MI summary hardening with real Linux GDB fixtures

## 目标

下一轮任务聚焦强化 **MI summary / backtrace / threads / raw MI audit** 在真实
Linux + GDB 输出下的稳定性。

本轮不是新增 action，也不是改变外部 schema；目标是用真实 GDB session 产生的
evidence 作为 fixture，补强 summary 解析、降噪和回归覆盖。

## 当前背景

当前已有：

- `tests/mi_summary_tests.cpp`：手写 raw MI / console stream 样例。
- `scripts/smoke_type_sanitizer.sh`：真实 GDB 下验证 C++ type sanitizer。
- `scripts/smoke_capability_matrix.sh`：覆盖 probe、replay、hypothesis、thread crash 等能力。

仍缺少一个专门面向真实 GDB 输出的 summary smoke，用来稳定约束：

- `backtrace` summary 的线程边界、frame 提取、source path 相对化和 truncation。
- `threads` summary 的当前线程、普通线程、LWP/thread id 和 stopped frame。
- `raw_mi` / `GdbCommand` evidence 的 raw audit metadata。
- 真实 GDB 输出下 sanitizer 不回退到长模板噪声。

## 实现范围

允许修改：

- `examples/mi_summary_fixture.cpp`，新增专用 fixture。
- `scripts/smoke_mi_summary_live.sh`，新增 live smoke。
- `CMakeLists.txt`，新增 fixture target 和 CTest。
- `src/gdb/mi_utils.cpp` / `.hpp`，仅当真实输出暴露 MI parser 或 summary edge case。
- `src/evidence/evidence_store.cpp` / `.hpp`，仅当真实输出暴露 evidence summary edge case。
- `tests/mi_summary_tests.cpp`，为修复过的 edge case 增加最小单元测试。
- `docs/evidence_model.md` / `.en.md` 和 `docs/known_limitations.md`，仅当 summary 行为或限制说明变化。
- `docs/ai/progress.md`
- `docs/ai/handoff.md`
- `docs/ai/decision.md`，仅当新增或改变项目级决策。

原则上不要修改：

- `docs/agent_actions.md` / `.en.md`，除非发现 action 输出语义文档和实际行为不一致。
- `docs/task_format.md` / `.en.md`。
- replay、probe、hypothesis、report 的用户可见行为。

禁止：

- 新增用户可见 action。
- 修改 CLI 命令语法。
- 修改 action JSON schema。
- 修改 response JSON 字段语义。
- 修改 replay plan schema。
- 修改 evidence raw 文件布局。
- 把 summary 当作 raw evidence 的替代品。
- 引入第三方 demangler、protobuf、IDL/codegen 或新 JSON 库。
- 做 unrelated refactor；`src/cli.cpp` 拆分、EvidenceStore index 写入策略优化、
  hypothesis assertion 扩展都不属于本轮任务。

## 具体要求

### 1. 新增真实 GDB summary fixture

新增 `examples/mi_summary_fixture.cpp`。

fixture 至少包含：

- 稳定的 noinline 调用链，便于 `backtrace` summary 断言函数名。
- 至少一个 worker thread，便于 `threads` summary 断言当前线程与普通线程。
- 一个稳定停点，便于 action 在用户函数 frame 上执行。
- 至少一个 STL/template 局部或参数，便于验证 summary 不回退成长模板噪声。
- 工作目录内的源码路径，便于验证 path sanitizer 相对化。

建议流程：

1. 程序启动后先 `raise(SIGTRAP)`，让 daemon `create` 后停住。
2. smoke 设置 breakpoint 到 fixture 的稳定 noinline 函数。
3. `continue` 到 breakpoint。
4. 在该停点执行 `backtrace`、`threads`、`locals`、`frame_select` 和 `raw_mi`。

### 2. 新增 live smoke

新增 `scripts/smoke_mi_summary_live.sh`。

脚本行为：

- 非 Linux、缺少 `gdb` 或缺少 `python3` 时按现有 smoke 风格 skip。
- 启动 daemon，创建 session，执行 fixture flow。
- 从 action response 中读取 evidence id，再从 `assets/evidence/index.json` 找到
  `summary_file`、`view_file` 和 `raw_file`。
- 校验所有 evidence artifact 文件存在。
- 校验 report 中引用的 evidence id 都存在于 index。

必须断言的 summary 信号：

- `backtrace` summary 包含 fixture 调用链中的稳定函数名。
- `backtrace` summary 包含相对路径，例如 `examples/mi_summary_fixture.cpp`，不能依赖工作目录绝对路径。
- `threads` summary 包含当前线程标记和至少一个普通线程。
- `threads` summary 保留 LWP/thread id 或等价 GDB thread identity 信号。
- `raw_mi` result summary 包含 `result:done` 和关键 payload，例如 `value=` 或 `threads=`。
- 对应 evidence Markdown view 包含 `Raw MI Audit` 表，并至少出现 `result`、`stream`、
  `async`、`prompt` 中真实输出具备的 record kind。

噪声回归断言：

- summary 不应包含仓库工作目录的绝对路径。
- summary 不应包含明显未降噪的 `std::__cxx11::basic_string<char, std::char_traits<char>`。
- summary 不应包含 `> >` 模板 closing spacing 噪声。

### 3. 按真实输出修 summary 实现

如果 smoke 暴露真实输出 edge case，允许小范围修复：

- MI record classification。
- MI payload field extraction。
- backtrace frame line simplification。
- thread list line simplification。
- working-directory path normalization。
- 常见 STL/type sanitizer 降噪。

每个修复必须在 `tests/mi_summary_tests.cpp` 或 `tests/type_sanitizer_tests.cpp` 中补最小单元测试。

## 验证

至少运行：

```bash
cmake --build build
./build/gdb-agent check examples/segfault_task.md
./build/mi_summary_tests
./build/type_sanitizer_tests
./build/task_parser_tests
ctest --test-dir build --output-on-failure
git diff --check
```

重点确认：

- 新增 `mi_summary_live_flow` 在 Linux + GDB 环境实际运行，不应只 skip。
- 现有 `capability_matrix_flow`、`core_dump_mode`、`type_sanitizer_flow` 不回归。
- raw evidence、raw SHA-256、evidence id、evidence index、report 引用和
  `session_summary.json` count 保持兼容。
- 用户可见 action JSON、response JSON、replay plan、report schema 和 raw evidence 文件布局不变。

## 完成标准

本轮完成时应满足：

- 新增 `mi_summary_fixture` target。
- 新增 `scripts/smoke_mi_summary_live.sh`。
- 新增 CTest `mi_summary_live_flow`。
- live smoke 能真实覆盖 backtrace、threads、locals/frame_select、raw_mi 和 raw audit metadata。
- 若修改 summary/sanitizer 行为，相关单元测试和文档已同步。
- `docs/ai/progress.md` 和 `docs/ai/handoff.md` 已记录本轮实际完成内容和验证结果。
- 本轮相关改动已按仓库约定 commit 并 push。
