# Transaction validation and ledger admission — 2026-10-21

Base: merged main `b03b0cd` (PR #21 durable intake). This increment adds the
bounded local worker/validation/admission path. It does not deploy a service or
complete HTTP, projection coverage, shared projection batches, or distributed
storage acceptance.

## Delivered behavior

- Receipt continues to acknowledge immutable bytes and durable work intent before
  parsing. Strict JSONL records are enumerated with stable import ordinals,
  byte/line/hash locators, source evidence and book/source-scoped identities.
- Native typed construction checks exact decimals, calendar/nanosecond ranges,
  canonical text and supported cash/equity events. Python validates wire shape,
  maps source identity and checks immutable account/instrument snapshots. All
  authoritative lifecycle acceptance remains in C++.
- Dependency/declared-group closure seals atomic membership before validation.
  Unrelated valid records advance while malformed records retain errors. Unknown
  references or missing predecessors remain blocked. Explicit revalidation pins
  another immutable context and retains all prior outcomes; committed groups
  cannot be revalidated or changed in place.
- Durable job leases/generations fence stale workers. Bounded jittered retries and
  operational attempt history stay separate from financial validity. Independent
  workers can parse/validate concurrently; per-book admission serializes using an
  OS lock, expected ledger/group revisions and the current lease generation.
- Admission rechecks the latest prefix. Canonical records/hashes, accepted
  sequence/revision, commit receipt, record status, event and work completion
  commit in one SQLite transaction. In-memory publication follows durable
  commitment. Conflicting admissions get separate immutable audit runs without
  altering earlier validation results. Recovery decodes and strictly reaccepts
  committed records; retries cannot append a group twice.
- The additive pure core `prepare_batch`/`publish` split owns its suffix and
  invalidates stale in-process plans. Existing `accept_batch` remains strict.
  Validation uses indexed causal targets and suffix-only iterative cycle checks;
  geometric container growth avoids reallocating the entire prefix on each
  append. Per-record canonical evidence and incremental cache sync avoid a full
  ledger copy/hash per group. Projection/checkpoint preparation is unchanged.
- The local API/CLI exposes record/status/error/history/commit reads, background
  once/drain/watch modes, reference/context registration and version-checked
  revalidation. Snapshot pages preserve context and record states across worker
  progress. Schema 1 migrates atomically to schema 2 while preserving old receipts,
  original bytes, retry keys and parse intent. Processing resource bounds are
  pinned at first worker creation.

## Executed correctness checks

Host: macOS 15.7.8 arm64, Apple M3 Max, 16 logical CPUs, 48 GiB RAM.
Compiler: `/usr/bin/clang++`, AppleClang 17.0.0.17000604, C++23/libc++.
No dependencies were installed. Ninja is unavailable, so equivalent explicit
Unix Makefiles builds replace the repository's Ninja presets.

```sh
cmake -S . -B build/review-debug -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DLUCA_BUILD_TESTS=ON -DCMAKE_CXX_FLAGS='-Wall -Wextra'
cmake --build build/review-debug --parallel 4
ctest --test-dir build/review-debug --output-on-failure --parallel 2

cmake -S . -B build/review-release -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DLUCA_BUILD_TESTS=ON -DCMAKE_CXX_FLAGS='-Wall -Wextra'
cmake --build build/review-release --parallel 4
ctest --test-dir build/review-release --output-on-failure --parallel 2
```

Both pass **37/37 root tests**, including installed/embedded consumers and package
option checks. The native prepared-admission test remains active under Release
and covers stale/foreign-prefix rejection, exact value-copy preparation, atomic
rejection, canonical standalone decode/trailing-byte rejection, exact decimal
errors and a 10,000-record causal chain without recursive traversal.

The root processing entry runs **22 integration tests** against the actual native
engine. The unchanged intake entry runs **29 tests**. Root Python 3.12.2 with
SQLite 3.41.2 selects DELETE/EXTRA. Both suites also pass separately on bundled
Python 3.12.14 with patched SQLite 3.53.1 selecting WAL/FULL:

```sh
PYTHON=/Users/andrew/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3
"$PYTHON" -B -m unittest discover -s tests/ingestion -v
LUCA_INGESTION_ENGINE="$PWD/build/review-release/tools/ingestion-engine/luca-ingestion-engine" \
  "$PYTHON" -B -m unittest discover -s tests/processing -v
```

Processing checks cover mixed malformed/valid data, exact precision errors,
atomic/dependency groups, parent-after-child ordering, duplicate economic origins,
cycles, inexact reversals, missing dependencies/snapshots, reference changes and
immutable history, snapshot pagination, two competing corrections, stale leases,
revalidation during prepared admission, operational failure/attempt retention,
legacy schema migration, CLI operation, monotone recorded time, resource bounds,
canonical-log corruption and fixture replay. Real child processes receive SIGKILL
at nine parse/validation/admission phases, including before/after the durable
transaction and after cache publication; restart produces one receipt/commit and
one accepted record at every phase.

A regression caught during testing allowed an unknown reference to replace an
already-invalid native field result with `blocked`. Reference checks now preserve
structural invalidity; its retained-error test passes. The competing-correction
test scheduling was also corrected so both validations actually examine the same
prefix before either admission.

```sh
cmake -S . -B build/review-sanitize -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DLUCA_BUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build/review-sanitize --target luca_prepared_admission_test \
  pacioli_event_lifecycle_test pacioli_lifecycle_serialization_test \
  luca-ingestion-engine --parallel 2
ctest --test-dir build/review-sanitize \
  -R 'luca_prepared_admission|pacioli_event_lifecycle|pacioli_lifecycle_serialization|luca_processing_test' \
  --output-on-failure
```

All **4/4 sanitizer entries** pass, including the 22 host tests using the sanitized
native helper. Existing unrelated unused-capture/fixture warnings remain; no new
build failure was suppressed. `git diff --check` passes.

## Performance evidence

See the raw JSON and methodology below. Timing is local and workload-specific;
these checks establish a baseline, not production service levels or a comparison
with Geneva. Power-loss/disk-loss durability, network latency, report workloads,
GPU execution and cross-machine scaling are not measured.

The grouped run used patched SQLite 3.53.1, WAL/FULL and macOS full-sync, the
Release native engine, three fresh-store repetitions per size, one worker and
explicit input groups of at most 256 cash records. Input construction and output
checks are outside timing. Medians:

| Records | Receipt | Background processing | Warm single-record preparation, no persistence | Durable incremental 256-record processing |
| --- | --- | --- | --- | --- |
| 1,000 | 40.6 ms | 0.716 s | 61 µs | 0.195 s |
| 10,000 | 39.9 ms | 6.523 s | 62 µs | 0.219 s |
| 100,000 | 61.6 ms | 69.573 s | 56 µs | 0.230 s |

The 100k processing run is about 1,437 records/second in this specific durable
reference profile. Acknowledgement is separate from processing. Warm preparation
includes native IPC, typed checks and suffix canonicalization, excludes durable
writes and does not publish the probe. The actual incremental workload uses the
same persistent worker/cache and a new 256-record atomic group. Peak process
RSS was approximately 955 MiB for Python and 194 MiB for a native child; this
includes benchmark fixture/state setup, not retained memory per record.

Mixed-validity worker comparisons use four 100-record imports, independent
singleton groups, 20 retained invalid records and 380 verified committed records.
Each cell is one observation, not a latency percentile or scaling guarantee:

| Worker count | Shared book | Separate book shards |
| --- | --- | --- |
| 1 | 32.854 s | 31.083 s |
| 2 | 12.539 s | 29.395 s |
| 4 | 14.839 s | 26.089 s |

This makes the current reference host's per-group transaction/worker overhead
visible. Partitioning on one local disk did not demonstrate linear scaling.
Grouped versus singleton numbers have different atomicity/workloads and cannot
be treated as an interchangeable optimization. A future production adapter
should measure commit coalescing that preserves independent group outcomes,
connection/scheduler overhead and storage contention before adding workers.
GPU reporting and shared projection preparation remain separate work.

For the core regression comparison, the same C++ benchmark source was compiled
against `b03b0cd` and this branch with AppleClang 17, `-std=c++23 -O3 -DNDEBUG`.
Each run seeds a prefix outside timing, accepts 100 preconstructed individual cash
records through the unchanged strict `accept` API, then checks the complete
canonical ledger SHA-256 outside timing. All nine before/after result hashes match.
Medians over three repetitions:

| Initial prefix | Before | After |
| --- | --- | --- |
| 1,000 | 20.684 ms | 0.117 ms |
| 10,000 | 199.285 ms | 0.430 ms |
| 100,000 | 2,883.710 ms | 7.211 ms |

The comparison removes full-prefix index/cycle reconstruction from each small
acceptance. Publication still has occasional geometric vector/index growth, as
included in these timings; it does not promise constant physical allocation cost.
These in-memory timings exclude I/O, parsing, worker coordination and reporting.

```sh
PYTHON=/Users/andrew/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3
"$PYTHON" -B tests/benchmarks/processing_benchmark.py \
  --engine build/review-release/tools/ingestion-engine/luca-ingestion-engine \
  --records 1000 10000 100000 --group-size 256 --repetitions 3 \
  --output tests/benchmarks/results/2026-10-21-macos-arm64/processing-grouped.json

# Extract base ledger headers into a separate temporary directory first.
git archive b03b0cd libs/ledger | tar -x -C /path/to/base-headers
/usr/bin/clang++ -std=c++23 -O3 -DNDEBUG \
  -I /path/to/base-headers/libs/ledger/include \
  tests/benchmarks/prepared_append_benchmark.cpp -o /tmp/luca-append-before
/usr/bin/clang++ -std=c++23 -O3 -DNDEBUG -I libs/ledger/include \
  tests/benchmarks/prepared_append_benchmark.cpp -o /tmp/luca-append-after
/tmp/luca-append-before > /tmp/strict-append-before.json
/tmp/luca-append-after > /tmp/strict-append-after.json
```

Raw artifacts: [grouped/worker pipeline](../../tests/benchmarks/results/2026-10-21-macos-arm64/processing-grouped.json),
[strict append before](../../tests/benchmarks/results/2026-10-21-macos-arm64/strict-append-before.json),
[strict append after](../../tests/benchmarks/results/2026-10-21-macos-arm64/strict-append-after.json),
and [comparison environment](../../tests/benchmarks/results/2026-10-21-macos-arm64/strict-append-environment.json).
The pipeline artifact identifies all Python source hashes and the exact engine
binary hash. Source fingerprints were checked after execution; the artifact records a later
EOF-only whitespace cleanup whose parsed Python AST is unchanged.

## Remaining acceptance

The host is trusted-local POSIX/Python/SQLite, with no HTTP listener/authentication,
operator retry endpoint, production distributed adapters, projection coverage,
metadata-history quota/purge, backup/restore or power-loss certification. Local
objects/events still need the proposed HTTP representation mapping; canonical
hashes are retained per record, without an additional aggregate candidate hash. Windows,
network filesystems and Linux/GCC runtime acceptance remain unverified. Each native
worker retains a bounded book cache; process count/memory must be budgeted outside
this host. Resource limits and immutable context/version behavior are part of the
local contract, documented in the host guide. Wider platform integration must pin
the reviewed OSS revision and independently verify its gateway/security/coverage
contract. No platform repository or server planning state was changed.
