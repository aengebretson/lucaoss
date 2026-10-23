# Shared application API — v1 design

Implemented follow-up: [bounded period P&L queries](cli-mcp-pnl.md) consume the reviewed period contract; closed-book period retention remains unavailable.

The dependent [immutable-version adapter increment](cli-mcp-versions.md) adds
schema-6 replay compatibility, edit/delete and hash-verified source evidence.
Earlier limits in this guide describe its original implemented slice.

Additional implemented operations: [bounded accounting reports, accepted transaction
history, settlement and refund submission](cli-mcp-reporting.md). The report-job
API below remains a proposal.

Implemented bounded slice: [CLI and stdio MCP foundations](cli-mcp-foundations.md).
It exposes existing ingestion/status/errors, policy inspection, canonical
transactions and economic LIFO lots through the optional SQLite application facade.
The broader storage-independent API, report jobs and unfinished accounting
operations described below remain proposals; use `foundations-v1.tools.json`
for the executable MCP catalog.

Status: reviewable contract proposal, 2026-10-21. No new application service,
unified CLI, MCP server or report-job implementation is delivered by these
contracts. The existing native ingestion runtime and `luca-ingest` remain usable.

## Deployment and ownership

Luca OSS is a C++ library with optional execution and application adapters. It has
no graphical UI. Enterprise links the same application operations used by native
CLI tools and an optional MCP server. No gRPC or REST service is required.

For a local MCP installation, a compatible host can launch `luca-mcp` over stdio;
the host owns that process's lifecycle. Alternatively an operator can run a CLI
command from a shell without an LLM or MCP process. These executable names beyond
the existing `luca-ingest` are proposed. A remote MCP endpoint is deferred.

The reference store is a **directory**, containing per-book SQLite metadata,
immutable source payloads, transfers, locks and the pinned storage profile. A
SQLite file by itself is not a complete backup or portable data set. Supply a
configuration with tenant/book/source bindings, limits and immutable reference
and calculation contexts. Stop writers and preserve the entire store with a
supported backup procedure; live backup/restore certification remains open.

Processing requires a worker owner: the proposed MCP process owns a bounded
runtime by default; CLI submission alone does not launch a detached worker.
`luca worker run` is the explicit shell/service worker. Shutdown joins owned
threads and preserves pending work for the next runtime. Neither an LLM session
nor an MCP connection owns the durable financial history.

## Boundaries and artifacts

- Pure financial libraries retain typed exact values, lifecycle semantics and
  deterministic projections. They acquire no storage, JSON, MCP or auth dependency.
- A proposed optional `luca::application` component owns typed application
  requests/results and orchestration over a conforming store/executor. Its
  declarations must not expose SQLite, JSON libraries, sockets or MCP types.
  This target is not yet created; storage-independent execution is not claimed.
- The initial implementation wraps `luca::ingestion_sqlite` for durable operations
  and calls existing portfolio/accounting projections. A future enterprise
  persistence adapter must satisfy the same transactional/fencing/recovery
  guarantees; two unrelated database writes are not a conforming replacement.
- Enterprise owns identity, policy decisions, account/instrument mapping,
  source integration and all screens. It pins a reviewed OSS commit/release. API v1 is a semantic version, not a
  promise of C++ binary ABI stability; build compatible artifacts with the
  enterprise toolchain and run the independent consumer checks.

The [C++ declaration sketch](contracts/application_api.hpp) is compilable for
syntax review but is not an installed or implemented API. The separate
[SQLite host construction sketch](contracts/sqlite_application.hpp) binds existing
stores, immutable calculation contexts, a host-supplied `Authorizer` and explicit
execution budgets. Receipt/read executors are owned by the host;
`start_workers()` starts background validation/report work, while
`run_ready_work(max_jobs)` supports bounded CLI once/drain modes. Host destruction
resolves queued calls and joins workers; durable pending jobs remain. Reject
mismatched store/book bindings and unsupported context definitions before serving
calls. Enterprise owns host lifetime; sessions retain shared closed state after
host stop/destruction, so later calls fail with STORE_UNAVAILABLE rather than
accessing a freed store. The machine-readable
[application schema](../contracts/application/v1.schema.json) defines adapter
documents. The [MCP catalog](../contracts/mcp/v1.tools.json) contains self-contained
tool schemas; it is not a complete MCP handshake or `tools/list` response.
[CLI mappings](cli.md) use these same operation names and result documents.

## Operation mapping

| C++ book-session operation | CLI family | MCP tool |
| --- | --- | --- |
| `capabilities` | `capabilities` | `luca_capabilities` |
| `stage_payload` with owned reader | `payload stage` | Optional local `luca_stage_file` adapter opens an authorized reader |
| `submit_import` | `import submit` | `luca_submit_import` |
| `get_import_status` | `import status` | `luca_get_import_status` |
| `read_records` | `import records` | `luca_read_records` |
| `read_diagnostics` | `import errors` | `luca_read_diagnostics` |
| `revalidate_import` | `import revalidate` | `luca_revalidate_import` |
| `request_report` | `report request` | `luca_request_report` |
| `get_report_status` | `report status` | `luca_get_report_status` |
| `read_report_page` | `report rows`, composed export | `luca_read_report_page` |
| `get_evidence` | `report evidence` | `luca_get_evidence` |

Book scope and caller identity live in the authorized C++ session; adapter
requests carry `book_id` only to select that session. `request_id` maps to
`CallOptions`. The JSON payload discriminator maps to the typed C++ variant;
nullable JSON fields map to `std::optional`, decimal counts to checked unsigned
integers, and monetary/quantity strings to existing exact domain constructors.
No Enterprise caller needs a JSON document to invoke the C++ operations.

CLI worker startup and MCP stdio process startup are runtime administration,
not financial tools. Configuration/context registration remains an explicit
operator workflow; the initial MCP catalog does not expose it.

## Calls, identity and authorization

`Application::open_book(caller, scope)` creates an authorized book session.
The injected `Authorizer` checks inspect/submit/revalidate/run-report/read-report
permissions and the source grant where applicable; it can use enterprise policy
without putting an authentication framework into the library.
Caller identity is established by the embedding host or trusted local profile,
never by a model-supplied role or tenant field. Authorization is checked on every
operation, including reads/cursors/evidence and retries. The session does not
make later revocation ineffective. A book ID is a selector, not a credential.
The first contract is single-book; multi-book reports need an explicit revision
vector and are not simulated by merging unrelated results.

Enterprise maps its authenticated principal to the C++ caller context and uses
its authorization policy at this boundary. MCP/CLI local profiles establish the
tenant and permitted books/sources. Remote authenticated identity is a later
adapter concern. No MCP tool exposes arbitrary SQL, arbitrary filesystem access,
an override to mark a record valid, or an edit/delete of accepted events.

Stable IDs distinguish book, account, instrument, source, staged record, economic
event, lifecycle record, journal entry/line, import, report and evidence. Adapters
map broker execution/allocation IDs explicitly; a transport idempotency key is
not financial event identity. Preserve source evidence and correction lineage.

## C++ ownership and asynchronous completion

Operations return `Task<T> = Result<std::future<Result<T>>>`. The outer result
reports enqueue/backpressure failure before ownership transfers. A successful
enqueue is volatile, not a durable receipt. The inner result is the operation
completion. Returned values own their data; callers may destroy input buffers
after transferring ownership. Requests are passed by value. A staged source
reader is moved into the executor, never borrowed across a call boundary.

The executor has explicit queue, byte, worker and per-book limits. Enterprise
does not execute filesystem/SQLite or report work on its network event loop.
It owns runtime startup/shutdown and may provide separate bounded ingestion and
report execution budgets. These are application facilities, not financial-core
scheduler dependencies. Implementation must document allocation/setup exceptions;
ordinary operational failures use `Result`.

A caller deadline or stop request can stop waiting or work before a durable
operation commits. After durable commitment it cannot retract receipt, admission
or a report job. An uncertain outcome returns `OUTCOME_UNKNOWN` with the original
idempotency key; retry the identical command/key to recover its identity. Never
report definite non-acceptance after an ambiguous commit. Read calls are safe to
repeat. Report jobs are distinct from futures and survive process restart.

## Submission and retained validation

`stage_payload` stores a bounded stream and returns an immutable scoped payload
handle after bytes/length/hash are durable. Staging alone creates no import or
validation intent; unattached handles expire under quota/retention rules.
MCP `luca_stage_file` can stage only a regular file under a configured input-root
ID. Resolve relative paths securely beneath an opened root, reject escapes and
symlinks, and copy/hash the bytes into staging; reject detectable source changes
while reading. A caller cannot provide a server root path or fetch URL.
This tool is optional and advertised only when input roots are configured.

`submit_import` accepts either that handle or an owned inline UTF-8 payload
(adapter limit 64 KiB, also subject to the store's lower configured limit).
Common receipt semantics are unchanged: persist bytes, idempotency binding and
processing intent before resolving success. Same scoped key/fingerprint returns
the original import; a different fingerprint returns `IDEMPOTENCY_CONFLICT`.
An expired handle cannot be submitted; retries of already received commands
still recover their original receipt before testing handle expiry.

Validation is asynchronous. Malformed transaction contents become retained
diagnostics after receipt. Unsupported envelope/format/context, authorization,
quota or storage failure can fail receipt. `validation` and `commitment` remain
separate fields. Counts describe enumerated records; enumeration completeness is
explicit. Unknown/unparsed rows must not be represented as zero errors.

`read_records` and `read_diagnostics` pin a snapshot on the first page; subsequent
cursors preserve it despite revalidation/admission. Filters are part of cursor
identity. `revalidate_import` names immutable context and group IDs with expected
revisions. A stale selection fails explicitly; prior outcomes remain inspectable.
No generalized retry/operator repair command is promised by this slice.

## Report selection, readiness and coverage

Initial kinds: positions, settled cash, settlement obligations, journal lines and
trial balance. Cash/equity only; production accounting policies, valuation/P&L,
options and allocation rules are not invented by this interface. Capabilities
advertise implemented kinds and exact context IDs. Unsupported kinds/filters
fail explicitly. Economic account and chart-of-account identifiers are distinct.

Every report command requires an immutable calculation context ID, economic
cutoff, recorded/knowledge cutoff and settlement date. Contexts pin engine,
projection, normalization, reference snapshot and accounting policy/version
(policy is absent for non-accounting projections). No implicit `latest` context
or currency conversion. Structured filters are frozen with the job and totals.

Selection is either an exact accepted revision or `current_at_request`.
For readiness `available`, the selected accepted revision **and related staging
coverage** are captured transactionally with durable job acceptance. Materialize
that exact selection later; do not recapture a newer ledger when a worker starts.
For `require_imports`, require `current_at_request`, explicit existing import IDs
and a deadline. Persist a waiting job; once enumeration of every required import
is complete and every required group is committed, freeze the then-current
revision and coverage atomically. Enumeration failure or invalid/conflicting
required groups fail
with `REQUIRED_INPUTS_INVALID`; deadline expiry fails with `INPUTS_DEADLINE_EXCEEDED`.
Blocked dependencies can wait until that deadline. No global cross-book snapshot
is implied. Capturing an exact older revision also captures current staging
coverage separately; admitted-after-selection records must be distinguished from
unadmitted records rather than presented as part of that historical revision.

Job states: waiting_for_inputs, queued, running, available, failed. A successful
report request is a durable job receipt, not a completed report. Job status
includes any pinned selection, immutable coverage and retained failure. A result
becomes available only after all required partitions and evidence are complete;
incomplete partitions are never exposed as a final result. An available result
can still disclose staged inputs excluded from its accepted-history selection.

Coverage counters are book-wide at the captured snapshot, not filtered report-row
counts. `pending` includes pending/validating enumerated records; unenumerated
records remain unknown when `enumeration_complete` is false. Each exclusion
category is disjoint: classify admitted-after-selection first, then invalid,
blocked, pending/validating, and valid-but-uncommitted (including admission
conflicts). Already admitted records included in the selected prefix are not
counted again merely because their status history has an old diagnostic.

Report command idempotency fingerprints the requested kind/context/cutoffs,
selection mode, readiness and filters, not a later resolved revision. An identical
retry returns the original job and its original selection, even after new
admissions. It never schedules a fresh report implicitly.

Allowed filters in v1: positions accept economic account/instrument; settled cash
and settlements accept economic account/currency; journals and trial balances
accept economic account/currency/chart account. Other nonempty filter dimensions
fail. Chart-account filters apply after whole-entry projection/reduction. The
manifest labels journal controls as `whole_journals_before_chart_filter`: they
verify the complete selected journals, not just the displayed account subset.
Non-accounting reports have control scope `none` and no journal controls.

The result manifest identifies the input revision, coverage snapshot, context,
all cutoffs, operation versions, row count and per-currency journal controls.
No currencies are summed together. Position/cash rows do not fabricate balanced
journal controls. All pages, totals, exports and evidence refer to the same
immutable result ID. Display live ingestion status separately from this manifest.

Rows have typed discriminators and opaque row IDs scoped to a result. Canonical
sort keys are account/instrument for positions, account/currency for cash,
account/date/currency/direction for settlements, entry/line IDs for journals, and
chart account/currency for trial balances. No arbitrary sort expression in v1.
Evidence is a paginated list of contributing source/event/journal links and byte
locators; correction links retain their causal lifecycle record ID. It must be
possible to follow causality to prior source evidence without reading live state.
The first evidence page for a requested row also returns its prior lifecycle
links as evidence targets; subsequent ancestry can be requested by evidence ID.

## Schema, paging, errors and retention

`luca.application/1` is independent of the MCP protocol version. Adapter results
use `{api_version, ok, result}` or `{api_version, ok:false, error}`. MCP successful
domain responses use `structuredContent` matching `outputSchema`; operation
failures also set MCP `isError:true`. Protocol/unknown-tool errors remain MCP
errors. Request/response field names use snake_case; unknown request fields fail.
Additive schema changes must not change existing meanings; incompatible contracts
get a new major version and explicit migration. Future enum values are not
silently treated as success.

Exact money (scale 6), quantity/price (scale 8), counts and byte offsets are
strings on JSON boundaries. C++ uses existing `Money`, `Quantity`, `Price`,
`Timestamp` and identifier types. Schemas constrain lexical form; native parsers
still enforce representability, precision, NFC, dates and financial invariants.
No floating-point financial values or fabricated canonical hash promises.

Page limits are 1–1000, default 100, and may be lower in configured capabilities.
Opaque cursors bind authorized scope, resource/result, filters and snapshot.
Record/diagnostic pages expire after the advertised retention interval (initial
reference target 15 minutes); expiry returns `CURSOR_EXPIRED`, never a live
continuation. Restarting the query creates a new explicit snapshot. Reports and
evidence have separate advertised retention/quotas; expired results return
`RESULT_EXPIRED`. Durably retain job failure/expiry status and idempotency bindings
for the declared command retention period. Import idempotency bindings last for
the receipt lifetime, independently of report-job retention; an expired report
command key must not silently create a new job under an old identity. Do not silently purge financial source
evidence. A result-expiry implementation is future work, not an existing purge API.

Stable operation errors include INVALID_ARGUMENT, UNAUTHORIZED, FORBIDDEN,
NOT_FOUND, UNSUPPORTED_CAPABILITY, IDEMPOTENCY_CONFLICT, STALE_REVISION,
CURSOR_EXPIRED, RESULT_EXPIRED, RESOURCE_LIMIT, STORE_UNAVAILABLE,
INPUTS_DEADLINE_EXCEEDED, REQUIRED_INPUTS_INVALID, OUTCOME_UNKNOWN, CANCELLED,
DEADLINE_EXCEEDED and INTERNAL.
Errors carry request ID, safe detail, retryable and optional retry delay.
Transaction diagnostics remain data and use the existing adapter's diagnostic
codes. Native mappings must be explicit; never collapse invalid records into
transport failure or leak unauthorized resource existence through errors.

## Enterprise acceptance and implementation sequence

1. Review this contract and schemas, then implement a typed facade over the
   existing native store. Keep the public installed API separate from this draft.
2. An independently built enterprise-style C++ consumer supplies trusted identity,
   book binding and explicit runtime ownership; it stages/submits, receives a
   receipt, inspects a retained invalid record and admits valid groups.
3. Add durable report selection/jobs and complete coverage, using existing exact
   projections. Verify funding/purchase/correction/settlement/reversal under the
   fixture policies, including line/source evidence and concurrent intake.
4. Add native CLI mappings, preserving `luca-ingest` compatibility. Compare direct
   C++ and CLI structured financial results for the same pinned selection.
5. Add the optional stdio MCP server. Negotiate a protocol revision supported by
   the target host and test discovery, schemas, authorization, disconnect/restart
   and the same financial results. The catalog alone proves no host integration.
6. Measure cold/warm reports, ingestion/report contention, queue age, memory,
   cancellation ambiguity and recovery. Shared preparation, incremental evidence
   and partition laws need implementation evidence, not a transport speed claim.

This document changes no enterprise production service, deployed database,
financial policy or accepted performance budget.
