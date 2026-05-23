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

### core dump

```

MVP fields:

- `problem`: required, free-form text.
- `executable`: required.
- `working directory`: required.
- `args`: optional shell-like argument string.
- `core dump`: optional. When present, the MVP loads the core instead of
  running the program.

