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

Action validation failures that reach a live session write `ToolError`
evidence. For GDB-backed actions such as `backtrace`, `locals`, `args_info`,
`registers`, `threads`, `frame_select`, `evaluate`, `hypothesis_check`, `run`,
and `continue`, a GDB `result_class=error` keeps the raw
command/control-command as evidence and writes separate `ToolError` evidence.
The action response's `command_evidence` points to the raw command evidence,
while `evidence` points to the error evidence. Static evidence command timeouts
are also recorded as structured failures; `run`/`continue` run deadlines are
inferior timeout/interruption semantics, not GDB command failures.

`raw_records` audits the structure of raw MI without replacing the raw file.
Current record kinds include `result`, `async`, `stream`, `prompt`, and
`unknown`; stream types include `console`, `target`, and `log`.
`included_records`, `related_records`, and `concurrent_records` continue to
describe evidence attribution. Raw hash, raw byte count, and kept summary byte
count remain the integrity audit fields.

MI summaries extract low-noise signals from result/async payloads, such as
`msg`, `value`, `reason`, `thread-id`, `stopped-threads`, `frame`, `bkpt`, and
`wpt`. These fields help Agents scan evidence quickly, but they do not replace
raw MI; precise conclusions should still inspect the raw file and session MI
log. Backtrace/thread summaries preserve frame number, function, source
location, shared-library source, and thread boundaries, falling back to the
original low-noise line when a field cannot be parsed safely.

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

For core dump tasks, `session_summary.json` also records the `core_dump` path
and `core_loaded`. Core mode is a static debugging target. Loading the core
writes a `Core load` `SessionEvent` evidence entry, and static evidence actions
such as `backtrace`, `threads`, `frame_select`, `locals`, `args_info`, and
`evaluate` produce normal evidence. `run`, `continue`, and probe mutation
actions are rejected in core mode by the state guard and recorded as
`ToolError` evidence.

In core mode, the final report also includes a `Core Dump Snapshot` section. It
aggregates the core path, executable, working directory, `core_loaded` state,
key static evidence ids, and core-mode guard `ToolError` evidence from the
task, session outcome, and evidence index. This is a report entry point only;
it does not change the raw evidence layout and does not make
`session_snapshot.json` or `session_summary.json` restorable.

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

The final report's `Replay Plans` section lists structured plan file metadata:
name, tags, source session, failure policy, and task fingerprint. The `Replay
Execution Audit` section summarizes replay runs, steps, and warnings from
structured `ReplayRun`, `ReplayStep`, and `ReplayWarning` evidence; the report
does not parse replay results back out of action response text.

Core-mode mixed replay plans use the same evidence chain: static action steps
may succeed; dynamic action steps fail through the core guard and are audited
through `ToolError` plus `ReplayStep.error_evidence`; `continue_on_error`
continues to later steps, while `stop_on_error` records later steps as
`skipped` with a skip reason.

## Probe Store Snapshot

The authoritative runtime probe state is the in-memory `ProbeState`.
`assets/probes.json` is generated from `ProbeState` only during `finish`/report
writing. It is a final report snapshot, not a runtime synchronization database
and not a live GDB session restore file.

Use `probe_list` to observe current active probe metadata while the session is
live; deleted probes are omitted by default after `probe_delete`. Cross-session
reproduction should use replayed high-level actions rather than reading an old
`probes.json` to restore breakpoints, watchpoints, or catchpoints.
`assets/probes.json` may retain deleted history, but deleted entries are marked
with `deleted:true` so historical probes are not confused with live probes.

Probe hit evidence (`BreakpointHit`, `WatchpointHit`, `CatchpointHit`) stores
the relevant metadata snapshot for that hit, such as number, kind,
location/expression/event, catchpoint selector, condition, comment, purpose,
hit count, and `on_hit_policy`. This keeps the stop context explainable even if
the session exits unexpectedly.
If a watchpoint stop record lacks a probe number, the tool attributes the hit
and runs on-hit actions only when exactly one active watchpoint can be
identified. If the stop cannot be uniquely attributed, the tool records
degraded watchpoint evidence instead of inventing a probe number.

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

## Hypothesis Artifacts

The machine-readable entry point for the hypothesis workflow is:

```text
<assets>/hypotheses/index.json
```

Each hypothesis also has a human-readable record:

```text
<assets>/hypotheses/<hypothesis-id>.md
```

`index.json` uses the `gdb-agent-hypotheses-v1` schema. Each hypothesis entry
contains:

- `id`
- `title`
- `description`
- `tool_status`
- `agent_conclusion`
- `agent_inference`
- `checks`

Each check entry contains:

- `id` and `check_id`
- `description`
- `expression`
- `assertion`
- `expected`
- `observed`
- `status`: `passed`, `failed`, or `unknown`
- `evidence`
- `error_evidence`

`observed` comes from the newly created evidence summary for that
`hypothesis_check`. It is a compact, lossy view; `evidence` points to the raw
GDB evidence entry, which remains authoritative. `status` is the tool's
assertion result over `observed`, not a root-cause conclusion.

Numeric assertions (`greater_than`, `less_than`, `greater_equal`, `less_equal`,
`equals_number`, `not_equals_number`, and `between`) parse integers from
`observed` and `expected`. `between` expects `LOW..HIGH` and uses inclusive
bounds. Decimal integers, negative integers, and `0x` hexadecimal integers are
supported; floating-point values are not supported. Address assertions
(`address_non_null` and `address_equals`) parse one `0x...` hexadecimal
address. Unknown assertions, missing required `expected` input, unparseable
integers/addresses, invalid `between` bounds, LOW greater than HIGH, or multiple
different integers/addresses produce `status:"unknown"` and reference
`ToolError` evidence through `error_evidence`. `unknown` means the tool could not
evaluate that check; it does not support or refute the hypothesis.

The final report aggregates hypotheses, checks, evidence ids, agent inference,
and final agent conclusion from `index.json`. The Hypotheses section includes
observed summaries and lists `unknown` checks or checks with `error_evidence` as
needing attention, with the corresponding error summary when available. The
report also summarizes `ToolError` evidence separately and shows
`command_evidence` when a raw GDB command evidence link exists. If the index is
missing or cannot be parsed, the report falls back to listing the per-hypothesis
Markdown files.

Reports should cite evidence ids rather than relying on summaries alone.
Reports now include each evidence item's raw hash so an Agent can verify that
the cited raw file still matches the report.

The summary layer applies limited noise reduction, including C++ `std::string`
/ `std::string_view` normalization, common default
allocator/comparator/hash/equality/default-deleter compression, `std::array` /
`std::function` / `std::chrono::*` spacing and ratio normalization,
`std::pair<const K, V>` key-const compression, relative path shortening, and
stable backtrace/thread summaries. Custom deleter, allocator, comparator, hash,
and equality types are preserved because they may be debugging clues. None of
these transformations modify raw MI.
