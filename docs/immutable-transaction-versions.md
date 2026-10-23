# Immutable dated transaction versions

Accepted records are append-only. A replacement is one validated admission group:

1. A dated `cancel` targets the active original record.
2. An adjacent `correct` targets that cancel. Both retain the original economic
   identity, have different immutable record identities, and share a system
   recorded timestamp. The replacement payload's economic time cannot exceed
   the void-effective time. Its type, account, instrument and currency remain in
   the original natural key.

New `accept`/`prepare_batch` calls reject correction of an already-published cancel.
A malformed replacement rejects the whole group before mutation. The native
worker persists both records, evidence, commit and sequence change in one SQLite
transaction under its admission lock. Causal grouping joins the two envelopes;
explicit `atomic_group` is also supported. Native stable import keys recover the
same receipt after an uncertain response, reload or worker interruption. Failed
validation/admission remains in retained source, diagnostics and status history.
An accepted dated deletion is an immutable cancel/audit version whose source
records deletion, actor and reason; prior economic/source evidence is retained.
There is no physical transaction removal operation.

`resolve(recorded_through, economic_as_of)` uses independent inclusive nanosecond
cutoffs. Before the dated void, the original remains active, including when the
replacement is already known. At the void instant, the replacement becomes the
active payload, or deletion leaves no active payload. The gate propagates to
subsequent versions. Earlier knowledge excludes the entire pair. Legacy plain
corrections retain retrospective behavior; period recognition/adjustments are a
separate reporting contract, not a changed interpretation of accepted records.

`transaction_versions(ledger, recorded_through)` exposes immutable version links,
each original's own void-effective and void-recorded timestamps, and a distinct
replacement `activation_effective_at`. An active replacement does not have its
own void timestamp. Actors/reasons reside in immutable source evidence linked by
record provenance; the financial kernel does not invent user identities.

`restore_record(draft, accepted_sequence)` restores already-accepted canonical
history in exact sequence order. It is deliberately separate from admission so
replay can restore paired records sequentially without permitting standalone
new corrections of a terminal cancel. Hosts must restore complete admission
commit boundaries before exposing a projection. Existing canonical record v1/v2
bytes remain unchanged. Full canonical-ledger decoding uses this restoration API.

Closed periods reject both members when the void gate is in the closed interval.
A gate after a close is allowed because pre-gate resolution preserves the old
payload. Actual bank settlement is a separate immutable stream: it remains bound
to economic identity across replacement. Dated voids move surviving actual cash
to suspense; refunds remain explicit new cash events. Settlement admission/history
uses the same gate as lifecycle reporting, including before-void settlement after
a later-recorded replacement.

## Native compatibility and policy knowledge

Schema 6 is a semantic compatibility boundary. Older binaries reject it; it also
adds nullable `policy_revisions.recorded_ns`. New policy admissions record system
knowledge time, clamped against preceding policy, ledger and cash evidence.
`policy_history()` adds a separate `knowledge` array without changing existing
resolved-policy evidence bytes. Migrated policy timestamps remain unknown rather
than being fabricated from effective dates. Historical timestamp selection must
fail explicitly when policy knowledge is unavailable, or use an explicitly pinned
policy prefix from retained report context. Economic and knowledge cutoffs and all
ledger/policy/settlement highwaters are separate report inputs.

Upgrade only with a coherent backup of book and application evidence. Rollback
requires restoring the pre-upgrade backup; no downgrade/reopen operation exists.
This change does not operate any live database or policy.

## Verification

Native tests cover invalid pair atomicity, identical retry after reopen, terminal
cancel protection and retained diagnostics. Pure tests cover nanosecond void and
recording boundaries, distinct version metadata, both closed-period members,
separate-admission rejection and full canonical-ledger roundtrip. Application lots
replay after a native edit pair uses the shared restoration API. Policy tests
exercise schema5-to6 migration with unknown legacy timestamps and unchanged
pinned policy results. Existing settlement, crash/recovery and replay suites
remain applicable.
