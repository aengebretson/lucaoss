# Native CLI contract — proposed v1

Implemented bounded slice: [CLI and stdio MCP foundations](cli-mcp-foundations.md).
It exposes existing ingestion/status/errors, policy inspection, canonical
transactions and economic LIFO lots through the optional SQLite application facade.
The broader storage-independent API, report jobs and unfinished accounting
operations described below remain proposals; use `foundations-v1.tools.json`
for the executable MCP catalog.

Status: design only. `luca-ingest` already implements native ingestion and worker
commands; the unified `luca` command, reporting operations, output modes and exit
contract below are not implemented. See [current commands](../apps/ingestion-host/README.md#cli)
and the [shared application API](application-api.md).

No LLM, MCP process, HTTP server or graphical UI is required to use the CLI.
Use one configured store directory and book binding for CLI and local MCP; do not
copy only the SQLite database. Commands call the same C++ application operations
as an embedded enterprise consumer. A CLI implementation may not invoke an MCP
server as its financial engine or require a Python runtime.

## Common arguments and output

`luca --profile FILE --book BOOK [--output text|json] COMMAND ...`

The profile establishes trusted local tenant/source/book access, storage root,
context registry and limits. It contains no user-supplied authority assertion.
Machine automation selects `--output json`: stdout is exactly one versioned
application response object, even on operation failure. Human diagnostics/progress
go to stderr; do not mix progress with JSON. `text` is readable, not a stable
parsing contract. Financial amounts remain decimal strings in JSON.

`--request-id ID` propagates an operational correlation ID; it is not an
idempotency key. `--timeout DURATION` bounds a caller wait, never deletes durable
work. Durations use a positive integer plus `ms`, `s` or `m` (for example `250ms`
or `30s`); explicit waiting requires a finite timeout. Import, revalidation and report requests require `--key KEY`; payload
staging can create multiple expiring handles and has no financial effect.
Paths are opened only by local CLI adapters;
submitted source bytes are copied to immutable staging and referenced by handle.
Secrets are not emitted in errors. Export never overwrites a file by default.

| Proposed command | Shared operation / behavior |
| --- | --- |
| `capabilities` | Read limits, supported kinds, formats, contexts, worker/adapter capabilities and retention. |
| `payload stage --file FILE` | `stage_payload`; return durable unattached payload handle, size/hash and expiry. |
| `import submit --file FILE --metadata FILE --key KEY` | Stage then `submit_import`; return after durable receipt only. |
| `import submit --payload-id ID --metadata FILE --key KEY` | Submit an existing scoped handle; identical retry recovers the original import. |
| `import status IMPORT` | Read retained status and enumeration/validation/commitment counters. |
| `import records IMPORT [--validation STATE] [--commitment STATE] [--cursor CURSOR] [--limit N]` | `read_records`; frozen snapshot pages. |
| `import errors IMPORT [--record ID] [--cursor CURSOR] [--limit N]` | `read_diagnostics`; errors and blocked reasons remain data. |
| `import revalidate IMPORT --request FILE --key KEY` | Explicit context and group revisions; prior validation history retained. |
| `report request --request FILE --key KEY` | Durable report job receipt; input readiness and selection are explicit. |
| `report status REPORT` | Job state, pinned selection, coverage, manifest or retained failure. |
| `report rows REPORT [--cursor CURSOR] [--limit N]` | Immutable result pages. |
| `report evidence REPORT --target-id ID [--cursor CURSOR] [--limit N]` | Row/evidence target lineage under the same report snapshot. |
| `report export REPORT --format jsonl|csv --file FILE` | Page a single immutable available result; output is data only, completion summary on stdout. |
| `worker run --mode once|drain|watch` | Explicit runtime ownership; `drain` does not imply blocked/future work finished. |

`report export` is a local CLI composition over `report rows`; it has no separate
financial calculation. Write a temporary destination and rename atomically only
after all pages succeed. CSV has versioned per-kind columns and no implicit
currency conversion. On expiry/failure, do not publish a partial file as complete.
The summary identifies result/revision/context and row count; retain a manifest
sidecar so an exported file can be traced back to its snapshot.

Administrative reference/context registration, resumable upload, original payload
retrieval, validation history and recovery remain available through `luca-ingest`.
They are not removed or claimed to have unified names yet. No implicit schema
migration or worker startup occurs merely to print `--help` or `--version`.

## Waiting and shell outcomes

Submission/report requests return receipts by default. Optional
`--wait admitted|report --wait-timeout DURATION` first obtains the durable receipt,
then polls shared status. `admitted` applies to an import, `report` to a report.
The final JSON still returns that operation's status schema; a retained receipt
identifies accepted work even when waiting expires. No wait silently runs a
worker or changes source data. A receipt's success never implies all records
validated or all financial work finished.

| Exit code | Meaning |
| --- | --- |
| 0 | Requested operation completed; for receipt-only commands this means durable receipt, not financial validity. |
| 2 | Invalid command/options/request schema. |
| 3 | Identity or authorization failure. |
| 4 | Requested resource/cursor/result unavailable, stale, expired or conflicting key/revision. |
| 5 | Retryable resource/storage/backpressure failure. |
| 6 | Explicit wait reached a retained invalid/conflicting import or failed report. |
| 7 | Explicit wait/deadline expired; durable work may remain pending. |
| 8 | Other nonretryable operational/internal failure. |
| 9 | Outcome unknown after mutation; retry the identical command/key. |
| 130 | Interrupted caller; durable work remains queryable/recoverable by key. |

Reading an invalid record or failed report is a successful inspection (exit 0).
Only an explicitly requested wait applies financial completion exit codes.
In a completed import wait, any invalid or conflicting group yields 6; pending
or blocked work can wait until timeout. Receipt-only scripting must inspect
status if it requires financial completion.

## Example flow (proposed syntax)

```sh
luca --profile local.json --book fund-a --output json capabilities
luca --profile local.json --book fund-a --output json import submit \
  --file trades.jsonl --metadata submission.json --key trades-20261021-001
luca --profile local.json --book fund-a --output json worker run --mode drain
luca --profile local.json --book fund-a --output json import status IMPORT_ID
luca --profile local.json --book fund-a --output json import errors IMPORT_ID
luca --profile local.json --book fund-a --output json report request \
  --request report.json --key cash-report-20261021-001
luca --profile local.json --book fund-a --output json worker run --mode drain
luca --profile local.json --book fund-a --output json report rows REPORT_ID
```

`submission.json` contains metadata only; CLI supplies the key and staged payload.
`report.json` contains report selection/context/readiness/filters only, without
book or key. The command adapter assembles the complete shared request. MCP uses
the same fields as defined in [its catalog](../contracts/mcp/v1.tools.json).

First acceptance compares CLI JSON financial results, coverage and evidence with
the independently installed C++ consumer. These examples do not claim installed
executables, completed report jobs or an MCP deployment today.
