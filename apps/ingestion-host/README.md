# Native asynchronous ingestion

Luca's reusable ingestion runtime, durable store, background workers and CLI are
C++23. No Python interpreter, subprocess financial engine, or HTTP server is
required. Financial values/lifecycle rules remain in the pure C++ libraries.
Storage and scheduling belong to the optional `luca::ingestion_sqlite` component.

## Build and embed

Prerequisites for this optional component: POSIX local filesystem semantics,
SQLite >=3.38, nlohmann/json **3.12.0**, and native threads. JSON is an external
adapter representation, not an authoritative financial numeric type. Dependencies
must be supplied explicitly; CMake does not download them. Ordinary financial
library builds do not discover or link any of these dependencies.

```sh
cmake -S . -B build/native -DLUCA_BUILD_INGESTION=ON \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/dependencies
cmake --build build/native --parallel 4
cmake --install build/native --prefix /path/to/luca-install
```

Installed C++ consumers request the component explicitly:

```cmake
find_package(Luca CONFIG REQUIRED COMPONENTS ingestion_sqlite)
target_link_libraries(my_application PRIVATE luca::ingestion_sqlite)
```

Financial-only consumers continue to use `luca::ledger`, `luca::accounting`, etc.
The component export is separate so installing ingestion does not make JSON,
SQLite or threads mandatory for those consumers. JSON is pinned exactly because
its public C++ type is part of this optional adapter's compiled interface.

```cpp
#include <luca/ingestion/sqlite.hpp>
using namespace luca::ingestion;

// Config/metadata use the versioned external schemas below.
auto opened = SqliteStore::open("/private/local/data", config);
if (!opened) { /* handle opened.error().code */ }
auto store = *opened;
auto reference = store.register_reference("ref42", {
    {"accounts", Json::array({"portfolio-a"})},
    {"instruments", Json::array({"ABC"})}});

AsyncHost runtime(store, 4); // native workers, bounded receipt executor
// payload is owned while queued/in flight. No detached threads or interpreter.
auto queued = runtime.submit(metadata, "submission-001", std::move(payload));
if (!queued) { /* backpressure or stopped executor */ }
auto receipt = queued->get(); // std::future<Result<Json>>; resolves after durability
// Validation/admission is separate background work; inspect retained status.
auto status = store.status((*receipt)["import_id"]);
runtime.stop(); // finish accepted receipt submissions, then join workers
```

`SqliteStore::receive(metadata,key,istream,bytes)` provides bounded streaming
intake for a caller-owned I/O executor. It does no transaction parsing. The
`AsyncHost` convenience executor accepts owned buffers, bounded by 16 outstanding
submissions and 64 MiB of queued/in-flight payload bytes by default; configure
those budgets in its constructor. One receipt thread owns this convenience
queue. Applications can concurrently call the thread-safe streaming store from
their own bounded storage pool; independent stores/books can have independent
runtimes. Database/storage reads are synchronous and must also stay off a network
event loop. A future becoming ready means durable receipt, not financial validity.
Shutdown need not finish all financial work: remaining intent stays durable for
restart. `SqliteStore::open` and operations return `std::expected`; worker/runtime
construction can throw `Failure` for incompatible configuration or resource setup.

## CLI

`luca-ingest` is a native executable. Use `example-config.json` and metadata:

```json
{"source_id":"custodian-a","format":"luca.transactions.jsonl/1","validation_context_id":"cash-equity-v1-ref42","atomicity":"independent_groups"}
```

```sh
build/native/libs/ingestion/luca-ingest --root /tmp/luca-data \
  --config apps/ingestion-host/example-config.json receive \
  --metadata /path/to/metadata.json --key batch-001 --payload /path/to/input.jsonl

# /path/to/ref.json contains {"accounts":["portfolio-a"],"instruments":["ABC"]}.
build/native/libs/ingestion/luca-ingest --root /tmp/luca-data \
  --config apps/ingestion-host/example-config.json register-reference ref42 \
  --definition /path/to/ref.json
build/native/libs/ingestion/luca-ingest --root /tmp/luca-data \
  --config apps/ingestion-host/example-config.json worker --mode drain
```

Worker modes: `once` claims at most one job, `drain` stops when no work is currently
ready, and `watch` runs until SIGINT/SIGTERM. `drain` can leave future retries or
blocked records. Read commands: `status ID`, `records ID`, `record ID RECORD`,
`group ID GROUP`, `diagnostics ID`, `validation-runs ID`, `commits ID`, `events ID`,
`work-attempts ID`, `ledger-state`, `ledger-records`, and `capabilities`.
Pages accept `--cursor`, `--limit`, and JSON `--filters`. Payload reads use
`payload ID --output FILE [--offset N] [--length N]`; existing output files are
rejected. Relative status URLs name future HTTP resources, not a running listener.

Other commands preserve durable transfer/context workflows:

- `upload-create --request FILE`: `{key,expected_bytes,sha256,chunk_bytes}`.
- `upload-put UPLOAD --index N --payload FILE --sha256 HASH`, `upload-status UPLOAD`,
  and `upload-verify UPLOAD`.
- `receive-upload UPLOAD --metadata FILE --key KEY` attaches a verified upload.
- `register-context CONTEXT --definition FILE` adds an immutable definition.
- `revalidate IMPORT --key KEY --request FILE` takes
  `{validation_context_id,groups:[{group_id,expected_revision}]}`.
- `recover [--after TRANSFER_ID] [--limit N]` reclaims interrupted direct transfers
  and expired unattached uploads. Follow `next_after` until null.

## Financial behavior and retained state

Supported versions: `luca.transactions.jsonl/1`, `source-book-identity/1`,
`cash-equity/1`, `luca/0.1`. Cash/equity JSONL fields and lifecycle semantics are
specified in [the API contract](../../docs/staged-ingestion-api.md). Financial
numbers are exact decimal strings, never JSON floating-point amounts. C++ typed
constructors reject overflow, excess precision, invalid timestamps/dates and zero
trade quantities. Canonical identifiers are NUL-free UTF-8 NFC and are never
silently normalized. Duplicate fields, malformed/invalid UTF-8, unsupported
fields, invalid source timestamps and excessive JSON nesting become diagnostics.

Every nonblank input record retains a stable ordinal, original byte/line/hash
locator and import-specific source-evidence identity. Canonical record/economic
IDs are framed hashes of tenant/book/source labels, independent of submission
retry keys. Original payloads never change. Explicit atomic groups and same-import
causal closure are sealed before admission; whole-import atomicity is supported.
Ambiguous group membership fails enumeration instead of guessing a partial commit.
Missing predecessors/snapshots remain blocked and wake through indexed keys.
Reference changes require a new immutable snapshot/context and explicit
version-checked revalidation. Earlier outcomes/errors remain inspectable.

Validation and commitment remain separate. A valid record can belong to a failed
group or lose an admission race to another correction. Admission records its own
immutable run, rechecks the current C++ ledger and commits canonical bytes/hashes,
sequence/revision, receipt, record status, event and job completion in one SQLite
transaction. Cache publication follows durability; recovery strictly decodes and
reaccepts committed records. Retries cannot duplicate a group. Operational
failures use retry history and `attention_required`, not fabricated financial
invalidity. No public operator retry/repair endpoint is supplied yet.

Each `Worker` owns its in-process ledger cache. Different workers can parse and
validate concurrently. Per-book admission uses an OS lock, lease generations and
expected group/ledger revisions. SQLite still has one writer per book. Separate
books have independent databases and admission locks. Source authorization and
book access are the caller's responsibility before opening the trusted-local
store; there is no authentication server in this component.

Defaults: payload 256 MiB; retained/reserved payload bytes 4 GiB; 1,024 pending
imports/transfers; chunk 4 MiB/8,192 chunks; page 1,000 items; upload TTL 24 hours.
Worker defaults: batch 256 records, import 100,000 records, record 64 KiB, group
1,000 records, book cache 1,000,000 records, lease 60 seconds and three attempts.
Processing resource bounds are pinned at the first worker; batch/lease/retry
options stay worker-local. Completed/blocked imports release pending slots so
prerequisites can arrive; original bytes still count toward retention. Metadata,
history, ledger disk space and process memory require external budgets.

## Durability, migration and scope

Tenant/book scope is hashed into a private local directory. Original files are
flushed, installed read-only and directory-synced before receipt. Receipt,
idempotency binding, context, event and parse intent commit atomically. Retries
with changed content/options fail; identical content under a new key remains a
new submission. Transfer locks use a bounded 4,096-stripe namespace.

Durability mode is pinned before opening SQLite. Patched SQLite uses WAL/FULL;
older SQLite uses DELETE/EXTRA to avoid the upstream WAL-reset bug. Reopening a
WAL store on an unpatched runtime is rejected. macOS full-sync settings are
preserved. Schema-1 stores from the original intake host migrate atomically to
schema 2; receipts, payloads, keys and resumable transfers retain their identities.
Older hosts cannot reopen schema 2. No automatic profile conversion, evidence
purge, backup/restore or power-loss certification is implied.

The frozen Python implementation under `tests/ingestion/legacy_reference` is only
a schema-1 compatibility fixture. Python remains useful in tests and benchmark
analysis; it is absent from the installed native ingestion runtime.

The optional REST adapter will use **µWebSockets over µSockets**, following the
user's choice. Those dependencies are not needed for this non-networked slice.
The event loop must keep socket ownership on its thread, offload blocking
storage/financial work, enforce bounded streaming/backpressure, handle aborted
requests safely, and defer responses until durable receipt. See
[the native architecture decision](../../docs/native-ingestion.md).
