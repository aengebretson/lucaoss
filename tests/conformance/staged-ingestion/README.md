# Staged cash lifecycle

Original JSONL is submitted unchanged. Ordinals 1–3 form one causal admission
closure; ordinal 4 is independent; ordinal 5 has unrepresentable monetary
precision. `expected.tsv` records per-record validation, commitment and accepted
sequence. Two groups commit exactly four records; the invalid record remains
inspectable with `EXCESS_PRECISION`. Replay resolves the cancelled economic chain
to no active event, leaving the independent USD 10 movement. Existing lifecycle
projection conformance tests establish that cancellation rule. The native ingestion
checks cover staging outcomes, accepted order, receipts and strict canonical replay.
