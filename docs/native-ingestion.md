# Native asynchronous ingestion boundary

Decision, 2026-10-21: the user requires Luca's asynchronous ingestion APIs and
execution to be C++. The earlier Python orchestration in PR #22 is superseded.
The optional native runtime now directly calls the existing financial library.

| Responsibility | Implementation |
| --- | --- |
| Financial types, exact arithmetic, lifecycle preparation/acceptance, canonical encoding | Existing pure C++ library targets |
| JSONL shape/identity adapter and reference checks | Native C++ ingestion adapter |
| Durable receipt, immutable payloads, transfer, status/errors/history, leases and admission coordination | Optional compiled `luca::ingestion_sqlite` library |
| Owned receipt queue and background execution | C++ futures, bounded queue, `std::jthread` and stop tokens |
| Command-line operations | Native `luca-ingest` today; reporting commands planned |
| Enterprise integration | Direct C++ library integration by default |
| Agent integration | Optional MCP adapter, planned |
| Legacy store generation and cross-language migration assertions | Python test fixtures only |

`SqliteStore` has an explicit book scope and owns no global mutable state. It
provides synchronous storage operations returning `std::expected`. A caller can
embed it directly or use `AsyncHost` for owned submissions. Submission futures
resolve after durable receipt. They do not authorize ledger acceptance. Workers
consume durable jobs with leased generations and retain outcomes/diagnostics;
strict admission is ordered per book. Cache reconstruction never becomes the
source of truth. All resource limits and ownership rules are explicit.

The optional component depends on SQLite, nlohmann/json 3.12.0 and native threads.
The standard library does not supply a JSON parser or a transactional durable
store, so these dependencies remain at the external/storage adapter boundary.
Its public JSON objects represent versioned external documents; authoritative
financial values remain typed C++ objects. The separate package component export
keeps dependencies out of financial-only consumers. Group closure uses compact
identity/link records; complete transaction documents remain in durable staging.
Storage hashing uses the native macOS digest provider when available, with a
checked portable fallback; canonical financial encoding/hashes are unchanged.
No generic scheduler,
interpreter, cloud framework or broker is added to financial targets.

## Integration direction

Updated decision, 2026-10-21: default to linking Luca OSS directly into the
enterprise C++ backend. Native CLI tools and an optional MCP server expose the
same application operations. OSS supplies no graphical UI; enterprise owns
screens, dashboards, presentation and user workflows. The earlier selection of
µWebSockets/µSockets is withdrawn. Neither library was added as a dependency.
REST and gRPC hosts are deferred until a concrete consumer requires them.

Keep financial calculations in the pure libraries. Specify a small C++
application API for submission, durable status/errors, pinned report requests,
report jobs, result pages and evidence. The existing storage-specific JSON API
is not yet that complete application contract. CLI and MCP must call the same
operations rather than implement separate financial or validation rules.

### Native CLI

The existing `luca-ingest` executable supports receipt, uploads, workers,
status/record/error/history inspection, revalidation and ledger reads. Extend
this foundation with report requests, job status, paginated results, export and
evidence drilldown as the report API is implemented. A unified `luca` command
layout is proposed, not implemented; preserve existing command compatibility
or document an explicit migration.

Commands should offer readable text and stable versioned JSON output, documented
exit codes, errors on stderr, explicit book/store configuration and bounded
input/output. Define receipt-only completion separately from optional bounded
waiting for validation/admission or report completion. A successful submission
does not assert financial validity. CLI exit, timeout or interruption must not
discard already durable work. Do not silently launch detached workers; document
whether a command owns a worker or requires an independently running worker.
These are acceptance requirements, not claims about current CLI formatting.

### Optional MCP adapter

Plan tools for bounded submission, import status, retained diagnostics, report
requests/status/pages and source evidence. Keep large batches in authorized
staging and large results in paginated/exported artifacts, outside model context.
Start with stdio for a client-launched local process; add Streamable HTTP only
when remote access is required. Pin a protocol version compatible with target
clients. MCP transport does not choose a particular HTTP framework.
Sources: [MCP tools](https://modelcontextprotocol.io/specification/2026-07-28/server/tools),
[MCP transports](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports).

Every entry point must preserve scope authorization, immutable source evidence,
bounded ownership/backpressure, scoped idempotency and acknowledgement only
after durable receipt/work intent. Local commands use trusted local book/store
configuration; remote callers require authenticated authorization. Notifications
and connection state never replace durable job status. Financial values remain
exact, and adapter serialization does not redefine canonical ledger bytes.

### Next bounded slice

Review the drafted [C++ application API](application-api.md), [CLI command
contract](cli.md) and [MCP schemas](../contracts/mcp/v1.tools.json) before
implementing new adapters. Prove one cash/equity history through direct C++ and
CLI: durable receipt, retained invalid/blocked records, admission, pinned
positions/cash/journals/trial balances and evidence. Reports must identify the
accepted revision, policy/reference versions and time cutoffs, with staged
exclusions and projection lag distinguished. Then expose the same bounded
operations through MCP and verify equivalent results. Shared batch preparation,
incremental evidence and concurrent ingestion/report benchmarks remain explicit
roadmap work; no new throughput or production accounting claims are implied.

There is no MCP server or network listener in the current runtime. Report jobs,
CLI reporting/export commands and the full application API remain planned.

## Verification and limits

The native tests exercise direct C++ calls, native threads/futures, concurrent
admission, lease/context fencing and fourteen real SIGKILL boundaries. Separate
migration tests create original schema-1 stores with frozen Python fixtures, then
exercise only the native CLI. An installed consumer explicitly imports the native
component and completes ingestion without an interpreter. Financial-only package
consumers remain independent of the adapter dependencies.

See [the host guide](../apps/ingestion-host/README.md) and
[current executed verification](reviews/NATIVE_INGESTION_20261021.md). JSON DOM
allocation, per-group durable transaction costs, worker cache memory and storage
contention still need profiling and representative budgets. C++ alone does not
guarantee linear scaling or establish reporting/GPU performance.

## Enterprise FIX intake increment

The [source-envelope adapter](execution-envelopes.md) adds an explicit format for
retaining original execution/mapping evidence alongside a normalized transaction.
It shares the existing native worker and strict financial admission. Enterprise
can link this component directly from its C++ OMS outbox worker; this is an
initial consumer, not implementation of the full proposed application facade,
report service, unified CLI or MCP server.
