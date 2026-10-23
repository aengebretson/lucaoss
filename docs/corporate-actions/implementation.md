# Implementation sequence and trade-accounting handoff

This sequence is a proposed acceptance plan, not an authoritative planning-state
transition. No assignment journal, deployment manifest or current demo is changed.

1. Preserve the decided Luca-derived eligibility, automatic approved market ex-date
   recognition and gross same-currency scope. Resolve D-MARKETS coverage. Integrate the
   separate trade-accounting PRs first and inspect their final APIs. Keep this
   specification/fixture PR independently reviewable against the recorded bases.
2. Define pure C++ announcement, eligibility, entitlement and actual action-payment
   value contracts. Coordinate event variant/canonical serialization ownership.
   Implement versioned canonical bytes, strict decoders, source/alias bindings and
   explicit account/custody partitions. No new storage dependency in the kernel.
3. Add durable staged source reception with stable retry identities, asynchronous
   validation, retained dependencies/errors and immutable revisions. Decide one
   coordinated schema owner; this PR reserves no migration number. Test crash at
   receipt/work/admission boundaries and retry after lost acknowledgement.
4. Implement same-currency gross **cash-dividend entitlement** first by resolving an approved
   market/role rule and deriving eligibility from explicit internal positions, trades,
   actual securities deliveries and claims. Pin computed snapshots, reconcile
   external observations, emit no position/lot
   change, retain gross/rounding evidence and operational unpaid claim. Exercise ER-01–12 with resolved or retained blocked rule outcomes, plus CD-01,
   07, 09, 11–15, 18–19, 22–23 as their selected profiles allow.
5. Connect automatic market ex-date recognition and actual action payments to
   the reviewed accounting algebra. Support unpaid/partial/full/delayed receipts,
   separate cash and accrual views, unallocated receipts and actual refunds. Pass
   CD-02–06/13–14/17/20. A trade-record-only settlement API is insufficient: agree
   a typed entitlement obligation reference rather than fabricating a trade.
6. Add historical/revised result retention, fan-out manifests and period fences
   spanning ledger/action/eligibility/payment/policy cutoffs. Prove replay,
   restart/resume or explicit rebuild, account isolation, stale-preview rejection,
   exact journal balance and independent observation reconciliation. A future-dated
   void changes only results on/after its boundary; paid cash remains evidenced.
7. Add optional short/PIL, withholding/fees and FX one approved profile at a time
   (CD-08/10/21/24/25). No synthetic tax rates become production defaults. Document
   unsupported states in CLI/Enterprise results with retained error drill-down.
8. Add splits/stock dividends/fractions, identity history, then spinoffs/mergers
   with elections and approved basis inputs. Add calls/liquidations last. Prove
   per-lot principal and each charge conservation, atomic multi-security effects,
   dependent-trade ordering and source-to-result lineage before family admission.

## Inputs/effects required from the shared accounting integration

| Corporate-action supplies | Accounting integration needs to expose/agree |
|---|---|
| Typed entitlement revision + currency/signed gross and calculated market ex-date boundary | Explicit recognition mapping and distinct income/expense/receivable/payable classifications; no lot-selection inference |
| Actual payment/allocation evidence + gross/deductions/net, actual/known time | Cash movement exactly once, partial discharge and refund support, unallocated/surplus control; scheduled dates never prove payment |
| Complete policy resolution target/revisions/origins/digest and context | New independently versioned dimensions; existing cash/accrual preference alone must not silently activate dividends |
| Action, eligible snapshot, allocation and source chains | Effect/entry/line IDs with direct immutable references and independent reconciliation drill-down |
| Proposed revisions/void boundaries, selected prefixes, closed original dependencies | Admission and close fences across all contributing histories; retained blocked candidate; no silent historical rewrite |
| Security transformations and per-lot component allocation | Ordered atomic quantity/carrying effects; no fake equity trades or cash movements to mimic unsupported families |

Names in this document are semantic contracts, not additions to the current C++,
HTTP, CLI or MCP interfaces. Reporting remains a projection, never a second posting
store. Platform UI/authorization can follow supported kernel contracts; OSS gains
no UI. First implementation should deliver a synthetic end-to-end book and
independent tests, then obtain integration/release approval separately.
