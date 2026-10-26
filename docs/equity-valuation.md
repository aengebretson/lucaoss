# Equity position valuation, version 1

`equity-position-valuation/1` is an additive pure C++ snapshot operation in
`<luca/accounting/equity_valuation.hpp>`, linked through `luca::accounting`.
`equity_valuation::project(const Inputs&)` returns `std::expected<Report, Diagnostic>`.
It has no clock, I/O, storage, admission, journal or mutable cache effects.
Existing equity period reports, their unrealized nulls, charts, hashes and close
anchors retain their meanings. This API does not create recognized income,
financial statements, a mark stream, NAV, cash or an automatically closed report.

## Host inputs and ownership

First resolve the complete dependent economic lifecycle at explicit economic
`as_of` and inclusive knowledge `known_through`, project its lots under the
approved lot policy, then select one **complete** book/entity/custody/instrument/
currency pool. Filtering source events before lot replay is invalid. Supply that
pool's open lots, reliefs and trade evidence as a `LotSnapshot`, including an
immutable retained `evidence_id` binding the complete source/policy prefixes and
both cutoffs. The snapshot cutoffs must exactly equal `Context`.

The snapshot is a trusted output of the existing lot projection, not a second
raw-transaction intake format. Structural checks cannot establish source-history
completeness, the truth of a caller's evidence ID, or the knowledge dates of
already-resolved lots. Hosts must retain the original source manifest and return
its evidence on drill-down. Full replay, corrections and source/policy selection
remain the existing lot/lifecycle operations. No ledger-account financial filter
may split a shared dependent pool.

`Inputs` owns the `LotSnapshot`, immutable `observations`, `MarkPolicy` and
`Context`. `Report` retains these full inputs. Snapshot `LotReport` rows must all
belong to its one pool; incomplete source coverage must be excluded by the host,
not asserted complete by omitting rows. A flat pool can retain relief/trade
history and has zero market value/unrealized without a required mark.

## Explicit observations and policy

Each observation supplies immutable ID/revision, instrument, currency, exact
`Price`, market/model type, price basis, independent effective and recorded-known
timestamps, and `Provenance`. Market bases are bid, ask, midpoint or last; model
observations require model basis. Quotes are observations, never trade events.
The host selects/constructs the quoted price under a reviewed adapter policy;
this API does not compute a midpoint or substitute an execution price.

`MarkPolicy` requires ID/version, ordered unique market/model type preferences,
one explicit `market_basis` and nonnegative maximum age in nanoseconds. Only the
chosen market basis is eligible. A model is a fallback only if explicitly listed.
Within the first preference with eligible observations, select maximum
`(effective_at, known_at)` with `effective_at <= as_of`,
`known_at <= known_through`, and age `<= max_age`. Conflicting observations tied
at that latest boundary fail `AMBIGUOUS_MARK`; a newer unambiguous observation
supersedes earlier ties. Duplicate ID/revision fails even if bytes agree: hosts
must deduplicate retained immutable inputs first.

Future effective or later-known observations remain evidence but cannot be
selected. Unrelated instrument/currency, negative prices, malformed IDs and
invalid enum combinations fail explicitly. Missing or stale eligible marks
produce an owned incomplete report with unavailable optional values, never zero.
Age comparison uses wide arithmetic even at extreme timestamp bounds.

For a current view, the host supplies the actual valuation instant. A full-day
period ends at local midnight converted to UTC and answers a different question;
using tonight's boundary for an intraday freshness check can make current quotes
stale. Preserve full-day and retained historical report semantics separately.

## Exact financial results

- Position is signed open quantity: long positive, short negative.
- Signed carrying value sums existing `lot_carrying_amount`: long acquisition
  principal plus allocated opening charges; short opening proceeds minus those
  charges, negated for presentation.
- Cumulative economic realized P&L sums the existing net lot relief, including
  allocated opening and closing charges. Accrual/cash recognition is separate.
- Market value multiplies the **complete pool's signed quantity once** by the
  selected quote, half-even to six-place `Money`. Prices/quantities retain their
  eight-place input precision. This avoids per-lot valuation rounding drift.
- Unrealized is signed market value minus signed carrying. Total economic P&L
  is cumulative realized plus current unrealized. It is **not period activity**;
  period total requires realized activity plus the change in unrealized between
  independently pinned endpoints.

Checked existing `Money`/`Quantity` arithmetic and `value` report overflow rather
than truncate, wrap, silently widen semantics or convert through binary floating
point. No FX translation or mixed-currency total is supplied. Positive/negative
market values describe long/short exposures; they are not actual cash payments.

Unknown/scheduled charges preserve the available provisional amounts but set
`charges_complete=false`, `supported_complete=false` and
`CHARGES_INCOMPLETE`. Empty explicitly known charges mean confirmed zero.
Consumers must expose incomplete coverage and must not call incomplete numbers
fully supported totals. Missing/stale marks set market value, unrealized and
total economic to null while retaining carrying, realized, source and diagnostics.
Flat positions have zero unrealized; unknown historical charges still flag
incompleteness. Model/market valuation never records settlement or bank cash.

## Identity, retention and verification

Length-framed SHA-256 input identity binds the version, full typed lot/charge/
policy evidence, immutable source manifest identity, all observations (including
unselected/future rows and provenance), complete mark policy and both cutoffs.
Report identity binds that input digest and operation version. Repeating identical
inputs reproduces all fields. Changing evidence can change identity even when
amounts agree. Array order is part of retained input evidence; reordering marks
can preserve selection and amounts while producing another identity.

Hosts retain the full report and pinned inputs, not just a digest, and reject
conflicting reuse of a retained report/source identity. This API does not add
native retained-close integration; do not relabel its snapshots as period-close
anchors or insert them into an existing report's evidence.

Executable hand-calculated snapshots are in
`tests/conformance/equity-valuation/snapshots.tsv`; the native test projects their
actual ledger history, then checks exact expected position, carrying, realized,
value, unrealized and total. Additional tests cover knowledge/freshness cutoffs,
unknown charges, short/fractional quantities, half-even valuation, overflow,
conflicting marks, malformed pools and evidence identity. Existing period and
derivative regressions remain separate compatibility checks.
