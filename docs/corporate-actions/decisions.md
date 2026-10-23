# Decisions and authoritative references

The user decided on 2026-10-23:

- **D-ELIGIBILITY: Luca calculates eligibility** from company announcements,
  versioned approved market rules and its own positions/trades/deliveries/claims.
  External custodian/DTC records are reconciliation observations, not overrides.
  Design for Luca operating as a custodian or calculating participant allocations.
- **D-RECOGNITION: automatic approved market ex-date rules** drive the accrual
  recognition date. Retain the derivation and exceptions, not an operator-chosen
  date for each event. Cash views still require actual payment evidence.
- **Initial scope: same-currency gross cash dividends**. Block withholding, fees,
  payments in lieu and FX until their treatment is separately approved.

Concrete market/role/calendar coverage remains **D-MARKETS**. The ER fixture rule
is synthetic US ordinary-equity coverage for design validation, not an approved
live market configuration. Production rule activation requires authoritative
market scope, effective dates and exception handling. Fixture assumptions do not
approve any remaining tax or accounting treatment. Existing recognition settings
alone cannot encode these dimensions. Retain resolver target, selected revisions,
field origins, effective/knowledge cutoffs and digest with each result.

| ID | Decision and concrete alternatives | Fixture dependency |
|---|---|---|
| D-ELIGIBILITY (decided) | Luca-derived eligibility; externally supplied counts remain observations. Computed snapshots are immutable derived evidence. | ER-01–12, CD-07/23. |
| D-RECOGNITION (decided) | Automatic approved market ex-date rule with derivation evidence. Missing/ambiguous market rules block recognition. Do not infer income from scheduled pay date. | ER-01–12; CD-01–06, 18. |
| D-MARKETS | Select initial markets/security classes and roles, market calendars/timezones, rule precedence, cum/ex exceptions and late-designation authority. US fixtures are illustrative, not global conventions. | Every ER case remains conditional on a real approved market profile. |
| D-PIL | Classify manufactured-payment expense and substitute receipts under contract/jurisdiction; alternatively block lending/short entitlements initially. Need lending evidence, not only negative position. | CD-08/24. |
| D-WITHHOLDING | Recoverable tax asset, nonrecoverable expense, or unresolved reclaim suspense; require evidence, no inferred rate/treaty eligibility. | CD-10. |
| D-FEES | Separate expense, different approved capitalization, or unsupported. No deduction hidden in gross rate. | CD-10. |
| D-FX | Retain original-currency entitlement and actual FX conversion as separate facts; or approved functional-currency recognition/revaluation rule. Need rate source, direction, effective/known times and rounding residual allocation. | CD-21. |
| D-ROUNDING | Account-entitlement half-even at six money decimals (fixture); alternatively agent cash precision and explicit residual distribution. Per-lot rounding vs aggregate rounding can differ. | CD-09, RA-03. |
| D-CORRECTION | In open periods regenerate current projection with original retained; after payment classify refund obligation/suspense explicitly. Do not equate source cancellation with legal extinction. | CD-11/12/15/16/20. |
| D-CLOSED | Reject affected closed history (first-slice proposal); separately authorized current-period adjustment with historical linkage; reopening remains unsupported. | CD-19. |
| D-FRACTIONS | Retain exact fractional right; deliver supported fractional security units; or agent sale/cash-in-lieu with actual price/fees/allocation evidence. Truncation alone is not settlement. | RA-02–04. |
| D-BASIS | Explicit approved per-lot allocation and valuation inputs; or block basis/journals while showing known quantities. Jurisdiction-specific tax basis/holding periods are separate from economic carrying values. | RA-05–11. |
| D-ELECTION | Approved timely instruction; explicit agent default if no election; or block missing/late/conflicting instructions. Proration is supplied actual outcome, never guessed. | RA-08/09. |
| D-LIQUIDATION | Distinguish full retirement, continuing partial distribution and quantity redemption. Explicit proceeds/basis/income/capital classification; finality proof required. | RA-10–12. |

## Sources checked 2026-10-23

These constrain design claims; they do not select Luca's policies. Fixture dates,
quantities and tax deductions are synthetic, not provider records.

- [FINRA Rule 11140](https://www.finra.org/rules-guidance/rulebooks/finra-rules/11140),
  paragraphs (a)–(c), supplies scoped ex-date designation, different treatment for
  large distributions and late information. Therefore Luca resolves a scoped
  rule/calendar and authoritative designations rather than using one global formula.
- [SEC Investor.gov dividend dates](https://www.investor.gov/introduction-investing/investing-basics/glossary/ex-dividend-dates-when-are-you-entitled-stock-and)
  explains record/ex-date ownership distinctions and special distributions. This
  supports separate eligibility evidence, not a universal unsettled-claim formula.
- [DTCC distributions lifecycle](https://www.dtcc.com/products-and-services/asset-services/corporate-actions-processing/distributions)
  separates announcements, entitlements, instructions and cash/security allocations.
  Luca uses those as distinct stages, without claiming DTCC integration.
- [IFRS 9, 5.7.1A, official 2024 issued text](https://www.ifrs.org/content/dam/ifrs/publications/pdf-standards/english/2024/issued/part-a/ifrs-9-financial-instruments.pdf?bypass=on)
  states conditions concerning an established right, probable economic benefits
  and reliable measurement for dividend profit/loss recognition. The official
  search excerpt was available; opening the full text redirected to a subscription
  page. No full-standard review or IFRS-compliance conclusion is claimed. Profile A
  explicitly assumes satisfaction of its approved recognition criteria; no claim
  equates declaration or ex-date with those criteria in every jurisdiction.
- [IRS Publication 550 (2025)](https://www.irs.gov/publications/p550) distinguishes
  substitute payments and discusses distribution/basis treatments. It illustrates
  why tax classification cannot be inferred from a generic dividend label. Its
  US-specific rules are **not implemented** or adopted as global fixture policy.

No tax rate, withholding entitlement, merger tax qualification, basis rule or
holding-period carryover is inferred from these sources. Accounting-policy and
market-rule registries should be independently versioned, cited and approved.

- [DTC Distributions Service Guide, May 6, 2026](https://www.dtcc.com/-/media/Files/Downloads/legal/service-guides/Service-Guide-Distributions.pdf),
  pp. 25–29 and 33: separate participant-position entitlements, interim adjustments,
  receipt/allocation and failed-deliver-order tracking with route exclusions.
  This motivates role-specific calculations and independent delivery evidence.
  It does not certify the synthetic ER rule as a complete DTC implementation.
