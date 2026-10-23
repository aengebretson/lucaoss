# Corporate-action fixture package v1

All data are synthetic. **All 57 action/rule scenarios are unsupported by the current
engine.** Passing these checks proves internal contract/arithmetic consistency,
not corporate-action posting, storage, replay, tax or market-rule implementation.
Use [the specification](../../../docs/corporate-actions/README.md) as the semantic
contract and [coverage matrix](../../../docs/corporate-actions/coverage.md) to see
existing support and implementation gaps.

- `cash-dividends.json`: 29 scenarios, including paired cash/accrual views,
  unsettled claims, short/substitute rights, withholding/fees alternatives,
  duplicate/conflict expectations, linked rate/quantity corrections, cancellation,
  refund, late knowledge, dated void, closed-period rejection and blocked policies.
- `eligibility-rules.json`: 12 Luca-derived ex-date/eligibility cases covering
  ordinary/large/late events, finite calendars, unsettled customer claims versus
  participant positions, ex-date purchases and independent reconciliation breaks.
- `other-actions.json`: 16 mechanical transformation/basis examples, fractions,
  identity continuity, elections, mergers, calls and liquidation distinctions.
- Adjacent `*.schema.json`: closed JSON Schema 2020-12 shapes with exact numbers
  as strings. Unknown members, floating amounts, missing evidence and invalid
  dates fail. The dependency-free checker implements only the keywords actually
  used here and rejects unknown schema keywords. External JSON Schema validators
  can also consume the schemas; semantic checks remain necessary.
- `validate.py`: exact rational and half-even arithmetic, component controls,
  balance-line checks, revision/cutoff checks, source hashes, resolved-policy
  digest/target/lineage, quantities and supplied basis conservation. It validates
  fixture-declared results, not arbitrary incoming corporate-action events.
- `../test_corporate_actions_contract.py`: adversarial schema, arithmetic,
  identity, lineage, policy, cutoff, election and finality mutations. Existing
  CTest conformance discovery runs this file with no root build-file change.
- `current_support.cpp`: checks actual current C++ trade/cash normalization and
  verifies corporate-action type rejection leaves the accepted ledger unchanged.
  Run its standalone build below; it does not post the future JSON fixtures.

From OSS root:

```sh
python3 tests/conformance/corporate-actions/validate.py
python3 -B -m unittest discover -s tests/conformance -p test_corporate_actions_contract.py -v
cmake -S tests/conformance/corporate-actions -B /tmp/luca-ca-boundary
cmake --build /tmp/luca-ca-boundary --parallel 2
ctest --test-dir /tmp/luca-ca-boundary --output-on-failure
```

Use a new build directory for each candidate. The normal C++/conformance suite
remains required to establish compatibility with existing behavior. No network,
secret, production account or live demo is required.

## Reading expected values

Cash-dividend amounts are action-only deltas, USD unless the case explicitly blocks
FX. `income` is signed revenue (negative for the explicitly assumed short expense),
not a tax label. `balance_lines` are aggregate expected control balances, **not an
ordered production journal**. They let the validator check both balance and exact
state effects without imposing the other session's entry/line API. Operational
`gross/unpaid` stay separate from recognized GL receivables/payables. A blocked
posting has known operational quantities/amounts but no invented accounting result.

`resolved_inputs` declares selected records; checks verify their eligibility at
explicit economic/knowledge cutoffs and inclusion of predecessor/source evidence.
Snapshots before and after amendments/late information/dated cancellation are
separate immutable expected views. These checks are not a general lifecycle
resolver or proof of crash/restart/checkpoint equivalence. Those acceptance tests
are listed for future implementation in the coverage matrix.

Other-action quantities describe projected legal/mechanical entitlements and
explicit retirement assumptions. No physical delivery or payment is confirmed in
these cases. Fractional rights use numerator/denominator strings; an unknown price,
allocation, election or finality remains null/blocked. Carrying allocations are
supplied, single-lot examples with separate principal, commissions and fees; multi-lot charge conservation needs additional
production-engine tests before support can be claimed.

`specified_fixture` means the example's assumptions fully determine its arithmetic;
it does not approve product policy. `conditional` additionally identifies material
open decision IDs. The decided direction is Luca-derived eligibility and automatic approved market
ex-date accrual, reconciled externally. The initial scope is same-currency gross
dividends; conditional deductions, payments in lieu and FX remain outside it.
Source SHA-256 hashes verify embedded synthetic payload bytes. Policy digests use
sorted compact JSON for this fixture contract only; production canonical encoding
and resolver digests must use their own versioned contracts.
