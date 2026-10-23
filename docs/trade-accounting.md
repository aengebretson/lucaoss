# Equity trade accounting and actual settlement

`equity-journals/1` is a bounded C++ reporting contract for cash equity purchases,
sales, short sales, covers and trades crossing zero. It reuses custodian LIFO
relief, exact Money arithmetic and immutable lifecycle/policy evidence. OSS has no
UI; Enterprise embeds these same functions. This contract does not assert GAAP,
IFRS, jurisdictional tax or financial-statement compliance.

## Approved recognition rules

The user selected these rules on 2026-10-23:

* Capitalize opening commissions and fees. A long lot carries principal plus its
  allocated opening charges. A short lot carries opening proceeds less opening
  charges. Closing charges reduce realized proceeds or increase cover cost.
* Recognize cash-basis realized P&L proportionally only when **both** opening and
  closing trade amounts have actually settled. These rules do not select lots.

`RecognitionBasis::accrual` recognizes net economic P&L when a lot is relieved.
`RecognitionBasis::cash` initially posts the same amount to
`control.deferred-realized-pnl`, then transfers the eligible portion to
`income.realized-pnl`. `unspecified` refuses journal reporting with
`RECOGNITION_POLICY_REQUIRED`; economic lot reports remain available. Existing
versioned entity defaults/account/custody overrides choose the recognition basis.
A report retains every contributing account's resolved policy; the closing
trade's policy controls its relief recognition. Different custody accounts never
share lot relief. Different account recognition bases can share a custody pool.

This cash view is a defined P&L-recognition overlay on trade-date positions,
inventory and obligations, not a pure cash-only statutory balance sheet.
Commissions and fees are separately retained and allocated per component by the
lot kernel. Opening costs are not expensed again at payment; their allocated cost
enters P&L through relief. Closing costs enter that relief's net P&L. Missing or
scheduled charges remain visibly provisional (`charges_complete=false`).

For each relief, settlement is allocated pro rata across the **entire opening
and closing trades**, rather than assigning payments to the first lot fragment.
The eligible fraction is the lesser of the two cumulative settled fractions.
A settled fraction is actual signed net cash in the contractual direction divided
by contractual net trade cash, clamped to [0,1]. Wrong-direction cash after a
correction counts as zero and produces a reconciliation break. A trade with zero
net cash obligation has no payment prerequisite. Net P&L times the eligible
fraction rounds half-even to six decimals; each journal posts the change in the
cumulative rounded target. Full settlement exhausts the exact P&L and a refund
reduces recognition and reopens the deferred control. This is an explicit
proportional allocation convention, not a jurisdiction's cash tax rule.

## Journal map

Signed Money is debit-positive at this mapping boundary; emitted journal lines
are strictly positive debit/credit values, independently balanced per currency.
No currency conversion or legal netting is inferred. Generic cash movements are
accepted only as positive capital contributions; withdrawals/expenses require a
separate classification contract and fail `CASH_CONTRIBUTION_REQUIRED`.

| Event or portion | Debit | Credit |
| --- | --- | --- |
| Cash contribution | Cash | Contributed capital |
| Open long | Equity securities: principal + opening charges | Trade payable |
| Close long | Trade receivable: proceeds less closing charges | Relieved long carrying cost; net realized gain |
| Open short | Trade receivable: proceeds less opening charges | Short-equity liability |
| Cover short | Relieved short carrying value | Trade payable: cover principal + closing charges; net realized gain |
| Actual receipt | Cash | Trade receivable |
| Actual payment | Trade payable | Cash |
| Cash-view gain release | Deferred realized P&L | Realized P&L |

Losses reverse the gain line. One trade can relieve multiple lots and open a new
opposite-side lot; its entry combines those exact components. Realized P&L
includes allocated charges on both legs. A negative net sale cash amount is a
payable; excessive charges are never silently dropped.

`RecognitionPhase::actual_settlement` uses the event's actual UTC effective date
and retains the original scheduled date only as contractual context. The old
fixture trade-date/settlement-date policies remain unchanged and continue to be
forecast/example APIs; they must not be used as actual bank cash.

## Actual settlement evidence and native processing

`SettlementEvent` is a separate immutable cash stream. It fixes identity, account,
original trade record and economic-event identity, signed cash/currency, actual
effective time, knowledge time, source records, settlement sequence and
`ledger_through` at admission. Optional `reverses` identifies an exact refund of
one prior non-refund event. A refund is a new dated cash event; it never deletes
bank evidence. Partial refunds and arbitrary cash adjustments require a later
contract. Actual future payments cannot be asserted before their knowledge time.

The optional SQLite adapter adds:

```cpp
receive_settlement(key, request, actor); // durable receipt, not financial completion
settlement_status(key);                // pending, committed, invalid + retained request/error
settlement_events(after, limit);       // immutable accepted sequence pages, max 1000
```

Requests have exactly `account`, `trade_record`, `cash`, `currency`,
`effective_at`, nullable `reverses`, and a nonempty `source` object. Decimal/date
values are strings. The source is evidence, never authority to override fields.
Stable keys recover identical receipts; conflicting content/actor reuse fails.
The C++ `Worker` processes received settlements when its ordinary ingestion work
is drained. Background `Worker::run` and existing `AsyncHost` workers execute the
same operation. No interpreter, detached thread or new framework is introduced.

Admission validates account/currency/source binding, original admitted trade,
actual date, current active trade, amount direction, cumulative overpayment,
refund identity and period close. Invalid requests remain queryable. Complete
settlement histories are validated in the pure core against the exact ledger
prefix known at each admission; later trade corrections cannot invalidate
previously valid bank evidence. Events cannot have gaps or reused identities. Knowledge times are nondecreasing
and cover the full admitted ledger prefix; native admission clamps its logical
knowledge time against ledger/settlement history if the wall clock moves back.

Schema 5 adds `settlement_requests` and `settlement_events` transactionally when
opening schemas 1–4. Older binaries reject schema 5. Rollback requires a coherent
pre-upgrade backup. This PR does not operate a production store. Current bounds:
100,000 ledger records for settlement admission/reporting in Enterprise, 100,000
settlement requests per book, 65,536 request bytes, 1,000 event rows per page.
Exceeding a bound fails explicitly. Bulk settlement intake/checkpoints are later
performance work.

## Corrections, dated voids and closes

Trade corrections replay affected lot history and regenerate journals. Settlement
cash stays bound to the stable economic event, even when the active trade record
changes. Excess or opposite-direction settlement after correction produces
`SETTLEMENT_EXCESS_AFTER_CORRECTION` with the actual-cash evidence. A void/delete
removes the trade from the active economic projection at the lifecycle cutoff;
existing cash then posts against `control.settlement-suspense` and produces
`SETTLEMENT_ON_VOIDED_TRADE` until an explicit refund clears it. No refund is
fabricated from cancellation or a scheduled settlement date.

These are **regenerated snapshot journals**, not a persisted posted journal.
Dated voids preserve historical reports before the void cutoff and original
source/lifecycle evidence. They do not emit a conventional cancellation-date
adjustment journal. Do not compute period activity by filtering the current
journal table's entry dates. An append-only posting/adjustment contract and
financial statements remain later work; this limitation follows the existing
[dated-void projection contract](dated-void-periods.md).

Trade admission, settlement admission, policy edits and closes share the same
book admission lock and SQLite transaction boundary. A close reviews ledger,
policy **and settlement** highwaters; pending work prevents close. Receipts pin
all three. Backdated settlement/refunds in closed periods are retained as invalid.
A later actual refund can post in an open period. Closed reports reconstruct all
three pinned prefixes and the economic cutoff; later edits do not rewrite their
retained result evidence. Existing close calls without a settlement highwater
remain valid only for books with no accepted settlement events.

## Shared API, evidence and algebra

```cpp
project_trade_accounting(ledger, policies, settlement_prefix, context, rules);
reconcile_trade_accounting(resolution, report);
```

Headers are `<luca/accounting/trade_accounting.hpp>`,
`<luca/accounting/actual_settlement.hpp>` and
`<luca/reconciliation/trade_accounting.hpp>`. Link `luca::accounting` for journals and both `luca::accounting` and
`luca::reconciliation` for accounting controls. The existing opt-in target graph
remains unchanged. Financial targets remain free of SQLite/JSON,
transport, filesystem and clock dependencies.

The caller supplies an immutable complete ledger prefix, settlement prefix,
knowledge/economic cutoffs, mapping identity and resolved policies. The policies
identify the complete custody partitions selected for reporting; records outside
those account bindings are excluded. Never omit another account sharing a lot
pool. Cross-book policies fail. Original acceptance sequences remain intact.

Within `(book, legal entity, custody account, instrument, currency)` the lot fold
is ordered by `(effective_at, acceptance_sequence)` and is neither commutative
nor invertible. A correction/void requires replay of the affected complete pool.
Independent complete pools/books can run concurrently. Actual cash is ordered by
`(effective_at, settlement_sequence)`; P&L relief joins both trades' evidence before
release. Journal presentation is deterministic by recognized date, effective time
and entry ID. Existing exact journal reduction retains its bounded associative
merge law for disjoint validated entries under the same result contract.

Results own journals, lot relief, obligations, actual settlements and balances.
Each entry/line retains lifecycle/economic/source lineage. Trace rows add
opening/closing relief records, contributing settlement IDs and resolved policy
digests. Knowledge timestamps include all contributing trade and settlement
records. Trial balances retain line references. Currency controls balance both
posting turnover and net balances. Reconciliation independently checks actual
cash, unsettled obligations, long/short carrying values and economic realized P&L
against journals; breaks never mutate authoritative state. These internal
controls are not bank-statement reconciliation; existing external observation
APIs remain separate.

Native `retain_policy_result` accepts `equity-journals/1` evidence with ledger and
settlement highwaters, mapping, resolved policy and result digest. It re-resolves
persisted policy history and checks available prefixes in its transaction.
Enterprise retains evidence for every participating policy. Existing
`economic-lifo/1` evidence remains byte-shape compatible. Every distinct cutoff
creates a distinct result; a new current-time report need not have the same
retention ID as a prior report. Pinned historical context must reproduce it.

## Verification

The portable `tests/conformance/trade-accounting/book.tsv` fixes a charged
long→short→long book: final position 1 share, long carrying amount 9.20, realized
net P&L 62.30 and full-settlement cash 10,053.10 including the 10,000 contribution.
Tests also cover exact partial cash recognition, refunds, delayed settlement,
corrections, dated voids, wrong-direction cash, malformed histories, historical
cutoffs, restart, before/after-commit process termination, clock rollback, idempotency,
retained errors, stale close reviews and closed
settlement/refund rejection. Enterprise tests the shared installed C++ API,
manual settlement outbox, journal evidence, paging and pinned restart recovery.

`luca_trade_accounting_benchmark N` measures only the in-memory full reporting
projection over N charged trades and N actual settlements. It validates controls
and materializes all trace evidence. It is not receipt/admission throughput and
makes no improvement claim. Build Release with `LUCA_BUILD_BENCHMARKS=ON`.

Corporate actions, other lot methods, rebates, cross-currency charges, financing,
margin, partial refunds, fair value, statement presentation and conventional
period adjustment journals remain outside this slice.
