# Period accounting verification — 2026-10-23

Base: `22fd8e5607abf7dfd6549294e2f8f1f1b21055a8` from the separately
reviewed [transaction-version PR #30](https://github.com/aengebretson/lucaoss/pull/30).
The focused period diff is on `work/period-accounting-20261023`. This receipt
covers source implementation and local tests, not integration, release or
production use.

## Implemented and checked

The [contract](../period-accounting.md) and public C++ headers implement half-open
UTC periods, independent inclusive knowledge, explicit recorded-date correction
recognition, explicit-date voids and recorded replacements with causal gates.
Boundary replay derives balanced changes to trade/settlement/recognition journals
and dependent lot relief; no source event is edited and no refund is inferred.
Owned exact-prefix evidence, stable effects/report identities, classified
rollforwards, daily and period economic/recognized/deferred P&L, account/instrument
selection after full-pool relief and bounded comparative statements are covered.

The portable [hand-calculated fixture](../../tests/conformance/period-accounting/README.md)
fixes a long→short→long book: fully paid cash 1052.20, remaining inventory 9.20,
economic/recognized P&L 61.40. Partial actual cash leaves recognized 32.175 and
deferred 29.225. Refund recognition changes -19.425. A recorded correction reduces
economic P&L by 10 and yields recognized 48.824107/deferred 2.575893. Dated void
retains actual cash, reclassifies 18.4 to suspense, and an explicit refund clears
it. Linked backdated void/replacement separately produce -4.6/+5.6; a future void
combines their net +1 at its gate.

Unit assertions additionally verify:

* Every account/currency opening + signed movement = closing; every source and
  derived entry balances; statements reconcile to endpoint trial balances.
* Daily sums equal period P&L; adjacent periods compose both identities and full
  effect evidence under the same pinned input/knowledge convention.
* Before/after effective/recording boundaries, including exact nanoseconds,
  answer distinct questions; unknown policy/cash/versions stay excluded.
* Full-prefix canonical restart and retries reproduce owned results and IDs;
  independent complete books execute concurrently with serial equivalence.
* Cross-account custody relief uses full history; account cash/accrual overrides
  do not change lot selection; filtered totals cover all selected rows.
* Voids of deferred corrected targets honor the explicit void date, retain causal
  ancestry and cannot resurrect at a later correction boundary.
* Policy/cash/lifecycle ID namespaces cannot leak unknown unrelated records or
  future knowledge into effect lineage.
* Closed anchors reject mutated prefixes, late financial history, backdated
  same-setting policy evidence, and empty-book policy-history changes. Exception
  actual digests match an independently recomputed current-history fingerprint.
* Unsupported mapping/timezone, missing recognition policy and exceeded replay
  limits fail explicitly; unsupported statement components remain null.

## Executed checks

Local environment: Apple M3 Max, macOS arm64, AppleClang 17, C++23, CMake with
Unix Makefiles. Ninja is unavailable locally, so equivalent explicit configure
commands were used instead of the repository's Ninja presets.

```sh
cmake -S . -B ../output/debug -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Debug \
  -DLUCA_BUILD_INGESTION=ON -DLUCA_BUILD_BENCHMARKS=ON
cmake --build ../output/debug --parallel 2
ctest --test-dir ../output/debug --output-on-failure --parallel 2

cmake -S . -B ../output/release -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Release \
  -DLUCA_BUILD_INGESTION=ON -DLUCA_BUILD_BENCHMARKS=ON
cmake --build ../output/release --parallel 2
ctest --test-dir ../output/release --output-on-failure --parallel 2
```

Final results: **46/46 Debug**, **46/46 Release**. Root checks include native
admission/policy/settlement/close/restart suites, direct application/stdio adapter
regressions, legacy lifecycle/portfolio/accounting suites, conformance, and
installed/embedded/option package consumers. The installed consumer builds and
runs `examples/period-accounting/main.cpp` through only `luca::accounting`, with
recognized P&L 18.2 and correction economic activity -4. No target dependencies
were widened.

The complete focused period test was separately compiled with AppleClang C++23,
`-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer`, the two public core
include directories and its portable query-fixture definition. It passed without
sanitizer diagnostics. `git diff --check` passed.

An independent read-only AI review examined the complete financial diff against
the pinned base, fixtures, APIs, installed example, benchmark and documentation.
Its findings were repaired and independently reprobed: policy knowledge timestamps,
truthful adjustment phase, exact closed-prefix and closing-policy evidence,
ancestor void recognition/lineage, namespace collisions and accurate exception
fingerprints. Final review: **no outstanding actionable findings**. Review did not
modify the checkout. Changing the source or base requires new appropriate checks
and review; this receipt does not approve a future merge target automatically.

## Bounded synthetic benchmark

Release executable `luca_period_accounting_benchmark N`, N even and <=500,
constructs N charged alternating purchases/sales plus N immutable actual
settlements and a contribution. It times only full in-memory period reporting,
including input/effect evidence. It checks exact economic/recognized P&L, every
rollforward and every derived entry balance. It does not measure ingestion,
database, native-store settlement admission or end-to-end service performance.

| Trades | Actual settlements | Derived effects | Elapsed ms |
| ---: | ---: | ---: | ---: |
| 20 | 20 | 51 | 10.75 |
| 100 | 100 | 251 | 230.35 |
| 250 | 250 | 626 | 1526.06 |

A repeat at 100 trades took 224.40 ms and reproduced the same report identity:
`period:071f05a6214186daf41fb81a9d6320bb0244b14ce9c2c45df224aff7449fe64d`.
These are single local samples under shared machine load, not service budgets or
improvement claims. Quadratic full-boundary replay is explicitly bounded;
affected-pool checkpoints and intra-book partition merging remain future work.

## Compatibility and limits

No store migration or live policy/data change is introduced by the period diff.
It depends on the version-core PR's schema-6 and canonical paired-replacement
behavior; that migration remains separately reviewed. Existing snapshot APIs and
canonical enum numeric values remain compatible; adopting the new period effects
requires handling the additive `period_adjustment` recognition value.

Only UTC-coordinate intervals and the declared cash/equity cost-based chart are
supported. Hosts may evidence local-date→UTC-bound conversion and use aggregate
results without relabeling UTC daily rows. Corporate actions, marks, income,
separately classified expenses, FX and cash-flow classification remain explicitly
unsupported. Deferred/suspense controls are unclassified statement bridges and
make supported coverage incomplete. No statutory accounting claim is made.

Full period-result persistence/exception retention and UI/CLI/MCP publication are
host responsibilities; this PR supplies the pure typed contract and evidence.
Native admission/close crash/retry suites pass, but no new distributed worker,
persisted period-cache crash recovery or live production behavior is claimed.
No merges, deployment, live financial data changes or policy changes occurred.
