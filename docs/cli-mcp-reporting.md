# Bounded CLI and stdio MCP accounting reports

The dependent [immutable-version adapter increment](cli-mcp-versions.md) adds
schema-6 replay compatibility, edit/delete and hash-verified source evidence.
Earlier limits in this guide describe its original implemented slice.

This increment is implemented in the optional `luca::application_sqlite` C++
facade. Enterprise can call the same `SqliteApplication::call` operations without
starting either adapter. Authoritative settlement, LIFO, recognition, journal,
trial-balance and reconciliation rules remain in their typed C++ libraries.
No database migration, report worker, network listener or product Python runtime
is introduced. The broader `application/v1` report-job design remains a proposal.

The executable schemas are
[`foundations-v1.tools.json`](../contracts/mcp/foundations-v1.tools.json).
The document envelope remains `luca.foundations/1`; these are additive operations.
The adapter request and response names below are the C++ document boundary, not
new financial-domain types. Unknown request fields and unsupported filters fail.

| C++ operation | CLI command | MCP tool |
| --- | --- | --- |
| `accounting_report` | `luca report` | `luca_accounting_report` |
| `query_transactions` | `luca transactions query` | `luca_query_transactions` |
| `submit_settlement` | `luca settlement submit` | `luca_submit_settlement` |
| `submit_refund` | `luca settlement refund` | `luca_submit_refund` |
| `settlement_status` | `luca settlement status` | `luca_settlement_status` |

Every CLI command takes `--profile FILE --request JSONFILE`. CLI `--file` remains
exclusive to import submission. MCP uses the existing newline-delimited stdio
initialization and `tools/list`/`tools/call` exchange. A model cannot select a
store path, actor, profile or permission. Schemas/help/version preserve their
existing behavior.

## Reports and reproducibility

`accounting_report` requires `book_id`, `accepted_through`, `policy_through`,
`settlement_through`, `mapping_digest`, `effective_at`, `recorded_through`, and
`collection`. Sequence values are unsigned decimal strings. Obtain available
highwaters and the configured mapping digest from `capabilities.ledger_state`
and `capabilities.mapping_digest`. A highwater is a selector, not a credential.

Example request:

```json
{
  "book_id": "synthetic-book",
  "accepted_through": "2",
  "policy_through": "1",
  "settlement_through": "1",
  "mapping_digest": "<64 lowercase hex characters from capabilities>",
  "effective_at": "2026-02-01T00:00:00Z",
  "recorded_through": "2026-10-23T23:59:59Z",
  "collection": "obligations",
  "limit": 100,
  "format": "json"
}
```

The prefixes are immutable and explicitly selected. `accepted_through` must end
at an atomic admission commit boundary; a partial commit fails
`INPUT_REVISION_SPLITS_COMMIT`. A selected settlement whose admission references
a later ledger prefix fails `INPUT_REVISION_INCOMPATIBLE`. Future/unavailable
highwaters fail explicitly. Concurrent appends do not change selected prefixes.
There is no automatic latest selection, import-readiness wait or multi-book
snapshot in this operation.

Both timestamps are UTC, parsed at nanosecond precision; economic and recorded
cutoffs are inclusive. `effective_at` is an as-of cutoff, not a period end for
activity accounting. `recorded_through` filters trade and actual-cash knowledge.
Policy knowledge is independently selected by the explicit `policy_through`;
legacy revisions do not support inferred timestamp-based policy selection.
Callers must preserve that highwater rather than describe the result as all
information known at the timestamp without qualification. No local timezone,
implicit current date or currency conversion is inferred.

| Collection | Meaning |
| --- | --- |
| `journals` | Complete regenerated journal snapshot through the cutoffs, flattened to entry/line rows. It is not a persisted posting log or period activity. |
| `trial_balance` | As-of chart-account balances, debit-positive net, separated by book/currency. |
| `obligations` | As-of contractual trade cash, actual cash and outstanding cash, including fully settled zero-outstanding rows. Scheduled dates do not count as payments. |
| `actual_settlements` | Accepted actual cash events visible through both cutoffs, including explicit refunds, source evidence and admission ledger sequence. |
| `reconciliation` | Internal accounting controls comparing independently derived cash, obligations, inventory and economic realized P&L to journals. These are not external bank-observation comparisons. |

The pure accounting engine applies the approved capitalized opening charge and
both-legs cash recognition rules. LIFO selection remains independent of the
recognition basis. Each resolved policy and digest is returned. Lot/relief
amounts remain accessible with the existing `lots` operation using matching
ledger/policy cutoffs and mapping. Unrealized valuation P&L is explicitly
unavailable. Period P&L and dated edit/delete adapters await the separate reviewed
accounting/version contract; retrospective lifecycle corrections must not be
presented as dated transaction editing.

Every response includes `manifest`, `items`, decimal-string `total_rows`,
`has_more` and nullable `next_cursor`. The manifest retains the input prefixes,
cutoffs, tenant/book, engine/projection/rule versions, full configured account
bindings, mapping version/digest, full selected policy revisions and resolutions,
and ledger/settlement content digests. Journal rows retain record/economic/source
lineage and relief/settlement/policy trace links. Trial-balance rows retain their
contributing journal line identifiers. The content-derived `result_id` binds the
complete selected rows, manifest and display filters; it is not a durable job ID.
Immutable input history stays in the existing store. Save the manifest with an
export to retain the report selection; the application does not persist report
artifacts or promise that a later engine binary reproduces an older engine.

`charges_complete` and accounting breaks are preserved. Coverage always says
`admitted_prefix_only`, `staged_inputs_included=false`, and
`staging_completeness=unknown`. Inspect import status/records/errors independently.
A successfully completed report can exclude pending, invalid or unenumerated
imports; it never asserts those categories are zero or that ingestion is complete.

## Paging, display filters and exports

Optional `limit` is 1–100 and cannot exceed the configured store page limit.
Pass the returned opaque `cursor` with the identical selection. Cursors bind the
result identity and offset. Changing book, mapping, prefixes, cutoffs, collection
or filters fails `INVALID_CURSOR`; page size and format may change. Continuation
replays the bounded immutable inputs, so it survives process restart and needs
no cursor cache/expiry. Authorization is checked again on every page. Cursors
are selectors, not secrets or access grants, and must not be constructed by clients.

Journal rows follow core order (recognition date/effective time/entry ID, then
line order). Trial balances follow chart-account/currency order. Obligation,
actual-cash and reconciliation row ordering is the deterministic core ordering.
There is no arbitrary sort expression. Optional `currency` filters every
collection; `chart_account` filters journal/trial-balance rows only. Unsupported
combinations fail. Full custody pools are projected before filtering. Currency
controls remain scoped to **whole journals before display filters**, so filtering
one account does not create a false imbalance or discard evidence.

`format=json` returns the page as the standard machine-readable JSON document.
`format=jsonl` also returns `export` with UTF-8 `application/x-ndjson` content:
a `kind=manifest` record, `kind=row` records, then a `kind=page` continuation
record. Monetary values remain exact six-decimal strings with explicit currencies;
quantity/price values are eight-decimal strings. Do not add currencies together.
Each export page repeats the manifest, allowing it to be saved independently or
combined by `result_id` and offset. Export is inline; no server-side path is opened.
CSV, Arrow, Parquet and automatic full-file download are not implemented.

Reports bound each ledger/settlement prefix to 10,000 records, canonical/evidence
input to explicit byte budgets, pages to 100 rows/1 MiB, and simultaneous report
calls to one per application instance (`REPORT_BUSY`, retryable). Oversized input
fails `PROJECTION_LIMIT_EXCEEDED`; oversized pages fail
`PAGE_BYTE_LIMIT_EXCEEDED` and can be retried with a smaller limit. There is no
silent truncation. Report calls are synchronous and include replay/materialization;
MCP serializes stdio requests as before. A disconnected read creates no durable
job and is safe to repeat with the same selection. Larger-scale durable jobs,
checkpointed reports and independent report executors remain later work.

## Accepted transaction history

`query_transactions` requires `book_id`, `accepted_through`, `mapping_digest`,
`effective_at`, and `recorded_through`. It shares report manifests, paging,
exports and bounds. Optional `period_from`/`period_to` are UTC effective-datetime
bounds **[from,to)**; either may be omitted. Date-only and reporting-timezone
conversion belong to the caller. Filters are a closed object with optional
`account`, `instrument`, `source`, `type` and `status`. `source` is the ingestion
source ID, not an external-record ID. Types are `equity_trade`, `cash_movement`,
`cancellation`. Status means `active`, `superseded`, `voided`, or `cancellation`
in the selected lifecycle view. Rows are ordered by immutable acceptance sequence.

The query includes accepted history, not only active transactions. Each row
retains canonical bytes/digest, immutable normalized `record_id` and
`economic_event_id`, causal/next record relationship, recorded datetime,
`cancellation_at`, the predecessor's applicable dated `void_at`, source IDs,
immutable admission receipt, original import source metadata and validation
context. Normalized IDs are not interchangeable with submitted broker/local IDs;
source imports retain their external identity evidence. All rows have
`processing_status=committed`; staged invalid/blocked/pending rows live in
`import records` and `import errors`. Undated legacy cancellations are omitted
from a query that supplies period bounds because no economic datetime can be
invented. Accounting policy applies at report time and is not retroactively
stamped onto transaction acceptance evidence.

## Actual settlement and refunds

Both submission operations require the existing submit grant, an allowed source
and a configured economic account. Reads require inspect; native worker ownership
still requires the independent process grant. Read-only profiles omit submission
tools from discovery. The local profile optionally supplies `settlement_actor`
(default `local-profile`), an immutable trusted audit identity for request retries.
Changing it intentionally changes the idempotency fingerprint. Existing static
profile grants and book ownership remain in force; dynamic revocation is not added.

`submit_settlement` accepts `book_id`, `idempotency_key`, and `request` containing
exactly `account`, `trade_record`, signed decimal `cash`, `currency`, UTC
`effective_at`, and `source={source_id,reference}`. `trade_record` is the normalized
immutable ledger record ID returned by transaction queries. The facade supplies
native `reverses=null`. A refund uses `submit_refund` with the same fields plus
`request.reverses`, the prior settlement's ID. It must exactly reverse one prior
non-refund payment; partial refunds and arbitrary cash adjustments are unsupported.
The native C++ worker validates binding, dates, direction, cumulative amount,
refund uniqueness, active trade and closed periods. No cash event is fabricated
from a scheduled settlement date, edit or deletion.

A successful call means only **durable receipt**. It returns the key as `id`,
`state=received`, `durable_acceptance=true`, and
`completion=pending_or_in_progress`. It does not claim completed validation,
admission or report availability. CLI submission starts no worker. Use
`worker run` or a processing-enabled MCP host. `settlement_status` takes
`book_id`/`idempotency_key`; state is `pending`, `committed` with immutable `event`,
or `invalid` with retained `code` and submitted request/actor. Admission sequence
and ledger highwater serialize as decimal strings. Retained invalid status is a
successful inspection, not a transport error.

The key is scoped to the configured tenant/book and shared by settlement/refund
requests. Identical native request and trusted actor return the original receipt,
including after restart or completion. Different content returns
`SETTLEMENT_IDEMPOTENCY_CONFLICT`. Source/account grants are rechecked on retries.
Storage failure with ambiguous receipt commitment returns `OUTCOME_UNKNOWN`, exit
9; retry the **identical** selection, request, key and actor to recover identity,
then inspect status. Never submit a new key merely because the response was lost.
Closed-period and other financial rejections occur asynchronously and remain
queryable; receipt success cannot bypass them.

## Errors and permission boundaries

Both adapters return `{api_version,operation,ok,data,error}`. MCP domain failures
set `isError=true` and preserve the same structured document. Error fields are
`code`, `retryable`, `exit_code`, `outcome`, and recovery guidance in `retry`.
Protocol errors remain JSON-RPC errors. Existing CLI exit categories apply:
0 successful call/inspection, 2 invalid request or limit, 3 scope/grant denial,
4 missing/conflicting/stale selection, 5 retryable operation, 8 unclassified
internal/domain failure, 9 uncertain mutation receipt. `REPORT_BUSY` is retryable;
`INVALID_CURSOR`, mapping conflicts and unavailable prefixes require inspecting
and correcting the selection. No arbitrary SQL, paths, policy changes, period
closes, edit/delete commands or authorization overrides are exposed here.
