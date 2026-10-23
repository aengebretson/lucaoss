# Immutable transaction versions through shared application operations

This increment composes the bounded [reporting application](cli-mcp-reporting.md)
with the independently reviewed [immutable version core](immutable-transaction-versions.md)
from [PR 30](https://github.com/aengebretson/lucaoss/pull/30), originally
`9d4b78c07a842c796aaa1e46a82d6a5276daba0c`, with reviewed-owner follow-ups
`e65d495` and `22fd8e5` for activation metadata on all causal descendants. It keeps financial admission and temporal
rules in typed C++, native durable processing in C++, and CLI/stdio MCP as thin
adapters over the Enterprise-embeddable `SqliteApplication` operations. No period
P&L or conventional period posting activity is inferred by these operations.

The core dependency upgrades the optional native store to semantic schema 6 and
adds policy knowledge timestamps. Older binaries refuse that schema. Follow its
backup/compatibility contract before any operator upgrade; rollback requires the
coherent pre-upgrade store. This PR operates only disposable synthetic stores.

## Commands, tools and permissions

| C++ operation | CLI command | MCP tool |
| --- | --- | --- |
| `edit_transaction` | `luca transactions edit` | `luca_edit_transaction` |
| `delete_transaction` | `luca transactions delete` | `luca_delete_transaction` |
| `transaction_evidence` | `luca transactions evidence` | `luca_transaction_evidence` |

CLI uses `--profile FILE --request JSONFILE`; MCP keeps the established stdio
initialization and tool exchange. Exact self-contained input/output schemas are
in [the executable catalog](../contracts/mcp/foundations-v1.tools.json).

Mutations require the existing book submit grant, the original transaction's
source in `submit_sources`, and its account in the host's configured bindings.
The source ID and original account come from immutable accepted source evidence;
the caller cannot choose a different source/tenant/book authority. Evidence reads
require inspect. Read-only profiles omit edit/delete from tool discovery.
The operator may set `transaction_actor` in the profile (default `local-profile`,
1–128 UTF-8 bytes). This is trusted audit identity; no request accepts an actor,
role, profile path, filesystem path or authorization override. Static host grants
remain the authorization model; dynamic revocation is outside this increment.

## Mutation requests

Both operations require:

- `book_id`: authorized configured book selector.
- `idempotency_key`: 1–128-byte native import retry key, scoped to book/source.
- `original_record_id`: immutable **normalized** ledger record/version identity,
  as returned by `transactions query`. An economic ID is not a version ID.
- `validation_context_id`: operator-registered native context with adapter
  `luca.transactions.envelope.jsonl/1`; its existing source-book normalization,
  reference snapshot and financial rules are retained at durable receipt.
- `void_at`: explicit inclusive UTC void-effective datetime, nanosecond precision.
- `reason`: nonempty retained audit reason, bounded to 2,048 bytes at runtime.

An edit additionally requires `replacement_event_utf8`: at most 8,192 bytes of
native event JSON, without a lifecycle wrapper. This uses the existing cash/equity
adapter fields and exact decimal strings. Invalid JSON, excessive nesting or
duplicate fields are preserved as invalid event contents for native asynchronous
validation; no duplicate financial field is silently selected. Financial/type,
precision, natural-key and closed-period decisions remain in the native adapter
and typed engine. Example:

```json
{
  "book_id": "synthetic-book",
  "idempotency_key": "quantity-fix-001",
  "original_record_id": "event-<normalized identity from query>",
  "validation_context_id": "cash-equity-envelopes-v2",
  "void_at": "2026-01-20T12:00:00Z",
  "reason": "Correct synthetic executed quantity",
  "replacement_event_utf8": "{\"type\":\"equity_trade\",\"effective_at\":\"2026-01-10T12:00:00Z\",\"instrument_id\":\"ABC\",\"quote_currency\":\"USD\",\"quantity\":\"11\",\"price\":\"100\",\"settlement_date\":\"2026-01-12\",\"charges\":[]}"
}
```

The facade looks up the original admitted source labels, preserving the original
source namespace, economic identity and account. It deterministically derives new
source version labels from operation/book/source/key/original version. An edit
submits two source envelopes in **one whole-import atomic group**: a dated cancel
of the original, then a linked `correct` targeting that cancel. A deletion submits
an immutable dated cancellation/audit envelope. Source evidence retains operation,
trusted actor, reason, original canonical/source IDs and the entire command.
The aggregate generated payload is bounded to 64 KiB; an oversized command fails
before receipt with `LIMIT_EXCEEDED`.

The native worker admits an edit's two records atomically, with one system
recorded timestamp and distinct immutable version identities. It refuses a stale
head, incompatible type/account/instrument/currency, a replacement effective after
the void gate, invalid financial data and closed-period changes. An invalid
replacement publishes neither member. A deletion never physically removes source,
original record, prior void or replacement history. Generic legacy `correct`
imports retain their existing retrospective semantics; these edit tools always
use the dated paired contract.

## Receipt, completion, retries and errors

Success means **durable import receipt only**:
`durable_acceptance=true`, `completion=pending_or_in_progress`, with the native
`import_id`. It does not assert validation, admission or a completed projection.
CLI receipt does not start a worker. Use `worker run` or a processing-enabled MCP
host, then inspect `import status`, `import records` and `import errors`. Financial
rejections remain durable diagnostics; successful inspection of an invalid import
has exit 0. A missing original/version or denied scope can fail before receipt.

Identical command/key and trusted actor recover the same import after a lost
response, worker restart or completed processing. The actor/reason/context and
exact replacement-event text are part of the native payload fingerprint. Changed
content, whitespace in that text, actor or operation under the same key returns
`IDEMPOTENCY_CONFLICT`; it does not create a second version. These keys share the
existing source import namespace, so use distinct keys for unrelated imports.

Ambiguous receipt commitment returns `OUTCOME_UNKNOWN`, exit 9. Retry the identical
request and original host actor/configuration to recover the receipt, then inspect
its retained status. Do not choose a new key merely because a response was lost.
After a definite retained validation failure, a corrected command uses a new key;
its failed predecessor and diagnostics remain. Authorization is rechecked on every
call/retry. Existing structured envelopes, error fields and exit categories from
the [reporting guide](cli-mcp-reporting.md) apply unchanged.

## Query and source evidence

`query_transactions` uses the reviewed `transaction_versions` C++ helper. It adds
`predecessor_id`, `void_version_id`, `replacement_version_id`, `void_effective_at`,
`void_recorded_at` and `activation_effective_at`. These are distinct concepts:
original versions retain their own dated void, while a currently active
replacement has an activation gate and no own void until a later deletion/edit.
Known future activation is labelled `pending_activation`; `active` remains the
engine's selected payload at the independent economic/knowledge cutoffs. Existing
`void_at` remains the economic-cutoff-filtered compatibility field. Full known
version metadata can disclose an upcoming gate even while the original remains
active. A cancellation record's type/status remains `cancellation`. Legacy undated
voids leave their datetimes null while preserving `voided` status for the original.

Period filtering stays [from,to) on the record's economic datetime or explicit
void datetime; activation metadata is separate and does not silently change that
filter dimension. Before the gate, financial reports and active filters preserve
the original. At the gate they select the replacement, or no payload after a
pure deletion. Earlier knowledge excludes the pair. Full accepted history is
restored using `restore_record`, and every projection highwater must end at a
complete admission commit. A prefix splitting a pair fails
`INPUT_REVISION_SPLITS_COMMIT`; it cannot present half an edit as financial truth.

`transaction_evidence` takes `book_id` and normalized `record_id`. It returns the
original source transaction, import/source IDs and metadata, decimal-string byte
locator/line, SHA-256 and lossless `payload_utf8`. It reads at most one 64-KiB
accepted source line and checks its hash/length against the retained locator.
`authority=source_evidence_only` explicitly separates source assertions from
computed financial truth. For a version mutation, parse the returned envelope's
`source` to inspect operation, actor, reason and original command. Source bytes
cannot override canonical fields or write back to the ledger. Missing records
fail explicitly; no arbitrary SQL or filesystem read is exposed.

## Cash, policies and compatibility

A trade edit/delete does not alter immutable bank cash. Existing settlement
queries retain actual payments/refunds; the typed accounting core reports any
cash attached to a voided trade as suspense with a reconciliation break. A refund
is still an explicit separate supported cash event. Closed-period fences and
retained closed inputs remain effective across these commands.

Report manifests now include selected `policy_knowledge` and
`policy_selection=explicit_sequence`. Schema-6 policy history adds its native
knowledge metadata without changing resolved-policy evidence shapes. Migrated
legacy recorded timestamps remain null; reports preserve them rather than
inventing knowledge dates. The explicit `policy_through` remains mandatory and
independent of ledger/settlement recorded cutoffs. No automatic historical-policy
selection, period P&L, valuation inputs, derived period adjustments or financial
statements are advertised by this increment.
