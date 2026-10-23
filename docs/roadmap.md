# LUCA Roadmap

Implemented bounded slice: [CLI and stdio MCP foundations](cli-mcp-foundations.md).
It exposes existing ingestion/status/errors, policy inspection, canonical
transactions and economic LIFO lots through the optional SQLite application facade.
The broader storage-independent API, report jobs and unfinished accounting
operations described below remain proposals; use `foundations-v1.tools.json`
for the executable MCP catalog.

Policy configuration boundary: implemented versioned entity defaults,
custodian/account overrides, LIFO-only execution, independent recognition
preferences and retained resolved evidence. See [contract](policy-configuration.md).
Full cash/accrual journals, alternate lot methods and migration of closed legacy
books without policy history remain separate work.

LUCA should grow outward from a small deterministic ledger core. Capabilities belong in one of three rings:

1. **Core** — defines financial truth and reproducible portfolio state.
2. **Ecosystem** — connects LUCA to external formats, runtimes, and languages.
3. **Applications/products** — use LUCA to solve workflows; these are not core requirements.

The version milestones below describe capability groups, not rigid release gates. Engineering slices may establish cross-cutting foundations or introduce a narrow part of a later capability when that work reduces architectural risk for the whole project.

## Minimal-core delivery roadmap (2026-10-21)

The minimal core is a financial-algebra library plus bounded portable execution
hosts. One immutable cash/equity history must produce reproducible positions,
cash, settlement obligations, balanced journals, trial balances and reports with
source evidence. Durable staged ingestion and efficient parallel reporting are
part of the baseline alongside financial correctness.

This section updates the completion sequence for the original four slices below.
It records requirements and gaps; it does not dispatch tasks, change authoritative
task state, resume paused software tests, or claim acceptance. The combined
assignment source remains [Luca Platform planning](https://github.com/aengebretson/luca-platform/tree/main/planning).

### Current position

Original review baseline: `fdde74b8cb91eb5d5b63370e4215b9d1d901bbbc`
(2026-09-30). The table includes the journal-balance increment and local
verification, durable intake and validation/admission increments on 2026-10-21. "Partial" means useful implementation exists
but full scope or acceptance remains open. Implementation, compilation, executed
correctness checks and measured performance are separate evidence.

| Checkpoint | Required outcome | Position at the implementation baseline |
| --- | --- | --- |
| 1. Accepted financial baseline | Exact values, immutable lifecycle, portfolio projections, both journal policies and executable full-path conformance. | Partial: the financial primitives and optional host pass all 38 root checks in both Debug and Release locally with AppleClang 17; wider compiler/CI acceptance remains open. |
| 2. Posting and balance algebra | Public exact journal reduction by explicit book, chart account and currency; trial balances with contributing line identities. | Bounded implementation verified locally: ten policy/time views, whole-entry partition merge, owned evidence and sanitizer checks pass. Book assignment/coverage remain explicit caller responsibilities. |
| 3. Durable staged ingestion | Fast durable receipt, asynchronous validation, retained transaction errors/status and idempotent promotion to accepted events. | Bounded native C++ implementation: durable bytes/receipts, leased asynchronous validation, retained per-record errors/status, versioned revalidation and atomic canonical ledger admission pass crash/concurrency checks. HTTP, report coverage and distributed production adapters remain open. Existing core acceptance stays strict. |
| 4. Execution and partition contracts | Typed context/dependency boundaries, complete financial keys, deterministic ordering and permitted merge rules. | Partial: descriptors, exact-cash contracts and tested journal-balance partition contracts exist; general executable composition and parallel scheduling do not. |
| 5. Shared batch preparation | Resolve/select/order once per compatible snapshot and reuse compact immutable batches across projections. | Planned: lifecycle projection reuses a resolved set, but projections prepare economic views separately; no shared prepared-batch public boundary. |
| 6. Efficient incremental evidence | Update affected partitions and verified evidence without scanning all history for each small append; preserve full-replay equivalence. | Partial: canonical formats/checkpoints and bounded continuation exist. Admission now prepares suffixes against indexed causal targets and persists only new canonical records/hashes. Projection manifests still rebuild/hash full history; cutoffs cannot advance and prefix-targeting changes require replay. |
| 7. Complete portable accounting report | One history produces portfolio views, both journal views, trial balances and an explained external discrepancy through C++ and bounded CLI/Python access. | Partial: the C++ journal-to-balance walkthrough now runs alongside existing cash-report examples and an exact-cash CLI/Python client; the complete external-observation accounting report/host remains open. |
| 8. Representative performance acceptance | Declared latency/throughput/cost budgets, correctness-checked benchmarks, concurrent workloads and scaling evidence. | Partial: executed synthetic journal-balance and durable-intake baselines include million-record inputs and bounded concurrent intake. Local validation/admission baselines now include mixed validity, atomic groups, incremental appends and 1/2/4-worker book partitions. Reporting, production scaling and workload-specific service budgets remain open. |
| 9. Package and release acceptance | An independent consumer installs a pinned package, reproduces results and sees documented platform/performance limits. | Partial: installed/embedded consumer execution and option checks pass with AppleClang 17 on macOS. Current Linux/GCC runtime, compiler CI and release remain open; current exact arithmetic blocks native MSVC. |

Evidence: [integration review](reviews/OSS_FOUNDATIONS_20260930.md),
[accounting](accounting-foundations.md), [algebras](financial-algebras.md),
[transformation inventory](transformation-contract.md),
[checkpoint continuation](checkpoint-result.md) and
[benchmark infrastructure](../tests/benchmarks/README.md).

The subsequent [journal-balance increment](journal-balances.md) adds checkpoint 2's
public API and a ten-view accounting walkthrough, plus installed/embedded
consumers and focused reduction/merge benchmarks. It does not complete checkpoint
7's external-observation reporting host or checkpoint 8's representative service
levels. Actual local results and limitations are recorded in
[the verification report](reviews/JOURNAL_BALANCES_20261021.md); authored checks
and compilation alone do not establish runtime acceptance.

### Durable staged ingestion

The [architecture/design](staged-ingestion.md) and
[API specification](staged-ingestion-api.md) define the bounded increments.
The first [local intake host](../apps/ingestion-host/README.md) implements durable
receipt, original bytes, work intent, retries, resumable transfer and restart
cleanup. Actual verification and intake-only measurements are in
[the intake review](reviews/DURABLE_INTAKE_20261021.md). The next
[native validation/admission increment](reviews/NATIVE_INGESTION_20261021.md) adds
retained per-record errors/status, context-pinned runs, dependency wakeups,
lease fencing and atomic canonical admission/recovery, plus local processing
benchmarks. Runtime APIs/workers are C++; Python is retained only in tests and
analysis. Direct C++ integration is the default; CLI tools and an optional MCP
adapter share application operations. No network framework is selected. Next:
review the drafted [C++ application API](application-api.md), [CLI commands](cli.md)
and [MCP tools](../contracts/mcp/v1.tools.json); implement the facade and deliver projection
coverage over pinned accepted revisions and the direct C++/CLI accounting path,
then expose the same operations through MCP. Report jobs, CLI reporting/export
commands and MCP are not implemented. OSS has no graphical UI; enterprise owns
screens and presentation. REST/gRPC hosts are deferred. See
[integration direction and CLI requirements](native-ingestion.md#integration-direction).
Shared projection batches and incremental projection evidence remain separate
checkpoints.

Retain original submissions as immutable evidence, including invalid/unparseable
records. Distinguish durable receipt from authoritative ledger acceptance. A
bounded host acknowledges only after payload/reference and processing intent
are durable, returning an import identity and status location; an HTTP adapter
can use `202 Accepted`. Large uploads support resumable transfer.

Validate asynchronously against explicit rule/reference-data versions. Retain
per-record diagnostics and dependency status through received, validating,
blocked/invalid, ready, committed and projected states. Corrections are new linked
submissions. Define partial-import and dependent-group atomicity so an unrelated
bad record need not prevent useful progress. Invalid staged data never bypasses
canonical value/lifecycle checks; missing dependencies can remain blocked.

Couple durable receipt with durable work intent. Stable submission identities
and payload fingerprints make retries/promotion idempotent. Demonstrate recovery
after interruption without lost or duplicated effects. Use bounded queues,
backpressure and separate ingestion/report resource budgets. Status is durable;
notifications are optional. Provide one bounded durable reference host, keeping
storage, transport, queues and scheduling in replaceable adapters/hosts without
hidden I/O or mandatory database/cloud/broker dependencies in the library.

### Execution contracts and shared batches

Every supported operation declares typed inputs/results, complete evaluation
context, engine/policy/reference versions, financial partition keys, dependencies,
ordering and supported merge laws. Descriptor strings alone do not validate
composition or authorize optimization. Preserve exact arithmetic, rounding,
diagnostics and evidence under every supported execution plan, including
finite-width intermediate-overflow behavior.

Parallelize independent validation and resolved event-to-journal maps. Resolve
dependent lifecycle chains in order; merge balances only through compatible,
evidenced reductions. Handle cross-partition economic dependencies explicitly;
avoid a global lock/sequence across unrelated books. Hosts schedule ordinary
deterministic operations across threads/processes; distributed/GPU executors
remain optional.

Prepare one immutable, lifetime-safe resolved/selected/ordered batch for a pinned
snapshot and compatible cutoffs. Share it across position, cash, settlement and
journal projections instead of repeating sorting/materialization. Use compact
values and evidence IDs, with immutable detail available for drill-down.
Columnar layouts/SIMD are measured implementation options, not external schemas
imposed on the financial model.

### Incremental state, evidence and report consistency

Update affected partitions and reuse compatible derived views. Define
correction/policy/reference-data invalidation and the earliest required replay
boundary. Extend fixed-cutoff continuation deliberately for advancing cutoffs
and settlement eligibility; until supported, keep explicit replay requirements.

Design verified immutable segments and incremental evidence summaries so small
appends do not routinely reconstruct/rehash all accepted history. Preserve
existing canonical bytes/digests. Any new segment/manifest digest scheme needs a
reviewed versioned format, verification rules and migration plan.

Pin reports to input revision, complete cutoffs and policy/context versions while
ingestion continues. Identify staged/blocked records excluded from that revision;
disclose partial completion instead of presenting an incomplete import/partition
set as final. Cache under complete financial identities and retain traceability
from reports through balances, lines and events to source evidence.

Prove supported incremental/partitioned execution equals full replay. Lifecycle,
lot allocation and checked finite-width arithmetic carry no blanket
commutativity, inverse or arbitrary-regrouping permission.

### Performance acceptance and optional acceleration

Capture representative slow user workflows and specify hardware, data shape,
concurrency, financial completeness and numerical performance budgets before
claiming speedups. Begin measurement with the baseline and carry it through every
increment. Include cold/warm paths, skewed account sizes, concentrated activity,
late corrections, invalid/dependent imports, concurrent reports/imports and
worker failure/recovery. Exercise million-event workloads and larger
10/100-million-event datasets where declared hardware permits; record resource
limits rather than assuming all sizes fit memory.

Measure durable receipt p50/p95/p99 separately from upload duration, validation/
commit throughput, queue age/drain time, projection freshness, cold/warm report
latency, correction-to-report latency, checkpoint/evidence cost, memory, bytes
read/transferred, cost per workload and core/worker scaling efficiency. Include
evidence construction and output materialization in end-to-end timing. Establish
regression budgets. Compare Geneva on equivalent measured workloads and outputs;
current microbenchmarks establish no service levels or Geneva advantage.

Establish a strong multicore CPU baseline first. Evaluate optional GPU execution
for measured large scans, reductions, joins or repeated numerical workloads,
including packing, transfer and evidence costs. Accelerated financial operations
must match the versioned CPU contract, exact rounding, overflow/errors and
canonical output. Wider accumulators or changed reduction semantics require a
reviewed versioned contract. GPU availability and a general distributed
scheduler are not minimal-core dependencies.

### Delivery sequence and completion

1. Establish checkpoint 1 and checkpoint 8's representative workloads/budgets.
2. Specify posting/book dimensions and execution contracts; deliver checkpoints
   2 and 4 while designing the staged-ingestion boundary.
3. Deliver checkpoint 3's durable host and checkpoints 5-6's shared batches and
   incremental evidence. Independent paths can proceed with explicit ownership.
4. Integrate checkpoint 7's accounting report and portable clients.
5. Complete correctness/performance and checkpoint 9's package/release acceptance.
   The user explicitly resumed correctness tests and benchmarks on 2026-10-21;
   actual results and remaining platform limits are recorded in the verification report.

Completion requires recorded acceptance of all nine checkpoints: received
imports survive interruption, validation errors remain inspectable, accepted
events produce complete traceable accounting/portfolio reports, supported
execution plans agree, and declared performance budgets pass on documented
workloads. Compilation or a quick asynchronous acknowledgement alone is
insufficient.

Retain the narrow contribution/purchase/exact-reversal accounting policies.
Ordinary sells/cost basis, income/accruals, FX, corporate actions, full
P&L/NAV/tax, broad adapters and platform-scale orchestration remain separate
extensions. Complete the accounting/reporting path before expanding the
financial domain or building a generic execution framework.

## Core roadmap

### v0.1 — Ledger and basic state

Prove the fundamental model with cash and simple equity activity.

- Account, instrument, money, quantity
- Source records and provenance
- Canonical economic events
- Immutable ordered ledger
- Effective-time and settlement-time semantics
- Deposit and withdrawal events
- Equity trades, fills, cancellations, and corrections
- Position projection
- Settled/unsettled cash projection
- Settlement obligations
- Full replay and deterministic output
- Initial state snapshots/checkpoints
- Synthetic conformance fixtures

### v0.2 — Reconciliation

Make reconciliation a generic comparison between deterministic projections and external observations.

- Position observations
- Cash observations
- Trade observations
- Matching policies
- Tolerances
- Aggregation keys
- Structured breaks/exceptions
- Drill-down from aggregate break to source events
- Batch reconciliation
- Parallel partitioning by appropriate domain keys

### v0.3 — Asset servicing / corporate actions

Extend the event model beyond ordinary trades.

- Dividends and interest
- Splits and reverse splits
- Mergers
- Spin-offs and distributions
- Cash-in-lieu and fractional handling
- Elections and entitlements where applicable
- Corporate-action corrections
- Conformance scenarios for complex state transitions

### v0.4 — Portfolio accounting

Treat accounting as a deterministic projection over the same economic events.

- Journal projection
- Lots and cost basis
- Realized/unrealized P&L
- Accruals
- Income and expenses
- Trade-date and settlement-date views
- General-ledger/subledger projections
- NAV components
- Traceability from balances to events

### v0.5 — Securities finance

Add brokerage/prime-brokerage economic events.

- Stock borrow and loan
- Recalls and returns
- Financing accruals
- Borrow fees/rebates
- Collateral movements
- Margin-related observations
- Securities-finance reconciliation

### v0.6 — Performance and large-scale state

The minimal-core program above already requires staged ingestion, explicit
parallel execution contracts, shared batches, incremental evidence and measured
performance acceptance. This capability group expands scale beyond that baseline.

Harden the compute model for very large datasets without changing financial semantics.

- Explicit partition-aware APIs
- Efficient incremental replay
- Snapshot versioning and rebuilds
- Batch/columnar execution paths
- Reproducible performance benchmark suite
- Parallel scaling tests
- Memory and throughput targets

## Ecosystem roadmap

These capabilities connect LUCA to the outside world but do not define the core model.

### Data and protocol adapters

- Generic CSV
- Parquet
- Apache Arrow
- FIX
- FINOS Common Domain Model (CDM)
- SWIFT / ISO 20022
- Selected broker/custodian formats
- Fund-administrator files
- Blockchain/on-chain activity

### Bindings and execution hosts

- C++ library API as the default enterprise integration
- Native CLI tools for ingestion, workers, status, reports, export and evidence
- Optional MCP server over the same application operations
- Python bindings
- C ABI where useful
- Deferred REST/gRPC service host if demanded by integrations
- WebAssembly only if a concrete local/browser use case emerges

### Agent integration

Agents sit outside authoritative financial calculations.

- Schema and source mapping assistance
- Exception investigation
- Break explanation
- Evidence gathering
- Operational workflow orchestration
- Human approval at consequential boundaries

## Potential applications and commercial products

These should be built only when a real user workflow justifies them.

- Managed reconciliation service
- Broker operations platform
- Portfolio-accounting service
- Exception-management UI
- Agent-assisted operations console
- Customer-facing API platform
- Hosted portfolio-state service
- Broker/custodian integration service
- Desktop or web applications

The open-source core should remain useful without any of these products.

## Architectural guardrails

The roadmap should preserve the following constraints from the beginning:

- deterministic financial core
- no hidden database I/O in projections
- immutable/versioned event inputs
- explicit event ordering and causality
- incremental processing plus full replay
- versioned snapshots
- partition-aware APIs
- batch-oriented processing
- adapters outside the core model
- storage and distribution outside financial semantics
- conformance tests for executable behavior

## Current baseline

The repository has already implemented a meaningful portion of the original near-term sequence:

- strong account, instrument, currency, money, quantity, source-record, and provenance values;
- cash-movement and equity-trade events;
- an append-only in-memory ledger with deterministic ordering;
- position projection;
- settled-cash projection;
- open-settlement-obligation projection;
- exact position and settled-cash reconciliation;
- unit and executable conformance tests;
- a reproducible benchmark suite.

The next work should no longer repeat that original bootstrap checklist. It should turn the existing primitives into a consumable engine, close foundational lifecycle/replay gaps, and prove the accounting thesis.

## Current implementation program

The four original slices retain their technical scope below. The minimal-core
delivery roadmap above now defines the completion sequence and performance
acceptance; finishing the older slice checklist alone does not complete it.

### Slice 1 — LUCA identity and package

Turn the repository into a deliberate open-source C++ dependency.

- establish LUCA as the single canonical project identity;
- define domain-oriented `luca::...` CMake targets;
- support both `add_subdirectory` and installed `find_package` consumption;
- prevent tests, examples, and benchmarks from polluting parent builds;
- add install/export rules and an external package-consumer test;
- add one end-to-end public-API example;
- establish Linux GCC/Clang and Windows MSVC CI;
- prepare a first preview release and repository rename.

Detailed design: [Slice 1 — LUCA identity and package](slices/01-identity-and-package.md).

### Slice 2 — Event lifecycle

Define immutable lifecycle semantics before the event domain expands.

- cancellation;
- reversal;
- correction;
- supersession;
- causal lineage between events;
- deterministic replay after late lifecycle events;
- conformance scenarios that prove positions, cash, settlement, and provenance after lifecycle changes.

The ledger must never silently mutate accepted historical facts to implement a correction.

### Slice 3 — Serialization and checkpoints

Make replay and derived state portable, hashable, and incrementally reproducible.

- versioned canonical serialization outside domain value objects;
- deterministic event and projection representations;
- input and state hashes;
- position, cash, and settlement checkpoints;
- projection version and event-watermark metadata;
- conformance tests proving full replay equals checkpoint plus incremental events.

Storage technology remains outside financial semantics.

### Slice 4 — Accounting foundations

Prove that the same event history can produce balanced, traceable accounting output.

- journal-entry and journal-line value types;
- balanced debit/credit invariants;
- a narrow accounting-policy interface;
- journal projection for cash movements and equity trades;
- trade-date and settlement-date views;
- full lineage from journal line to economic event to source-record provenance;
- conformance fixtures covering a cash contribution, an equity purchase, settlement state, and balanced journals.

This slice begins the v0.4 accounting capability but does not attempt lots, tax accounting, complete P&L, NAV production, or universal charts of accounts.

## Program completion criteria

The four-slice program is complete when:

- an unfamiliar C++ developer can install and consume LUCA through documented public targets;
- LUCA Enterprise can pin and build LUCA without using internal headers or source files;
- event corrections and reversals have explicit immutable semantics;
- deterministic serialized inputs and state can be hashed and replayed;
- incremental replay from a checkpoint is equivalent to full replay;
- cash and equity activity produce positions, cash, settlement obligations, reconciliation breaks, and balanced journals from one authoritative event history;
- all behavior is demonstrated through reusable conformance fixtures.

The bounded portable ingestion/reporting hosts in the minimal-core program are
part of its acceptance. Broader adapters, cloud products, extensive UI,
platform-scale distributed orchestration and agent layers follow the accepted
financial and performance foundations.

## Backlog clarification — transformations and portable accounting (2026-09-23)

The combined execution backlog lives in [Luca Platform planning](https://github.com/aengebretson/luca-platform/tree/main/planning/ROADMAP.md). It is the sole cross-repository assignment source; this roadmap retains the OSS financial requirements. This clarification is planned work, not a claim of new implementation, and does not expand the active packaging task.

- **O2 — lifecycle:** corrections/reversals preserve original evidence and causal lineage, with consistent position/cash/settlement and later journal outcomes. Design economic, knowledge and settlement time semantics explicitly.
- **O3 — portable replay:** canonical input hashes, partition/watermark, evaluation context, engine/projection/policy versions and output hashes identify a calculation. Reject incompatible checkpoints; prove full replay equals incremental replay, including correction invalidation.
- **O4 — accounting:** source evidence → economic events → immutable ledger → versioned accounting transformations → journals/balances → reconciliation against separate observations. Prove balanced cash/equity accounting, trade/settlement views and side-by-side narrow policy versions with full lineage and explicit rounding/currency rules.
- **O5 — composable transformations/projection contracts:** design a small typed public interface for normalization, mappings, ordered folds, reductions and reconciliation. State which algebraic laws apply to each operation and prove claimed identities/associativity/commutativity with domain fixtures. Do not assume order independence for lots, corrections or dependent state. An independent consumer must be able to define a narrow policy/projection without modifying core source.
- **Portable execution:** authoritative summarization, accounting and reconciliation stay in OSS libraries. C++ consumers, CLI/Python tools, local cron processes, third-party infrastructure and Luca-hosted jobs reuse the same implementation with explicit authorized inputs. Platform identity, API transport, storage, scheduling and UI remain outside the core.

### Concrete acceptance demonstration

From a contribution, equity purchase, settlement and correction/reversal fixture, produce positions, cash, settlement obligations and balanced journals; reconcile them with deliberately differing external observations. Reproduce canonical results with the same versioned inputs/policy through standalone and hosted execution. Show source/intermediate/output lineage and an independent custom policy, then display the result in an externally developed app using platform APIs.

### Sequencing and design decisions

Preserve slices 1–4. Review O5's minimal contracts alongside the accounting policy boundary, without blocking narrow accounting on a generic framework. Follow with portable hosts and a platform adapter pinned to a reviewed OSS revision. General plugin loading/ABI, expression languages and expanded accounting remain separately justified work. User-defined transformations do not make external results authoritative without validation. Precise extension signatures and supported algebraic laws require a reviewed design and conformance evidence.

Feature detail: [O4](https://github.com/aengebretson/luca-platform/blob/main/planning/features/O4.md), [O5](https://github.com/aengebretson/luca-platform/blob/main/planning/features/O5.md), [F1](https://github.com/aengebretson/luca-platform/blob/main/planning/features/F1.md). Developer API, editor, app packaging/publication and external Git live in platform D1–D4; they are not dependencies of standalone OSS use.


## Implementation increment: financial algebras and reporting projections

The September 2026 integration adds O1 package naming with legacy compatibility,
O3 state-plus-manifest continuation, O4 settlement-date fixture accounting,
O5 public financial algebra declarations and a custom reporting projection, and
F1 exact trade comparison with a bounded CSV observation adapter. Public API
and restrictions are linked from [the architecture](design.md#additive-public-interfaces).

These are bounded implementations, not completion of O1–O5/F1 or a public release.
General composition, portable accounting hosts, broader policies, corporate
actions, lots/P&L, securities finance and broader scaling remain sequenced
follow-ups; the minimal-core performance requirements above are now foundational.
See [the eleven feature designs](oss-program-20260930/README.md). Runtime acceptance
for this increment is pending while automated test execution is paused.

## Enterprise execution intake increment — 2026-10-22

An additive [source-envelope format](execution-envelopes.md) is implemented in the
native reference runtime. The paired enterprise review diff adds a durable OMS
execution outbox and direct-library C++ delivery, preserving source, receipt and
validation/commitment status. Current admission is the existing cash/equity
model; unsupported derivatives and missing booking data remain explicit retained
exceptions. Historical replay, production policy/instrument expansion, full
report coverage and shared application/CLI/MCP implementation remain open. This
adds a bounded enterprise consumer to checkpoint 3, not acceptance of every
minimal-core checkpoint or a production deployment.

## Custodian lots and charges implementation

C++ LIFO long/short lot relief, separate exact trade charges, economic P&L and firm aggregation are implemented in the [lot accounting contract](lots-and-charges.md). Native `cash-equity/2` ingestion retains charge errors and canonical v2 amounts. Next: general-ledger sale/short recognition, broker schedule selection/administration, affected-pool lot checkpoints and broader workload benchmarks. This implementation supersedes the earlier long-only/FIFO proposal for the first lot slice; no tax-compliance or deployment claim is implied.

### Shared-book lifecycle increment

Implemented for review: dated cancellation, pure closed-period admission guard,
native durable close receipts and accepted-prefix fencing. Enterprise consumes
these via its shared log and explicit report cutoffs. See
[the API and boundaries](dated-void-periods.md). Production migration, bulk import
UI, option accounting and full accounting statements remain subsequent work;
this increment does not claim those milestones complete.
