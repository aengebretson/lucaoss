# Narrow accounting-foundations contract

Status: executable design contract with public C++ journal value types and the
bounded trade-date and settlement-date journal projections. This document and
the portable fixtures define the initial O4 journal semantics. The implementations
cover `fixture.trade-date.v1` and `fixture.settlement-date.v1`; they do not claim
that LUCA implements all of O4,
or that either fixture policy is suitable for production, GAAP, IFRS, tax, NAV,
regulatory, or client reporting.

## Boundary and inputs

Accounting is a deterministic projection after lifecycle resolution:

```text
immutable source evidence
        -> normalized lifecycle records
        -> O2 resolved active-event set
        -> versioned fixture accounting policy
        -> journal entries and balances
```

The accounting projection consumes the active records selected by the O2
lifecycle contract. It does not choose a correction head, cancel a record,
reinterpret a reversal, or mutate an earlier record. A correction produces a
new result from a new resolved input set. A reversal remains a separate active
economic event linked to its target. The original and correction evidence stay
in lineage even when only the correction is active.

Every evaluation supplies the same explicit context used by the portfolio
projections:

- `recorded_through`, an inclusive knowledge cutoff;
- `economic_as_of`, an inclusive economic cutoff; and
- `settlement_as_of_date`, an independent date-granular settlement cutoff.

The result envelope also fixes `engine_version`, `projection_version`, the
policy identity and version, the lifecycle-resolution identity and version,
and the complete selected input identities. A change to any of those values
creates a distinct result and rejects prior-result reuse. Historical source and
lifecycle records are never edited to make the result agree.

The fixture vocabulary follows the O5 transformation boundary: lifecycle
resolution is an ordered fold, journal mapping is a versioned map over its
resolved output, and balance production is an exact reduction grouped by
account and currency. Ordered replay of resolved records uses
`(effective_at, acceptance_sequence)`. Journal presentation uses
`(recognized_on, acceptance_sequence, phase_ordinal, journal_entry_id)`.
Nothing in this contract permits arbitrary reordering across dependent
lifecycle records.

## Closed portable shapes

`tests/conformance/accounting-foundations/valid-cash-equity-lifecycle.json`
uses `luca.accounting-foundations.v1`. All objects are closed: missing or
unknown members fail validation. Decimal quantities and monetary values are
JSON strings; binary floating point is not accepted.

### Lifecycle input and evidence

A source record contains its stable `source_record_id`, source identity,
observation time, and payload hash. A lifecycle record contains stable record
and economic-event identities, account, action, recorded time, acceptance
sequence, economic effective time, causal references, immutable provenance,
and a closed cash-movement or equity-trade payload.

An evaluation does not ask accounting to resolve those records. Its
`resolved_inputs` name the active record, full causal record lineage, economic
event, source-record lineage, and optional reversal target supplied by the O2
resolution. These declarations are checked for referential and immutable
lineage consistency, but the accounting validator is deliberately not a
second general lifecycle resolver.

### Journal entry

A journal entry has exactly these concepts:

- stable `journal_entry_id`;
- policy identity and version;
- source event type and active lifecycle-record identity;
- economic `effective_at` and knowledge `recorded_at` of that active record;
- `recognized_on` and a phase (`immediate`, `trade_date`, or
  `settlement_date`);
- phase ordinal for deterministic presentation;
- explicit trade-date and supplied-settlement-date context (nullable only for
  a cash contribution);
- full lifecycle-record, economic-event, source-record, and reversal lineage;
  and
- two or more closed journal lines.

The economic and recorded *evaluation cutoffs* live in the result envelope and
are referenced by its selected journal-entry identities. They are result
inputs, not properties that can mutate a reusable journal-entry fact.

### Journal line

A line contains a stable `journal_line_id`, its parent entry identity, account
identifier, `debit` or `credit` side, positive exact amount, currency, and the
same explicit lineage as its entry. Lineage is intentionally repeated rather
than recovered through storage or network access. A line cannot use a negative
amount to change side, cannot be zero, and cannot exceed the six-decimal money
scale.

Each entry balances independently in exactly one currency. Cross-currency
debits and credits do not offset. Currency totals retain all contributing entry
identities; account balances retain all contributing line identities. A
balance is therefore traceable through entries and lines to lifecycle records,
economic events, and source evidence.

### Public C++ value boundary

`<luca/accounting/journal.hpp>` supplies the closed `JournalLine` and
`JournalEntry` value types. `<luca/accounting/trade_date_projection.hpp>`
supplies the concrete fixture trade-date projection. Both are also included by
`<luca/ledger.hpp>`, and the `luca::ledger` CMake target installs the headers
through the existing public header-directory rule. This is a concrete policy
implementation, not a generic policy interface.

Factories construct the supporting policy identity, date, settlement context,
and ordered lineage values before constructing a line or entry. The entry
factory returns either one completely valid value or a `JournalError`; it does
not expose a partially constructed entry. Construction checks:

- non-empty stable entry, line, parent, account, active-record, policy, version,
  recognition-rule, and lineage identities;
- non-empty record, economic-event, and source-record lineage, retaining caller
  order and optional reversal identity;
- positive `Money` line values, at least two lines, unique line identities,
  matching parent and repeated lineage, and one entry currency;
- structurally consistent cash/equity date and phase context; and
- exact, overflow-checked equality of debit and credit `Money` totals.

No constructor accepts a binary floating-point monetary amount. The factory
sums declared lines with the existing six-decimal `Money::add` operation and
retains the resulting exact debit and credit totals on the entry. The stable
C++ diagnostic categories are `schema_shape`, `duplicate_identity`,
`policy_context_mismatch`, `unbalanced_entry`, `mixed_currency_entry`,
`invalid_line_amount`, `lineage_missing`, `lineage_mismatch`, and
`arithmetic_overflow`. Diagnostic messages provide detail but are not stable
matching keys.

The value types own their lines and lineage and expose them only as immutable
views. They do not select lifecycle heads, interpret source evidence, infer a
policy, reorder caller data, mutate economic events, or access files, databases,
clocks, websites, or services.

### Public trade-date projection

`project_trade_date_journals` consumes a `LifecycleResolution` and an explicit
`TradeDateProjectionContext` containing the inclusive knowledge and economic
cutoffs plus the independent settlement cutoff. It does not resolve lifecycle
records again. It rejects a selected record or lineage member outside the
declared cutoffs and validates the active event ordering before mapping any
entry.

The immutable `TradeDateProjectionResult` identifies the fixture accounting
engine, journal projection, lifecycle contract and policy versions. It retains
the evaluation context, ordered active-record and economic-event identities,
the complete selected lifecycle-record identities, deduplicated source-record
identities in first-seen order, and the journal entries. Each entry repeats its
full event lineage. Results own their values and do not retain pointers into the
lifecycle ledger.

Entry IDs use the versioned fixture identities already fixed by the portable
walkthrough: `td.opening-cash`, `td.trade-v1`, `td.trade-v2`, and
`td.reversal`, followed by the phase. Line IDs append `.debit` or `.credit`.
Those stems alias the corresponding `opening-cash-record`, `trade-record-v1`,
`trade-record-v2`, and `reversal-record-v1` lifecycle identities. Other record
identities use `td.<active-record-id>.<phase>` so the installed-package API can
exercise the same projection outside the walkthrough. An alias collision fails
with `journal_invariant`; a result never contains duplicate entry or line
identities. Entries are presented by recognized date, selected record
acceptance sequence, phase ordinal and entry ID. A trade phase uses ordinal zero
and a settlement phase uses ordinal one.

The projection returns one complete result or a
`TradeDateProjectionError`; it never returns partially projected entries. Its
stable diagnostic categories are `invalid_context`, `unsupported_event`,
`unsupported_currency`, `invalid_reversal_treatment`, `arithmetic_overflow`,
and `journal_invariant`. The last category preserves the underlying journal
factory category in its explanatory message or identifies a duplicate projected
identity. A settlement date before its trade date is invalid context. Ordinary
negative trades and cash withdrawals are unsupported events. Cash lifecycle
reversals fail as `invalid_reversal_treatment`; the fixture policy does not
infer a cash posting for them. A negative equity record is accepted only when
lifecycle resolution identifies an exact reversal target; lifecycle validation
rejects partial reversals before a resolution can be produced.

### Public settlement-date projection

`<luca/accounting/settlement_date_projection.hpp>` exposes
`project_settlement_date_journals` through the installed `luca::ledger` target.
It consumes the same resolved lifecycle inputs and explicit three-cutoff context,
but recognizes supported equity activity only at the supplied settlement date.
Contributions remain immediate. The owned result preserves selected identities,
versions and lineage; it does not modify the trade-date implementation.
See [the settlement-date API and restrictions](settlement-date-projection.md).
Both projections remain bounded fixture policies with runtime acceptance tracked
separately from compilation evidence.

### Result and portfolio cross-check

The later [public journal-balance API](journal-balances.md) supplies owned exact
balances, currency controls and entry/line evidence from these projected journals.
It is an opt-in accounting target with explicit book/context and partition rules;
the richer portable result/cross-check shape below remains fixture vocabulary.

A result has a stable `result_id`, all version and context inputs, declared
resolved input identities, selected journal-entry identities, exact currency
totals, exact account balances, a portfolio cross-check, and an explanation of
the policy difference. The result must be recomputable without a website,
database, clock, network service, or host-specific state.

The cross-check records the position quantity, settled cash, and open
settlement obligation from the existing portfolio semantics at the same three
cutoffs. It also records the journal cash-control balance and, where the policy
recognizes it, the payable or receivable control balance. Position quantity is
not silently converted to money. For the settlement-date policy an unsettled
obligation is intentionally absent from the journal and is reported as a
policy difference, not treated as a mismatch or hidden netting rule.

## Exact arithmetic and rounding

Money uses six decimal places and equity quantity uses eight. Cash contribution
amounts are copied exactly. Trade value is:

```text
abs(quantity) * price
```

and is rounded once to six money decimals using decimal half-even. Fixture
values are exact at that boundary: `100 * 50 = 5000.000000` and
`80 * 55 = 4400.000000`. Journal-line addition and balance reduction perform
no further rounding. Zero lines, extra precision, binary floating point,
incorrect rounded values, and arithmetic that mixes currencies are rejected.

## The two fixture-only policies

The fixture contains exactly two named policy identity/version pairs. They use
the same chart fragment only so their timing difference is isolated:

- `asset.cash`
- `asset.equity-securities`
- `asset.trade-receivable`
- `liability.trade-payable`
- `equity.contributed-capital`

This is not a universal or recommended chart of accounts.

### `fixture.trade-date.v1` version `1`

Supported events are positive USD cash contributions and USD equity trades in
the fixture's single account. A contribution posts debit cash and credit
contributed capital on its effective date. A positive equity quantity posts
debit equity securities and credit trade payable on trade date; settlement
posts debit payable and credit cash. A negative exact-offset reversal posts
debit trade receivable and credit equity securities on its own effective date;
settlement posts debit cash and credit receivable.

This policy exposes unsettled payable or receivable controls. It orders the
resolved inputs as specified above and applies the one half-even valuation
rounding boundary. It does not net payable and receivable controls.

### `fixture.settlement-date.v1` version `1`

The contribution rule is identical. An equity trade produces no journal before
its supplied settlement date. On settlement, a positive quantity posts debit
equity securities and credit cash; a negative exact-offset reversal posts debit
cash and credit equity securities. The event remains present in portfolio
position and settlement projections before accounting recognition; that
difference is explicit in the result explanation.

### Unsupported cases

Both policies reject, rather than infer, withdrawals, sells other than the
fixture's exact reversal, non-USD or cross-currency activity, FX, fees,
commissions, taxes, lots, cost-basis selection, accruals, income, corporate
actions, partial reversals, multiple reversals, cancellation reinterpretation,
settlement-calendar calculation, failed settlement, legal netting, impairment,
fair value, realized or unrealized P&L, and NAV. They do not establish a
production chart or accounting approval.

## Walkthrough

One immutable history supplies every evaluation:

1. A `100000.000000 USD` contribution posts debit cash and credit contributed
   capital under both policies.
2. The original equity purchase is `100` shares at `50`, settling June 4. On
   June 2 the trade-date policy has a `5000.000000` security/payable entry;
   the settlement-date policy has no trade entry. Portfolio state has 100
   shares, `100000.000000` settled cash, and a `5000.000000` payable.
3. A late correction replaces that active payload with `80` shares at `55`.
   Results from the old knowledge cutoff remain reproducible; the new result
   uses the correction's `4400.000000` entries and retains both original and
   correction evidence in every affected line.
4. On June 4, portfolio settled cash becomes `95600.000000` and the payable
   clears. The trade-date policy settles its payable; the settlement-date
   policy recognizes the security and cash together. Both cash-control balances
   agree with portfolio cash.
5. An exact `-80` share reversal is economically effective June 5, recorded
   late, and settles June 6. Before reversal settlement, portfolio position is
   zero, settled cash is still `95600.000000`, and a `4400.000000` receivable
   is open. The trade-date policy recognizes the receivable; the settlement-
   date policy retains the settled security balance until June 6.
6. On June 6 the reversal settlement restores cash to `100000.000000`, clears
   all security and settlement-control balances, and leaves only cash against
   contributed capital under either policy.

The fixture hand-records those debit/credit totals and portfolio calculations.
The dependency-free validator recomputes journal sums with `Decimal`, checks
the bounded values, and validates each fixture more than once in the focused
test.

## Stable rejection categories

The negative mutation vectors fix these categories:

| Category | Rejection |
| --- | --- |
| `schema_shape` | Missing/unknown fields or malformed scalar shape. |
| `duplicate_identity` | Reused source, lifecycle, policy, entry, line, evaluation, result, or balance identity. |
| `invalid_policy` | A policy changes its supported events, accounts, currency, rounding, ordering, or fixture-only disclaimer. |
| `unsupported_event` | An input event is outside cash contribution or equity trade. |
| `invalid_cutoff` | A result changes or omits its evaluation cutoffs. |
| `policy_context_mismatch` | Result policy/version, engine/projection version, resolved input set, or selected entries do not belong to the declared context. |
| `unbalanced_entry` | Debit and credit totals differ. |
| `mixed_currency_entry` | A journal attempts cross-currency balancing. |
| `invalid_line_amount` | A line is zero, negative, non-decimal, or exceeds six decimals. |
| `rounding_mismatch` | A balanced posting does not equal the policy's exact/half-even event valuation. |
| `lineage_missing` | Required record, economic-event, source-record, or reversal lineage is empty or absent. |
| `lineage_mismatch` | Lineage does not equal immutable provenance or the active O2 resolution. |
| `invalid_reversal_treatment` | A reversal is not an exact offset or posts with the wrong side/account/target treatment. |
| `portfolio_cross_check` | Position, cash, settlement, control, or bounded hand arithmetic differs at the same cutoffs. |

`invalid-mutations.json` applies at least one portable mutation for every
category, including distinct zero/inexact and missing/mismatched-lineage cases.
The test also applies every vector twice so rejection is deterministic.

## Deferred production decisions

This task intentionally does not choose a policy ABI, runtime policy loading,
persistent account masters, multi-book ledgers, functional currency, FX
translation, rounding residual accounts, close and reopen behavior, posting
authorization, settlement-event ingestion, lots, cost-basis methods, P&L,
accruals, NAV, tax, financial-statement presentation, or accounting-standard
compliance.

It also adds no CMake target, canonical serialization extension, database,
platform adapter, CLI, Python binding, deployment, or backtesting behavior. The
installed-package consumer exercises the same header-only implementation
through `luca::ledger`; hosted, CLI and Python execution adapters remain
deferred.
