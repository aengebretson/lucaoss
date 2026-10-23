# LIFO lot conformance

`trades.tsv` contains decimal input strings and independently calculated expected
state after each accepted trade. All trades are USD, one custodian account and one
equity; timestamps increase by row. Commission and exchange fee are actual amounts.

The first two purchases open 10 shares at 10 and five at 20. The sale of 12 relieves
five from the second lot and seven from the first, producing net P&L 187.20. The
next sale closes three long shares and opens two short. Two covers then close the
shorts and open one long. Final long principal is 25, allocated opening charges
0.12, cumulative realized net P&L 301.06, and cumulative trade cash flow 275.94.

Quantity scale is eight decimals; Money scale is six. Unit tests consume this file
and additionally test short LIFO order, pool isolation, exact residual conservation,
source-preserving correction/cancellation and legacy charge completeness.
