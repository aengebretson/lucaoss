# Period accounting hand calculations

`book.tsv` fixes the source book; `queries.tsv` is executed by
`luca_period_accounting_test`. Day 0 is 2026-01-01 UTC. Queries use half-open
recognition intervals and inclusive knowledge. All amounts are exact USD.

The first sale relieves LIFO cost 61 + 30.6 = 91.6 against net proceeds 118,
realizing 26.4. The second sale closes seven shares: allocated proceeds 96.6
minus carrying 71.4 = 25.2, and opens a three-share short carrying 41.4.
The covers realize 27.6 - 22.4 = 5.2 and 13.8 - 9.2 = 4.6, then open a
one-share long carrying 9.2. Economic P&L is 61.4. Full cash is
1000 - 102 - 61 + 118 + 138 - 22.4 - 18.4 = 1052.2.

At the end of day 6, opening b1 is half paid. First-sale eligible P&L is
12.75 + 13.65 / 2 = 19.575; second-sale eligible P&L is 25.2 / 2 = 12.6.
Neither cover is paid. Recognized P&L is 32.175, deferred 29.225. Later
payments release that exact remainder without new economic P&L.

Unit scenarios additionally fix these independently calculated expectations:

* Refund b1-half on day 9: recognition drops by 13.65/2 + 25.2/2 = 19.425;
  actual cash increases by 51. A new actual payment on day 10 restores it.
* Correct b1 price 10→11, effective day 1, recorded day 11: economic P&L
  falls 10 on day 11. Contractual opening cash becomes 112; actual 102 stays
  unchanged. First-sale b1 P&L is 10.65; second-sale P&L is 18.2.
  Cash-recognized total is 12.75 + round(10.65×102/112,6) +
  round(18.2×102/112,6) + 5.2 + 4.6 = 48.824107. Economic total is 51.4;
  deferred is 2.575893.
* Void c2 effective day 13, recorded day 12: economic and recognized P&L
  each fall 4.6; no cash changes. Its actual 18.4 payment moves to debit
  suspense. An explicit exact refund day 14 clears suspense and increases cash.

Rounding uses the existing cumulative half-even six-decimal trade rules, not a
new per-report rounding convention. The tests verify every account rollforward,
all entry balances, endpoint statement equations, daily/period P&L reconciliation,
adjacent composition, source/lot lineage and pinned replay.
