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
- raw SHA-256
- capture timestamp
- raw byte count and kept summary byte count
- `truncated` and `lossy_summary` flags
- record attribution fields: `included_records`, `related_records`, and
  `concurrent_records`

The machine-readable evidence index is:

```text
<assets>/evidence/index.json
```

It contains the same metadata as the per-evidence Markdown view. Raw files are
kept complete; summary files are capped by a byte limit recorded in the index.
If a summary is capped, `truncated` is set to `true`. Summary text is treated as
lossy whenever it is sanitized, decoded from MI streams, summarized, or
truncated.

The full MI session stream is stored as:

```text
<assets>/logs/session.mi.raw.log
```

The MVP also writes machine-readable session files:

```text
<assets>/task.normalized.json
<assets>/session_summary.json
<assets>/session_snapshot.json
```

Reports should cite evidence ids rather than relying on summaries alone.
Reports now include each evidence item's raw hash so an Agent can verify that
the cited raw file still matches the report.
