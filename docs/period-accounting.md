# Period activity and bounded statements

`period-accounting/1` is a pure C++ reporting operation. It extends
[equity snapshot accounting](trade-accounting.md); it does not turn regenerated
snapshot rows into a posted ledger. Immutable source, lifecycle, policy and
actual-settlement evidence remain authoritative. Derived effects and retained
reports are reproducible evidence/caches. No GAAP, IFRS, tax, complete fund
accounting, valuation or cash-flow compliance is claimed.

## Application contract

Link only `luca::accounting`; include
`<luca/accounting/period_accounting.hpp>` and optionally
`<luca/accounting/period_pnl_rows.hpp>`.

```cpp
project_period_accounting(ledger_prefix, complete_account_bindings,
    known_policy_prefix, settlement_prefix, period_context,
    explicit_recognition_rules, optional_retained_close_anchor, replay_limits);
select_period_pnl(report, ActivityPnlFilter{account, instrument});
retain_period_anchor(report);
```

All operations return owned values; the projection/selection return
`std::expected<..., PeriodError>`. There is no SQLite, JSON, transport, I/O,
hidden clock, process cache or mutable global financial state. The existing
`project_trade_accounting` signature and its snapshot semantics are preserved.
The internal selected-resolution helper reuses its exact lot, charge, actual
cash, cash-recognition and journal/trial-balance operations. This is additional
reporting, not a new event type or admission path.

Enterprise owns screens, authorization and retained-report storage. CLI/MCP own
application schemas, paging and transport. Adapters must present `report_id`,
input hashes/highwaters, context, recognition choices, completeness diagnostics
and null unsupported components. Never reinterpret `opening.journals` or
`closing.journals` as period entries. Use `effects`, `balances`, `pnl`,
`daily_pnl` and `statements`. Account/instrument P&L filters use
`select_period_pnl` after the complete pool fold; its totals cover **all**
selected rows before paging. Rows carry full pool/account identity, opening,
movement, closing, charge completeness, record/settlement and effect IDs.
Economic P&L attribution follows the closing trade account, even when relief
uses another account's opening lot. Filtering ledger inputs first is invalid.
Daily P&L on the base report is by book/currency; a filtered daily query uses
one UTC day's period report then the same post-fold selection.

The current CLI/MCP snapshot catalog and Enterprise snapshot UI are independently
reviewed consumers. This PR does not publish a period tool or switch their
screens. The version-core dependency is
[PR #30](https://github.com/aengebretson/lucaoss/pull/30), pinned at
`22fd8e5607abf7dfd6549294e2f8f1f1b21055a8`; its transaction-version API and
native admission/replay remain separately owned. Do not ship the experimental
endpoint-difference UI rollforward under the `period-accounting/1` name.

## Cutoffs, knowledge and pinned evidence

A period is `[from, until)` at nanosecond precision. Opening is strictly before
`from` (`from - 1ns`); closing is strictly before `until` (`until - 1ns`). An
effect exactly at `from` belongs to this period; one exactly at `until` belongs
to the next. Equal-timestamp causes form one atomic accounting boundary; effect
presentation is ordered by `(recognition_boundary, effect_id)`. There is no
sequence-dependent intra-timestamp daily P&L. Minimum representable `from` and
empty/reversed intervals fail explicitly.

Version 1 supports **UTC** only. `reporting_timezone` is mandatory;
`UNSUPPORTED_REPORTING_TIMEZONE` rejects other labels. Daily rows are UTC dates,
sparse by days containing supported activity, including net-zero activity.
A missing day is not a claim about unsupported income/valuation coverage.
The core does not perform timezone-database conversion or local DST day
construction. A host may supply any exact UTC interval, including an independently
converted local midnight→next-midnight interval (23/25 hours or unusual midnight
rules). Retain its zone, local date, exact UTC bounds and conversion evidence
separately. Use the aggregate period P&L for that interval; for local daily rows,
query each converted local-day interval. Do not relabel `daily_pnl` UTC subrows
as local days.

Knowledge is an independent inclusive `recorded_through`. All boundaries and
both endpoints use the **same** knowledge cutoff and immutable prefixes:

* Current known activity: select the current complete ledger, policy and
  settlement revisions and an explicit current knowledge timestamp.
* Historically known activity: use the historical knowledge timestamp and pinned
  prefixes used for that report. Later-recorded versions/cash/policies are absent.
* Reproduce a retained report: restore exactly its prefixes, target bindings,
  rules, mapping, timezone and cutoffs; do not substitute today's policy/history.

The caller supplies the exact complete ledger prefix (`accepted_through ==
ledger.size()`), contiguous complete policy and settlement prefixes (their
highwaters equal supplied sizes), and all account bindings of this book slice.
Never split a dependent custody pool. Every ledger account requires a binding;
known records outside the requested date interval still participate in replay.
The native application must capture mutually compatible prefixes under its
existing book-read/admission contract and use committed transaction boundaries.
No canonical acceptance or settlement sequence is renumbered.

`KnownPolicyRevision` retains the existing `PolicyRevision` plus its independent
recorded/knowledge datetime. Knowledge timestamps and sequences are validated;
unknown revisions do not enter policy resolution. Legacy policy rows without
known recorded datetimes require an explicitly evidenced historical prefix;
this API does not invent their knowledge date. Schema 6's nullable policy
knowledge must be checked by the adapter: null is a missing-input diagnostic,
not permission to substitute effective time. The engine reuses entity default,
custody and account override resolution at every boundary. A revision effective
in the past can change current-known history; a retained close catches that.
`RECOGNITION_POLICY_REQUIRED` or missing entity policy fails explicitly. Existing
economic lot APIs remain available independently of recognition policy.

Owned input evidence includes every immutable lifecycle version, policy revision
and recorded time, settlement/source/refund relationship, target binding and
rule. Separate content hashes bind ledger, policy and cash prefixes; the input
hash additionally binds bindings, mapping, timezone and recognition convention.
The report identity binds those inputs, period, knowledge and history digest.
Effective, recorded, recognition and void-effective datetimes remain distinct on
source evidence, causes, endpoint journals and derived effects. Effect knowledge
is no earlier than any participating source or resolved policy knowledge date.

## Recognition and adjustments

The user selected **recorded datetime** for corrections and linked replacements
on 2026-10-23. Callers must explicitly choose
`CorrectionRecognition::recorded_time`; no live policy is changed by a report.
Original contributions/trades recognize at source economic effective time.
Actual cash/refunds recognize at their actual effective time, never scheduled
settlement. Late original evidence therefore changes current-known historical
activity; historical knowledge queries still exclude it.

Legacy `correct` and undated cancellations change recognized accounting at their
recorded datetime. A correction moving its new payload into the future removes
the old contribution at recording and recognizes its new payload at that later
effective time. Dated voids remove the economic identity at their explicit
void-effective datetime. A linked replacement recognizes at the later of its
recorded datetime and its predecessor's void-effective gate. Thus, for a
backdated void `V < R`, void and replacement are distinct adjustments at `V` and
`R`. For a scheduled future void `R < V`, both wait until `V`. Both versions and
the cancel link remain immutable; neither the ledger nor source data is edited.

A dated void of a corrected version whose recorded recognition lies **after**
the void date removes the whole economic chain on the void date. The deferred
version remains causal evidence; its later recording boundary cannot resurrect
an already voided identity. This rule preserves the explicitly chosen void date.
The three-argument report-only lifecycle resolver implements these gates; the
existing two-argument economic resolver retains its retrospective snapshot
meaning. Consequently economic as-of state and recognized accounting state can
differ in a backdated correction/replacement history. Applications must label
which question they answer.

At each boundary the engine resolves and replays **all dependent lot history**,
projects exact accounting before and after, and derives the balanced change for
each affected snapshot journal. Removed entries, new versions, dependent relief,
settlement reclassification and cash-recognition changes are all included. An
original trade reversal alone is insufficient: LIFO relief is ordered and has no
universal inverse. Source trades with `reverse` action fail explicitly with
`PERIOD_REVERSAL_REQUIRES_CANCEL_OR_CORRECTION`; negative trade amounts do not
supply a generic reversal policy.

Every effect retains before/after journal evidence, source/lifecycle and causal
ancestry, relevant boundary causes, actual-settlement IDs and resolved policies
before/after. Zero-net changes retain their causes in history and versions in
input evidence even if no posting is necessary. Signed line differences are
turned into positive debit/credit lines and validated through `JournalEntry`.
Equity effects have the additive `RecognitionPhase::period_adjustment`; their
cause classification separately distinguishes original activity, settlement,
refund, correction, void, replacement and policy change. They never assert an
actual payment merely to represent a correction. Cash-contribution effects use
the existing immediate phase.

Stable effect IDs hash the operation/chart convention, book, recognition
boundary, affected journal anchor, before/after signed amounts, participating resolved policy digests and relevant
causal identities/dates. They do not depend on the requested period boundaries,
wall-clock evaluation, report page, storage insertion or retry attempt. Adjacent
periods under identical prefixes/knowledge/conventions contain disjoint effects
whose union equals the combined period. Input/result identities distinguish new
source evidence even where financial totals happen to agree. Hosts retaining
caches must key by identity and compare evidence on conflicting identity reuse.
They must never apply a replayed effect as a new source transaction.

## Cash and P&L

Actual settlements remain bound to their stable economic identity and original
admitted cash/source evidence. Correction/void never creates or erases a payment.
When a void leaves actual cash, the snapshot moves that cash's offset to
`control.settlement-suspense`; the derived period adjustment reclassifies the
offset, while cash itself is unchanged. Only an explicit accepted refund moves
cash. Excess/wrong-direction settlement after correction remains visible as the
existing reconciliation break.

Economic realized P&L is net lot-relief P&L under the approved capitalization
rule: opening commissions/fees enter long carrying cost or reduce short carrying
proceeds; closing charges reduce proceeds/increase cover cost. Recognition never
selects lots. Accrual recognizes net relief at the trade boundary. Cash recognizes
the cumulative eligible fraction only when **both** matched trade legs have
actually settled, allocated over the full amounts of each trade. Existing
six-decimal half-even cumulative rounding and the minimum settled fraction are
reused, including exact zero-obligation and wrong-direction rules.

`pnl` shows cumulative opening, signed period movement and cumulative closing
for economic realized, accounting-recognized and deferred P&L (gains positive).
`deferred = economic - recognized`; it reconciles to the deferred journal control.
`daily_pnl` sums boundary changes, not entry-date-filtered current snapshots.
Later cash may release deferred P&L with no new economic P&L. An exact refund may
reverse recognition and reopen deferred P&L without reversing economic relief.
Corrections may alter both economic relief and settlement eligibility; neither
cash nor P&L is counted twice.

## Statements and controls

The mandatory mapping identity is `cash-equity-chart/1`. Unknown mappings fail.
The chart maps debit-positive trial-balance amounts as follows:

| Chart account | Statement section |
| --- | --- |
| `asset.cash` | Assets |
| `asset.equity-securities` | Assets, cost carrying value |
| `asset.trade-receivable` | Assets |
| `liability.trade-payable` | Liabilities |
| `liability.short-equities` | Liabilities, carrying value |
| `equity.contributed-capital` | Contributed capital |
| `income.realized-pnl` | Recognized realized income / unclosed earnings bridge |
| `control.deferred-realized-pnl` | Disclosed unclassified deferred control |
| `control.settlement-suspense` | Disclosed unclassified settlement control |

`balances` proves `opening + signed movement = closing` for each book/chart
account/currency. Classified movements retain their contributing effect IDs.
Comparative statement lines use those exact balances; no conversion or legal
netting occurs. The income statement uses the period's recognized realized P&L
and discloses economic/deferred movement separately. The as-of balance sheet
uses cost-based long/short carrying values and actual cash/trade obligations.

Cumulative recognized earnings are an explicit unclosed equity bridge, and period
result is their closing-minus-opening difference. No retained-earnings policy,
year-end transfer, invented closing transaction or contribution to cash is
inferred. The exact statement control is:

`assets - liabilities - contributed_capital - unclosed_earnings
 - deferred_credit_control - suspense_credit_control = 0`.

Control balances remain visibly outside a completed equity classification and
produce `UNCLASSIFIED_STATEMENT_CONTROLS`. They are not silently called retained
earnings or ordinary liabilities. Trial balances retain endpoint line evidence;
effects retain the sources explaining their change. Negative asset/liability
amounts remain signed at their actual chart account; no presentation netting is
invented after an overpayment or correction.

Unrealized marks, other income, separately classified expenses, FX translation
and cash-flow statements are **null/unsupported**, with machine-readable
diagnostics. Opening-charge capitalization and closing-charge netting are
supported; a null expense statement does not erase those charges from net P&L.
`charges_complete=false`, settlement breaks and unclassified controls make
`supported_complete=false`. Even `supported_complete=true` means only the
supported cash/equity slice reconciles; it is never a completeness claim for
financial statements as a whole.

## Closed periods, retention and execution

Existing native admission fences are unchanged. Ledger, policy, settlement and
close writes remain governed by their existing common lock/transaction and
committed prefix rules. This operation cannot admit events, reopen a period,
change policy, overwrite a retained report or synthesize an open-period repair.

Retain the entire result and input evidence with the close receipt, not merely
an anchor hash. A retained anchor fixes its original input highwaters/digest,
exclusive closing boundary, history digest and report identity. For a later open
period, the engine first verifies that the original ledger/policy/cash prefixes
are still **exact** prefixes, including actors, source evidence and policy
knowledge. Rewritten/unavailable evidence fails
`CLOSED_INPUT_EVIDENCE_CHANGED` / `CLOSED_INPUT_PREFIX_UNAVAILABLE` even when
amounts match. It then replays through the retained boundary, including selected closing policy and balance evidence
even for an empty book: changes affecting that history fail `CLOSED_HISTORY_EXCEPTION` with expected/actual digests and
the retained report ID. A request overlapping the close requires the original
retained report (`CLOSED_PERIOD_RETAINED_REPORT_REQUIRED`).

The host must durably retain these errors together with the triggering pinned
inputs as exceptions; pure C++ has no persistence side effect. It must return the
retained result for closed-history queries. No reviewed current-period repair
policy exists in v1, so such exceptions cannot be converted into invented
adjustments. New open-period corrections whose recognition is after the close
can proceed if the protected history and immutable prefixes still match.
Restart/retry replays the retained canonical inputs and yields the same result.
Existing schema-6 canonical restore preserves paired replacement atomicity.

Version 1 executes full ordered boundary replay. Limits bound boundaries
(default 4096) and replay work (default 2,000,000 summed ledger/policy/settlement/
target rows times boundaries). Exceeding them fails `PERIOD_REPLAY_LIMIT_EXCEEDED`;
no truncation or sampled result is returned. Both endpoints and all intervening
boundaries must fit. Performance is approximately quadratic in long histories;
affected-pool checkpoints are future work, not a claimed optimization here.
Independent **complete books** are the tested parallel partitions. Arbitrary
within-book slices, temporal shards, splitting dependent pools, checkpoint
continuation and merging partial period reports are not supported by this v1 API.
The underlying exact journal reducer's validated disjoint-entry merge law is
unchanged. Do not claim distributed worker or persistence crash testing for this
pure projection; native admission/close crash and retry coverage belongs to the
existing store tests.

## Compatibility and acceptance evidence

This adds installed public headers and one trailing journal recognition enum
value. Existing enum numeric values, snapshot signatures, canonical lifecycle
formats, store schemas and retained `equity-journals/1` results are unchanged by
this period PR. Consumers with exhaustive recognition-phase switches must add
`period_adjustment` before adopting it. No native `retain_policy_result` schema
for period results is added: adapters must retain the full versioned result and
inputs using their report store, or return an explicit retention capability
diagnostic. Existing snapshot evidence is never relabeled as period evidence.

See [the hand-calculated fixture](../tests/conformance/period-accounting/README.md),
[unit acceptance](../tests/unit/period_accounting_test.cpp),
[installed walkthrough](../examples/period-accounting/main.cpp), and
[verification record](reviews/PERIOD_ACCOUNTING_20261023.md). Benchmarks measure
this bounded reporting operation over synthetic exact inputs, not receipt,
settlement admission, database or production throughput.
