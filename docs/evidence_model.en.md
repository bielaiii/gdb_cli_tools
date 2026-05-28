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
- raw MI audit field: `raw_records`, with each record carrying sequence, token,
  record kind, result/async class, and stream type when applicable

The machine-readable evidence index is:

```text
<assets>/evidence/index.json
```

It contains the same metadata as the per-evidence Markdown view. Raw files are
kept complete; summary files are capped by a byte limit recorded in the index.
If a summary is capped, `truncated` is set to `true`. Summary text is treated as
lossy whenever it is sanitized, decoded from MI streams, summarized, or
truncated.

`raw_records` audits the structure of raw MI without replacing the raw file.
Current record kinds include `result`, `async`, `stream`, `prompt`, and
`unknown`; stream types include `console`, `target`, and `log`.
`included_records`, `related_records`, and `concurrent_records` continue to
describe evidence attribution. Raw hash, raw byte count, and kept summary byte
count remain the integrity audit fields.

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

`session_snapshot.json` and `session_summary.json` are historical records and
report inputs. They do not represent a live GDB session and cannot restore an
old GDB process. Restart reproduction should replay high-level actions.

## Probe Store Snapshot

The authoritative runtime probe state is the in-memory `ProbeState`.
`assets/probes.json` is generated from `ProbeState` only during `finish`/report
writing. It is a final report snapshot, not a runtime synchronization database
and not a live GDB session restore file.

Use `probe_list` to observe current probe metadata while the session is live.
Cross-session reproduction should use replayed high-level actions rather than
reading an old `probes.json` to restore breakpoints, watchpoints, or
catchpoints.

Probe hit evidence (`BreakpointHit`, `WatchpointHit`, `CatchpointHit`) stores
the relevant metadata snapshot for that hit, such as number, kind,
location/expression/event, condition, comment, purpose, and hit count. This
keeps the stop context explainable even if the session exits unexpectedly.

Reports should cite evidence ids rather than relying on summaries alone.
Reports now include each evidence item's raw hash so an Agent can verify that
the cited raw file still matches the report.

The summary layer applies limited noise reduction, including C++ `std::string`
normalization, common allocator compression, relative path shortening, and
stable backtrace/thread summaries. None of these transformations modify raw MI.
