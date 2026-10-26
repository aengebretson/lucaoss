# Equity valuation conformance fixtures

`snapshots.tsv` contains independently calculated expectations. Start a fresh
ledger whenever `scenario` changes; each row adds one signed trade and values the
complete resulting pool at `mark`. All dates/currency are explicit in the native
harness and all commissions are explicitly reported per execution. Values are
USD; quantities and prices are decimal strings.

- Buy 100 at 200, mark 202: cost 20,000 and unrealized 200. Sell 40 at 203:
  realized 120, remaining cost 12,000/value 12,120/unrealized 120, total 240.
  Sell the remaining 60 at 201: cumulative realized 180 and zero open value.
- The charged variant capitalizes the $1 opening commission. A 40-share sale
  relieves $8,000.40 cost and nets $8,119.60 proceeds after $0.40 commission:
  realized $119.20, remaining cost $12,000.60 and unrealized $119.40.
- A 100-share short at 200 with $1 opening commission has signed carrying
  -$19,999; marking at 198 gives signed value -$19,800 and unrealized $199.
  Covering 40 at 197 with $0.40 commission realizes $119.20 and leaves $119.40
  unrealized on the 60-share short.
- The fractional lot multiplies 0.125 shares by the explicit mark.
- Tiny positive-price purchases round principal to zero. Their marks produce
  half-micro and one-and-a-half-micro values, rounded half-even to zero and two
  micro-dollars. Two such lots at the same 150 mark value to three micro-dollars
  at the complete pool, rather than four from separately rounded lots. Input
  prices remain positive under the existing lot contract.

The existing lot projection allocates fees and realizes P&L; the new operation
reuses its results and computes current valuation. This fixture does not imply
live provider connectivity, actual settlement or recognized-income accounting.
