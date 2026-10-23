# Luca-derived eligibility and market ex-date calculation

The user selected Luca as the calculation authority: a company announcement plus
approved market rules and internal activity produces entitlements. External
custodian/DTC quantities and amounts are separate observations used to reconcile.
The design must work when Luca operates the custodian calculation itself.

## Separate three layers

1. **Announcement and market terms:** issuer/paying-agent assertion, stable action
   and security identity, rate/currency, record/pay dates, event classification and
   any authoritative exchange/market ex-date designation. Luca resolves a versioned
   rule for the security, market, role, effective date and knowledge cutoff.
2. **Position and claim rights:** Luca derives participant/registered-holder rights
   from actual securities positions and deliveries; customer economic entitlements
   also track qualifying trades and contractual claims. These are different results
   with different counterparties and obligation kinds, not competing numbers in a
   single field. Cash settlement alone does not establish securities delivery.
3. **Allocation and reconciliation:** calculate account amounts, preserve exact
   rights/components and compare independently with issuer funds, participant and
   customer totals, external allocations and actual cash. Differences create breaks;
   observations never overwrite Luca's ledger or derived rights.

DTC's [May 2026 service guide](https://www.dtcc.com/-/media/Files/Downloads/legal/service-guides/Service-Guide-Distributions.pdf)
pp. 25–29/33 describes participant entitlements, interim position adjustments,
fund allocation and failed-delivery tracking. Its procedures differ by event and
route. The Luca contracts below are proposed designs informed by those distinctions,
not a reproduction of DTC's full operational rules or a claim of DTC compatibility.

## Required derivation inputs and evidence

A market-rule definition requires jurisdiction/market/venue, instrument class,
participant vs customer role, rule ID/version, source citation, approval, effective
interval and knowledge sequence; exchange and settlement calendar IDs/versions;
timezone and boundary convention; distribution classification threshold and its
valuation evidence; exception/late-information rules and authoritative-designation
precedence. Calendar coverage gaps block the calculation. Preserve original
company dates and independently calculated dates; a sourced conflicting designation
creates an explicit rule/source conflict, not a hidden override.

Position inputs require an opening anchor with ledger-prefix/source evidence,
subsequent trades/corrections/voids, scheduled and **actual quantity delivery** facts,
account/custody mappings, stock-loan/borrow/return events, due-bill/counterparty claims,
and partition cutoffs. A securities delivery has security, signed quantity, delivering
and receiving parties, trade/transfer link, actual effective and known times, route,
source, revision and reversal evidence. It is not the trade session's cash settlement.
Validate security/account/prefix matches and retain unmatched transfers.

Derived output includes record-date settled base, each interim adjustment, separate
claim receivable/payable quantities, customer economic right, ex-date/recognition
date, rule/calendar/source lineage and unresolved requirements. Never count an
unsettled economic right both as issuer income and again as new claim income when
cash arrives. Reclassifying collection counterparty is not a second entitlement.

## Illustrative US rule fixtures (not production approval)

[FINRA 11140](https://www.finra.org/rules-guidance/rulebooks/finra-rules/11140)
(b) differentiates ordinary distributions from distributions at least 25% of value;
(c) addresses late information. ER-01–05 use a narrowly scoped synthetic calendar
and this structure: ordinary timely record-business-day ex-date; preceding business
date for a nonbusiness record date; first business date after payable for the large
case; sourced designation required for late information. ADR/foreign and unmodeled
exceptions must not use this default. Actual exchange designation/rule scope must
be resolved before activating a real profile. Market-value threshold inputs are
explicit, not a live price lookup. Rule changes never rewrite earlier results.

ER-06/07 demonstrate a single economic book viewed at two roles. Settled base is
100; pre-ex failed purchase +30 and failed sale -20 imply customer right 110. Luca
calculates base issuer amount 32 and net claim 3.2, totaling customer amount 35.2
at rate .32. The participant base can reconcile to an external 32 while the customer
book needs its claims explanation. An external customer quantity of 100 creates
an amount break -3.2; accepting that observation as the new right would conceal it.
This exact formula is a declared fixture profile, not a rule for all routes/markets.

ER-08 excludes an ex-date purchase from the current right. ER-09 includes a
qualifying interim delivery once. ER-10 keeps a scheduled but undelivered purchase
out of participant settled base and exposes its separate claim. ER-11 blocks the
ordinary route rule for stock loan; repo/CNS/free/inter-depository and other
unmodeled routes likewise require their own approved profile. ER-12 blocks missing
calendar coverage. These tests validate arithmetic and conditional expectations;
no engine or live rule registry is implemented.

## Custodian/depository operating contracts to implement later

Pin all accounts in an allocation manifest, distinguish participant/omnibus
accounts from customer beneficiaries, and require an explicit mapping rather than
assuming one ledger account equals one DTC participant. Sum component rights and
per-account amounts by action/currency/kind, retaining rounding residual allocation.
Reconcile customer allocations to participant issuer rights plus claims and other
explicitly classified components. Do not enforce equality across different roles
without this bridge. For a depository-like operator, input registered participant
positions and agent funding; for a custodian-like operator, input custody positions,
customer subledgers and claim records. Same C++ transformations, different explicit
roles and partitions. Neither role implies a license/operational qualification claim.

Announcements can establish forecast obligations before funding. Actual issuer
funding, participant allocation and customer payment are separate cash movements;
link them and conserve funds, without recognizing each transfer as new income.
Same-account internal transfer must not double count book cash. Partial funding,
unallocated funds, account errors and blocked beneficiaries retain per-record state.
Don't allocate unavailable funding as paid. Allocation and reconciliation require
stable counterparty/account references, role and funding lineage.

Accrual recognition is automatic on the approved profile's ex-date once the selected
rule and required eligibility inputs resolve. A derived recognition date is distinct
from the date Luca learned it. Late information recomputes only open results;
closed-period protections remain in force. If the profile cannot resolve the event,
retain forecast data and `MARKET_RULE_REQUIRED`/specific dependency errors instead
of falling back to an operator date or a custodian quantity. Cash reporting still
requires separately confirmed payment; ex-date is never proof of funding.
