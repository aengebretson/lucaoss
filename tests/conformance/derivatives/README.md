# Derivative conformance, version 1

Synthetic values are hand-calculated. `round-trips.tsv` is executable language-neutral
input/expected data (quantity in whole contracts; prices in index points; charges
and P&L in USD). The C++ unit runner reads every row. Gross is
`(exit-entry) * signed_quantity * factor`; net subtracts both charges.

Two SPXW contracts bought at 10.00, sold at 12.50: premium obligation -2000,
close obligation +2500, gross/net realized 500. Actual bank payments are separate.
Opening charge 2 and one-contract close charge 1 yield realized248, remaining
cost1001; mark12.50 yields fair value1250, unrealized249. A final one-contract
close with charge1 leaves net realized496 and no position.

European call strike6000 bought2@10: official fixing6000 extinguishes two contracts,
terminal obligation0, realized-2000. Fixing6020 yields terminal obligation4000,
realized2000. Two partial terminal events of one contract exhaust the position;
a third fails. A model or missing fixing fails. A zero market mark alone does not
expire a position. Short/put fixtures exercise the signed payoff independently.

ES buy2@6000 with charges2: official settlement6001 creates VM obligation100,
realized98, resets basis6001; mark6001.25 gives remaining25, total123. Actual
VM100, actual fees-2 and collateral deposit-1000 give bank cash-902 and collateral
asset1000; trading P&L stays123. Settlement6002 adds100 (cumulative realized198),
remaining0 if that settlement is the selected mark. A stale market-first policy
can explicitly select6001.25 instead, yielding remaining-75 and total123.
Neither trade posts a full-notional cash movement. One ES point is50 and tick.25
is12.50. Per-contract endpoint rounding is to USD cents, ties away from zero.

The unit runner additionally covers independent knowledge cutoffs, recorded-time
correction recognition and dependent replay, dated void plus linked replacement,
actual cash/refunds surviving voids, future corrections, closed full-history
hashes, prefix rewrites, changed/superseded marks, quote selection permutation,
zero prices, exact rational factors, half-even micro-money ties and overflow,
limits, complete-pool parallel scheduling, retries and adjacent-period effects.
No transport, distributed runtime, production data or legal compliance is asserted.

`spx-v1-evidence.txt` captures the installed first-increment option consumer's
input/history/closing fingerprints and ordered effect IDs. The ES extension
compiles the original three-field Rules aggregate and checks all seven lines,
preventing unrelated ES policy fields from changing retained option evidence.
