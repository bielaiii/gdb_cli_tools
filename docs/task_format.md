# Task Format（中文版）

Task file 使用 Markdown。每个字段是一个三级标题，字段值放在标题后面。`args` 和
`core dump` 允许为空。

英文版文档保留在：

```text
docs/task_format.en.md
```

## 示例

```markdown
### problem

Program crashes with SIGSEGV.

### executable

/path/to/program

### working directory

/path/to/workdir

### args

--flag "value with spaces"

### stdin

/path/to/input.txt

### env

ASAN_OPTIONS=abort_on_error=1
LOG_LEVEL=debug

### run timeout

30000

### core dump

```

## MVP 字段

- `problem`：必填，自由文本，描述要排查的问题。
- `executable`：必填，被调试程序路径。
- `working directory`：必填，被调试程序工作目录。
- `args`：可选，shell-like 参数字符串。
- `stdin`：可选，inferior 的输入文件。默认是 `/dev/null`。
- `env`：可选，环境变量覆盖项，每行一个 `KEY=value`。
- `run timeout`：可选，运行 deadline，单位毫秒。默认是 `30000`。
- `core dump`：可选。存在时，MVP 加载 core，而不是运行程序。

## 路径解析

Task file 中的相对路径按以下方式解析：

- `working directory` 相对 task file 所在目录解析。
- `executable`、`stdin` 和 `core dump` 相对 `working directory` 解析。
- 解析后会被规范化成可用于校验和报告的路径。

## Args 解析

`args` 按 shell-like argv 解析，保留引号值和反斜杠转义：

```markdown
### args

--name "hello world" --path /tmp/a\ b
```

解析结果是：

```json
["--name","hello world","--path","/tmp/a b"]
```

不要简单按空格切分 `args`；这会破坏带空格的参数值。

## I/O 约束

MVP 不支持 PTY，也不支持交互式 stdin。inferior stdin 默认是 `/dev/null`，或者来自
`stdin` 字段指定的输入文件。inferior stdout/stderr 会重定向到 assets 目录，并在 stop
event 后作为 `InferiorOutput` evidence 捕获。
