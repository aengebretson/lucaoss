# Staged ingestion API — proposed v1 contract

Status: target HTTP contract, 2026-10-21; no HTTP listener is implemented. Read with [architecture and durability rules](staged-ingestion.md).
This is the normative human-readable wire specification for implementation
review. An OpenAPI document should be derived when signatures are accepted.

The first [local reference host](../apps/ingestion-host/README.md) implements the
receipt/transfer, record/status/error/history reads, revalidation and automatic
ledger admission through a Python API/CLI and optional C++ engine. HTTP endpoints
remain proposed; the local mappings and bounded differences are documented in
[the host guide](../apps/ingestion-host/README.md).

## Common rules

Base path: `/v1/books/{book_id}`. Tenant identity comes from authenticated request
context, never an arbitrary body field. Every import, upload, context, record,
group, run and event lookup enforces tenant/book scope. Source authorization is
checked before intake. IDs are opaque strings; links are relative to this base
unless explicitly absolute. No caller supplies ledger acceptance sequences.

JSON is UTF-8. Financial amounts, quantities and prices are decimal strings;
sequence numbers are decimal strings; revisions/cursors are opaque strings.
Timestamps use RFC 3339 UTC with up to nanosecond precision; settlement dates use
`YYYY-MM-DD`. IDs are not silently trimmed or case-folded. The adapter defines
exact field validation and uses existing core constructors. Reject excess
precision rather than silently rounding source input.

Every mutation requires `Idempotency-Key` (nonempty, at most 128 UTF-8 bytes).
Keys are scoped by tenant/book/source for import receipt and by
tenant/book/resource/operation for other mutations. Same key and operation
fingerprint returns the original result identity; different input returns `409`.
For upload chunks, `(upload_id, chunk_index)` itself is the idempotency identity.
Receipt keys remain bound for the receipt lifetime. Concurrent same-key requests
are serialized/reconciled; `202` is never returned for an uncommitted receipt.

Creation returns `Location`, a body containing the resource ID/status URL, and
`Cache-Control: no-store`. Status reads return a strong `ETag` for that
representation and support `If-None-Match`/`304`. Ordinary status reads are current
at one durable transaction snapshot, not a projection of an in-memory queue.
`202 Accepted` means durable asynchronous work, not successful financial
validation; see [HTTP 202](https://www.rfc-editor.org/rfc/rfc9110.html#name-202-accepted).

HTTP errors use `application/problem+json` with `type`, `title`, `status`,
`detail`, `instance` and extensions `code`, `request_id`, `retryable` following
[RFC 9457](https://www.rfc-editor.org/rfc/rfc9457.html). Stable `code`, not prose,
drives client behavior. Bad transaction data inside a receipted payload is exposed
through durable diagnostics, not a later HTTP error on the original load.

| HTTP response | Use |
| --- | --- |
| `400` | Invalid API envelope, unsupported option or malformed transport metadata. Payload transaction syntax is checked asynchronously. |
| `401` / `403` | Authentication/authorization failure before durable receipt. |
| `404` | Resource absent or not visible in this scope. |
| `409` | Idempotency conflict, frozen upload, stale revalidation selection, or incompatible immutable options. |
| `410` | Expired upload/page/event cursor; include a fresh status URL where available. |
| `413` | Published size/chunk/atomicity envelope limit exceeded. |
| `415` | Unsupported request transport type; a supported adapter can still find malformed transaction content asynchronously. |
| `429` / `503` | Capacity unavailable before receipt, with `Retry-After` when known. Retry the same key because response loss can make commitment ambiguous. |

No transport error asserts that a concurrently retried request could not have
created a receipt. Clients resolve uncertainty by idempotent retry. The server
must not return a successful receipt while depending on a volatile queue.

## Endpoint inventory

| Method and path | Request | Response |
| --- | --- | --- |
| `GET /ingestion-capabilities` | None | `200` supported format/context IDs, limits, retention and durability profile. |
| `POST /imports` | Multipart metadata + bytes, or JSON metadata + completed upload ID | `202 ImportReceipt`; retry returns same import ID and receipt. |
| `GET /imports/{import_id}` | Optional conditional header | `200 ImportStatus` or `304`. |
| `GET /imports/{import_id}/payload` | Optional byte `Range` | `200` original byte stream or `206` selected bytes; `416` for an invalid range. |
| `GET /imports/{import_id}/records` | Cursor, limit, optional validation/commitment/group/commit filters | `200 Page<StagedRecord>`. |
| `GET /imports/{import_id}/records/{record_id}` | Optional `run_id` | `200 StagedRecord`, including current/requested run and immutable links. |
| `GET /imports/{import_id}/groups/{group_id}` | None | `200 AdmissionGroup`, membership and commit receipt link. |
| `GET /imports/{import_id}/diagnostics` | Cursor, limit, optional record/group/run/code filter | `200 Page<Diagnostic>`; includes import-scoped parse errors. |
| `GET /imports/{import_id}/validation-runs` | Cursor, limit, optional record/group filter | `200 Page<ValidationRun>`, immutable history. |
| `POST /imports/{import_id}/revalidations` | Context ID plus bounded explicit group/revision selection | `202 RevalidationReceipt`; scheduling only. |
| `GET /imports/{import_id}/commits` | Cursor, limit | `200 Page<CommitReceipt>`. |
| `GET /imports/{import_id}/events` | `after` cursor, limit | `200 EventPage`, durable polling. |
| `POST /uploads` | Expected byte length, SHA-256, chunk size | `201 UploadSession`; incomplete upload, not an import receipt. |
| `PUT /uploads/{upload_id}/chunks/{chunk_index}` | Bytes plus `X-Chunk-SHA256` | `204` after chunk bytes/manifest are durable. |
| `GET /uploads/{upload_id}` | None | `200 UploadSession`, durable received ranges and expiry. |

No public endpoint changes an existing source payload, marks a record valid,
bypasses lifecycle acceptance, edits a committed event or deletes evidence. Valid
groups promote automatically. Explicit/manual promotion, cancellation, webhooks
and SSE are later extensions, not hidden requirements for this increment.

## Receipt and payload contract

`POST /imports` supports:

- `multipart/form-data`: exactly one `metadata` JSON part and one binary `payload`
  part. The payload part's exact bytes are retained; multipart boundaries and
  metadata are not part of the payload hash.
- `application/json`: metadata fields plus `upload_id`. The upload must be
  complete, checksum-verified, in this scope and not attached to another import.

Metadata fields:

| Field | Type / rule |
| --- | --- |
| `source_id` | Required authorized source identity. |
| `format` | Required versioned adapter ID; initially proposed `luca.transactions.jsonl/1`. |
| `validation_context_id` | Required immutable context ID from capabilities; pins supported adapter, normalization, rules, engine and reference snapshot. No implicit `latest`. |
| `atomicity` | `independent_groups` (default) or `whole_import`. |
| `client_reference` | Optional bounded opaque label; no financial identity semantics. |
| `replaces` | Optional `{import_id, record_ids}` linking corrected source submissions; no automatic cancellation of old accepted economics. |
| `upload_id` | Required only for JSON transport. |

Unknown metadata fields are `400`; host limits bound all strings/lists. Transaction
fields are not inspected on the receipt path. A known context whose reference
snapshot becomes temporarily unavailable creates blocked work asynchronously.
Unsupported context/format identifiers can be rejected from a local capability
registry before receipt. The import retains a copy/digest of its context definition
so registry changes cannot change its interpretation.
Limits discovered only after parsing, including an oversized atomic group, become
retained `GROUP_LIMIT_EXCEEDED` diagnostics and prevent that group's commitment.
They do not turn an already durable receipt into an HTTP rejection.

Request fingerprint version `submission/1` is SHA-256 over a length-delimited
encoding of tenant/book/source, format, context ID, atomicity, client reference,
replacement links and verified payload SHA-256/length. Optional fields have
explicit absent markers; replacement record IDs sort uniquely for hashing.
Strings are exact UTF-8; no metadata-object ordering or upload ID affects this
fingerprint. Publish exact fingerprint vectors with the implementation.

The intake implementation fixes this encoding: the ordered fields are version
`submission/1`, tenant, book, source, format, context ID, atomicity, optional client
reference, optional predecessor import ID, optional predecessor record-ID array,
payload SHA-256 and base-10 byte length. Each absent field is byte `00`. Each
present field is byte `01`, an unsigned 8-byte big-endian UTF-8 byte length, then
its UTF-8 bytes. The predecessor array is sorted/unique compact JSON with literal
UTF-8 (no ASCII escaping). Concatenate the fields and hash with SHA-256. Defaults
are materialized before hashing. Reusable vectors are in
[submission-fingerprints.json](../tests/ingestion/fixtures/submission-fingerprints.json).

```http
POST /v1/books/fund-a/imports
Idempotency-Key: custodian-2026-10-21-part-01
Content-Type: multipart/form-data; boundary=example

--example
Content-Disposition: form-data; name="metadata"
Content-Type: application/json

{"source_id":"custodian-a","format":"luca.transactions.jsonl/1","validation_context_id":"cash-equity-v1-ref42","atomicity":"independent_groups"}
--example
Content-Disposition: form-data; name="payload"; filename="transactions.jsonl"
Content-Type: application/octet-stream

<original transaction bytes>
--example--
```

```http
HTTP/1.1 202 Accepted
Location: /v1/books/fund-a/imports/imp-001
Cache-Control: no-store
Content-Type: application/json

{
  "import_id": "imp-001",
  "receipt": "durable",
  "received_at": "2026-10-21T15:00:00Z",
  "payload": {"algorithm": "sha256", "digest": "<64 lowercase hex characters>", "bytes": 48102},
  "durability_profile": "local-fsync-v1",
  "status_url": "/v1/books/fund-a/imports/imp-001"
}
```

`ImportReceipt` is immutable. It intentionally has no promised transaction count
or validation success. Its durability profile is declared in capabilities;
`local-fsync-v1` is a proposed profile name, not currently certified behavior.
The authenticated payload endpoint streams original bytes with a hash-derived
strong ETag and supports a single byte range for diagnostic drill-down. It never
returns reconstructed JSON or exposes a mutable storage URL. Access and storage
read failures are independent of historical receipt/validation truth.

### Initial transaction format

`luca.transactions.jsonl/1` is a proposed adapter schema, not the existing
canonical binary serialization. One UTF-8 JSON object per physical line; blank
lines are ignored but remain in payload offsets. JSON strings escape embedded
newlines. Adapter limits bound line length. Source ordinals count nonblank lines
starting at 1, including invalid lines. Preserve exact byte ranges, line numbers
and hashes; do not reconstruct evidence from parsed JSON.

Each object has `record_id`, `economic_event_id`, `action`, `account_id`, optional
`external_record_id`, optional source `source_event_at`, optional `atomic_group`,
and the following action fields. Missing/unknown/duplicate JSON keys are retained
validation errors. IDs supplied by the source are mapped through a versioned
source/book namespace before creating core identities; raw IDs remain evidence.
`causal_record_id` refers to that same source/book namespace in this first adapter;
cross-source identity reconciliation needs an explicit later mapping contract.

| Action | Additional fields |
| --- | --- |
| `originate` | `event`, describing a supported cash movement or equity trade. |
| `correct` | `causal_record_id`, replacement `event`; same economic identity and compatible account as predecessor. |
| `cancel` | `causal_record_id`; no `event`; same economic identity/account as predecessor. |
| `reverse` | `causal_record_id`, exact-offset `event`; a new economic identity as required by core lifecycle rules. |

`event.type=cash_movement`: required `effective_at`, `currency`, `amount`.
`event.type=equity_trade`: required `effective_at`, `instrument_id`, `quantity`,
`price`, `quote_currency`, `settlement_date`. Sign semantics and all financial
invariants are the existing core's. `Money` permits at most 6 decimal places,
`Quantity` and `Price` at most 8. No scientific notation, NaN, implicit FX or
silent rounding. Unsupported domains receive `UNSUPPORTED_EVENT_TYPE`.
Any financial policy that needs further fields requires a new adapter version.

Receipt time supplies evidence observation time; source time is retained
separately. Host admission supplies core recorded time and sequence. A transaction
cannot set its own recorded time to backdate when the host learned it.

```json
{"record_id":"cash-1","economic_event_id":"deposit-1","action":"originate","account_id":"portfolio-a","external_record_id":"bank-8842","event":{"type":"cash_movement","effective_at":"2026-10-20T12:00:00Z","currency":"USD","amount":"1000.000000"}}
```

### Resumable uploads

`POST /uploads` requires `{expected_bytes, sha256, chunk_bytes}` within advertised
limits. The response includes `upload_id`, `expires_at`, received chunk ranges
and chunk URLs. Expiry/cleanup must not delete bytes referenced by an import.
The maximum chunk count bounds the range response size.

Chunk indexes start at zero. Chunk sizes must match the declared layout except
the final remainder. Repeating an index with identical bytes returns `204`;
different bytes return `409`. A chunk is acknowledged only after its bytes and
manifest entry are durable. Concurrent chunk writes may proceed independently.

After all chunks, creating an import verifies the full digest, freezes that upload
and atomically binds the receipt. Assembly/checksum verification may happen during
upload processing; clients see `verifying` until ready, and import creation returns
`409 UPLOAD_NOT_READY` with retry guidance. It must never return `202` merely
because verification was queued. Sessions expose `uploading`, `verifying`,
`ready`, `attached`, `failed` or `expired`; bad full checksums are upload failures,
not transaction validation failures. A retry of the import creation resolves its
existing idempotency binding before checking whether the upload is attached.

## Durable status and record schemas

`ImportStatus` contains immutable metadata/receipt, `revision`, `enumeration`,
`work_health`, progress counters, required validation context, group/record page
links and optional projection coverage. Counters come from one committed snapshot.

```json
{
  "import_id": "imp-001",
  "revision": "r17",
  "enumeration": {"state":"complete", "records":1000},
  "validation_counts": {"pending":0,"running":0,"valid":980,"invalid":15,"blocked":5},
  "commitment_record_counts": {"not_committed":20,"queued":0,"committed":980,"conflict":0},
  "work_health": "idle",
  "all_records_committed": false,
  "commits_url": "/v1/books/fund-a/imports/imp-001/commits",
  "diagnostics_url": "/v1/books/fund-a/imports/imp-001/diagnostics"
}
```

Validation counts are record counts under current runs and sum to enumerated
records. Commitment counts are record counts derived from group outcomes and sum
independently; do not add the two axes together. Before enumeration completes,
total is null and counts describe only records already enumerated. Import-scoped
errors are counted separately from rows. Whole-import membership with an unknown
parse boundary cannot report complete coverage. Idle means no runnable work,
including an import with invalid/blocked records; it does not mean success.
`all_records_committed` is true only after successful complete enumeration, zero
unresolved import errors and commitment of every required group. Empty imports
have no ledger commit and explicit zero-record coverage.
Record-list filters are named `validation_state`, `commitment_state`, `group_id`
and `commit_id`; diagnostic filters are `record_id`, `group_id`, `run_id` and `code`.
Filters select rows within the captured snapshot; they do not mutate status.

`StagedRecord` fields:

| Field | Meaning |
| --- | --- |
| `record_id`, `ordinal`, `source_record_id` | Stable staging identity, source location order and core evidence identity. A staging ID is distinct from source/canonical record IDs. |
| `locator` | `{byte_offset, byte_length, line, sha256}` against immutable payload; null record locator only for import-scoped failures. |
| `canonical_record_id`, `economic_event_id`, `group_id` | Present only when determinable; group remains null until sealed. |
| `validation` | `{state, run_id, context_id, ledger_revision_examined}`; fields may be null before a run exists. |
| `commitment` | `{state, commit_id}`; commit ID only after durable acceptance. |
| `diagnostic_ids`, `dependencies` | Bounded inline summaries plus paginated links; never a potentially unbounded nested result. |
| `replaces`, `replaced_by` | Submission links; do not alter the original bytes or historical status. |

`AdmissionGroup` includes ID, sealed member IDs/hash (paged if large), atomicity,
group revision, dependency keys, current run, commitment/work state and commit
link. A good record can be `valid/not_committed` because another group member is
invalid. Stable group membership cannot change after sealing.

`ValidationRun` includes run ID, group/record IDs, context definition/digest,
reference snapshot ID, ledger revision examined, start/finish times, deterministic
outcomes and diagnostic links. Operational attempt logs are separate: a process
crash is not a completed invalid validation run.

## Diagnostics, revalidation and corrections

`Diagnostic` required fields are `diagnostic_id`, `scope` (`import`, `record`,
`group`), `code`, `severity` (`error`, `warning`, `info`), `message`, `run_id`
(nullable for enumeration), `record_id`/`group_id` when known, `field_path`
(JSON Pointer when applicable), `locator`, `related_ids`, `classification`
(`invalid`, `dependency`, `conflict`) and `retryable`. Structured `details` must
be bounded. General worker/storage failures use work-health/attempt diagnostics
separately and do not manufacture financial-invalid outcomes.

Initial stable code vocabulary includes `PARSE_ERROR`, `FIELD_REQUIRED`,
`UNSUPPORTED_EVENT_TYPE`, `INVALID_DECIMAL`, `EXCESS_PRECISION`, `VALUE_OVERFLOW`,
`UNKNOWN_ACCOUNT`, `UNKNOWN_INSTRUMENT`, `REFERENCE_SNAPSHOT_UNAVAILABLE`,
`MISSING_PREDECESSOR`, `DUPLICATE_IDENTITY`, `CAUSAL_CYCLE`,
`CONFLICTING_SUCCESSOR`, `INCOMPATIBLE_ACCOUNT`, `INVALID_REVERSAL`,
`GROUP_MEMBERSHIP_UNKNOWN`, `CROSS_BOOK_DEPENDENCY` and `ADMISSION_CONFLICT`.
`GROUP_LIMIT_EXCEEDED` identifies a parsed atomic group above host limits.
Map to exact existing core diagnostic categories with reusable conformance cases;
do not collapse every lifecycle error into an unstructured string.

```json
{
  "diagnostic_id":"diag-27", "scope":"record", "record_id":"staged-27",
  "group_id":"group-27", "run_id":"run-2", "code":"INVALID_DECIMAL",
  "severity":"error", "message":"Amount must be an exact decimal string.",
  "field_path":"/event/amount", "locator":{"line":27,"byte_offset":6400,"byte_length":211,"sha256":"<record hash>"},
  "related_ids":[], "classification":"invalid", "retryable":false, "details":{}
}
```

`POST /imports/{id}/revalidations` accepts:

```json
{"validation_context_id":"cash-equity-v1-ref43","groups":[{"group_id":"group-9","expected_revision":"g4"}]}
```

Select entire sealed uncommitted groups, bounded by the published limit. The
metadata transaction atomically records this selection, supersedes older runnable
attempts, creates durable work and returns `{revalidation_id, import_id, status_url}`.
Stale group revisions or committed groups return `409` with no command scheduled.
The context must retain the same adapter/normalization/identity/grouping semantics;
reference/rule updates may revalidate but cannot mutate sealed membership. Changes
to those immutable semantics require a linked new submission. A commit racing
revalidation is serialized by these same checks: one wins; no later result can
undo an accepted commit. Revalidation never deletes an earlier error/run.

Dependency arrival can schedule equivalent new runs automatically; events identify
the triggering dependency and selected snapshot. Deterministic invalid input is
not retried endlessly. Correct its bytes by creating a linked import. If an earlier
record already committed, the new payload must express a valid core correction,
cancellation or reversal. `replaces` alone has no financial effect. Never merge
staging rows by mutable external labels or overwrite an accepted origin.

## Commit, pagination and notification schemas

`CommitReceipt` is immutable and contains `commit_id`, `import_id`, `group_id`,
sealed membership/candidate hashes, validation run/context, book/partition identity,
prior and resulting ledger revision, accepted canonical record IDs/sequences,
assigned recorded times and source evidence links. Large record lists use paged
record detail links filtered by commit ID. A receipt is proof of host acceptance
under its durable store
contract, not a new canonical evidence digest or proof of external completeness.

All snapshot list endpoints use `limit` (default 100, maximum from capabilities)
and opaque `cursor`; `Page<T>` is `{items, snapshot_revision, next_cursor}`. First-page filters
are bound into the cursor, as is the selected import revision. Pages retain a
consistent snapshot while workers advance; the host may use status history or
materialized pages rather than holding a database reader transaction open.
Expired cursors return `410`, never silently restart or skip results. Capabilities
declare snapshot/event cursor retention. Diagnostic history is retained even if a
short-lived page cursor expires. Record lookup with a prior run ID can recover
historical outcomes without depending on that cursor.

Event polling uses `after` instead of a snapshot cursor and returns
`EventPage = {items, next_after, high_watermark}`. `next_after` advances only through
returned events, so a limited page cannot skip undelivered events. Empty responses
retain a valid resume cursor. Import status includes a cursor for its captured
revision, letting consumers take a snapshot then resume without a gap.
`StatusEvent` is
`{event_id, import_id, revision, type, occurred_at, subject_ids, links}`; payloads
contain IDs and bounded summaries, not full transaction bytes. Types include
`import.received`, `import.enumerated`, `validation.completed`,
`validation.blocked`, `work.attention_required`, `group.committed`,
`group.conflicted` and `projection.advanced`. Changes covering multiple records
may emit one batch event. Per-import revisions increase transactionally; consumers
deduplicate event IDs. There is no global event order or exactly-once delivery.
When a cursor expires, `410` supplies a status URL to take a fresh snapshot and
resume from its event cursor. Lost notifications cannot lose underlying status.

Capabilities must publish actual byte/record/group/chunk/page/work-queue limits,
supported contexts, retention policy, cursor lifetimes and durability failure
domain before the host is usable. Initial numeric limits and throughput/latency
budgets are implementation acceptance inputs to calibrate on representative
hardware; this proposal does not invent measurements or advertise an unbounded API.
