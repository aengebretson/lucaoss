# LUCA performance benchmarks

This directory contains LUCA's reproducible performance baseline. It uses a
small internal `std::chrono::steady_clock` runner rather than adding Google
Benchmark: the current cases need only warm-up, repetitions, filtering, and
machine-readable output, so the internal runner keeps benchmarks dependency-free
and entirely outside production libraries.

## Build and run

Benchmarks are optional (`PACIOLI_BUILD_BENCHMARKS` defaults to `OFF`) and are
independent of `PACIOLI_BUILD_TESTS`. Build and report meaningful numbers with
optimizations enabled:

```sh
cmake -S . -B build-bench \
  -DCMAKE_BUILD_TYPE=Release \
  -DPACIOLI_BUILD_TESTS=OFF \
  -DPACIOLI_BUILD_BENCHMARKS=ON
cmake --build build-bench
./build-bench/pacioli_benchmarks --size 100000
```

Supported sizes are `10000`, `100000` (the default), and `1000000`. Select one
with `--size`, or set `LUCA_BENCHMARK_SIZE`. The million-event suite is manual
and is not registered with CTest. `--filter substring` runs selected cases,
`--repetitions N` changes the five-run default, and `--format json` or
`--format csv` produces capture-friendly output. For example:

```sh
./build-bench/pacioli_benchmarks --size 10000 --filter ledger --format json > ledger.json
```

Debug output is useful only as a compile/smoke check, not as a production
performance measurement. The executable records compiler/version, CMake build
type, operating system, architecture, dataset size, and repetitions.

## Deterministic workload

The generator uses a fixed pattern (metadata seed `0x4c554341`) and no entropy:

- 100 accounts, 1,000 instruments, and USD/EUR/GBP;
- 10% `CashMovement` and 90% `EquityTrade` events;
- alternating positive and negative cash/quantities;
- seven future settlement offsets plus settled dates;
- repeated account/instrument and settlement aggregation keys;
- monotonically generated IDs and second-granularity timestamps; and
- a deliberate deterministic timestamp permutation for out-of-order cases.

Synthetic event and observation construction happens before measured regions.
Ledger append necessarily constructs a fresh ledger in the measured region so
sequence assignment, entry copies, duplicate-index lookup, and index maintenance
are included; the source event vector is generated beforehand. Lookups also
prebuild both present and absent `EventId` values.

## Cases and output

The suite reports total input, result/matching/break count, median elapsed time,
minimum/maximum elapsed time, and events/lookups/rows per second for:

- ledger append and existing/missing event lookup;
- full economic ordering for mostly ordered and out-of-order histories;
- broad/narrow inclusive as-of and half-open economic ranges;
- position projection over repeated account/instrument keys;
- settled-cash projection for mostly unsettled, mostly settled, and mixed events;
- settlement obligations for many-open and mostly-settled trades; and
- position and cash reconciliation at exact match, 1%, and 10% break rates,
  including deterministic missing, unexpected, and mismatch breaks.

Each case validates basic output once outside timing, executes one untimed
warm-up, then reports the median/minimum/maximum of five measured repetitions by
default. A volatile result sink prevents the measured result from being removed.

Static object sizes are reported for `EconomicEvent`, `LedgerEntry`, `Position`,
`CashBalance`, `SettlementObligation`, `PositionObservation`, and
`CashObservation`. On Linux, the runner also reads `/proc/self/statm` and reports
current process resident bytes after fixtures and measurements; other platforms
report zero for that explicitly Linux-only metric. RSS includes the runner and
all live fixtures, so it is a coarse process-level diagnostic, not retained heap
size per event.

## Interpreting baselines

The [2026-10-21 verification report](../../docs/reviews/JOURNAL_BALANCES_20261021.md)
records executed Debug/Release checks and an initial AppleClang/macOS benchmark
baseline, with raw JSON under `results/2026-10-21-macos-arm64/`.

The opt-in accounting surface also supplies `journal_balances.full` and
`journal_balances.merge_prepared_4` cases: use `--filter journal_balances` to select
them. Synthetic whole journals use one explicit book, 100 cash chart accounts,
three currencies and unit contributions. Setup and four-partial construction are
outside timing; measured work includes sorted evidence construction. Validation
compares full/merged results and independently checks the expected gross total.
The merge-only case excludes partial computation and is not an end-to-end
parallel speedup. The suite still prepares other existing benchmark fixtures;
its process memory includes that setup. Production staged-ingestion/report latency,
worker scaling and larger representative datasets remain separate acceptance.

Numbers depend on the processor, memory, operating system, compiler, standard
library, build flags, system load, and dataset. They are observations, not
architectural guarantees. Any performance claim must include hardware details,
compiler/version, build type and flags, dataset size, repetitions, and the exact
command. Use comparable Release builds, and do not infer an optimization mandate
from one result. Algorithm changes and profiling-driven optimization are
intentionally outside this initial measurement infrastructure.

## Native staged validation and admission

Enable `LUCA_BUILD_INGESTION=ON` and `LUCA_BUILD_BENCHMARKS=ON`, then run:

```sh
build/native-release/libs/ingestion/luca_native_ingestion_benchmark \
  /tmp/luca-native-ingestion.json
```

The native C++ runner separately measures durable receipt and background parse,
exact validation and atomic admission, with 1k/10k/100k records and three fresh-store
repetitions. Declared input groups contain at most 256 records. It also measures
256-record incremental appends with the same worker/cache and a six-cell worker/
book matrix with 400 independent mixed-validity records. Every result checks
committed/invalid counts. Grouping is input atomicity, not a permitted silent
optimization. No HTTP, reports or GPU work is measured.

The older `processing-grouped.json` artifact describes the Python prototype at
commit `22353bf`, which remains reproducible from Git history. Its source script
and runtime are removed from the application. Do not infer a language speedup from
unmatched durability profiles, compiler flags or workloads; see
[the native verification report](../../docs/reviews/NATIVE_INGESTION_20261021.md).

`prepared_append_benchmark.cpp` compares the unchanged public strict `accept`
contract before/after suffix preparation. Compile it with `-std=c++23 -O3` against
each revision's ledger include directory. It seeds 1k/10k/100k records outside
timing, appends 100 individual records for three repetitions and hashes the full
result outside timing. Before/after canonical hashes must match; this is an
in-memory acceptance comparison, not durable-ingestion throughput.

## Custodian lots

`luca_lot_benchmark` (enable `LUCA_BUILD_BENCHMARKS`) measures admission, lifecycle resolution and the LIFO fold for long/short crossing, 1,000 custodian pools, and deep short inventory. Each input has two separate charge components. See [lot accounting](../../docs/lots-and-charges.md) for workload counts and the initial measured baseline. This is a CPU in-memory benchmark, not network/storage throughput.

## Bounded application reporting

`python3 -B tests/benchmarks/application_reporting_benchmark.py BUILD [COUNTS...]`
creates disposable native books, admits synthetic trades in existing atomic
groups, and measures three full CLI report calls per size. It includes startup,
SQLite reads, complete projection/materialization/hash and first-page encoding;
receipt/admission setup is excluded. It records explicit projection-limit
rejections rather than treating them as available reports. The 2026-10-23
[review evidence](../../docs/reviews/CLI_MCP_REPORTING_20261023.md) describes the
measured synchronous boundary and distinguishes typed-core timings from complete
adapter latency. Results are not production SLAs or before/after improvements.
