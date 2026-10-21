# Durable staged-ingestion reference host

The local Python API/CLI receives opaque bytes, retains original evidence and
acknowledges durable receipt/work intent before transaction parsing. Separate
leased workers parse strict JSONL, retain per-record validation errors/status and
automatically admit valid dependency-closed groups to a durable canonical ledger.
Malformed records stay inspectable while unrelated valid records can advance.
The caller can poll durable status/events without keeping a request open.
API envelopes, authorization scope and advertised resource limits remain checked.

The C++ financial libraries and arithmetic have no storage dependency. The
optional `luca-ingestion-engine` tool uses the pure `luca::adapters` transaction
adapter and existing lifecycle rules through a small versioned process protocol.
Python 3.10+ with `sqlite3` and POSIX `fcntl`, local filesystem locking, atomic rename
and directory/file sync are required for this optional source-tree host. There is
no pip install, external database service or network listener. Windows, network
filesystems, replicated durability and production HTTP authentication are outside
this host profile. The caller must authorize the tenant/book before constructing
the store and keep its data directory private.

## Try a receipt

Run from the repository root. `example-config.json` uses illustrative context
version names. Register the immutable reference snapshot and start a worker as
shown below to execute validation and admission.

```sh
export PYTHONPATH="$PWD/apps/ingestion-host"
python3 -m luca_ingestion --root /tmp/luca-intake-example \
  --config apps/ingestion-host/example-config.json capabilities
```

Create a metadata file with this content, and a separate payload file containing
the exact original bytes to retain:

```json
{
  "source_id": "custodian-a",
  "format": "luca.transactions.jsonl/1",
  "validation_context_id": "cash-equity-v1-ref42",
  "atomicity": "independent_groups"
}
```

```sh
python3 -m luca_ingestion --root /tmp/luca-intake-example \
  --config apps/ingestion-host/example-config.json receive \
  --metadata /path/to/metadata.json --payload /path/to/original.jsonl --key batch-001
```

The result contains an immutable receipt, import ID, payload SHA-256/size and
durability profile. Its status URL identifies the proposed HTTP resource; it is
not a running URL in this local-only slice. Repeating the same key/content/options
returns that receipt. Changing content/options under the key fails with
`IDEMPOTENCY_CONFLICT`. A new key creates a new submission; identical payloads are
not silently deduplicated into a financial transaction.

Use the returned ID with `status`, `events` and `payload` commands:

```sh
python3 -m luca_ingestion --root /tmp/luca-intake-example \
  --config apps/ingestion-host/example-config.json status imp-RETURNED-ID
python3 -m luca_ingestion --root /tmp/luca-intake-example \
  --config apps/ingestion-host/example-config.json work
python3 -m luca_ingestion --root /tmp/luca-intake-example \
  --config apps/ingestion-host/example-config.json payload imp-RETURNED-ID \
  --output /tmp/retained-evidence.bin
```

The output path must not exist. Payload reads can select `--offset`/`--length`.
`work` is an inspection interface, not a worker lease or permission to process
the same item concurrently. Workers claim the persisted unique `parse_import`
intent through fenced leases. An empty import becomes enumerated/complete only
after processing; it appends no events. Invalid records never enter the ledger.

## Local API and resumable uploads

```python
import io
from luca_ingestion import IntakeStore

# config is the parsed JSON configuration shown above; metadata is separate.
store = IntakeStore("/private/local/data", **config)
receipt = store.receive(metadata, "batch-001", io.BytesIO(original_bytes), len(original_bytes))
status = store.status(receipt["import_id"])
events = store.events(receipt["import_id"], after="0", limit=100)
```

Direct intake requires a known byte count and a finite binary stream; reads are
bounded to 1 MiB, including an EOF check. Evidence copy handles partial output
writes. Receipt methods perform blocking storage I/O; a future async HTTP adapter
must schedule them outside its event loop. There is no financial validation on
their critical path. `IntakeError` exposes a stable code and retryability; actual
filesystem errors can also raise `OSError`. A failed/absent response is ambiguous
after commitment, so resolve it with the same idempotency key.

For resumable transfer:

1. `create_upload(key, expected_bytes=..., sha256=..., chunk_bytes=...)` reserves
   capacity and returns an upload ID, expiry and progress. This is not an import
   receipt. Repeated creation is idempotent.
2. `put_chunk(upload_id, index, bytes, sha256)` durably stores chunks out of order.
   Equal retries succeed; conflicting bytes/checksums fail. The final chunk
   transaction queues unique `verify_upload` work and exposes `verifying`.
3. `verify_upload(upload_id)` performs that recoverable transfer-verification
   operation, checks the full digest, installs immutable evidence and marks
   `ready`. It can be retried after process death; a checksum failure remains
   visible as `failed` with its error code. Transfer verification remains explicit.
4. `receive_upload(metadata, key, upload_id)` attaches the ready payload and writes
   receipt/parse intent/status event atomically. Retry resolves the same receipt
   even after the upload is attached. No second import can attach that upload
   under a new request key.

The CLI equivalents are `upload-create`, `upload-put`, `upload-verify`,
`upload-status` and `receive-upload`; `--help` lists their bounded inputs. Local
verification remains an explicit transfer operation; the processing worker consumes
parse, validation and admission work after receipt.
Chunk writes within an upload serialize under a process-safe lock; separate
uploads and book shards can proceed independently.

## Storage, recovery and limits

Each tenant/book routes through a length-framed SHA-256 key into its own private
directory/database. Scope identifiers never become filesystem paths. Original
payloads have unique transfer names and are installed read-only after durable
flush/rename/directory flush. `SourceRecord` and financial libraries remain
persistence-neutral. Book/source identity and context metadata are retained;
source authorization is checked at intake. Context definitions and limits are
immutable once registered. `register_context` adds named definitions, never
replaces them; `register_reference` adds content-digested immutable snapshots.
Existing constructors may supply the original context subset and reopen the
extended registry. Changes to quotas or processing resource bounds require a
reviewed migration, rather than silently changing interpretation or limits.

The SQLite transaction writes receipt, scoped idempotency binding, context,
initial event and parse work intent together. Failed direct transfers release
quota only after deleting their unreferenced files. A competing same-key request
keeps one receipt and removes the losing reservation/evidence. Crash-left direct
reservations and expired unattached uploads are reclaimed through `recover`.
Process locks prevent recovery from deleting an active writer's data; a fixed
4,096-transfer-lock namespace bounds lock-file accumulation, plus one initialization
lock. Hash collisions in this lock
namespace only serialize unrelated work. Recovery pages take `after`/`limit` and
return `next_after`; follow it until null. Attached evidence never expires with
its original upload session.

WAL is enabled only with SQLite >=3.51.3 or known patched 3.50.7+/3.44.6+ release
lines. Older SQLite uses rollback journaling with `synchronous=EXTRA`, rather than
exposing concurrent connections to the documented
[WAL-reset bug](https://sqlite.org/wal.html#walreset). WAL uses `FULL`. The selected
profile is reported in capabilities/receipts and durably pinned before the first
database open. Reopening with a newer runtime preserves the selected mode;
reopening a WAL store with an unpatched runtime fails before opening SQLite.
There is no automatic live mode conversion. File syncs and SQLite full-sync settings are
enabled on macOS. See [SQLite durability settings](https://sqlite.org/pragma.html#pragma_synchronous).
Rollback mode can block writers/readers and is not a production scaling claim.

Default per-book limits: 256 MiB per payload, 4 GiB reserved/retained payload bytes,
1,024 pending imports/transfers, 4 MiB per chunk, 8,192 chunks, 1,000 items per page,
and 24 hours for unattached uploads. Reservations include incomplete transfers;
already receipted content can be retried when capacity is full. These are logical
payload/queue limits, not physical disk reservations or limits on metadata size.
Disk/full-sync failures prevent acknowledgement. Receipt/evidence/idempotency
history has no automatic purge; failed uploads retain their data until expiry and
recovery. Schema 1 stores upgrade transactionally to schema 2, preserving all
receipts, keys, bytes and pending work; older hosts cannot reopen schema 2. No
public evidence delete or automatic metadata-history purge is provided.

Durability covers the configured local storage failure domain. Process-kill tests
prove restart behavior, not physical power-loss, disk-loss or backup/restore
certification. Private-directory ownership/read-only files are not protection
against a privileged local writer. A production adapter needs its own fault,
access-control and recovery acceptance.

## Validation and admission

Build the optional C++ tool (`LUCA_BUILD_TOOLS=ON`, the top-level default) with a
C++23 compiler supporting the existing exact-value core. The host never spawns a
shell and requires an explicit engine executable. Library-only builds do not
require Python, SQLite or the tool.

```sh
cmake --build build/review-release --target luca-ingestion-engine
```

Create `/tmp/ref42.json` containing
`{"accounts":["portfolio-a"],"instruments":["ABC"]}` and register it before or
after receipt. A missing snapshot leaves records blocked and wakes when the
snapshot is registered.

```sh
python3 -m luca_ingestion --root /tmp/luca-intake-example \
  --config apps/ingestion-host/example-config.json register-reference ref42 \
  --definition /tmp/ref42.json
python3 -m luca_ingestion --root /tmp/luca-intake-example \
  --config apps/ingestion-host/example-config.json worker \
  --engine build/review-release/tools/ingestion-engine/luca-ingestion-engine --mode drain
```

`worker --mode once` processes at most one ready job; `drain` stops when none is
currently ready (future retries/blocked dependencies may remain); `watch` polls
until SIGINT/SIGTERM. Multiple workers/processes can share a book. Each worker
owns a persistent native engine, syncs missing canonical records and discards its
cache on an operational failure. Different books have separate databases and
admission locks. Independent parsing/validation can run concurrently; admission
serializes per book to preserve one authoritative sequence. SQLite still has one
metadata writer per book. There is no automatic process pool or multi-node lease
service, and adding workers does not imply linear speedup.

```python
from luca_ingestion.worker import Worker, WorkerLimits

store.register_reference("ref42", accounts=["portfolio-a"], instruments=["ABC"])
with Worker(store, "/absolute/path/to/luca-ingestion-engine") as worker:
    worker.run_once()       # claim/lease one durable ready job
    worker.drain()          # bounded local processing, no receipt mutation
records = store.records(receipt["import_id"], validation_state="invalid")
errors = store.diagnostics(receipt["import_id"], limit=100)
commits = store.commits(receipt["import_id"], limit=100)
```

The supported adapter/context versions are `luca.transactions.jsonl/1`,
`source-book-identity/1`, `cash-equity/1`, `luca/0.1`. Required fields are
`record_id`, `economic_event_id`, `action`, `account_id`, and an event except for
`cancel`; `correct`/`cancel`/`reverse` require `causal_record_id`. Cash events use
`{type:"cash_movement", effective_at, currency, amount}`. Equity events use
`{type:"equity_trade", effective_at, instrument_id, quantity, price,
quote_currency, settlement_date}`. Financial numbers must be exact decimal
strings; timestamps are UTC with `Z` and at most nine fractional digits. The
native constructors reject excess precision, overflow, invalid dates and zero
trade quantities. Canonical text is NUL-free UTF-8 NFC; it is never normalized
silently. Original source labels are mapped by framed tenant/book/source hashes
into stable canonical record/economic IDs; evidence IDs instead identify this
specific import/ordinal. External/source timestamps stay in original evidence.
Duplicate JSON keys, invalid UTF-8/surrogates, unknown fields, non-finite JSON
numbers and malformed records become retained diagnostics.

Optional `atomic_group` seals explicit all-or-nothing membership. The host joins
same-import causal chains and duplicate identities into conservative groups,
topologically orders valid candidates, and checks the whole group with the
financial core. `whole_import` joins all records. An unparseable line alongside
an explicit group, or a malformed group label, fails enumeration because safe
membership cannot be established; nothing from that import is admitted. Group
membership does not change during revalidation. Missing predecessors are blocked
and wake after their record commits, using indexed dependency keys. Unknown
accounts/instruments require a new immutable reference snapshot/context and
explicit revalidation. Cross-account relationships, cycles, duplicate economic
origins and inexact reversals cannot bypass core acceptance.

Validation and commitment are separate: a record can remain `valid` while its
group is invalid because another member failed, or while admission becomes
`conflict` because another correction won. Completed validation runs and their
errors never change. Admission has its own immutable run and rechecks the current
ledger revision; a conflict does not rewrite the successful validation run.
Operational engine/storage errors produce retry attempts and `work_health`,
not fabricated business errors. Backoff is bounded and jittered; the default is
three attempts followed by `attention_required`. This reference has no public
operator retry/repair endpoint for exhausted operational work.

Default worker bounds are 256 records per parse batch, 100,000 records per import,
64 KiB per original record, 1,000 per admission group, and 1,000,000 ledger records
per book/cache; leases last 60 seconds. Processing resource bounds are pinned by
the first worker and disclosed in capabilities; incompatible later workers fail
before claiming work. Batch size, lease and retry settings remain worker-local.
Limits found after receipt produce retained import/group diagnostics or an
operational ledger-limit status. Completed or blocked imports release pending
queue slots, allowing prerequisites to arrive; original bytes still count toward
retention quota. Metadata/diagnostic/ledger disk usage has no separate quota, so
physical disk and process memory need host-level budgets in a production adapter.

## Read APIs, history and revalidation

`status` returns enumeration, validation counts, commitment counts, operational
health and one revision from a database snapshot. Counters update with state
deltas; this does not rebuild financial projections. `records`, `diagnostics`,
`validation-runs` and `commits` provide bounded snapshot pages; continuation tokens
expire after 15 minutes and must retain their original filters. Tokens are local
consistency cursors, not authentication credentials. Record pages capture state
and context at their import revision even if a worker advances later. Immutable
history and polling events have no automatic purge.

Local CLI commands include `records`, `record`, `group`, `diagnostics`,
`validation-runs`, `commits`, `work-attempts` and `ledger-state`. Python reads also
accept the documented filters; `ledger_records(after_sequence=..., limit=...)`
returns canonical bytes and per-record hashes. These local methods are not yet
a complete HTTP implementation: authentication, ETags, byte-range HTTP responses,
OpenAPI and report/projection coverage are future adapters. Relative URLs name the
proposed resources; no listener is started. Local group/receipt objects use the
Python field names shown in this guide; the target HTTP representations and event
vocabulary still require an explicit gateway mapping. Admission receipts identify
sealed membership and the accepted sequence range; canonical hashes are retained
per ledger record rather than as an additional aggregate candidate hash.

For reference changes, call `register_context(id, definition)` with the same
adapter/normalization and a new snapshot ID, then
`revalidate(import_id, key, validation_context_id=..., groups=[{"group_id":...,
"expected_revision":str(group["revision"])}])`. This atomically fences old work,
queues a new run and returns an idempotent scheduling receipt. It rejects stale,
already committed or incompatible selections. `record(..., run_id=old_run)`
returns the earlier immutable outcome alongside current state. The CLI equivalents
are `register-context` and `revalidate --key ... --request /path/to/request.json`.
Committed events change only through new lifecycle submissions.

Admission holds a per-book OS lock and uses a lease generation plus group and
ledger revision checks. The pure core prepares owned records without mutation;
SQLite commits canonical bytes/hashes, ledger sequence/revision, commit receipt,
per-record status, event and work completion together. Only then does the native
cache publish. Death before commitment leaves no accepted group; death after it
recovers the exact committed records through strict canonical decode/acceptance.
Retries cannot duplicate a group or reserve different accepted sequences.
Preparation and persistence touch the new suffix and indexed causal targets;
cache recovery reads only missing records. Full projection/checkpoint evidence
and report coverage remain separate roadmap work.

## Verification

```sh
python3 -B -m unittest discover -s tests/ingestion -v
LUCA_INGESTION_ENGINE="$PWD/build/review-release/tools/ingestion-engine/luca-ingestion-engine" \
  python3 -B -m unittest discover -s tests/processing -v
python3 -B tests/benchmarks/processing_benchmark.py \
  --engine build/review-release/tools/ingestion-engine/luca-ingestion-engine \
  --records 1000 10000 100000 --group-size 256 --repetitions 3 \
  --output /tmp/luca-processing-benchmark.json
```

POSIX root CMake test builds register intake and processing checks. Tests use real
files, SQLite transactions, threads/processes, stale leases and SIGKILL at parse,
validation and admission boundaries. See
[executed verification and scoped performance results](../../docs/reviews/VALIDATION_ADMISSION_20261021.md).

The additive C++ `LifecycleLedger::prepare_batch` returns an owned,
non-serializable `PreparedLifecycleAppend`. `publish` accepts it only against the
same in-process ledger prefix (an exact value copy shares that prefix); any append
invalidates previous preparations. Independently replayed ledgers prepare their
own plans. `accept_batch` remains the strict convenience prepare/publish path.
The new standalone canonical-record decoder only decodes owned input; recovery
still enforces sequence continuity and lifecycle acceptance against the prefix.
