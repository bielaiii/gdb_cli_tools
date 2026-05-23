# 项目目的

目标是提供一个面向 AI Agent 的 GDB 交互与证据管理工具。
AI 负责分析、提出假设和判断结论；本工具负责稳定执行 GDB 操作、
保存原始证据、生成结构化摘要，并辅助 AI 输出可信验证报告。

这是一个可以持续性对话的session，直到ai agent确认问题定位完毕。
ai使用该应用程序，需要提供一个md文件，格式如下图


problem （例如当前是并发问题，core dump, segfault）
executable
working directory
args
core dump

每一个key用3号标题，value放在下一行，之间全都用下一行做区分
args和core dump允许为空



## 技术
+ 使用c++20的协程，耗时的操作都要异步完成
+ cmake编译

### 特性
+ 放弃pty格式的支持
+ GDB/MI 支持
+ 断点/观察点管理层 (断点带注释，标记验真的猜想，或者当做备忘录)
+ 可以基于优化任务流，节省token相关的目的，重新设计流程和数据结构
+ Core dump 分析模式
+ 允许ai可以重复启动应用程序
    + 允许保存特定的操作，重启程序之后可以一次性执行特定的命令
+ 抽象ai的输入，避免ai直接输入mi命令。同时在应用程序中提供ai agent可以使用命令。文档也要提供一份。
+ 断点命中后的自动策略
    + 例如，支持ai条件断点后自动执行某些命令
+ GDB 输出降噪与结构化摘要
    > GDB 输出很啰嗦，而且有时一大堆模板类型会淹没重点。
    > 可以加一个 sanitizer/summarizer：
    > 原始输出保留
    > 给 Codex 的输出做摘要
    > 例如 C++ 模板 backtrace：
    > std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >
    > 可以简化成：
    > std::string
    > 再比如路径：
    > /home/xiang/project/build/debug/../../src/server.cpp
    > 规范化成：
    > src/server.cpp
    > 这个对 LLM 很重要，因为 token 成本和噪声都会影响判断。

+ 自动生成调试报告
+ “假设验证”工作流

    > 你说这个工具的目标是“和 Codex 一起定位 bug，并且验证问题”。那可以专门做一个 hypothesis workflow：
    > hypothesis create "fd_state 在 close 后仍然被 epoll 事件引用"
    > hypothesis add-check "p fd_state_table[fd]"
    > hypothesis add-check "bt"
    > hypothesis verify
    > 输出：
    > Hypothesis: fd_state use-after-close
    > Evidence:
    >   fd = 42
    >   fd_state_table[42].generation = 3
    >   event.generation = 2
    > Conclusion:
    >   likely stale epoll event
    > 这个方向很有意思，因为它不是简单地让 AI 猜 bug，而是强迫它走：
    > 提出假设 -> 设计 GDB 检查 -> 执行 -> 收集证据 -> 更新结论
    > 这会比“直接问 Codex 怎么修”可靠很多。

+ 提供清晰的数据结构说明，供ai agent在使用前了解如何使用，例如接收mi命令，返回的结构化数据的具体定义
    