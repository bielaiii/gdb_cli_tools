# Task Format

The task file is Markdown. Each field is a level-three heading followed by its
value. Empty `args` and `core dump` are allowed.

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

MVP fields:

- `problem`: required, free-form text.
- `executable`: required.
- `working directory`: required.
- `args`: optional shell-like argument string.
- `stdin`: optional input file for the inferior. Defaults to `/dev/null`.
- `env`: optional environment overrides, one `KEY=value` per line.
- `run timeout`: optional run deadline in milliseconds. Defaults to `30000`.
- `core dump`: optional. When present, the MVP loads the core instead of
  running the program.

Core Dump Mode is a static evidence mode: the tool loads the executable and
core dump, remains in stopped/core state, and allows static actions such as
`backtrace`, `threads`, `frame_select`, `locals`, `args_info`, `evaluate`, and
hypothesis checks. `run`, `continue`, and probe mutation actions are rejected
and recorded as `ToolError` evidence.

`args` is parsed as shell-like argv, so quoted values and backslash escapes are
preserved:

```markdown
### args

--name "hello world" --path /tmp/a\ b
```

This becomes:

```json
["--name","hello world","--path","/tmp/a b"]
```
