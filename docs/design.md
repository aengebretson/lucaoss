# LUCA Design

Equity trade journals and actual settlement are implemented by the bounded
[trade accounting contract](trade-accounting.md). It supersedes forecast-date
cash for this reporting API, preserves independent lot selection and recognition,
and documents the regenerated-snapshot limitation for voids and period activity.


Versioned policy configuration now uses a pure C++ resolver and optional SQLite
revision/evidence storage. Entity defaults and custody/account overrides resolve
lot selection independently of recognition preferences. See
[policy configuration](policy-configuration.md) for inheritance, replay and close
fencing; LIFO is the only implemented lot method.

LUCA is a deterministic investment ledger and portfolio-state engine. It ingests heterogeneous source records, normalizes economically meaningful activity into a compact canonical event model, and derives reproducible portfolio state.

The design goal is not to build a distributed system first. The goal is to make the financial core **partitionable, deterministic, replayable, and side-effect-light** so the same computation can run efficiently in one process, a batch job, or many distributed workers.

## Architecture

```text
FIX ─────────┐
CDM ─────────┤
SWIFT ───────┤
Broker API ──┼──► LUCA adapters
Custodian ───┤          │
Blockchain ──┘          ▼
                    Source records
                          │
                          ▼
                   Economic events
                          │
                          ▼
                       Ledger
                          │
                          ▼
                    Projections
              ┌───────────┼───────────┐
              ▼           ▼           ▼
          Positions      Cash      Accounting
              │           │           │
              └───────────┼───────────┘
                          ▼
                    Portfolio state
                          │
                          ▼
                   Reconciliation
                          │
                          ▼
                External observations
```

FINOS CDM, FIX, SWIFT, broker files, custodian feeds, administrator files, and blockchain events are adapters. They are not the internal model.

## Core model

LUCA separates three layers of truth:

1. **Source record** — what an external or internal system reported. Preserve it unchanged.
2. **Canonical economic event** — LUCA's normalized interpretation of economically meaningful activity.
3. **Derived state** — positions, cash, settlement, lots, journals, P&L, and other state computed deterministically from events.

A source record is immutable evidence identity and audit context, not a payload
container: `SourceRecordId` is LUCA's evidence identity, `SourceId` identifies
the originating system or feed, and `external_record_id` is the optional
source-native identity. `PayloadHash` verifies payload identity, while
`source_reference` is an optional opaque location or debugging reference. Raw
bytes remain in adapter-managed or external storage. Provenance
links canonical data to one or more source-record identifiers and records the
name, version, and small optional audit metadata of the normalization that
produced it. Neither type defines storage or serialization behavior.

External balances and snapshots are **observations**, not automatically ledger truth. They are reconciled against derived state.

Position observations are immutable external claims keyed by account and
instrument. Position reconciliation compares them exactly with sparse projected
positions at one explicit, shared `as_of`: absent observations, observations
without projected keys (including zero-valued observations), and unequal
quantities produce missing, unexpected, and quantity-mismatch breaks. Breaks
retain observation provenance where external evidence exists. Reconciliation
never mutates the ledger.

Settled-cash observations are likewise immutable external claims rather than
ledger truth. Cash reconciliation compares exact amounts by account and currency
only when both the economic `as_of` and `settlement_as_of_date` match exactly.
Missing, unexpected, and amount-mismatched balances produce explicit breaks;
zero observations remain explicit even though projected cash is sparse. The
comparison retains external provenance where present and never mutates the ledger.

### Canonical economic events

The initial closed `EconomicEvent` sum contains cash movements and equity
trades. Each immutable value carries a common header: event identity, account,
`effective_at`, and provenance referencing (rather than embedding) source-record
evidence. `effective_at` is the economic time at which the event affects LUCA's
portfolio state; it is distinct from source observation and source-event times.
LUCA value objects validate their own invariants at construction; successfully
constructed nested values are trusted by higher-level domain types.

Cash amounts are signed (positive increases cash), and equity quantities are
signed (positive buys and negative sells). An equity trade keeps its
currency-neutral price beside an explicit quote currency. Its contractually
supplied settlement date has date granularity and is separate from the economic
timestamp, with no implied clock, timezone, or calendar calculation. Multiple
events may have the same `effective_at` or arrive out of order, so that timestamp
alone is not a total replay order; tie-breaking and lifecycle ordering remain
ledger concerns.

### In-memory ledger

The initial `Ledger` owns immutable canonical events in contiguous `LedgerEntry`
values and assigns a local acceptance sequence beginning at 1. Event IDs are
unique within a ledger: a duplicate is rejected without consuming a sequence or
mutating the original entry. Acceptance order remains sequence order, while
economic replay and half-open economic-time range queries order entries by
`effective_at`, then sequence. The sequence is only a deterministic tie-breaker;
it is not exchange, broker, business, or global market ordering. Out-of-order
economic times are valid. This first ledger is in-memory, persistence-neutral,
and not concurrently mutable; persistence and synchronization belong in later
wrappers rather than its financial semantics.
The ledger layer owns reusable economic ordering and economic-time selection for
explicit entry spans. Full replay, inclusive as-of selection, and half-open range
queries share the same `effective_at` then sequence ordering. Selection filters
acceptance-order entries before sorting only the matching subset; a dedicated
time index may be added later without changing query semantics. Projections
consume these ordered views and define only their state transitions.

### Position projection

Positions are sparse derived values replayed from explicit ledger entries, not
authoritative mutable ledger data. It consumes the ledger's inclusive as-of
economic view, which includes events whose effective time is equal to the
requested timestamp. Equity trades change the account/instrument
quantity at `effective_at`; cash movements have no position effect, and settlement
dates do not affect this projection. Output is ordered by account then instrument,
with exactly zero quantities omitted, and checked aggregation reports overflow.

### Settled-cash projection

Settled cash is sparse derived state keyed by account and currency. Its explicit
`CashProjectionContext` separates economic `as_of` from
`settlement_as_of_date`. A signed `CashMovement` changes settled cash at
`effective_at`. An equity trade changes
settled cash by `-(quantity × price)` only when it is economically effective and
its date-granular settlement date is at or before the independently supplied
settlement evaluation date. The projection does not turn dates into timestamps
or represent unsettled obligations. Exactly zero balances are omitted.

### Open-settlement-obligation projection

Open settlement obligations are derived state. Economically effective equity
buys create payables and sells create receivables until their settlement date is
reached. Each obligation exposes a positive money magnitude and an explicit
direction, aggregated by account, settlement date, currency, and direction;
receivables and payables are not automatically netted. Settlement clocks,
calendars, legal netting rules, and actual settlement processing remain deferred.

## Deterministic computation

Financial calculations should behave like pure transformations wherever practical:

```text
versioned input events
        +
projection rules
        ▼
deterministic output state
```

Avoid architectures where calculations repeatedly fetch and mutate database rows. Financial logic should receive the data it needs explicitly rather than performing hidden I/O.

This provides four important properties:

- reproducibility
- testability
- caching
- parallel and distributed execution

## Partitioning and scale

The normal scaling model is:

> **parallel across independent partitions, ordered replay within a partition.**

Natural partition keys may include:

- portfolio
- account
- legal entity
- strategy or book
- instrument
- currency

The correct key depends on the projection. Position calculations may partition very finely; cash, NAV, margin, or cross-instrument events may require broader aggregation.

A future distributed worker should not own business state. It should receive explicit versioned inputs, for example:

```text
Projection job
  portfolio/account partition
  projection type
  snapshot identifier
  event range or watermark
  projection version
```

and return a deterministic result.

## Ordering and causality

Event order is financially meaningful. Buys, sells, corrections, settlements, corporate actions, and reversals cannot be replayed arbitrarily.

LUCA must define stable ordering semantics including, where needed:

- economic/effective time
- processing time
- settlement time
- causal relationships
- correction/reversal lineage
- deterministic tie-breaking

The engine should parallelize only where those semantics permit it.

The first executable lifecycle design is specified in [Immutable event lifecycle
contract](event-lifecycle.md). It introduces separate immutable record and stable
economic-event identities, recorded/knowledge selection, unbranched causal
lineage, terminal exact-offset reversals, and correction, cancellation,
reversal, and supersession semantics. Knowledge-cutoff resolution derives a
complete causal path and head for every selected economic identity.
Lifecycle resolution precedes every affected projection, after which active
payloads retain the existing economic ordering and explicit settlement
evaluation rules. This is a reviewable design contract, not an implemented public
API; its supported two-cutoff evaluation is intentionally narrower than general
bitemporal querying.

## Incremental state and snapshots

Pure event replay from inception is correct but can become impractical at scale. LUCA should support both full replay and versioned state checkpoints.

Normal operation should be incremental:

```text
State(t-1)
+
new events
    ▼
State(t)
```

A snapshot should record enough metadata to be independently verified and rebuilt, such as:

- portfolio or partition key
- projection version
- input event watermark
- as-of time
- state hash

If projection logic changes, snapshots can be rebuilt from immutable events.

## Three execution modes

LUCA should support the same semantics across three modes.

### Incremental processing

Use a prior verified state plus newly arrived events. This is the normal operational path.

### Full replay

Recompute state from immutable event history for audit, corrections, policy changes, validation, or rebuilding snapshots.

### Analytical projection

Run P&L attribution, exposure, historical analysis, or other analytics over event/state data without coupling those calculations to transactional persistence.

## Batch and columnar processing

The engine should avoid record-by-record ORM-style access in hot paths. Large event sets should be processable in batches using compact memory layouts.

Arrow and Parquet belong in the ecosystem because they can provide efficient in-memory and durable representations, but they are adapters/storage formats rather than LUCA's financial semantics.

## Accounting and reconciliation

Accounting should be a projection over economic events. Individual events can often generate journals independently, while balances are reductions over those entries.

Reconciliation should be generic over projections and observations rather than implemented as unrelated trade, position, cash, and NAV applications.

Conceptually:

```text
reconcile(
  projection(events),
  observation,
  matching_policy
)
```

Reconciliation should support comparison at different aggregation levels and drill down from an aggregate break to the first divergent events or assumptions.

## Performance principles

1. **No hidden I/O inside financial calculations.**
2. **Immutable, versioned inputs.**
3. **Explicit partition keys and ordering semantics.**
4. **Incremental processing with deterministic full replay.**
5. **Versioned snapshots/checkpoints.**
6. **Batch-oriented processing for large datasets.**
7. **Storage is outside the financial semantics.**
8. **Distribution wraps the core; it does not live inside it.**
9. **One financial implementation across local, batch, and distributed execution.**
10. **Measure performance with reproducible benchmarks and conformance tests.**

## What not to build yet

The open-source core should not require Kafka, Kubernetes, a distributed database, service mesh, or remote RPC between ledger components. Those may become deployment choices later.

A fast, deterministic single-process C++ engine is the best foundation for distributed execution because the distributed layer can simply schedule independent instances of the same computation.

## Performance roadmap

As the domain grows, publish reproducible benchmark scenarios measuring:

- event ingest throughput
- replay throughput
- incremental update latency
- reconciliation throughput
- snapshot load/write time
- memory per event/state unit
- parallel scaling efficiency

The long-term performance objective is simple:

> **financial semantics rigorous enough for accounting, implemented like a modern compute engine.**


## Financial algebras and reporting projections (O5)

LUCA uses financial algebras to describe typed operations and their valid laws, and named, versioned projections to derive financial and reporting views from immutable evidence. The public `FinancialAlgebraDescriptor` records operation kind, ports, ordering, partitions, policy/version and qualified law declarations. It validates declaration structure; it does not prove laws, run arbitrary computations, or grant optimization permission.

Concrete implementations include lifecycle resolution, portfolio projections, exact-cash reductions/comparisons, trade-date and settlement-date fixture journals, and exact trade comparison. The [custom reporting example](../examples/financial-algebra-report/README.md) reuses the existing financial operations while retaining projected and observed evidence separately. Typed generic composition and runtime policy loading remain future work. Distinguish stateless mappings from ordered state transitions and reducible summaries; composition requires compatible domain types, units and context. See [the public algebra contract](financial-algebras.md).

Document each transform's actual algebraic properties. Associative reduction may permit partition merging on its declared domain, but commutativity and invertibility are independent claims. Checked finite-width arithmetic also requires representable intermediate results for every permitted grouping; arbitrary regrouping may otherwise change whether overflow occurs. Lot allocation and causal corrections retain ordering. Reversing an economic event does not imply every derived computation has an inverse. Conformance fixtures must prove supported full, incremental and partitioned execution agree and demonstrate cases where reordering is invalid.

Engine, policy, context and input identities accompany canonical results; operational job IDs/timestamps are separate. Hosting layers supply storage, network access, authorization and scheduling. An external tool or hosted app uses the same OSS arithmetic. Custom financial output is not accepted into authoritative platform state solely because an agent or external program supplied it. See [roadmap clarification](roadmap.md#backlog-clarification--transformations-and-portable-accounting-2026-09-23) for scope and sequencing.


## Additive public interfaces

- [Durable staged ingestion](staged-ingestion.md) separates immutable
  durable receipt, versioned asynchronous validation, strict ledger admission
  and projection coverage. Its [proposed API](staged-ingestion-api.md) specifies
  retained diagnostics/status, resumable uploads, scoped idempotency and polling.
  The optional host owns storage/work intent and atomic canonical append/commit
  receipts; the financial core retains its synchronous validation semantics and
  has no new I/O dependency. The
  [native C++ reference runtime](../apps/ingestion-host/README.md) implements
  durable receipt, resumable transfer, leased asynchronous validation, immutable
  per-record diagnostics/outcomes and atomic ledger admission. The optional
  `luca::ingestion_sqlite` component owns SQLite/filesystem I/O and native C++
  workers; a bounded receipt executor exposes futures. It calls typed financial
  APIs directly, with no Python interpreter or subprocess bridge. Canonical
  records, sequence/revision, receipt, status, event and work completion commit
  together before cache publication. Pure prepared suffixes use indexed causal
  targets and per-record canonical evidence, without rebuilding the whole prefix
  for each append. Projection/checkpoint preparation is unchanged. Component
  exports keep SQLite/JSON/thread dependencies out of financial-only consumers.
  Patched SQLite uses WAL/FULL; older runtimes use DELETE/EXTRA with pinned profiles.
  The [integration direction](native-ingestion.md#integration-direction) defaults
  to direct C++ linking. CLI tools and an optional MCP adapter share application
  operations; report jobs/pages/evidence and the MCP server remain planned.
  OSS has no graphical UI; enterprise owns presentation and user workflows.
  Draft contracts are in the [shared application API](application-api.md),
  [CLI specification](cli.md) and [MCP catalog](../contracts/mcp/v1.tools.json);
  they add no installed runtime interface yet.
  No HTTP framework is selected. Projection coverage and distributed
  storage/performance acceptance remain separate work.

- [Journal balances](journal-balances.md) reduce whole journals under an explicit
  book/revision/policy/context contract and merge disjoint compatible partials.
  The opt-in `luca::accounting` target depends only on `luca::ledger`; existing
  journal headers and umbrella dependencies stay compatible. Debit and credit
  turnover accumulate separately, with sorted owned evidence and per-currency
  trial-balance controls. Parallel merge laws apply only within the documented
  monotone gross-total range; book assignment and input completeness are explicit
  caller responsibilities, not inferred from chart-account names.
- [Checkpoint continuation](checkpoint-result.md) returns state plus a refreshed canonical manifest under the existing conservative compatibility rules. Changed cutoffs or corrections targeting the old prefix still require full replay.
- [Settlement-date journals](settlement-date-projection.md) implement the narrow USD fixture policy alongside the existing trade-date projection. These are financial projection foundations, not production GAAP/IFRS/tax coverage.
- [Trade reconciliation](trade-reconciliation.md) compares explicitly mapped trade identities with separate immutable observations. The bounded CSV adapter is available through opt-in `luca::adapters`; core targets keep their existing dependency graph.

The library's core functions have no hidden database or network access. Configuring or compiling these interfaces does not establish runtime financial correctness; executed conformance and integration evidence must be recorded separately.

## Custodian lots and charges implementation

C++ LIFO long/short lot relief, separate exact trade charges, economic P&L and firm aggregation are implemented in the [lot accounting contract](lots-and-charges.md). Native `cash-equity/2` ingestion retains charge errors and canonical v2 amounts. Next: general-ledger sale/short recognition, broker schedule selection/administration, affected-pool lot checkpoints and broader workload benchmarks. This implementation supersedes the earlier long-only/FIFO proposal for the first lot slice; no tax-compliance or deployment claim is implied.

## Dated lifecycle cancellation and optional period storage

Dated cancellation is an explicit temporal input to lifecycle resolution, with
backward-compatible undated v1 bytes and dated v2 encoding. Period protection is
a pure ledger admission predicate; receipt persistence, locking and accepted
sequence fencing remain in the optional native SQLite host. See
[dated cancellation and period closure](dated-void-periods.md) for semantics,
embedding boundaries and schema upgrade/rollback constraints.
