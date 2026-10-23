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

ES accounting conformance is delivered in its separate increment.

The unit runner additionally covers independent knowledge cutoffs, recorded-time
correction recognition and dependent replay, dated void plus linked replacement,
actual cash/refunds surviving voids, future corrections, closed full-history
hashes, prefix rewrites, changed/superseded marks, quote selection permutation,
zero prices, exact rational factors, half-even micro-money ties and overflow,
limits, complete-pool parallel scheduling, retries and adjacent-period effects.
No transport, distributed runtime, production data or legal compliance is asserted.
