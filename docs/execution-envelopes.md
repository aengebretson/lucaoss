# Source-preserving transaction envelopes

Implemented adapter addition, 2026-10-22, on top of the native ingestion runtime.
Format ID: `luca.transactions.envelope.jsonl/1`. The existing
`luca.transactions.jsonl/1` format and canonical financial codec are unchanged.
There is no new storage schema or network service.

Each nonblank JSONL line is an object with exactly two members:

```json
{
  "transaction": {
    "record_id": "execution-1",
    "economic_event_id": "trade-1",
    "action": "originate",
    "account_id": "fund-account",
    "event": {
      "type": "equity_trade",
      "effective_at": "2026-10-21T15:00:00Z",
      "instrument_id": "AAPL",
      "quantity": "2",
      "price": "200",
      "quote_currency": "USD",
      "settlement_date": "2026-10-22"
    }
  },
  "source": {
    "schema": "example.execution/1",
    "message_representation": "application-captured source evidence",
    "external_execution_id": "execution-1",
    "mapping_version": "mapping-1"
  }
}
```

`transaction` uses the existing normalized cash/equity/lifecycle schema.
`source` is a nonempty evidence object; it has no authority to replace transaction
fields or bypass the financial constructors/reference/lifecycle checks. Exact
source bytes are the complete received envelope line, retained in the immutable
payload with byte/line/hash locators. The public normalized record view may
expose the inner transaction; original evidence is retrieved through the payload
locator. Diagnostic field paths are relative to the inner transaction, while
byte locators identify the complete envelope line. Duplicate fields, invalid
encoding, depth/size limits and malformed
source envelopes remain diagnostics. An ambiguous/malformed envelope prevents
unsafe group sealing/admission under the existing enumeration rules.

Context definitions select this adapter explicitly, with the existing
`source-book-identity/1`, `cash-equity/1`, `luca/0.1` and immutable reference snapshot.
There is no implicit reinterpretation of an old import or context. Evidence IDs
remain receipt-specific; economic/lifecycle identities remain source/book scoped.
A new submission with the same source record identity is not a new trade; strict
admission still rejects duplicates. Same-key/content receipt retries recover the
same import, while changed bytes/format/context under the same key conflict.

This lets an enterprise normalizer preserve the FIX callback, serialized FIX
message, qualified order/instrument snapshot and mapping provenance alongside its
canonical transaction request. FIX interpretation, broker/account mapping and
settlement calendars are explicit adapter concerns; this generic container does
not add a FIX session engine or change financial arithmetic. C++ processes both
the original and envelope formats through the same native worker/admission path.
Unsupported instrument economics remain retained invalid records and cannot be
made valid merely by attaching a source object.

The paired Luca Enterprise implementation is in `aengebretson/luca-platform`,
`services/order-manager/` and `docs/FIX_ACCOUNTING.md` in its review diff. It links
this optional native component directly; OSS still has no graphical UI. The
shared application API, broader CLI reporting and MCP server remain separate
contract/implementation work.

Native tests cover an originating record and causal correction in one envelope
import, retained invalid input, exact original-payload retrieval, receipt replay,
malformed source/group handling and no unintended canonical append. Existing
migration, crash, concurrency and financial-package checks remain applicable.
The installed native `luca-ingest` also retains explicitly supplied external
shared-library paths (`INSTALL_RPATH_USE_LINK_PATH`) so nonstandard macOS SQLite
installations can be located after installation. Dependencies are not vendored.

Enterprise source adapters use the public header-only
`luca::adapters::parse_transaction_date` and `parse_transaction_timestamp` helpers
for the same exact UTC/date interpretation as normalized transactions. They
return owned `TransactionDiagnostic` errors and introduce no JSON, storage or
network dependency. Callers do not depend on private `transaction_detail` helpers.
