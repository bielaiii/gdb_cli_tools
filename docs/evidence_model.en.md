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

`session_summary.json` records `replay_step_count`, `replay_warning_count`,
`probe_hit_count`, `on_hit_action_count`, and `on_hit_error_count` so an Agent
can quickly tell whether replay ran, whether force replay or legacy-plan
compatibility warnings were recorded, and whether probe/on-hit evidence or
errors were produced.

## Replay Evidence

Replay Store saves and replays only high-level actions. Structured plans use
`gdb-agent-replay-plan-v1` and include `schema_version`, plan name, tags,
source session id, created_at, task metadata, task fingerprint, plan-level
failure policy, and action list. `session_snapshot.json` is not replay input;
cross-session reproduction must use a replay plan or JSONL high-level actions.

Each replay writes one `ReplayRun` evidence entry with an overall replay result
snapshot, including plan name, schema version, force status, task metadata
match status, warning, error, and the step result list.

Every replay step writes `ReplayStep` evidence. Its summary includes:

- plan name
- step id and index
- action name
- action JSON
- status: `success`, `failed`, or `skipped`
- failure policy: `continue_on_error` or `stop_on_error`
- action evidence id
- error evidence id
- skip reason

If a replayed action returns `ok:false` or execution throws, the tool also
writes `ToolError` evidence and references it from the step's `error_evidence`.
If the plan task fingerprint does not match the current task, replay is
rejected by default and recorded as `ToolError` evidence. Force replay writes
`ReplayWarning` evidence and keeps the warning, `force:true`, and
`task_metadata_match:false` visible in the replay result.

Older JSONL replay files and older plans without a failure policy default to
`continue_on_error`. Older plans without task metadata or a fingerprint remain
readable but produce a warning. Unknown schema names or schema versions are
rejected with stable errors.

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
location/expression/event, condition, comment, purpose, hit count, and
`on_hit_policy`. This keeps the stop context explainable even if the session
exits unexpectedly.

When a probe has an on-hit policy, each automatic action also writes
`OnHitAction` evidence. That evidence records the action name, status
(`success`, `failed`, or `skipped`), `failure_policy`, newly produced
`action_evidence_ids`, `error_evidence`, `skip_reason`, and a policy-limited
response summary. The underlying action evidence still keeps its own raw and
summary files independently.

Hit evidence aggregates:

- `on_hit_policy`
- `on_hit_results`
- `on_hit_evidence_ids`
- `on_hit_error_ids`

When `stop_on_error` triggers, later on-hit actions are not executed, but they
are still recorded as `skipped` `OnHitAction` evidence. `continue_after_hit:true`
appends an automatic continue and records it as a `continue_after_hit` result;
the report records this behavior without turning it into a root-cause judgment.

Reports should cite evidence ids rather than relying on summaries alone.
Reports now include each evidence item's raw hash so an Agent can verify that
the cited raw file still matches the report.

The summary layer applies limited noise reduction, including C++ `std::string`
normalization, common allocator compression, relative path shortening, and
stable backtrace/thread summaries. None of these transformations modify raw MI.
