# Journal-balance verification and initial performance baseline

The user explicitly resumed correctness tests and performance benchmarks on
2026-10-21. This supersedes the recorded testing pause for the ongoing work.
Historical September review records retain their original unexecuted status.

Financial implementation: `eda02debdc0e926293ca47976755733fc9c06749`, based on
`6268ab0aac37b008d5862d91ecb0d510e4deaf10`. The verification follow-up changes
test assertion settings and the parent-consumer test inventory, not financial
arithmetic. Results below apply to the combined PR, not a release or deployment.

## Environment and checks

Apple M3 Max, 16 reported CPUs, 48 GiB memory; macOS 15.7.8 (24G824), native
arm64. AppleClang 17.0.0.17000604, CMake 3.29.2, Unix Makefiles. CMake selected
Python 3.12.2 for CLI/conformance checks.

| Check | Executed result |
| --- | --- |
| Full Debug build and CTest | 34/34 passed |
| Full Release build and CTest | 34/34 passed, financial test assertions active |
| Focused address/undefined-behavior sanitizer build and execution | 1/1 passed |
| Relocated installed-package consumers | 10/10 passed, also exercised by each root suite |
| Embedded consumers | 9/9 passed, also exercised by each root suite |
| Existing Python portable conformance collection | 62 checks passed within each root suite |
| Full synthetic benchmark suite at 10,000 inputs | 23 cases completed with built-in validation |
| Journal benchmarks at 100,000 and 1,000,000 entries | Both reduction/merge cases completed with equality and expected-total validation |

The new C++ conformance check reads ten independent policy/time expectations
through real lifecycle and journal projections. It executes independent-worker
reductions, compatible merges, identity/regrouping, evidence/ownership checks,
and currency/context/duplicate/overflow rejection. Passing tests establish these
bounded cases, not every possible accounting input or broader feature coverage.

The first full run caught the parent harness still expecting eight consumers
after adding the ninth; its inventory and required-name check are corrected.
The existing build also disabled assertions in older Release test executables.
All root test executables ending in `_test` now explicitly undefine `NDEBUG`;
benchmarks and non-test consumers retain their normal optimized build settings.
The full suites above were rerun after these corrections.

Existing baseline warnings remain: unused constexpr lambda captures in
`canonical_decode.hpp`, and existing conformance-engine initializer warnings.
Whole-project `-Wall -Wextra -Werror` compilation is not claimed. The new test
and benchmark source files passed focused strict warning/syntax checks.

## Reproduction

For Debug and Release, configure the same options with the corresponding build
type and directory, then build and execute:

```sh
cmake -S . -B build/review-debug -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_CXX_FLAGS="-Wall -Wextra" \
  -DLUCA_BUILD_TESTS=ON -DLUCA_BUILD_TOOLS=ON -DLUCA_BUILD_BENCHMARKS=ON
cmake --build build/review-debug --parallel 2
ctest --test-dir build/review-debug --output-on-failure

cmake -S . -B build/review-release -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_CXX_FLAGS="-Wall -Wextra" \
  -DLUCA_BUILD_TESTS=ON -DLUCA_BUILD_TOOLS=ON -DLUCA_BUILD_BENCHMARKS=ON
cmake --build build/review-release --parallel 2
ctest --test-dir build/review-release --output-on-failure

cmake -S . -B build/review-sanitize -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DLUCA_BUILD_TESTS=ON -DLUCA_BUILD_TOOLS=OFF -DLUCA_BUILD_BENCHMARKS=OFF
cmake --build build/review-sanitize --target luca_journal_balances_test --parallel 2
ctest --test-dir build/review-sanitize -R '^luca_journal_balances_test$' --output-on-failure
```

## Performance observations

Release benchmark flags include `-O3 -DNDEBUG`; one untimed warm-up precedes
five measured repetitions. Timed full reduction includes sorted entry/line/source
evidence construction. Input journal construction, parsing, durable ingestion,
lifecycle resolution and journal policy mapping are outside timing. Inputs use
one explicit book, 100 cash chart accounts, three currencies and unit
contributions; all journal cases produce 303 account/currency rows.

| Journals | Full reduction median | Journals/second | Merge of four prepared partials median |
| --- | --- | --- | --- |
| 10,000 | 0.0112752 s | 886,902 | 0.00767496 s |
| 100,000 | 0.133912 s | 746,758 | 0.0907438 s |
| 1,000,000 | 1.3947 s | 716,999 | 1.02039 s |

Merge timing excludes computing the partials and is not an end-to-end parallel
speedup. CPU/worker scaling has not been measured. The million-entry process
reported maximum RSS of 5,175,721,984 bytes; the runner also constructs existing
portfolio/reconciliation fixtures, so this is whole-suite process memory, not
memory attributable solely to the reducer or per-entry retention.

Commands were run sequentially after all test builds completed:

```sh
build/review-release/pacioli_benchmarks --size 10000 --repetitions 5 --format json
build/review-release/pacioli_benchmarks --size 100000 --filter journal_balances --repetitions 5 --format json
build/review-release/pacioli_benchmarks --size 1000000 --filter journal_balances --repetitions 5 --format json
```

Raw medians/minima/maxima, case metadata and the supplemental process-resource
measurements are committed in
[`tests/benchmarks/results/2026-10-21-macos-arm64`](../../tests/benchmarks/results/2026-10-21-macos-arm64/).
These are initial synthetic baselines with validated outputs, not performance
improvement claims, Geneva comparisons or production service-level acceptance.

## Remaining acceptance

Current Linux/GCC execution, the compiler CI matrix and native MSVC remain
unverified; MSVC is still blocked by the baseline exact-arithmetic implementation.
Book assignment and input coverage remain caller responsibilities. Durable staged
ingestion, advancing-cutoff replay, shared prepared batches, efficient incremental
evidence, full accounting/reconciliation reporting, representative concurrent
workloads, worker/GPU scaling and release acceptance remain separate roadmap work.
