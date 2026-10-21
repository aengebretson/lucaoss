# LUCA

## What it does

LUCA ingests financial activity from existing systems, normalizes it into a compact canonical ledger, and deterministically derives portfolio state.

```text
FIX ─────────┐
CDM ─────────┤
SWIFT ───────┤
Broker API ──┼──► LUCA
Custodian ───┤       │
Blockchain ──┘       ▼
                  Ledger
                    │
                 Portfolio
                   State
```

The project does **not** require firms to adopt a new interchange standard. FIX, FINOS CDM, SWIFT, broker feeds, custodian files, administrator files, and blockchain data are inputs through adapters. LUCA owns the internal computational representation.

For the deeper architecture and scaling principles, see [docs/design.md](docs/design.md). For phased implementation, ecosystem capabilities, and possible products, see [docs/roadmap.md](docs/roadmap.md).

## Core concepts

- **Source records** — immutable evidence received from an external or internal system.
- **Economic events** — normalized interpretations of economically meaningful activity.
- **Ledger** — ordered, auditable history of economic events with provenance.
- **Financial algebras** — typed domains, operations and explicit laws with stated ordering, currency, rounding and overflow preconditions.
- **Projections for reporting** — deterministic derivations of positions, cash, lots, settlement, P&L, accounting, and related state.
- **Observations** — external assertions of state from brokers, custodians, administrators, banks, or other systems.
- **Reconciliation** — comparison of projections with observations at transaction, position, account, portfolio, or aggregate levels.

## Build and consume LUCA

LUCA is a C++23 financial library. Applications, CLI/Python tools and hosted jobs
supply inputs, storage, scheduling and presentation; they reuse its financial
algebras and reporting projections. Ordered replay is distinct from an unordered
reduction. Associativity or commutativity applies only where the operation's
contract permits it, including representable intermediate exact values.

Prerequisites: CMake 3.24+, a C++23 compiler/standard library with `std::expected`,
and signed `__int128` support. GCC 12.2 compiled the package consumers in the
current packaging increment. Clang CI is authored but unexecuted. Native MSVC
is currently unsupported by the exact-arithmetic implementation; see the
[packaging evidence and limitations](docs/PACKAGING_BASELINE.md).

To install the header-only libraries without tests, tools or Python discovery:

```sh
cmake -S . -B build/package -DLUCA_BUILD_TESTS=OFF -DLUCA_BUILD_TOOLS=OFF -DLUCA_BUILD_BENCHMARKS=OFF
cmake --install build/package --prefix /absolute/path/to/luca-install
```

An external application can use:

```cmake
find_package(Luca 0.1 CONFIG REQUIRED)
add_executable(my_report main.cpp)
target_link_libraries(my_report PRIVATE luca::reconciliation)
```

Configure that application with
`-DCMAKE_PREFIX_PATH=/absolute/path/to/luca-install`. Alternatively, a pinned
source dependency uses `add_subdirectory(third_party/luca)` and the same targets:

| Target | Domain | Direct dependencies |
| --- | --- | --- |
| `luca::ledger` | Values, events, ledger, lifecycle, serialization, existing accounting headers | None |
| `luca::accounting` | Exact journal balances, trial-balance controls and partition merge (opt-in) | `luca::ledger` |
| `luca::portfolio` | Position, cash, settlement and replay projections | `luca::ledger` |
| `luca::reconciliation` | Observations and exact reconciliation | `luca::portfolio`, `luca::ledger` |
| `luca::luca` | Compatibility umbrella | All three domain targets |

The [complete external-consumer walkthrough](examples/package-consumer/README.md)
creates a deposit and equity purchase, projects position/cash/settlement views,
and reports a deliberate cash discrepancy against separate bank evidence.

The [journal-balance walkthrough](examples/journal-balances/README.md) derives
trial balances under both fixture accounting policies, preserving line/source
evidence through the new opt-in [accounting API](docs/journal-balances.md).

The [durable intake reference host](apps/ingestion-host/README.md) now retains
original payloads, durable receipts/work intent, scoped retry identities and
resumable uploads through a local Python API/CLI. Financial libraries have no new
storage dependency. The [architecture/design](docs/staged-ingestion.md) and
[API specification](docs/staged-ingestion-api.md) describe the wider feature;
asynchronous transaction validation, per-record diagnostics, ledger admission and
HTTP remain subsequent increments.

| Canonical option | Top-level default | Embedded default | Legacy alias |
| --- | --- | --- | --- |
| `LUCA_BUILD_TESTS` | ON | OFF | `PACIOLI_BUILD_TESTS` |
| `LUCA_BUILD_TOOLS` | ON | OFF | `PACIOLI_BUILD_TOOLS` |
| `LUCA_BUILD_BENCHMARKS` | OFF | OFF | `PACIOLI_BUILD_BENCHMARKS` |

Explicit legacy cache entries and parent normal variables remain supported.
When both names are defined, their boolean values must agree; a conflict stops
configuration with instructions for removing an obsolete cache entry. A
legacy-only configuration does not create a canonical cache entry, so changing
that legacy option on subsequent configures remains supported. Default-created
canonical cache entries also participate in conflict checking. To switch an
existing build to legacy options, remove the corresponding canonical entry
with `-ULUCA_BUILD_TESTS` (or `TOOLS`/`BENCHMARKS`); to migrate to canonical names,
remove the corresponding legacy entry with `-UPACIOLI_BUILD_TESTS`.

The source target `pacioli`, `<pacioli/ledger.hpp>` and `pacioli::Ledger` remain
compatible. Installed consumers use `find_package(Luca)` and `luca::...` targets;
there was no installed `pacioli` target. The repository is now
`aengebretson/lucaoss`; this change publishes no release or ABI guarantee.

Developer verification commands (tests and benchmarks resumed by the user on 2026-10-21):

```sh
cmake --preset dev
cmake --build --preset dev --parallel 2
ctest --preset dev
```

The full developer build generates conformance fixtures. Use the library-only
configuration above when consuming only the libraries. Local AppleClang 17
Debug/Release and sanitizer results are recorded in the
[journal-balance verification report](docs/reviews/JOURNAL_BALANCES_20261021.md).
Compiler workflow definitions remain manual-only and unexecuted; local results
do not establish the wider compiler CI matrix.

## Planned capabilities

### Ledger and event processing

- Immutable economic-event ledger
- Event provenance and source lineage
- Effective-time and settlement-time semantics
- Corrections, reversals, cancellations, and superseding events
- Replayable portfolio state as of any point in time
- Stable serialization for deterministic reproduction

### Portfolio state

- Positions by account, instrument, strategy, and portfolio
- Settled and unsettled cash
- Receivables and payables
- Tax lots and cost basis
- Settlement obligations and projections
- Realized and unrealized P&L
- Income, fees, financing, and accruals

### Economic events

Initial support will focus on simple cash securities and expand incrementally to:

- Trades and allocations
- Cash movements
- Fees and commissions
- Dividends and interest
- Splits and reverse splits
- Mergers and acquisitions
- Spin-offs and distributions
- Exercises and assignments
- Stock borrow, loan, recalls, and returns
- Financing and collateral events
- FX and multi-currency activity

### Accounting

- Double-entry journal projection from economic events
- Configurable accounting policies
- Trade-date and settlement-date views
- General-ledger and subledger projections
- NAV components and P&L attribution
- Traceability from accounting balances back to source events

### Reconciliation

- Trade reconciliation
- Position reconciliation
- Cash reconciliation
- Settlement reconciliation
- Accounting/NAV reconciliation
- Configurable matching, tolerances, and aggregation keys
- Drill-down from aggregate break to underlying divergent events
- Structured exceptions suitable for human or agent investigation

### Analytics

- Exposure and concentration views
- Portfolio turnover and activity statistics
- P&L explain
- Cash and settlement forecasting
- Event-driven portfolio analytics

### Adapters

Adapters translate external representations into ledger events or observations without making those representations part of the core model.

Planned adapter families include:

- Generic CSV / Parquet
- FIX
- FINOS Common Domain Model (CDM)
- SWIFT / ISO 20022
- Broker and custodian APIs/files
- Fund administrator files
- Blockchain/on-chain transaction data

## Design principles

1. **Deterministic financial core.** Financial truth is computed by explicit code, not probabilistic models.
2. **Events before mutable state.** Portfolio state is derived from economic history rather than treated as the primary source of truth.
3. **Provenance everywhere.** Every derived result should be traceable to source records and transformation rules.
4. **Observations are not events.** A custodian balance or administrator NAV is evidence to reconcile against, not automatically ledger truth.
5. **Interoperability through adapters.** Existing industry formats are accepted rather than replaced.
6. **Small canonical model.** Keep the internal representation compact and computationally useful; preserve source-specific extensions when needed.
7. **Executable semantics.** Correctness should be demonstrated with deterministic replay and conformance tests, not prose specifications alone.
8. **AI outside the ledger.** Agents may classify, map, investigate, and explain, but they do not perform authoritative ledger, accounting, or risk calculations.
9. **Technology-neutral inputs.** Traditional databases, files, APIs, standardized messages, and blockchains are all potential sources.
10. **Library first.** Build a reusable engine before services, dashboards, or agent interfaces.
11. **Scale by construction.** Keep projections side-effect-light, partition-aware, incremental, snapshot-friendly, and batch-oriented so distribution is an execution concern rather than a rewrite.

## Initial milestone

The first release should prove the architecture with the smallest useful domain:

```text
Deposit cash
     │
     ▼
Execute equity trades
     │
     ▼
Append canonical events
     │
     ▼
Replay ledger
     │
     ├──► Positions
     ├──► Cash
     └──► Settlement obligations
                 │
                 ▼
       Compare with observations
                 │
                 ▼
              Breaks
```

### v0.1 target

- `Instrument`
- `Account`
- `Money` / `Quantity`
- `SourceRecord` / provenance
- `EconomicEvent`
- `Ledger`
- position projection
- cash projection
- settlement projection
- state snapshots/checkpoints
- position and cash observations
- generic reconciliation engine
- generic CSV adapter
- synthetic test portfolio with intentional breaks

## Roadmap

The detailed roadmap lives in [docs/roadmap.md](docs/roadmap.md). At a high level:

- **v0.1 — Ledger:** trades, cash, positions, settlement, snapshots
- **v0.2 — Reconciliation:** matching, aggregation, observations, structured breaks
- **v0.3 — Corporate actions:** dividends, splits, mergers, spin-offs
- **v0.4 — Accounting:** journals, lots, accruals, realized/unrealized P&L
- **v0.5 — Securities finance:** borrow, lending, financing, collateral
- **v0.6 — Performance and adapters:** scale-oriented execution plus FIX, CDM, Arrow/Parquet, and selected broker/custodian formats
- **v0.7 — Automation:** agent-assisted mapping, exception investigation, and explanation

Broader applications such as hosted services, web or desktop interfaces, managed reconciliation, and broker-operations products remain possible future products rather than requirements of the core library.

## Non-goals

At least initially, this project is **not**:

- an OMS or execution system
- a pricing library or strategy engine
- a universal financial messaging standard
- a blockchain or distributed ledger
- a fund-administration application
- an AI system that decides financial truth

The objective is narrower: **make investment state reproducible from economic events, and make disagreements with external systems explicit and explainable.**
