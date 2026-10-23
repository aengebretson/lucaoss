# Implemented CLI and stdio MCP foundations

This bounded implementation adds the optional `luca::application_sqlite` C++
facade, `luca` CLI and `luca-mcp` stdio executable. It reuses the native ingestion
store/worker, canonical codec, policy resolver and economic LIFO projection.
Enterprise can link the same facade directly without starting either executable.
No REST, gRPC, socket server, Python runtime or new storage schema is introduced.

The broader [application API design](application-api.md),
[storage-independent declaration sketch](contracts/application_api.hpp),
[original CLI proposal](cli.md), and `contracts/mcp/v1.tools.json` remain proposals.
They include report jobs, upload handles, generalized async calls and authorizers
that this increment does not implement. The executable catalog is instead
[`contracts/mcp/foundations-v1.tools.json`](../contracts/mcp/foundations-v1.tools.json).
`luca schemas --profile FILE` and MCP `tools/list` return its permitted subset.
The catalog is embedded at build time; installed executables need no schema file.

## Build and install

Requirements are the existing C++23/POSIX toolchain, SQLite >=3.38, nlohmann JSON
exactly 3.12.0, and CMake >=3.24. No added dependency. Example:

```bash
cmake -S . -B build/foundations -DCMAKE_BUILD_TYPE=Release \
  -DLUCA_BUILD_INGESTION=ON -DCMAKE_PREFIX_PATH=/path/to/dependencies
cmake --build build/foundations --parallel 2
ctest --test-dir build/foundations --output-on-failure
cmake --install build/foundations --prefix /absolute/path/luca-install
```

`LUCA_BUILD_INGESTION=OFF` keeps all application/storage dependencies out of the
financial-only package. `LUCA_BUILD_TOOLS=OFF` builds the embeddable libraries
without executables. External consumers use:

```cmake
find_package(Luca CONFIG REQUIRED COMPONENTS application_sqlite)
target_link_libraries(enterprise_accounting PRIVATE luca::application_sqlite)
```

```cpp
#include <luca/application/sqlite.hpp>
using namespace luca::application;
// The host opens its store using the existing ingestion API and establishes
// trusted per-caller grants/bindings. Requests cannot supply these grants.
SqliteApplication application({BookBinding{
    store, {{"account-a", "entity-a", "custody-a"}}, "mapping/1",
    true, false, false, {}}});
auto result = application.call(Operation::import_status,
    {{"book_id", "book-a"}, {"import_id", receipt_id}});
```

This initial optional SQLite facade intentionally uses the existing `Json` storage
request/result types. Its closed `Operation` enum avoids MCP/CLI coupling. It is
not the proposed typed, storage-independent `BookSession` API or an ABI promise.
The financial calculation APIs remain typed and independent of JSON and storage.
Do not put this synchronous receipt/read call on an Enterprise network event loop;
the embedding host schedules it on its own bounded executor. Request/results own
their values. Calls and `drain` support concurrent threads/processes under the
existing store contract; host start/stop/destruction require exclusive lifecycle
ownership and must not race other lifecycle calls. The host must remain alive for
calls. Grants are immutable for a host lifetime; Enterprise must enforce revocation
by ending/replacing its caller binding. No authentication framework is added.

## Operator profile and database ownership

A profile selects at most 16 explicitly configured books. It is trusted local host
configuration, protected by ordinary file/OS permissions. Tool arguments cannot
supply profiles, paths, SQL, caller roles, account bindings or alternate tenants.
Book IDs must be unique within one profile; separate same-ID tenants need separate
hosts/profiles. Book grants apply to the entire book, including its custody pools;
these are not per-account grants. Accounts are explicitly supplied host bindings,
not an invented account master or a scan of accepted transactions. Include every
ledger account in a custody pool when configuring lot projections. An uncovered
active trade fails `LOT_ACCOUNT_REQUIRED`; the facade never silently drops it.
Use a new immutable mapping version when changing binding definitions.

```json
{
  "books": [{
    "root": "/absolute/path/to/disposable-store",
    "config": {
      "tenant_id": "example-tenant",
      "book_id": "book-a",
      "source_ids": ["source-a"],
      "contexts": {"v2-ref1": {
        "adapter": "luca.transactions.jsonl/1",
        "normalization": "source-book-identity/1",
        "rules": "cash-equity/2",
        "engine": "luca/0.1",
        "reference_snapshot": "ref1"
      }}
    },
    "mapping_version": "mapping/1",
    "accounts": [{"account": "account-a", "legal_entity": "entity-a", "custodian_account": "custody-a"}]
  }]
}
```

Omitted grants mean `inspect=true`, `submit=false`, `process=false`. For durable
submission add `submit=true` and `submit_sources=["source-a"]`; both this grant and
the native store source configuration must permit the source. To run native
workers add `process=true` explicitly. Submission alone never starts a worker.
An MCP process starts one C++ worker per process-enabled book at initialization;
EOF stops and joins them. A CLI invocation only runs the explicitly requested
bounded `worker run`. The embedding host calls `start_workers()`/`stop()` explicitly.
Pending jobs survive the process and use existing leases, retries and fencing.
A killed worker's in-flight lease may need up to the existing 60-second default
lease before another process reclaims its work. No detached daemon is launched.

The **store directory**, not an arbitrary SQLite filename, is the ownership and
backup unit: SQLite metadata, immutable payloads, locks, transfers and the pinned
storage profile. Opening a configured store retains existing native behavior:
create a new store or transactionally migrate old schemas 1–4 to schema 5, reject
newer/unrecognized schemas and incompatible contexts/limits. Read-only tool grants
mean no financial mutations or workers, **not** SQLite `mode=ro`: startup may
initialize/migrate storage, and the native adapter uses its normal locks/journal
behavior. Initialize/upgrade under operator control with a coherent backup before
connecting a read-only host. No tool exposes migration or reference/policy edits.
`--help` and `--version` do not open storage. Native `luca-ingest` remains the
operator interface for context/reference registration and existing administration.
The integrated storage component uses schema 5, including the actual-settlement
APIs. Older schema-4 binaries refuse an upgraded store. Use a coherent pre-upgrade
backup for rollback and rebuild all consumers against the integrated component. Do not point it at the live
demo database or copy only its SQLite file.

Multiple compatible native workers, CLI readers/submissions and MCP processes may
share one local store. Existing book admission locks, revision/lease fencing,
SQLite transactions and durability profiles remain authoritative. Do not bypass
the adapter with direct SQL writes. Network filesystems, different configured
processing limits, live backups and cross-book atomic snapshots are not certified.
A process-enabled MCP host mutates background processing independently of read
calls; use a profile with `process=false` for observation only.

## Commands and operation schemas

All financial commands accept `--request FILE`, whose JSON is the exact MCP tool
arguments, including `book_id`. `books` takes `{}`. Success/failure documents have
`api_version="luca.foundations/1"`, `operation`, `ok`, `data`, and `error`.
CLI prints one JSON document on stdout. MCP returns the same document in both
`structuredContent` and a text content block, setting `isError` on operational
failure. Native receipt/status/per-record/diagnostic/policy fields are reused,
with explicit wrapper versions. Errors contain `code`, `retryable`, `exit_code`,
`outcome` and actionable `retry` guidance. Private paths/exception messages are
not echoed. A read of retained invalid data succeeds with `ok=true`.

| CLI command | MCP tool | Result |
| --- | --- | --- |
| `books` | `luca_books` | Explicit configured book/tenant IDs and grants |
| `capabilities` | `luca_capabilities` | Existing contexts/limits, current ledger and policy watermarks, implemented operations |
| `accounts` | `luca_accounts` | Configured account/entity/custody bindings and mapping version |
| `import submit` | `luca_submit_import` | Durable receipt and import ID; no validation/admission guarantee |
| `import status` | `luca_import_status` | Enumeration, validation and commitment counts, work health |
| `import records` | `luca_read_records` | Frozen per-record status pages |
| `import errors` | `luca_read_diagnostics` | Retained diagnostic pages, including prior validation runs |
| `import record` | `luca_read_record` | One record and optional explicit `run_id` |
| `policy history` | `luca_policy_history` | Immutable revisions and sequence cutoff (native 10,000 revision bound) |
| `policy resolve` | `luca_resolve_policy` | Existing pure C++ resolution with complete selected evidence |
| `policy result` | `luca_policy_result` | Previously retained content-addressed result evidence |
| `transactions` | `luca_transactions` | Canonical lifecycle rows with decoded exact values, source/causal/import links and canonical hex bytes |
| `lots` | `luca_lots` | Existing economic LIFO trades, open lots or reliefs with resolved policy evidence |

CLI-only `schemas` prints the permitted tool catalog; `worker run --book ID
[--max-jobs N]` performs bounded native processing (default 10,000, max 100,000).
Its JSON job count does not assert that blocked, leased or future work finished.
There are no tools to write policy, create accounts, close periods, settle trades,
run journals/trial balances or create/export report jobs. MCP annotations mark
reads `readOnlyHint=true`, submission `readOnlyHint=false`, and all tools
`destructiveHint=false`, `idempotentHint=true`, `openWorldHint=false`. Submission
is idempotent only for the identical scoped key/fingerprint. The tool is omitted
from discovery without a submit grant, and calls still enforce book/source scope.
Annotations are hints; shared C++ grant checks enforce access.

## Identifiers, exact data and pagination

IDs are opaque, case-sensitive strings. Native limits apply; adapter IDs are at
most 256 UTF-8 bytes (evidence IDs at most 64 and idempotency keys at most 128).
Import identity, source record identity, economic event identity and canonical
lifecycle record identity are distinct. A key does not replace financial identity.
The native transport scopes idempotency by tenant/book/source/key; fingerprinting
covers normalized metadata and exact payload bytes/length/hash. Same key and
fingerprint recovers the original receipt; different content fails
`IDEMPOTENCY_CONFLICT`. Preserve whitespace and line bytes on uncertain retries.
Reusing financial IDs under a new transport key can cause retained admission
conflicts and is not a substitute for retrying the original command.

Amounts are decimal strings; currency is a separate uppercase three-letter code.
Quantity and price have 8 fractional digits maximum; Money has 6. The existing
parser rejects excess precision, exponent/plus notation and out-of-range values;
there is no silent float conversion or rounding in the adapters. Projection
outputs have fixed scales (quantity/price 8, Money 6), using the existing exact
C++ types and accounting allocation rules. Input timestamps are UTC RFC3339
`YYYY-MM-DDTHH:MM:SS[.1-9 digits]Z`, checked by the existing timestamp parser;
settlement dates are `YYYY-MM-DD`. Dates are scheduled obligations only, not
payment evidence. No implicit date, context, calendar or FX conversion is added.

Adapter sequence/cursor offsets are canonical unsigned decimal **strings** up to
9223372036854775807. Native inherited bounded counters and policy revision numbers
remain JSON integers; they are exact and below JavaScript's integer range. Ledger
sequence/recorded-nanosecond fields in transaction output and capability watermarks
are decimal strings. `capabilities.data.ledger_state.last_sequence` and
`policy_sequence` provide explicit input cutoffs for subsequent reads.

`import records/errors` use native `next_cursor`, `snapshot_revision` and a page
limit of 1–100 (also capped by the store's lower limit). Cursors expire after 15
minutes and bind tenant/book/import/collection/filters/snapshot/offset. Repeat
identical filters when continuing; do not decode or edit cursors. Separate reads
can produce different opaque cursor bytes because expiry uses each call's clock;
they still select equivalent snapshots. A cursor obtained through either adapter
works through the other. Cursor expiry requires starting a new snapshot, which
may include later status changes. Historical errors are never erased by reading.

`transactions` requires `accepted_through`, accepts `after` (default "0") and
`limit` 1–100, and returns `next_after` and `has_more`. Rows are in original
acceptance order. No new revision is recaptured on later pages. `lots` requires
`accepted_through`, `policy_through`, `effective_at`, `recorded_through`,
`mapping_digest` from capabilities, and
`collection=trades|open_lots|reliefs`. It projects at most 10,000 admitted lifecycle
records, reusing the existing lifecycle resolver and policy-lot fold; larger
prefixes fail `PROJECTION_LIMIT_EXCEEDED`. Its `after` is a row offset, with
`next_after`, `has_more`, `total_rows` and page limit 1–100. Repeat every cutoff,
collection and the same immutable host mapping across pages. The digest covers
tenant/book/version and the complete configured bindings; changing bindings
between CLI invocations fails `MAPPING_CONFLICT` rather than mixing pages. These are bounded
synchronous reads, not durable report jobs, cached snapshots or signed cursors.
Full policy evidence accompanies lot pages, including empty results.

Lot coverage says **admitted prefix only**; staged/unvalidated/blocked records are
excluded, so inspect relevant import status/errors separately. No all-inputs-ready
guarantee is invented. `recognition_applied=false` remains explicit even if the
stored preference says cash/accrual. Lots are economic cost/P&L with charge
completeness flags, not tax/general-ledger recognition. Result evidence retrieval
reads already-retained evidence; read-only lot calls do not persist a report or
new policy-result record.

## Receipt, completion, errors and retries

Inline UTF-8 JSONL is capped at **64 KiB in bytes**, additionally limited by native
store quotas. Larger files use the existing native upload/receive operations;
MCP never opens model-supplied file paths. CLI `--file FILE` copies the bytes into
`payload_utf8`; omit that member from the request file. Newline-delimited MCP
frames are capped at 1 MiB before parsing. Requests are served sequentially per
stdio connection; native validation/admission run separately in C++.

Successful submission means bytes, scoped key and processing intent have committed
under the existing fsync/SQLite durability contract. It returns promptly without
waiting for enumeration, validation or admission. `completion` deliberately says
`pending_or_in_progress`, including retries of imports that may already be done.
Read `import status` for the actual state. `valid` is separate from `committed`;
blocked/missing references, conflicting records, closed-period rejection or worker
attention can prevent admission. `all_records_committed` requires complete
enumeration, no import errors, and commitment of every enumerated record.

| CLI exit | Meaning |
| --- | --- |
| 0 | Operation succeeded; receipt success means durable acceptance only |
| 2 | Invalid command/options/envelope/schema/unsupported format or size |
| 3 | Book/source/grant denied |
| 4 | Missing/conflicting/stale/expired/unavailable resource or cursor |
| 5 | Retryable capacity/backpressure/storage failure with definite response |
| 8 | Other nonretryable operational/internal failure |
| 9 | Mutation outcome unknown; retry identical scoped key/metadata/bytes |

No implicit wait exists in this increment, so proposed wait exits 6/7 are unused.
A process killed by the OS can produce no receipt at all; recover with the same
key. A receive storage error that may follow commit is conservatively surfaced as
`OUTCOME_UNKNOWN`, with `retryable=true` and exit 9. Never treat timeout, disconnect
or cancellation as proof of non-acceptance. Cancel notifications do not undo
accepted work. Back off with finite retries for retryable failures; retained
validation/admission errors are data to inspect/correct, not retryable transport
errors. MCP protocol errors use standard JSON-RPC errors; financial/application
failures use tool results with structured error data. Only initialization, ping,
`tools/list`, `tools/call` and notifications needed for this stdio lifecycle are
supported. Negotiated versions: 2025-11-25 and 2025-06-18. Unsupported requested
versions receive 2025-11-25 so the client can disconnect if incompatible.

## Bash example and disposable demonstration

Run the complete demo from the repository root, passing the build directory:

```bash
python3 -B examples/cli-mcp/demo.py --build build/foundations
```

It creates and removes a temporary store, registers synthetic references/policy
through the existing native operator API, submits a mixed valid/invalid batch,
prints its receipt, restarts through a new native worker, compares CLI/MCP status,
retained errors and economic lot output, and recovers the receipt by retrying.
It never opens the live demo database. The summary reports actual outcomes.
Python is demonstration/test orchestration only; all product work runs in C++.

For your own explicitly prepared disposable profile:

```bash
luca --profile /tmp/luca-fixture/profile.json books
printf '%s\n' '{"book_id":"book-a"}' > /tmp/scope.json
luca --profile /tmp/luca-fixture/profile.json capabilities --request /tmp/scope.json
cat > /tmp/submit.json <<'JSON'
{"book_id":"book-a","idempotency_key":"bash-fixture-001","metadata":{"source_id":"source-a","format":"luca.transactions.jsonl/1","validation_context_id":"v2-ref1"}}
JSON
luca --profile /tmp/luca-fixture/profile.json import submit \
  --request /tmp/submit.json --file /tmp/trades.jsonl > /tmp/receipt.json
import_id=$(jq -r '.data.import_id' /tmp/receipt.json)
jq -n --arg id "$import_id" '{book_id:"book-a",import_id:$id}' > /tmp/import.json
luca --profile /tmp/luca-fixture/profile.json worker run --book book-a
luca --profile /tmp/luca-fixture/profile.json import status --request /tmp/import.json
luca --profile /tmp/luca-fixture/profile.json import errors --request /tmp/import.json
```

## LLM host setup

A standard host starts `/absolute/path/luca-install/bin/luca-mcp` with
`["--profile", "/absolute/path/readonly-profile.json"]` and owns its stdio pipes.
No API key, network listener or additional service is needed. Protect profile and
store files from untrusted local processes. Keep `submit` and `process` omitted
for easy observation-only use. To enable submission, configure explicit book and
source grants, and arrange a native worker owner separately or enable `process`.

For Codex, the [official MCP configuration guide](https://learn.chatgpt.com/docs/extend/mcp?surface=cli)
documents local stdio command/args configuration. Example to add manually to a
user or trusted project configuration (this task does not alter your setup):

```toml
[mcp_servers.luca]
command = "/absolute/path/luca-install/bin/luca-mcp"
args = ["--profile", "/absolute/path/readonly-profile.json"]
```

Ask the host: “List Luca books and accounts. Inspect supported operations. For
import ID …, show retained validation errors and distinguish valid records from
ledger-committed records. Resolve policy for this account at the stated policy
sequence/date. Show economic open lots at the explicit ledger/policy cutoffs.”
With an explicitly enabled submission profile, ask it to call
`luca_submit_import` with `book_id`, `idempotency_key`, `metadata` and
`payload_utf8`, retain the receipt, and inspect status separately. The host should
present mutating calls under its normal approval policy. No arbitrary file tool
or filesystem root access is exposed to the model.

MCP framing, lifecycle, annotations and structured tool outputs follow the
[2025-11-25 stdio transport](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports),
[lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle)
and [tools](https://modelcontextprotocol.io/specification/2025-11-25/server/tools)
specifications. Interoperability tests use a real stdio protocol peer, not a hosted
LLM/account integration; manual Codex UI verification remains separate.
