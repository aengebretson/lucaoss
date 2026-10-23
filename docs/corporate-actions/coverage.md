# Current support and missing behavior

Inspected baselines: OSS `391c91e`, Platform `5112a9e`. These are pinned `main`
commits; pending trade-accounting implementation is not counted as current support.
The [57-row machine-readable matrix](coverage.csv) lists each synthetic scenario,
validation category, expectation status and decision dependencies. All action/rule
rows deliberately say **engine unsupported**. A passing arithmetic fixture is not
an implemented event, production journal or tax rule.

| Capability | Current baseline evidence | Missing corporate-action behavior | Fixtures / validation |
|---|---|---|---|
| Canonical events | `libs/ledger/include/luca/event.hpp`: cash movement, equity trade only; canonical v1/v2 readers | Action/eligibility/entitlement/payment types, strict versioned encoding | `current_support.cpp`: 12 action types rejected; generic cash is not dividend support |
| Durable staging | `libs/ingestion`: C++ receipts, async validation, per-record diagnostics, atomic admission | Action dependencies, revisions, eligible-account fan-out, action payment staging/retry | CD-13/14 are duplicate/conflict expectations; crash/restart execution remains future |
| Lifecycle/history | `lifecycle.hpp`, lifecycle/dated-void tests | Action/entitlement revision graphs, payment linkage, affected-result invalidation | CD-11/12/15–20/26: exact cutoff/result checks; engine replay/rebuild remains future |
| Lots | `accounting/lots.hpp`, `lots_test.cpp`: net custodian pools, LIFO long/short, separate charges | Ordered action transforms and per-lot component allocation, multi-security atomic effects | RA fixtures conserve supplied single-lot components; production multi-lot replay remains missing |
| Policies | `accounting/policy.hpp`, `policy_test.cpp`: versioned resolution, cash/accrual preference stored | Market/role/calendar, dividend recognition/classification/rounding policies and retained resolution | Every CD trace; ER rule IDs, D-MARKETS and other open dimensions |
| Journals | Bounded fixture cash-contribution/uncharged-buy projections, exact balances/trial-balance controls | Dividend receivable/payable/income/cash mapping; new trade journals belong to other session | CD-01–06/08/10/17/20/22/25 check aggregate expected balanced effects, not new journal API |
| Eligibility from announcements | No action or market-rule calculator | Company terms → rule/calendar/ex-date → internal holdings/trades/deliveries/claims → computed snapshot | ER-01–12: ordinary/large/weekend/late, excluded routes, missing calendar, external breaks |
| Participant vs customer | Existing lot/account bindings are not a depository/customer rights model | Explicit role, omnibus/beneficiary mappings, independent securities-delivery evidence, claim counterparties | ER-06/07/09/10; customer 110 vs participant base 100, no observation override |
| Cash dividends | No type/entitlement/payment support | All five stages, automatic approved market ex-date accrual, actual gross payment and cash view | CD-01–29, including short/withholding/PIL/FX conditional exclusions |
| Actual action cash | Existing settlement projection uses scheduled trade dates; other session adds trade-only actual cash | Entitlement-specific payment/allocation/refund and unallocated cash; never fake a trade | CD-03–06/12–14/16/17/20; no action payment ingestion claimed |
| Period controls | Schema4 ledger/policy close fences and retained cutoffs | Coordinated fences for action/eligibility/delivery/payment/rule histories; current-period adjustments need approval | CD-19 retains rejected candidate and original result; new store fence tests remain future |
| Splits / stock dividends | None; lot helper `split` is monetary allocation, not a corporate action | Rational transforms, fraction rights/delivery/cash-in-lieu, dependent-trade ordering | RA-01–04; conditional basis/fraction rules |
| Identity changes | Instrument IDs exist; no corporate-action alias-history workflow | Effective-dated aliases, explicit continuity vs successor identity | RA-05/06 |
| Spinoffs and mergers | None | Atomic legs, election/proration evidence, explicit carrying/tax-basis inputs | RA-07–11 |
| Calls/liquidations | None | Full retirement, finality/residual claims, continuing distributions vs partial redemption | RA-12–16 |
| External reconciliation | Position/cash observation algebras and tests | Entitlement/rate/claim/agent/customer allocation comparisons with source drill-down | ER-06 break; no matching engine or provider connector delivered |
| Platform | Manual/FIX equity staging, lot/policy/period screens; corporate actions listed planned | Authorized action-entry/review, fan-out/status/errors, immutable reports and observations | Paired Platform handoff is documentation/contract checks only; no screen/demo changes |

## Acceptance evidence boundaries

The C++ boundary test directly exercises supported trade/cash normalization and
fail-closed unsupported event kinds. Existing C++ tests establish compatibility of
current lifecycle, lot, journal-balance and reconciliation behavior. The Python
checks validate closed schemas, hashes, traceability and exact future expectations,
with adversarial mutations; they cannot prove engine posting/restart/parallelism.

Before claiming any future family supported, require actual event ingestion,
canonical round trips, deterministic full/repeated replay, checkpoint resume or
explicit rebuild, partition isolation, actual payment/delivery handling, closed
history protection, balanced journals and independent observations. Multi-lot
short/fraction allocation, special-route claims, batch crash recovery and concurrency
remain missing even where a representative single-case arithmetic fixture passes.
No deployment, browser acceptance, production performance or jurisdictional
compliance is implied. See [implementation sequence](implementation.md).
