# Dated void

Base timestamp: 1700000000 seconds since Unix epoch. One custodian buys five XYZ
shares at USD 20, commission USD 1, effective offset 0 and recorded offset 100.
A cancellation is recorded at offset 110 and takes effect at offset 10.
`queries.tsv` specifies inclusive recorded/economic cutoffs and expected open-lot
counts. A remaining lot contains five shares with principal USD 100 and carrying
amount USD 101. At the void boundary it is excluded. Earlier knowledge still sees
the purchase. Canonical serialization/replay must produce identical results.
The native lot test consumes the queries; checkpoint tests cover future and
already-effective voids in both prefix and suffix positions.
