# Durable intake reference host

This first staged-ingestion increment receives opaque bytes, retains original
evidence and durably queues parsing. It supplies a trusted local Python API and
CLI. Transaction parsing, validation workers/diagnostics, lifecycle admission and
HTTP are subsequent increments; status correctly remains pending/queued here.
An invalid JSON/CSV/binary transaction payload still receives a durable receipt.
API envelopes, authorization scope and advertised resource limits remain checked.

The C++ financial libraries, package targets and arithmetic have no new dependency.
Python 3.10+ with `sqlite3` and POSIX `fcntl`, local filesystem locking, atomic rename
and directory/file sync are required for this optional source-tree host. There is
no pip install, external database service or network listener. Windows, network
filesystems, replicated durability and production HTTP authentication are outside
this host profile. The caller must authorize the tenant/book before constructing
the store and keep its data directory private.

## Try a receipt

Run from the repository root. `example-config.json` uses illustrative context
version names; this slice retains those definitions but does not execute their
financial rules or load reference data.

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
the same item concurrently. The persisted unique `parse_import` intent is consumed
by the next validation increment. No empty/invalid payload is labeled validated,
committed or projected.

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
   visible as `failed` with its error code. There is no background worker yet.
4. `receive_upload(metadata, key, upload_id)` attaches the ready payload and writes
   receipt/parse intent/status event atomically. Retry resolves the same receipt
   even after the upload is attached. No second import can attach that upload
   under a new request key.

The CLI equivalents are `upload-create`, `upload-put`, `upload-verify`,
`upload-status` and `receive-upload`; `--help` lists their bounded inputs. Local
verification is an explicit operation rather than a new proposed HTTP endpoint.
Chunk writes within an upload serialize under a process-safe lock; separate
uploads and book shards can proceed independently.

## Storage, recovery and limits

Each tenant/book routes through a length-framed SHA-256 key into its own private
directory/database. Scope identifiers never become filesystem paths. Original
payloads have unique transfer names and are installed read-only after durable
flush/rename/directory flush. `SourceRecord` and financial libraries remain
persistence-neutral. Book/source identity and context metadata are retained;
source authorization is checked at intake. Context definitions and limits are
pinned at shard creation; changing them requires a future reviewed configuration
migration, rather than silently changing existing interpretation or quotas.

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
recovery. No public delete or schema migration is provided in this increment.

Durability covers the configured local storage failure domain. Process-kill tests
prove restart behavior, not physical power-loss, disk-loss or backup/restore
certification. Private-directory ownership/read-only files are not protection
against a privileged local writer. A production adapter needs its own fault,
access-control and recovery acceptance.

## Verification

```sh
python3 -B -m unittest discover -s tests/ingestion -v
python3 -B tests/benchmarks/intake_benchmark.py --records 10000 100000 1000000 \
  --repetitions 3 --output /tmp/luca-intake-benchmark.json
```

POSIX root CMake test builds register `luca_durable_intake_test`; library-only
consumers do not discover Python/SQLite because of this host. Tests use real files,
SQLite transactions, concurrent processes and `SIGKILL` boundaries. Published
fingerprint vectors live under `tests/ingestion/fixtures`. Timed benchmarks cover
durable intake only; validation, admission, reporting and network latency remain
unmeasured. See [recorded verification](../../docs/reviews/DURABLE_INTAKE_20261021.md).
