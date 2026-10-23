# Cash dividends

Status: proposed contract with executable **fixture arithmetic**, not engine support.
[Decisions](decisions.md) distinguish fixture assumptions from approved product policy.

## Five distinct stages

1. **Announcement** records an issuer/agent assertion and revisions. It creates no
   account position, cash, receivable, payable or income by itself.
2. **Eligibility determination** calculates rights from Luca holdings/trades/deliveries/claims under an approved market
   rule and pins its computed snapshot and cutoffs. A record date alone does not establish the economic owner.
3. **Account entitlement** multiplies a signed eligible quantity by a rate, under
   explicit rounding and classification. It is an operational claim even if its
   accounting recognition is blocked or uses the cash view.
4. **Recognition** applies an explicitly resolved accounting rule using the automatically
   calculated market ex-date under the approved market profile. Retain the rule,
   calendar, date derivation and right evidence. Announcement and expected pay
   date do not prove recognition or payment.
5. **Actual payment** records independent bank/custodian confirmation and allocation.
   Scheduled dates are forecasts. Partial payments leave a balance; actual cash can
   exist before allocation or after a cancellation and must remain visible.

## Input contract

An announcement requires action/security IDs; source system/reference/revision and
immutable hash; revision/predecessor/status (`announced`, `amended`, `cancelled`);
announcement date, independently derived ex-date and any sourced market designation, record date with market timezone and boundary,
expected payment date, and economic effective/recorded/received timestamps.
Nullable unknown dates remain unknown and block rules that require them. Date-only
market facts must not be silently converted to midnight UTC. Revised dates retain
both the old and new values with their source. A special or late distribution can
have a different ordering of ex/record/pay dates; do not enforce a universal order.

Terms include dividend kind (ordinary/special/unknown), gross rate as exact
numerator/denominator, rate currency and per-unit denomination, currency precision,
rounding mode/scope, and any separate withholding/fee terms or unknown markers.
Unknown is not zero. A stated total is reconciled against calculated gross, never
used to hide a rate/quantity difference. Tax character is independent and nullable.

Each eligibility revision requires book/entity/custodian-account/ledger-account,
security/action revision, snapshot and mapping IDs, signed eligible quantity,
entitlement kind (`issuer_dividend`, `substitute_receipt`, `manufactured_payment`,
`market_claim`), rule version, observation boundary, knowledge/ledger/actual-settlement
cutoffs, trade/source/borrow/claim IDs, adjustments, derivation/version and evidence. Account
entitlement has its stable root ID and revision, quantity, signed rounded gross,
unrounded rational gross and rounding residual, currency, recognition evidence,
policy-resolution evidence and contributing source IDs.

Actual payments require payment and allocation IDs, agent/bank reference, source
hash, account/currency, actual effective and recorded timestamps, signed gross
amount discharged, withholding, fees, signed net cash and entitlement revision.
For positive receipts: `net cash = gross discharge - withholding - fees`. Negative
manufactured payments use signed gross discharge and cash; deductions need their
own signed-component contract before support. No guessed taxes or cross-currency
netting. A refund is a separate actual negative cash movement linked to the prior
payment; correcting a payment record must not invent cash movement.

## Eligibility and unsettled activity

**Decided:** Luca derives entitlement quantities and the accrual ex-date from
announcements, approved market rules and its own records, then reconciles against
custodian/DTC observations. It must be capable of calculating for a custodian or
participant operator. A computed eligibility snapshot is derived evidence; an
external snapshot does not replace the calculation. See the
[role-aware calculation contract](eligibility.md) and ER fixtures.

The trade implementation's actual settlements are **cash** evidence, not securities
delivery. Do not infer delivered quantity from payment, settlement percentage or
scheduled date. A future explicit quantity-delivery contract is required for any
settled-position calculation. Missing delivery/claim/rule coverage retains a blocked
result and a diagnostic rather than silently accepting an external quantity.

For the synthetic unsettled case, settled holdings `100`, eligible pre-boundary
purchase claim `+30`, and sale delivery obligation `-20` produce eligible `110`.
That formula is an explicit fixture assumption, not a universal record-date rule.
All three adjustments have their own record and source IDs. Scheduled settlement
dates do not prove delivery; failed settlement and due bills can change claim
ownership. Ex-date conventions are resolved and calculated under market/rule versions. The
participant base entitlement can be `100` (`32.000000`) while customer economic
rights plus claims total `110` (`35.200000`). CD-23/ER-07 label the participant
role explicitly; settled-only is not an optional override of the customer result.

A short `-40` position in a distinct custody pool has a potential manufactured
payment `-12.800000` at rate `0.32`. It is not negative issuer dividend income for
tax purposes by default. Borrow terms/agent obligation evidence and explicit
classification are required. Loaned long shares may receive substitute payments;
retain that kind separately, do not count both issuer and substitute receipts for
the same right. The fixtures do not derive lending rights from short quantity.

## Explicit fixture recognition profiles

These are proposed portable profiles, not GAAP/IFRS/tax labels. They keep LIFO
independent. Ex-date recognition is the selected product direction; its concrete
market/role profile must be approved before implementation support. For `125 × 0.32 = 40.000000`, no position or lot quantity changes.
All table entries are action-only deltas; existing book cash/inventory is excluded.

| Stage | Accrual profile A | Cash profile C |
|---|---|---|
| Announcement/eligibility | No journal; operational entitlement may be known | Same |
| Approved ex-date reached, unpaid | Dr dividend receivable 40 / Cr dividend income 40 | No journal; operational unpaid claim 40 |
| Actual gross receipt 16 | Dr cash 16 / Cr receivable 16; receivable remains 24 | Dr cash 16 / Cr income 16; operational claim remains 24 |
| Remaining receipt 24 | Dr cash 24 / Cr receivable 24; total income 40 | Dr cash 24 / Cr income 24; total income 40 |
| Scheduled payment date only | No cash effect | No income or cash effect |

Short conditional profile: on right/obligation establishment, Dr manufactured
payment expense 12.8 / Cr payable 12.8; actual payment Dr payable / Cr cash.
Cash profile recognizes that expense only as the corresponding gross obligation
is actually discharged. Fixture `income` uses negative sign for this expense to
simplify control equations; production accounts must distinguish the classification.

Conditional withholding profile: gross 40, supplied tax deduction 6, fee 1, net
cash 33. Accrual at right: Dr receivable 40 / Cr dividend income 40. At payment:
Dr cash 33, Dr explicitly assumed recoverable withholding asset 6, Dr fee expense 1,
Cr receivable 40. Alternative nonrecoverable withholding expense and uncertain
reclaim/suspense are unresolved D-WITHHOLDING. No rate or reclaim right is inferred.
Cash recognition of gross including deductions needs an explicit discharge and
classification rule; the initial arithmetic fixture covers accrual only.

Fixture balance control by currency:
`cash + receivable + withholding asset - payable - unallocated/refund liability
 = signed income - fee expense`.
Operational unpaid entitlement remains outside cash-profile journals. It is not
an invisible GL receivable. Cross-currency recognition/revaluation needs separate
balanced functional-currency entries and explicit rates; FX fixtures block posting.

## Revisions, payments and history

| Situation | Required behavior and fixtures |
|---|---|
| Identical request retry | Same receipt and effect identities; one entitlement/payment; CD-13. |
| Same key, changed bytes/terms | Retained conflict, original unchanged; CD-14. Different source keys still need qualified action/payment identity matching; no fuzzy auto-merge. |
| Rate/date amendment | Append linked revision; selected current result changes, earlier knowledge result stays reproducible; CD-11. Date-only amendment may move recognition and invalidate affected caches. |
| Quantity correction | New eligibility/entitlement revision; do not overwrite snapshot. CD-12 leaves actual cash 40 and refund liability 1.6 after corrected right 38.4. |
| Cancellation before payment | Extinguish right only with authoritative cancellation evidence, retain history; CD-15. |
| Cancellation after payment | Conditional D-CORRECTION fixture reverses the right and retains cash 40 with refund liability 40; no fabricated refund; CD-16. If recoverability/legal obligation is unclear, block classification and retain suspense. |
| Partial/delayed payment | Independent allocations with unique IDs, outstanding gross tracked; CD-04/05/06/20. Sum allocations cannot exceed entitlement without explicit surplus/suspense. |
| Payment before entitlement | Cash to unallocated liability pending identification; later allocation reclassifies it, with no second cash event; CD-17. |
| Refund/payment correction | New signed actual event referencing prior payment; CD-20 refunds 8, reopens unpaid 8, cash 32. Corrections retain original cash evidence. |
| Late information | Economic date and recorded cutoff independent; CD-18 shows unavailable then known result. Re-evaluation never pretends the late source was known earlier. |
| Closed period | Retain candidate/error, preserve closed result and cutoff; CD-19. Backdated correction blocks. Alternative explicit current-period adjustment needs accounting approval and link to original, never silent rewriting or automatic reopening. |
| Missing policy/FX rule | Retained blocked result and operational facts, no claimed posting expectation; CD-21/22/25. |

A source cancellation is not necessarily proof that a legal right ceased; require
its authority/classification before applying the cancellation profile. Dated voids
use an explicit boundary, maintain pre-boundary results and never imply a cash
refund. Period protection covers original and proposed effective dates, eligibility
revisions, payments and policy changes. Closing must fence action/eligibility/payment
and policy prefixes as well as ledger history; integration design belongs to the
trade-accounting owner. Historical retained result IDs are never overwritten.

Every effect must trace through allocation → entitlement revision → eligible
snapshot/rule/contributing trades → action revision → source, with policy target,
selected revision IDs, field origins, digest and cutoffs. Actual cash additionally
traces to bank/agent evidence. Income must not trace merely to a scheduled date.

## Selected first-slice scope

Implement Luca-derived gross dividends in one currency with automatic approved
market ex-date accrual and actual-payment cash recognition. Deductions,
manufactured/substitute payments and FX examples are future conditional tests and
must not become accepted production inputs in that first slice. Announcements,
market-rule revisions and external reconciliation observations retain independent
sources and versions. The fixture `right_established_at` is a **derived output** of
the market ex-date rule (04:00 UTC for the October New York start-of-date examples),
not an operator-supplied approval date. The chosen start-of-date boundary is an
explicit fixture convention pending D-MARKETS; production timezone rules are inputs.
