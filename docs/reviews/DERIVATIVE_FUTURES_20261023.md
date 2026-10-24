# ES settlement-to-market increment review — 2026-10-23

Stacked on [SPX contract/accounting PR34](https://github.com/aengebretson/lucaoss/pull/34),
base `2d67ff838d1b4287757b4a3e41d828bf81c56047`. Main remains
`ae820869d4b617fdb8ac9fdcefd37de2bbcabaf4`; no merge/deployment was performed.

This increment enables explicit ES settlement-to-market under a separately named
`es-settlement-to-market/1` policy, selected by the user on2026-10-23. It adds normal
futures endpoint rounding, position basis resets, daily/final official settlement,
exact payment boundaries, P&L-only obligations and actual VM/collateral separation.
No notional trade cash is posted. No canonical/native/admission/adapter changes.

Compatibility: Rules preserves its original option three-field aggregate and
appends optional<FuturesPolicy>. Missing ES policy fails; ES policy on options
fails. Absent ES policy adds no input-encoding bytes or journal-policy fields.
The independently installed PR34 consumer's report ID, input/history/closing
fingerprints and ordered effect IDs match exactly under the extension. The seven
fingerprints are retained in `tests/conformance/derivatives/spx-v1-evidence.txt` and
checked by the unit suite. ES journals separately bind their complete policy.

Independent read-only review verified both repaired SPX and final ES candidates.
The reviewer reproduced and verified fixes for source-order dependent correction
replay, policy evidence and retained compatibility. Corrected opening trades gate
visibility at recorded time but replay economic payloads in source effective/
sequence order. ES correction fixtures prove recomputed official-reset obligations,
realized and residual P&L separately, while actual VM cash remains immutable.
No blocking findings remained.

Validation (Apple M3 Max, arm64, Apple clang17):

* Option-only repaired Debug/native suite:50/50 passed.
* Combined Release/native suite: `ctest --test-dir ../output/release
  --output-on-failure -j 3`:50/50 passed.
* Final compatibility fixture added after the full suite: focused derivative
  Release test passed again; no financial source change after the full run.
* `clang++ -std=c++23 -O1 -g -pthread -fsanitize=address,undefined
  -fno-sanitize-recover=all` derivative fixture runner: passed, no diagnostics.
* Release install to isolated prefix; independent `examples/derivatives`
  configure/build/ctest against public installed `luca::accounting`:1/1 passed.
  SPX realized/cash500; ES realized100/residual25/actualVM100.
* Original three-field installed option consumer compiled against the final
  installed package and compared all seven fingerprints byte-for-byte: passed.

Equivalent Makefiles configurations were used because Ninja is not installed.
Configure Release with `-DCMAKE_BUILD_TYPE=Release -DLUCA_BUILD_TESTS=ON
-DLUCA_BUILD_INGESTION=ON -DLUCA_BUILD_BENCHMARKS=ON`; build with `-j 4`.
Debug uses the same options except benchmarks and Debug build type.

Reproducible `luca_derivative_benchmark`: seven runs per sample; each book opens
N one-contract ES lots, performs three daily resets, then closes the pool. Every
run checks realized N*200. Medians:250 lots7,468us;1000 lots30,793us;
2000 lots66,396us. Raw data are in
`tests/benchmarks/results/2026-10-23-macos-arm64/derivative-stm.jsonl`.
These are single-host pure replay timings, not before/after improvements, ingestion,
network, distributed or production throughput. Limits measure bounded row-fold
work; hashing/maps/metadata/charges add work. No checkpoint speedup is claimed.

Bounded scope: explicit accrual only; fixed intraperiod policy; UTC; whole standard
outright SPX/SPXW/ES contracts; fully supplied immutable inputs and complete pools.
Other products, cash recognition, within-period policy changes and partial-pool/
checkpoint merges remain unsupported. Native derivative admission, persisted
schemas, auth, report retention and Enterprise/CLI/MCP adoption require separate
review. Existing platform derivative exclusions stay intact. See
[the integration contract](../derivative-accounting.md) for all input/output,
rounding, observation, history/close and partition rules and authoritative sources.
