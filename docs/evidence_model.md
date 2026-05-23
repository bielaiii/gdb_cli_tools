# Evidence Model MVP

Every GDB action that captures data creates an evidence entry.

MVP evidence files are written under:

```text
<assets>/evidence/
```

Open the top-level Markdown file first. It is the human-readable view:

```text
<assets>/evidence/E0002.backtrace.md
```

Raw MI is intentionally stored one level deeper:

```text
<assets>/evidence/raw/E0002.backtrace.mi.txt
```

Summaries are also stored separately for compact Agent consumption:

```text
<assets>/evidence/summary/E0002.backtrace.summary.txt
```

Each evidence entry has:

- id, such as `E0001`
- kind, such as `GdbCommand`, `StopEvent`, or `ToolError`
- title
- command or action name
- human-readable Markdown view
- raw file
- summary file

The full MI session stream is stored as:

```text
<assets>/logs/session.mi.raw.log
```

Reports should cite evidence ids rather than relying on summaries alone.
