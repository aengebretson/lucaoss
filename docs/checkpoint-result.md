# Continued checkpoint state and evidence

Include `<luca/portfolio/checkpoint_result.hpp>` and link `luca::portfolio`.
`apply_checkpoint_suffix_with_manifest` accepts the same five inputs as
`apply_checkpoint_suffix` and returns an owned state plus refreshed evidence:

```cpp
std::expected<luca::CheckpointApplicationResult, luca::CheckpointApplicationError>
result = luca::apply_checkpoint_suffix_with_manifest(
    request, manifest, checkpoint_state, accepted_prefix, proposed_suffix);
if (result) {
  // result->state is PortfolioState; result->manifest is CheckpointManifest.
  // Construct the next CheckpointResumeRequest using this manifest's digest
  // and repeated identities/context/prefix/state digest.
}
```

The existing state-only API, compatibility decision, lifecycle semantics and
LCB1 wire schemas are unchanged. The result is an ordinary owned aggregate; it
introduces no new serialization schema. Serialize its state and manifest with
the existing canonical encoders. Applications retain the complete accepted
records separately to supply the next continuation. Neither storage nor I/O is
part of this function. See the [standalone consumer](../examples/checkpoint-continuation/README.md)
for a complete two-continuation example.

## Financial algebra and reporting projections

The portfolio projection is an ordered financial fold over lifecycle-selected
cash movements and equity trades. For a verified checkpoint of prefix `P` and
an ordinary compatible suffix `S`, the intended continuation law is:

`continue(project(P, C), S, C).state = project(P ++ S, C)`.

This law requires the identical evaluation context `C`, projection, engine,
policy and account partition; a nonempty contiguous suffix; no lifecycle edge
targeting prefix knowledge; and every suffix payload strictly after the prefix
resolved `(effective_at, acceptance_sequence)` watermark in that order. Equal
economic times are supported when the suffix has a later acceptance sequence.
Arithmetic must remain representable at every
ordered step. Checked intermediate overflow is an error even if a later payload
would offset it. The wrapper delegates all financial calculation to the existing
suffix application, with exact quantities, prices and currency-tagged money.
It introduces no new rounding or netting rules. Positions, settled cash and
receivable/payable settlement obligations retain their existing complete keys.
Reporting projections and their evidence remain derived values rather than
new authoritative events.

The operation is deterministic for identical inputs. It does not assert
commutativity, arbitrary regrouping, inverses, or unrestricted partition
independence. A correction replaces a selected head; cancellation removes it;
reversal retains its target and adds its own terminal economic event. Prefix
changes require full replay or an earlier compatible checkpoint. Advancing
knowledge, economic or settlement cutoffs remains incompatible.

Continuation now seeds the same checked accumulators used by ordinary and
lifecycle replay. It resolves the validated suffix directly, without rebuilding
suffix or per-event ledgers, and finishes sparse output once. Event valuation
errors still precede aggregate overflow checks; intermediate overflow cannot be
offset by a later event. Compatibility and full replay share lifecycle selection
and relationship rules, including linked dated voids/replacements before and at
their activation boundary. A dated suffix relationship remains subject to the
existing restriction against targeting checkpoint-prefix knowledge.

## Evidence construction

After the existing application succeeds, the helper reconstitutes the full
accepted record history using public lifecycle draft factories and batch
acceptance. That ledger is used only for canonical input hashing and lifecycle
resolution; the helper does not calculate a second portfolio state.

The result manifest is constructed with the existing validating factories:

- The event prefix spans all accepted records, beginning at sequence one, with
  the new count, final accepted record and full canonical input digest.
- Lifecycle lineage contains all record IDs in acceptance order, including
  superseded, cancelled and cutoff-excluded records. Source references cover
  that entire history, deduplicated in first-occurrence order (record order,
  then each record's declared provenance order).
- Active lineage comes from full lifecycle resolution under the original
  knowledge and economic cutoffs. Its order is economic time then acceptance
  sequence. Suffix-local corrections therefore do not leave stale active heads.
- The watermark identifies the last resolved active record, which can differ
  from the last accepted record. An inert or cutoff-excluded suffix can extend
  the accepted prefix while leaving state and watermark unchanged.
- The state digest hashes the actual returned state. Projection, engine,
  policy, partition, every context input and cutoff, serialization, digest
  algorithm and schema identities are preserved exactly.

The count is checked against unsigned 64-bit sequence/count limits and host
container capacity before addition/narrowing. v1 requires nonempty active
lineage and a real watermark. No placeholder watermark or wire extension is
invented if factory validation rejects the evidence. Under current ordinary
suffix restrictions, compatible prefix active records remain available.

## Errors and ownership

`CheckpointApplicationError` is a variant containing the five original
`CheckpointApplyError` alternatives (`CheckpointResumeError`, `LifecycleError`,
`PositionProjectionError`, `CashProjectionError`, `SettlementProjectionError`),
plus `CheckpointError` and `CheckpointResultConstructionError`. Original
application failures retain their error type, category and message. Rebuilding
full lifecycle evidence can also return the existing `LifecycleError`.

Factory failures are returned as the original `CheckpointError`.
`CheckpointResultConstructionError` owns a `category` and `message`:
`count_overflow` indicates an unrepresentable combined record count;
`canonical_encoding` indicates an existing canonical encoder rejected a typed
value. For example, core provenance currently permits repeated source IDs
within one record, whereas canonical v1 does not. Such a suffix can calculate
state through the state-only API but cannot produce a canonical evidence bundle.
The wrapper reports failure and publishes neither component.

Every input is const and remains unchanged on success or failure. Allocation
failure follows the standard containers' exception behavior, as in the existing
APIs; this is not a no-throw allocation boundary. Construction keeps full
history in memory and hashes it again; no performance improvement is claimed.

## Authored verification and limits

[Unit cases](../tests/unit/checkpoint_result_test.cpp) cover full replay,
repeatable bytes/digests, manifest encode/decode, multiple continuations,
suffix-local lifecycle changes, explicit context identities, source ordering,
economic watermark ordering (including equal-time ties), multi-account
partition preservation, cutoff-selected heads, conservative rejection,
intermediate overflow, construction errors and input immutability.
[Portable cases](../tests/conformance/checkpoint-result/README.md) declare
synthetic inputs and expected state/lineage independently of storage and hosts.

The root build registers the checkpoint unit suites. The dated replacement
regression consumes the portable columns in
`tests/conformance/checkpoint-dated-replacement/cases.tsv`: year, month, day,
hour (UTC), expected selected record ID and exact cash amount. It checks both
prefix- and suffix-local amendments against full replay before activation and at
the inclusive activation boundary. There are no new external dependencies,
financial schema changes or deployment requirements.
