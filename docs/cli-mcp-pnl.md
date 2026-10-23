# Bounded period P&L application queries

Implemented increment following [reporting](cli-mcp-reporting.md) and
[immutable versions](cli-mcp-versions.md). The financial contract is the reviewed
[pure C++ period-accounting/1](period-accounting.md), including
`select_period_pnl`. CLI, MCP and Enterprise call the same optional C++
`SqliteApplication::call(Operation::query_pnl, arguments)` operation. Financial
rules and full-boundary replay remain in installed, storage-independent C++.
The adapters do not filter snapshot journals by date to manufacture activity.

## Commands and executable schema

CLI: `luca --profile PROFILE pnl query --request REQUEST.json`.
Stdio MCP: `luca_query_pnl`, with the same arguments. Initialization, framing,
profiles, errors and permissions follow [foundations](cli-mcp-foundations.md).
The checked-in [tool catalog](../contracts/mcp/foundations-v1.tools.json) is both
runtime request validation/discovery and the concrete machine-readable output
schema. This is an additive operation under `luca.foundations/1`.

Example request (replace identifiers and prefixes with inspected book state):

```json
{
  "book_id": "synthetic-book",
  "accepted_through": "42",
  "policy_through": "3",
  "settlement_through": "9",
  "mapping_digest": "0000000000000000000000000000000000000000000000000000000000000000",
  "period_from": "2026-10-01T00:00:00Z",
  "period_until": "2026-11-01T00:00:00Z",
  "recorded_through": "2026-11-02T12:00:00Z",
  "reporting_timezone": "UTC",
  "correction_recognition": "recorded_time",
  "collection": "rows",
  "limit": 100,
  "format": "json"
}
```

`mapping_digest` must be the actual value returned by `capabilities`; zeroes above
are an illustration. There is no implicit latest prefix or clock. Revisions are
unsigned decimal strings. UTC datetimes require `Z`, support nanoseconds and are
calendar validated. `period_from` is inclusive and `period_until` exclusive;
opening is immediately before from and closing immediately before until. The
inclusive `recorded_through` cutoff is independent and identical at both endpoints.
Entity recognition defaults must exist at the opening boundary and at every
replayed financial boundary. Missing defaults fail rather than invent a policy.

The caller explicitly selects the reviewed `recorded_time` correction convention.
Original activity follows its economic date; corrections follow recording time;
dated voids retain their explicit date; a linked replacement honors both recording
time and its inherited void gate. Backdated void and replacement activity can
therefore occur on different dates. See the financial contract for examples.
Actual cash follows admitted settlement effective dates and survives trade voids;
only explicit supported refund events reverse cash. This read operation never
submits a correction, cash event or repair.

## Collections and meanings

| Collection | Contents | Selection and totals |
| --- | --- | --- |
| `rows` | Account-attributed P&L by complete book/entity/custody/instrument/currency pool, opening/activity/closing, charge completeness and record/settlement/derived-entry IDs | Optional exact `account` and `instrument` display filters; totals cover all matching rows before pagination |
| `daily` | Sparse UTC days with activity, currency, realized activity and derived-entry IDs | Whole book by currency; account/instrument filters rejected |
| `evidence` | Every pinned canonical ledger record with admission/source/context evidence, every selected policy revision and its independent knowledge timestamp, every actual cash event | Full prefix evidence; account/instrument filters rejected; financial totals are whole book by currency |

Financial fields distinguish `economic_realized`, `recognized_realized`, and
`deferred_realized` (economic minus recognized). Gains are positive. `activity`
is period activity; `opening` and `closing` are cumulative as-of realized P&L
balances. All three use the same explicit period recognition convention, which
can differ from older retrospective economic snapshot queries. Each exact amount
is an object with six-place decimal `amount` and explicit `currency`; no binary
floating-point financial output is used. Currencies are never summed together.

Lot relief runs over the complete input book and custody pools before account or
instrument selection. Attribution follows the closing trade account. An absent
filter value returns no selected rows/totals; it does not narrow the authoritative
calculation or its diagnostics. Daily rows are never mislabeled account detail.
For attributed single-day activity, request a UTC day's half-open period with
`collection=rows`, then select account/instrument.

`unrealized_pnl`, `other_income`, `expenses`, `fx` and `cash_flow` are explicitly
null. This increment has no valuation inputs or broader statement classification.
It does not expose the core's comparative financial-statement API. The old
`accounting_report` operation remains labeled as-of balance and regenerated
snapshot journals; it is not period activity.

## Completeness, ownership and retained inputs

Every page includes all financial diagnostics, `supported_complete`, and their
whole-input scope, before display filters. Unknown charges produce provisional
coverage; settlement breaks and unclassified deferred/suspense controls remain
visible. Even true means only the declared supported cash/equity cost-based slice,
not complete investment returns or statutory financial statements. Zero is never
substituted for unsupported values.

Only admitted prefixes are financial inputs. Staged imports, delayed workers,
invalid requests and observations remain separate; `staging_completeness=unknown`
is preserved. Inspect retained import/settlement status and errors separately.
A durable submission receipt is neither validation nor admission nor successful
reporting. Reports do not start workers, reserve a snapshot of the staging queue,
mutate policies, or convert observations into authoritative events.

`book_id` is a host-authorized book, never a path. Inspect permission is required;
submit permission is unnecessary. Complete configured account bindings and the
host mapping digest/version are captured in every manifest. The financial chart
is separately explicit as `cash-equity-chart/1`; its fixed core mapping identifier
is not substituted for the host's account-binding revision. Book/tenant identity,
all bindings, immutable accepted/policy/cash prefixes, input cutoffs, recognition
rules, independent policy knowledge, replay limits, source and canonical digests,
core input/history digests and core `report_id` are retained in returned documents.
The application page identity also binds the collection and filters.

`evidence` provides a paged machine-readable copy of the full financial input
prefixes, including inputs outside the knowledge cutoff. Such rows are explicitly
evidence, not assertions of period activity. Canonical lifecycle bytes preserve
immutable originals, linked versions and dated voids. Source receipt/context/digest
links are retained; fetch original source text and edit/delete actor/reason through
`transaction_evidence`. The evidence export does not embed entire original import
payloads. Keep the unchanged executable/API version and manifests with exports
when archiving a reproducible report.

A repeated pinned query reproduces its IDs and financial results after subsequent
admission and restart. Evidence is read through existing immutable native APIs;
there is no multi-input live-latest selection and no global ingestion pause.

## Retention capability and protected periods

This host does not yet durably store complete period results and close exceptions.
`period_retention_available=false` is advertised, and returned open-book pages
have `retention_available=false`. There is no report receipt or report job status.

**Every period-P&L request for a book with any native period close fails
`PERIOD_RETENTION_UNAVAILABLE`, including later open intervals.** The guard is
checked before input reads and again after replay to detect a concurrent close.
The operation does not accept caller-invented anchors, bypass close protection,
recompute a protected closed result, or claim retained-baseline verification.
Read-only queries are evaluated before any close that occurs after their final
availability check. Returned documents are not subsequently promoted to retained
closed reports. Existing native close/admission protection is unchanged.

Schema-6 policy revisions carry independent knowledge timestamps. A legacy null
or missing selected policy knowledge timestamp fails `POLICY_KNOWLEDGE_UNAVAILABLE`;
effective date is never used as an invented knowledge date. The financial owner
reviewed both conservative capability fences. Full retained-result/exception
storage and anchored closed-history publication remain future host work.

## Bounds, pagination, exports and errors

Execution is synchronous native C++, serialized with the existing application
report mutex. Per instance at most one report executes; contention returns
retryable `REPORT_BUSY`. Separate instances/processes retain their own bounded
budgets and existing SQLite concurrency behavior. There is no distributed report
scheduler or cancellation protocol.

Limits are advertised: at most 1,000 accepted records, 1,000 cash events, 256 policy
revisions and 256 complete account targets. Core replay has at most 512 boundaries
and 200,000 work units (ledger+policy+cash+target rows multiplied by boundaries).
Historical boundaries needed to derive opening balances count as work. Ledger
and cash evidence each retain the existing 32 MiB bound, policy evidence 4 MiB.
Count, byte and replay limits all apply; the first exceeded limit fails explicitly.
There is no partial projection, sample, silent truncation or automatic job switch.

Page `limit` is 1–100, reduced by configured native page size; maximum serialized
page is 1 MiB. Stateless opaque cursor semantics match accounting reporting: the
cursor binds complete result identity and offset. A continuation recomputes the
same bounded result. Changing any financial prefix, interval, cutoff, mapping,
collection or display filter invalidates it; changing page size or JSON/JSONL
format does not change the result. No ordering is taken from transport arrival:
rows use typed complete partition ordering, daily rows use UTC day/currency, and
evidence uses ledger sequence followed by policy sequence then cash sequence.

`format=json` returns owned structured documents; `jsonl` additionally returns
UTF-8 `application/x-ndjson` text containing manifest+offset, row records and page
continuation. Export every page and retain the common manifest. There is no
CSV/Excel export, output path, SQL, report artifact service, or automatically
retained result. Cursor/result identities are content checks, not signatures.

Errors use the shared structured envelope with code, retryability, outcome and
CLI exit code. Bounds and invalid periods/arguments exit 2; permission/book scope
exit 3; unavailable input/retention/knowledge, incompatible cursor or mapping
conflict exit 4; retryable contention exits 5; unexpected failures exit 8.
Native domain errors preserve their code; invariant failures are not disguised as
zero P&L. Unknown transport response on this read is recovered by retrying the
identical pinned request or cursor; there is no submission side effect or new
idempotency key. For settlement/edit/import uncertainty, use their separately
documented same-key durable receipt recovery.

## Measured execution decision

[Raw Release adapter samples](../tests/benchmarks/results/2026-10-23-macos-arm64/application-period-pnl.jsonl)
on local macOS arm64/AppleClang 17 include process startup, immutable SQLite reads,
complete pure boundary replay/row attribution, evidence hashing and first-page
serialization. Disposable charged-complete alternating equity trades have one
distinct boundary each; ingestion setup and actual settlements are excluded.
Three pinned calls reproduce each fixture's core report identity.

| Trades/boundaries | Median full CLI call | Outcome |
| ---: | ---: | --- |
| 20 | 16 ms | available |
| 100 | 102 ms | available |
| 250 | 578 ms | available |
| 440 | 1,820 ms | available |
| 450 | 63 ms | explicit replay-work-limit rejection |

This supports the current small bounded synchronous surface, not a service SLA or
maximum latency guarantee. The pure core separately projects 100 trades plus 100
actual settlements in about 226 ms; that excludes storage and transport. Larger
histories, unusually expensive permitted fixtures and durable report-job design
need separate profiling and host work. No performance improvement claim is made.
