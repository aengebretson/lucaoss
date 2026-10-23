# Dated cancellation and period closure

`LifecycleRecordDraft::cancel` accepts optional `cancellation_at`. Existing
undated cancellation retains its historical semantics and canonical v1 bytes.
Dated cancellation serializes as `luca.lifecycle-record.v2`, with one additional
canonical `cancellation_at` field; decoders accept both forms. The date cannot
precede the targeted record's effective time. Provenance and immutable source
relationships are retained.

`LifecycleLedger::resolve(recorded_through, economic_as_of)` uses inclusive
cutoffs. It excludes a dated cancellation until the economic cutoff reaches the
cancellation date. The prior event is then no longer recognized. This removes
its contribution from replay at/after that date; it does not create an offsetting
cash payment or a reversing journal entry. Historical knowledge before the
cancellation was recorded continues to resolve the prior event.

The pure `changes_closed_period(candidate, target, closed_through)` admission
predicate lives in `libs/ledger`. It rejects backdated events, retrospective
corrections/undated cancellations of closed events, and dated cancellations
within the closed interval. A cancellation effective after the interval is
allowed. Custodian lot projections replay the resulting event set; no special
lot mutation is performed by the adapter or UI.

The transaction envelope accepts optional top-level `void_at` for `cancel` only.
Invalid date/type or use on another action retains validation diagnostics. The
adapter maps the field to the core date; other financial rules remain unchanged.

## Optional native SQLite host

`SqliteStore::periods()` returns the close boundary, accepted sequence and latest
1,000 receipts. `close_period(request_id, through, expected_sequence, actor,
allow_new=true)` durably records an increasing inclusive UTC close boundary.
`allow_new` is a trusted host fence for pending upstream work: an exact persisted
retry still succeeds when false, while a new close fails. Never expose this flag
as a client authority override.

Close and admission share the admission file lock and SQLite write transaction.
New closes reject future boundaries, stale accepted sequences and unfinished
kernel work. The receipt retains reviewed accepted sequence, actor, boundary,
recorded knowledge and excluded-record count. Pending upstream work must be
fenced by the embedding application; the store cannot see an external outbox.
The admission worker rechecks the close rule under the lock and retains
`PERIOD_CLOSED` diagnostics and commitment conflict rather than discarding source.

A closed report is reconstructed using both the close boundary and accepted
sequence. Core projections have no clock, storage, authorization or global state.
The embedding application authorizes operators and selects report cutoffs.
CLI/MCP close commands are not added by this slice; use the C++ store API.

Schema 3 adds immutable period-close receipts. Opening an existing schema-2 book
upgrades it transactionally. Older native binaries reject schema 3: coordinated
rollback requires restoring a pre-upgrade backup of book/journal/configuration.
There is no downgrade or period reopen operation.

`tests/conformance/dated-void/queries.tsv` supplies portable expected economic and
recorded-time results. Unit tests cover exact replay, future/due checkpoint
prefixes and suffixes; native tests cover close fences, retained conflicts,
retries and restart. Financial statements and cash/accrual policy are unchanged.
