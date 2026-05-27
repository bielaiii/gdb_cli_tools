# Segfault Example 定位报告

本报告对应 `examples/segfault_task.md` 和 `examples/segfault.cpp`，用于展示 Agent 如何把
工具观察和最终判断分开记录。

## Task

- Problem: example program crashes with SIGSEGV.
- Executable: `./build/segfault`
- Working directory: `..`
- Args: empty
- Core dump: empty, so this is Run Mode.

## Source Evidence

### E-src-1 main creates a null pointer

`main` 中创建了空指针：

```cpp
Session *session = nullptr;
return handle_request(session);
```

### E-src-2 handle_request forwards the pointer

`handle_request` 不检查参数，直接把 `session` 传给 `read_session_value`：

```cpp
static int handle_request(Session *session) {
    return read_session_value(session);
}
```

### E-src-3 read_session_value dereferences the pointer

`read_session_value` 直接读取 `session->value`：

```cpp
static int read_session_value(Session *session) {
    return session->value;
}
```

## Tool Observations

在安装 GDB 的环境中，可以用下面的命令生成包含 raw MI evidence、summary、snapshot 和
session log 的完整报告：

```bash
cmake -S . -B build
cmake --build build
./build/gdb-agent serve examples/segfault_task.md --out examples/segfault_report.md --assets examples/segfault_report.assets
```

本示例定位报告只引用源码层面的最小证据，不把工具观察包装成自动根因判断。

## Agent Inference

崩溃路径是：

1. `main` 创建 `Session *session = nullptr`。
2. `main` 调用 `handle_request(session)`。
3. `handle_request` 调用 `read_session_value(session)`。
4. `read_session_value` 执行 `session->value`，对空指针解引用。

## Final Agent Conclusion

该示例的崩溃原因是空 `Session *` 被一路传入 `read_session_value`，随后未经检查就解引用。
修复方向可以是构造有效的 `Session`，或在 `handle_request` / `read_session_value` 边界加入
明确的空指针处理策略。
