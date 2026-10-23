# Custodian LIFO lots, shorts and trade charges

The C++ `luca::accounting` target now provides `accounting/lots.hpp` and
`accounting/charge_schedule.hpp`. This implements economic lot accounting for
cash equities: long purchases/sales, short openings/covers, partial and multiple
lot relief, and trades crossing zero. It does not implement a jurisdiction's tax
rules, borrow availability, margin, financing, corporate actions or general-ledger
sale journals. Cash/accrual recognition is independent of lot selection.

## Domain and durable input

`EquityTrade::create(..., optional<TradeCharges>)` accepts a bounded list of
separate charge components. Each has a stable local ID, category (`commission`,
`exchange`, `regulatory`, `other`), payee, amount/currency, state and optional
schedule version. Components must have unique IDs, nonnegative exact amounts,
and the trade's quote currency. The current limit is 64 components. Rebates and
cross-currency charges require a later explicit contract.

- Missing charges: legacy/unreported; never confirmed zero.
- Empty supplied list: explicit known zero.
- `reported`: supplied actual amount.
- `scheduled`: calculated from a pinned schedule version; provisional.
- `unreported`: missing component, represented by a zero placeholder, not an
  assertion of no charge. Nonzero unreported placeholders are rejected.

Execution price and gross principal never include hidden fees. `trade_cash`
returns signed cash flow: `-signed principal - commissions - fees`.
Cash/settlement projections use this net flow; missing charges retain legacy
gross behavior. The lot report exposes `charges_complete` so consumers can label
provisional results. Existing legacy portfolio result types do not contain this
new completeness flag; consumers needing complete-cost reporting must use the
lot report or inspect canonical charge state.

Native ingestion accepts `event.charges` under explicit `cash-equity/2` rules.
Each JSON component requires string fields `id`, `category`, `payee`, `amount`,
`currency`, `state`, `schedule_version` (the latter may be empty for reported
amounts). The transport and envelope versions remain unchanged. Old validation
contexts reject the new field; register a new immutable context to opt in.
Invalid charges remain staged with diagnostics and never reach the ledger.

Canonical encoding emits economic-event v2 with nine members for charged trades;
`charges` is the first canonical key and its array order is preserved. Each
component has six canonical members; currency is in its Money value. Uncharged
trades retain byte-identical v1 encoding. New decoders read both; old decoders fail
closed on v2. Upgrade readers before admitting v2 events. Do not overwrite old
contexts, reference snapshots or mapping definitions. Corrections/cancellation
retain all source evidence. Charged numeric reversals are rejected because
negating quantity does not refund charges.

## Shared application API and execution contract

```cpp
std::expected<LotReport, LotError> project_lifo_lots(
    const LifecycleResolution&, std::span<const LotAccount>);
std::expected<LotReport, LotError> project_lifo_lots(
    std::span<const ResolvedLifecycleEvent>, std::span<const LotAccount>);
std::expected<Money, ValueError> lot_carrying_amount(const OpenLot&);
std::expected<std::vector<FirmLotBalance>, LotError> firm_lot_balances(
    const LotReport&);
```

Resolve lifecycle chains once for a pinned ledger snapshot and cutoffs, then share
that immutable resolution with accounting and portfolio consumers. The span
variant permits independently scheduled complete partitions; its references must
remain alive for the call. This API returns owned results. Inputs must be ordered
by effective timestamp then the original acceptance sequence, with no duplicate
records. Reordered inputs are rejected. Do not reassign sequences when partitioning.

Each ledger account requires explicit book, legal entity, custodian account and
policy version. The financial partition key is exactly
`(book, legal entity, custodian account, instrument, quote currency)`. No lot relief
crosses that key. Conflicting policy bindings within a pool fail. Only LIFO is
implemented; the caller must not label an alternate method as LIFO.

Buys consume newest short lots before opening a long lot. Sells consume newest
long lots before opening a short lot. One pool is a net position; simultaneous
hedged long/short sleeves require distinct future pool semantics. Short inventory
is permitted without any fabricated opening long purchase. Trading authorization
and stock-borrow availability remain host responsibilities.

The fold is ordered and stateful within each pool, with no arbitrary merge or
inverse law. Complete independent pool results can be combined; slicing one pool
arbitrarily cannot. Corrections, cancellations, backdated entries or changed
policies require affected-history replay. Generic reverse events are rejected by
this projection. This slice does not provide persisted lot checkpoints.

## Outputs, allocation and evidence

`LotReport` contains original per-trade principal, components, commission total,
fee total, signed cash flow; remaining open lots; and each opening/closing record
pair relieved. Stable canonical record IDs join back to immutable lifecycle and
source evidence. Policy and pool identity accompany outputs; the host must attach
snapshot revision/cutoffs and its mapping identity.

Every partial relief proportionally allocates **each** opening and closing charge
component and principal in exact Money units, half-even. Allocation uses remaining
amount and remaining quantity; complete exhaustion takes the exact remainder.
This preserves original principal and every component across remaining inventory
and all reliefs without accumulating lost cents. It is deliberately order-dependent.

Gross P&L is closing principal minus opening principal for longs, reversed for
shorts. Net P&L subtracts both allocated opening and closing charges. Remaining
long carrying amount is principal plus charges; remaining short carrying amount
is opening proceeds less charges. These economic measures do not assert tax
basis or replace a general-ledger recognition policy. Legacy fixture journal
policies reject charged trades instead of silently dropping fees.

The firm view aggregates separate long and short quantities/principal and retained
charges by entity/security/currency. Custodian lots and relief remain authoritative;
firm aggregation never performs cross-account matching or currency conversion.

## Charge schedule primitives

`calculate_charges(quantity, price, currency, effective_at, rules)` accepts explicit
immutable `ChargeRule` inputs. Rules support flat, per-share or fraction-of-notional
rates, buy/sell applicability, half-open effective intervals, minimum/maximum,
currency quantum and explicit rounding. Rates are exact typed values. Limits must
be multiples of the quantum. Active duplicate component IDs are rejected.
Notional uses the kernel's six-decimal principal valuation boundary before the
rate; per-share fees round once to the declared quantum.

The host selects the broker, exchange and applicable regulatory rules. No network
lookup, hardcoded SEC rate or automatic actual-vs-estimate replacement exists.
The helper does not implement volume tiers, cumulative order/day minima, broker
schedule administration or a regulatory rate catalog. Enterprise currently enters
actual fees; this primitive is ready for a later schedule-selection adapter.

## Verification and performance

The portable `tests/conformance/lifo-lots/trades.tsv` fixture covers a hand-calculated
long-to-short-to-long path. C++ tests also cover two custodian pools, multiple short
lots, tiny residual allocations, corrections, cancellation, canonical v2 round-trip,
parallel complete-pool equivalence, schedules and missing charges. Native tests
cover staged error retention, restart, idempotency and v1 refusal of v2 fields.

Initial single-run baseline on Apple M3 Max, 48 GiB, Apple Clang 17,
C++23 `-O3 -DNDEBUG`, two charge components per trade:

| Workload | Trades / pools | Admission | Lifecycle resolution | Lot fold | Reliefs |
|---|---|---|---|---|---|
| Long/short crossing | 100,000 / 2 | 153 ms | 27 ms | 89 ms | 50,000 |
| Many custodians | 104,000 / 1,000 | 173 ms | 31 ms | 135 ms | 52,000 |
| Deep short inventory | 100,000 / 1 | 181 ms | 29 ms | 93 ms | 50,000 |

Process peak resident memory across the three workloads was about 455 MiB,
including ledger, lifecycle resolution, and fully materialized evidence/results.
These are in-memory baselines, not end-to-end throughput or improvement claims.
Build with `LUCA_BUILD_BENCHMARKS=ON` and `CMAKE_BUILD_TYPE=Release`, then run
`luca_lot_benchmark` to reproduce the fixed workloads. Concurrent intake,
late-correction tails, incremental checkpoints and p95/p99 report latency remain
benchmark expansion work. GPU execution is not implemented.
