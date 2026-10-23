# Other action contracts and representative fixtures

All families are future unsupported engine behavior. Universal identity, staged
validation, cutoffs, revisions, reconciliation and lineage follow the
[common contract](README.md). The portable RA fixtures check exact mechanical
arithmetic; explicit basis assumptions are examples, not tax or production policy.

## Stock dividends and forward/reverse splits

Require action/security revision, exact integer ratio numerator/denominator and
whether the ratio means **additional** or **replacement** shares. A stock dividend
adds units; a split replaces units. Retain effective/record/ex dates separately,
ordering against trades, pre-action lots, delivered units, fractional rights and
actual delivery evidence. Position transformation and delayed delivery obligation
must not double-count the same units. Short obligations transform separately under
explicit loan terms. LIFO opening order remains intact; lot IDs acquire transformation
lineage, not arbitrary acquisition timestamps. Per-lot principal, commissions and
fees remain conserved unless an explicitly supplied allocation transfers them.

RA-01: `125 → 250` in 2:1 split, carrying amount 1000 unchanged.
RA-02: reverse 1:3 of 10 gives exact `10/3`, delivery 3 and fraction `1/3`.
At supplied agent price 9 the fractional gross is 3; basis 90 allocates 81 to
delivered shares and 9 to the fractional right. Cash-in-lieu is a separate actual
sale/payment record, not cash on the split date. RA-03 retains the fraction
unpaid when price is unknown. RA-04: 10% stock dividend on 125 adds 12.5 (delivery
12, fraction .5) for total economic 137.5; illustrative carrying allocation
1000/110 to added units is not selected silently. Unknown basis blocks that result.

Fraction distribution scope (account vs lot), precision, signed rounding, residual
recipient and cash-in-lieu fees need approval. Never round away a fraction or
assume its price. A before/after quantity check does not prove per-lot basis or
short borrowing obligations correct.

## Names, symbols and CUSIPs

RA-05 changes name, symbol and a synthetic CUSIP alias with quantity/basis unchanged
under an explicit continuity approval. The stable security ID remains unchanged;
old aliases retain effective intervals and mapping evidence. Validate overlapping
aliases and venue/namespace qualification. CUSIP change alone does not prove legal
identity continuity; RA-06 blocks an unapproved identity assumption. A merger into
a legally different security uses successor identity and a transformation, never
just relabels the original security. CUSIP examples are synthetic labels, not a
claim of valid assigned identifiers. Reference changes create no cash or income.

## Spinoffs and stock/cash/mixed mergers

Require predecessor/successor security IDs, per-input output components, exact
ratios, actual delivered quantities, election options/deadline/timezone, submitted
instruction revision, acceptance/withdrawal evidence, supplied default/proration,
consideration cash, fractions and basis-allocation inputs. Allocation inputs name
method/version, valuations/currency/date, approved weights or explicit per-lot
amounts, original lot IDs and rounding/residual recipient. Reject missing weights,
weights that do not sum exactly to one and unexplained basis loss. Economic carrying
value, proceeds and tax basis remain distinct. Tax qualification and holding-period
rules are unresolved. No automatic fair values, tax gain or exchange ratios.

RA-07 spinoff retains parent 100, adds child 25 at 1:4; supplied basis 1000 splits
800/200. RA-08 stock merger replaces 100 old with 50 new at 1:2, explicitly carries
1000 into successor. RA-09 mixed election replaces 100 old with 25 new and 600 cash;
provided basis allocation 400 to shares, 600 to cash component. Actual cash is zero
until confirmed, even when consideration owed is 600. RA-10 all-cash merger retires
100, owes 1200 and explicitly relieves 1000 carrying amount: illustrative gain 200,
subject to recognition policy. RA-11 missing election/default or allocation blocks
basis/journals; offered consideration remains visible. Issuer changes/cancellation
must atomically rebuild all dependent securities per account, never one leg only.

## Full calls and final/partial liquidation

Full call requires instrument terms, callable quantity/denomination, supplied
principal price, premium, accrued interest and fees as separate components, notice,
redemption/finality date and actual payment. Do not treat full call as ordinary
trade or combine coupon/interest with redemption principal. Initial fixture is
cash-equity-like units, not proof of bond amortization/interest support.

RA-12 full call: 100 units fully retired; explicit carrying 1000, principal 1000,
premium 20, fee 0, interest 0, consideration 1020, illustrative excess 20.
RA-13 final liquidation: 100 units retired on finality evidence, consideration 900,
explicit carrying 1000, illustrative loss 100. Multiple installments retain one
claim lineage and no repeated retirement. Unknown final proceeds stay unknown.
RA-14 partial liquidation: 100 units continue, supplied cash distribution 200 and
explicit carrying reduction 200 leave carrying 800; no ordinary income is assumed.
RA-15 partial redemption: explicitly retires 20 of 100 units, consideration 220,
carrying relief 200, remaining quantity 80/carrying 800. This is a separate mechanic
from a continuing partial distribution. LIFO selects any relieved units only under
an approved redemption rule. RA-16 missing allocation/finality blocks classification.

Retain residual rights and contingent proceeds after retirement. Final liquidation
is not an instruction to delete historical security/lot records. A later payment,
correction or legal claim appends evidence; closed history remains protected.
