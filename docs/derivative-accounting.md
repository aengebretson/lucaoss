# Explicit contract economics and derivative accounting, version 1

Link `luca::accounting`; include `luca/accounting/derivatives/contracts.hpp` or
`projection.hpp`. The namespace is `luca::accounting::derivatives`. These are pure,
header-only C++23 value APIs, with immutable caller inputs, owned results and
`std::expected<T,Diagnostic>`. There is no market model, I/O, clock, reference-data
lookup, ledger admission, policy change or persistence in this operation.

This first increment implements SPX/SPXW premium accounting. ES definitions and
raw exact economics are present, but ES accounting is explicitly excluded pending
the separately reviewed settlement-to-market increment.

## Supported contracts and dimensions

`Contract::create(Terms)` validates instrument/underlying identity, immutable
revision, named product profile, contract quantity/quotation units, exact factors,
currency, ticks, strike/right/style/settlement, payoff profile, trading/fixing/
exercise/expiration/payment boundaries, independent effective/knowledge dates and
source provenance. The resulting private immutable terms have a length-framed
SHA256 definition digest. Different revisions/provenance/dates change identity.
A missing factor fails `MONETARY_FACTOR_REQUIRED`; a conflicting factor fails
profile validation. Neither symbols nor execution metadata supply defaults.

Profiles are explicit standard outright contracts:

* `spx_am_european_cash`, `spxw_pm_european_cash`: European cash index options,
  USD100 per premium/index point per contract; call/put strike; standard premium
  tick.05 below3, tick.10 from3. Only whole contracts. Official fixing is an
  underlying index level at the supplied fixing boundary, to two decimal places.
* `es_linear_cash`: USD50 per index point per contract; outright trade tick.25
  (USD12.50); whole contracts; explicit daily/final official settlement and
  separately supplied payment times. Zero and negative futures prices are
  arithmetically supported; exchange execution eligibility remains outside core.

AM versus PM is a declared profile, never inferred from a ticker. The host must
supply and evidence the actual series boundaries and holiday/calendar conversion.
This API does not validate a full exchange calendar. `contract_size` describes
index units per contract; `point_value` is money per point per contract. Conversion
uses the latter ONCE. Both are independently validated against the fixed profile.
They are not two multiplicative economic factors. Adjusted/multi-deliverable,
physical, American, futures-style options, spreads/complex-order tick exceptions,
inverse, swaps, FX and cross-currency products are excluded.

`quoted_money(q,p,f,currency)` computes contracts × points × currency/point/contract.
Quantity/price scales are8, Money6. Positive rational numerator/denominator factors
are exact; cancellation precedes checked 128-bit multiplication and ONE half-even
rounding to micro-money. Intermediate/result overflow fails; intermediate overflow
may conservatively reject an otherwise representable rational result. No floating
point, symbol parsing or pre-scaled equity is used. Fixed product factors100/50
may be supplied as equivalent fractions (normalized before digest). A new rational
product needs its own established profile; the general arithmetic is not a product
admission rule.

`price_change` proves `(to-from)*signed_contracts*point_value`, rounded once to
Money. For ES cash settlement, the separate normal-futures rounding rule first
converts EACH one-contract monetary endpoint to USD cents (ties away from zero),
subtracts, then multiplies whole signed contracts. This avoids per-lot/reset drift.
The separately reviewed ES increment will implement that convention for residual valuation and VM.
The raw `price_change` and settlement formula coincide for cent-exact endpoints;
sub-cent endpoints intentionally differ. SPX quoted premiums/ticks and official
fixing levels are exact under the supplied scope. Charge allocation reuses existing
LIFO half-even micro-money proportional splitting and residual exhaustion.

## Observations and selection

`Observation` owns ID, revision, instrument, contract digest, type, price, currency,
effective/known dates and provenance. Market, model, official settlement and
official fixing values remain distinct. `MarkPolicy{id,version,preference,max_age}`
is mandatory: choose the first allowed TYPE with eligible evidence, then greatest
`(effective_at,known_at)` within that type. Ties at the selected maximum are errors;
older ambiguous ties cannot affect a newer selected mark. There is no fallback
from model/market to official fixing/settlement for lifecycle processing. Fixing
and reset events reference EXACT observation ID/revision pairs. Observation IDs
are distinct from lifecycle/payment IDs and are never resolved as ledger records.

Missing/stale marks give null value/unrealized/total P&L plus retained diagnostics
under cost-based recognition. If `recognize_unrealized=true`, any replay boundary
missing required valuation fails `VALUATION_REQUIRED_FOR_RECOGNITION`; missing
values cannot silently reverse marked income to zero. Zero is an eligible explicit
mark. Completeness includes intermediate boundaries and age-expiration gaps.

Option premium value is signed contracts × premium mark ×100. It is separate from
cost carrying and economic unrealized P&L. The optional `notional` is null for
options because a premium quote supplies no underlying level. The explicit
`notional_exposure(contract,q,underlying_level)` computes signed underlying
reference exposure separately; its scalar does not become official price evidence
or enter premium P&L. ES signed notional is contracts × futures level ×50, separate
from residual carrying value. Negative quantities/values are signed; no presentation
or legal netting is implied.

## Reporting and recognition

`project(Inputs,Context,optional<ClosedAnchor>,Limits)` consumes one COMPLETE
account/custody/instrument/contract-revision pool. Inputs own Contract, fully
resolved existing Policy plus explicit `policy_known_at`, MarkPolicy, Rules,
contiguous immutable Revision prefix, observation prefix and actual Payment prefix.
Do not filter lots/trades by a display date first. This narrow API does not combine
accounts into a custody pool: adapters must route consistently or return exclusion.
A derivative revision cannot be changed underneath open lots; explicit migration
is required. `resolve_policy` is reused and the supplied result is revalidated,
not trusted by its digest alone. Policy knowledge must be evidenced by the host.

`derivative-rules/1` requires explicit accrual recognition and existing
capitalize-opening-charge option treatment. Options reuse OpenLot/LotAmounts/
LotRelief, LIFO splitting and charge netting without constructing EquityTrade.
Long/short positions, partial closes and crossing through zero are supported.
Terminal quantity is the SIGNED POSITION being consumed (positive long, negative
short); it cannot open an opposite position. Expiry and exercise both require
explicit official fixing and the European expiration boundary. Intrinsic
`max(0,fix-strike)` for calls or `max(0,strike-fix)` for puts creates terminal cash
obligation and relief. Partial terminal events consume each fragment once; excess
or duplicate consumption fails. A worthless terminal event records zero cash and
extinguishes the lot. Missing lifecycle evidence leaves a position with attention;
a zero market mark alone never expires it.

ES contract economics are validated, but this increment returns
`UNSUPPORTED_FUTURES_ACCOUNTING` from the accounting projection. The separately
reviewed ES increment will establish settlement-to-market resets and payment
boundaries. No futures event is accepted through this premium-option path.

Existing cash-basis recognition and within-period policy changes are intentionally
rejected (`DERIVATIVE_ACCRUAL_POLICY_REQUIRED`, `INTRAPERIOD_POLICY_CHANGE_UNSUPPORTED`).
The existing equity both-legs cash policy is not automatically extended to
non-premium/zero-obligation futures. All new chart/rule/profile versions are
separate from `cash-equity-chart/1`, `period-accounting/1` and their null fields.
No GAAP/IFRS/tax/fund financial-statement compliance is claimed.

## Actual cash, journals and history

`Payment` is immutable ACTUAL bank cash, with independent effective/knowledge dates,
sequence and provenance. Obligation payments bind stable economic identity;
collateral payments carry no trading-obligation link. Collateral deposits reduce
cash/increase margin-collateral and never trading P&L. Collateral can precede the
first trade. Refunds explicitly reference an earlier actual payment, have opposite
direction, follow its effective date and cannot exceed its magnitude cumulatively.
Correction/void does not erase cash. Inactive cause cash offsets suspense with
attention; excess/wrong-direction cash retains reconciliation diagnostics. Only
actual payment/refund changes cash. Scheduled payment times do not imply payment.

`Revision` supports originate, predecessor-linked correct and dated void, plus one
linked replacement after a void. Contiguous sequence/monotone knowledge, immutable
contract binding and chain shape are validated. Corrections recognize at recorded
time (new future payload waits until its effective date); predecessor persists
before recording. Dated void acts at its explicit effective date, including when
it precedes a corrected version's recording. Replacement waits until the later
of recording, own effective date and inherited void activation. A void cannot
resurrect at a deferred correction's later date. Whole dependent pool replay is
required; numeric negation is not lot undo. These are REPORT envelopes, not a new
canonical admission path. Hosts must preserve atomic paired-admission groups.

Periods are UTC `[from,until)`, nanosecond precision; endpoints strictly before
bounds, inclusive independent knowledge cutoff, same full prefixes at both ends.
Equal-time causes are atomic. Report owns opening/closing snapshots, economic and
recognized realized activity, optional total P&L activity (`delta(realized+U)`),
balanced boundary effects, cost/fair-value balances, actual cash, outstanding
obligations, collateral, lot relief and all input evidence. Accrual recognized
realized equals economic realized; marked recognition is an explicit rule.
Financial errors return retained code/ID; the host must retain failed request
inputs/diagnostics. Completeness is for this supported slice only. Optional option
notional absence is disclosed without invalidating supported premium accounting.

Journals use the existing validated JournalEntry and exact trial-balance reducer.
The appended `JournalEventType::derivative_projection` requires an explicit
period_adjustment boundary without trade dates. Existing enum values, equity/cash
construction, canonical v1/v2 bytes and native storage schemas remain unchanged.
Consumers using exhaustive journal-type switches must adopt the appended enum
before adopting this new operation. No derivative is disguised as equity.
`derivative-chart/1` deliberately reports signed carrying/obligation/control
balances; it does not classify them into existing equity financial statements.
Effects are differences of fully replayed snapshots, never regenerated snapshot
rows summed as activity. Stable effect IDs bind static conventions, boundary and
before/after causal state; adjacent periods contain disjoint matching effects.
Closing journals are snapshots, effects are activity. Never add both to P&L.

Every report hash pins exact definition, policies/knowledge, rules, all event/
observation/payment contents/provenance and cutoffs; full owned inputs accompany
it. A ClosedAnchor retains original inputs, bounds, knowledge, report ID, closing
state and WHOLE period history digest. Open-period queries verify immutable
prefixes and replay the retained period, including zero-effect evidence and
interior marks that are superseded by close. Changed evidence or historical results
fail `CLOSED_INPUT_EVIDENCE_CHANGED`/`CLOSED_HISTORY_EXCEPTION`. Source backdating
of an open-period recorded-time correction does not rewrite closed activity.
Overlapping closed queries require the original retained report. No current-period
repair, reopen or mutation is synthesized. Hosts retain the complete report AND
anchor atomically with their own close receipt, and retain exceptions. Existing
equity anchors cannot stand in for this new evidence contract.

## Execution and adapter adoption

Full replay is supported. Append a complete prefix and recompute for continuation;
retry/restore of the same inputs gives identical reports. No checkpoint-continuation
or partial-pool report merge is claimed. Safely schedule independent COMPLETE pools
in parallel with disjoint account/contract/cash ownership. Inside each pool,
revisions are accepted in sequence, resolved activity folds by recognition boundary
then sequence, and lot/terminal/reset order is retained. Merge only presentation
rows or validated disjoint journals with identical balance contract. Never sum
partial lots, time shards, VM resets or derivative snapshots as period income.

Default per-stream row cap10000, boundary cap4096, row-fold work budget2000000
are checked BEFORE endpoint replay. Work is `(boundaries+3)*(event+observation+
payment+1)` row-fold units, not CPU instructions; maps, charge vectors(up to64),
provenance/hashing and lot allocations add cost. Closed-history verification is a
separate bounded replay. Hosts must bound byte/metadata sizes before constructing
Inputs and choose measured lower budgets as needed. No production/distributed
throughput claim is made; see benchmark source and review evidence.

Enterprise/CLI/MCP owners confirmed existing explicit derivative exclusion must
remain. Before adapter enablement: introduce reviewed derivative DTO/admission
and persisted immutable envelope schema under ONE canonical/serialization owner;
freeze all input/result/anchor fields; pin reviewed OSS revision; add retention,
atomic version links, close protection, auth, export/paging schemas and direct
C++/CLI/MCP equivalence tests. Never map derivative totals into equity report
fields or silently combine supported and excluded products. Retain unsupported
input evidence and diagnostics. No adapter, live instrument, policy, deployment
or canonical-format migration is included here.

Installed consumer: configure/build/install Luca, then independently configure
`examples/derivatives` with `CMAKE_PREFIX_PATH` pointing at that installation. It
uses only public installed headers/target. Language-neutral fixtures and the
regression runner are under `tests/conformance/derivatives` and
`tests/unit/derivatives_test.cpp`.

## Authoritative specifications and scope record

Consulted2026-10-23; these sources establish the bounded conventions, not a general
exchange calendar or a live instrument feed:

* [Cboe SPX specifications](https://www.cboe.com/tradable-products/sp-500/spx-options/spx-specifications)
  and [Cboe SPX fact sheet](https://cdn.cboe.com/resources/spx/spx-fact-sheet.pdf):
  USD100 point value, standard ticks, European cash exercise and AM/PM fixing bases.
* [CME ES specifications](https://www.cmegroup.com/markets/equities/sp/e-mini-sandp500.contractSpecs.html)
  and [CME index notional/tick guide](https://www.cmegroup.com/education/courses/introduction-to-equity-index-products/discover-equity-index-notional-value-and-price):
  USD50 point value and outright.25/USD12.50 tick.
* [CME money calculation guide](https://www.cmegroup.com/clearing/files/CME-Money-Calculations-Futures-and-Options.pdf),
  normal-futures section: cash settlement variation and per-contract endpoint
  rounding to currency precision before signed quantity. Actual payment evidence
  and collateral remain independently supplied in this API.

Do not infer future series, special order conventions or official settlement
values from these documents. Hosts must verify their specific definition source.
