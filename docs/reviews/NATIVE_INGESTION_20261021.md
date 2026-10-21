# Native C++ ingestion verification — 2026-10-21

PR #22 is revised to keep durable intake and all asynchronous validation/admission
execution in C++. The initial Python prototype at `22353bf` is superseded; its
[historical verification](VALIDATION_ADMISSION_20261021.md) and benchmark data are
retained as prior evidence. This report describes the native revision.

## Delivered boundary

- Optional compiled `luca::ingestion_sqlite`, with `SqliteStore`, native `Worker`
  and bounded `AsyncHost`. Submission futures resolve after durable receipt,
  separate from background financial validation/admission. Threads have explicit
  stop/join ownership; there is no interpreter, subprocess bridge or detached work.
- The native store preserves original bytes, receipts and scoped retry bindings;
  supports resumable uploads, immutable reference/context registration,
  version-checked revalidation, retained diagnostics/outcomes, snapshot pages,
  durable events, operational retries/attention status and recovery.
- C++ typed constructors/lifecycle gates remain authoritative. Group closure,
  causal ordering, duplicate/cycle/reversal checks and revision/lease fencing are
  unchanged in intent. Canonical append/status/receipt/event/work completion
  remain atomic, and cache publication follows durable commitment.
- The C++ CLI directly links the runtime. Installed consumers request
  `find_package(Luca CONFIG REQUIRED COMPONENTS ingestion_sqlite)`. A separate
  target export keeps SQLite/JSON/thread dependencies out of financial-only
  consumers. Dependencies are SQLite >=3.38, nlohmann/json exactly 3.12.0 and POSIX
  native threads/filesystem behavior; CMake downloads nothing.
- Python runtime files and the financial helper process are removed from the
  application. Original schema-1 Python intake code remains only as a frozen test
  fixture that creates legacy stores for native migration/compatibility tests.
- The optional future REST adapter is selected as µWebSockets over µSockets.
  Neither an HTTP dependency nor a listener is introduced by this slice; see
  [the native architecture decision](../native-ingestion.md).

## Correctness and packaging

Host: macOS 15.7.8 arm64, Apple M3 Max, 16 logical CPUs, 48 GiB RAM. Compiler:
AppleClang 17 (`clang-1700.6.4.2`), C++23/libc++, Unix Makefiles. Native JSON headers
are the existing nlohmann/json 3.12.0 installation. No dependencies were installed.
Debug uses existing Homebrew SQLite 3.45.3 and selects DELETE/EXTRA. Release and
sanitizers use the already bundled native SQLite 3.53.1 and select WAL/FULL. macOS
full-sync settings remain enabled. The component pins the mode before opening
SQLite and refuses a WAL store on an unpatched runtime.

Commands (dependency paths are local to this host):

```sh
cmake -S . -B build/native-debug -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DLUCA_BUILD_INGESTION=ON -DLUCA_BUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS='-Wall -Wextra' \
  -DCMAKE_PREFIX_PATH='/opt/homebrew;/opt/homebrew/opt/sqlite'
cmake --build build/native-debug --parallel 3
ctest --test-dir build/native-debug --output-on-failure --parallel 2

SQLITE_NATIVE=/Users/andrew/.cache/codex-runtimes/codex-primary-runtime/dependencies/native/poppler/poppler
cmake -S . -B build/native-release -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DLUCA_BUILD_INGESTION=ON -DLUCA_BUILD_TESTS=ON -DLUCA_BUILD_BENCHMARKS=ON \
  -DCMAKE_CXX_FLAGS='-Wall -Wextra' -DCMAKE_PREFIX_PATH=/opt/homebrew \
  -DSQLite3_INCLUDE_DIR="$SQLITE_NATIVE/include" \
  -DSQLite3_LIBRARY="$SQLITE_NATIVE/lib/libsqlite3.3.53.1.dylib"
cmake --build build/native-release --parallel 4
ctest --test-dir build/native-release --output-on-failure --parallel 2
```

Both configurations pass **38/38 root entries**, including the native installed
consumer and existing financial-only installed/embedded/package-option consumers.
The native entry exercises twelve grouped scenarios, including fourteen distinct
real SIGKILL phases. Checks cover mixed malformed/valid records, exact values,
atomic/dependency groups, parent-after-child ordering, duplicate origins, cycles,
inexact/exact reversal, reference/context revalidation, immutable history,
concurrent competing corrections, stale lease generations, in-flight prepared
admission revalidation, snapshot pages, resource/queue backpressure, native
futures/shutdown, resumable transfers, published submission fingerprint vectors,
missing references/predecessors, clock regression, operational retry exhaustion,
canonical-log corruption and retained source timestamp semantics.

Three Python migration tests create original schema-1 receipts or partial uploads
with frozen fixture code, then call only the native CLI. They verify unchanged
receipt identities, payload bytes, retry keys, work completion, resumable chunk
continuation and rejection of unauthorized source scope. The 29 original intake
fixture checks are retained separately; they verify legacy compatibility inputs,
not current native behavior. The combined migration/fixture root entry runs 32
Python tests. Python is a test dependency only for this component.

The native installed consumer constructs `AsyncHost`, receives a submission,
shuts down/join threads, resumes remaining durable work with a native worker and
verifies ledger admission. It uses no Python or helper executable.

Focused sanitizer build:

```sh
cmake -S . -B build/native-sanitize -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DLUCA_BUILD_INGESTION=ON -DLUCA_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=/opt/homebrew \
  -DSQLite3_INCLUDE_DIR="$SQLITE_NATIVE/include" \
  -DSQLite3_LIBRARY="$SQLITE_NATIVE/lib/libsqlite3.3.53.1.dylib" \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build/native-sanitize --target luca_native_ingestion_test \
  luca_prepared_admission_test luca-ingest --parallel 2
ctest --test-dir build/native-sanitize \
  -R '^luca_(native_ingestion_test|prepared_admission_test|durable_intake_test)$' \
  --output-on-failure
```

All **3/3 focused sanitizer entries** pass, including native async/threaded/crash
checks and native CLI migration. Existing unrelated unused lambda capture/fixture
warnings remain. Formatting/whitespace checks are part of the final diff review.

Two port-review regressions were fixed and checked: source evidence timestamps
retain their full valid calendar range rather than inheriting the financial
nanosecond range; unavailable reference contexts keep structurally sound members
blocked even when another group member is invalid. Neither case can cause a
false ledger admission.

## Performance evidence

The native Release runner measures the same synthetic cash shapes, group sizes,
1k/10k/100k counts, three fresh-store repetitions, 256-record incremental appends
and six worker/book workloads as the historical Python prototype. It verifies
committed/invalid counts on every run. SQLite version/profile and input byte
counts are retained; JSON object field order can differ without changing the
financial workload. These are local measurements, not production service levels,
HTTP/GPU/reporting acceptance, or a comparison with Geneva.

```sh
build/native-release/libs/ingestion/luca_native_ingestion_benchmark \
  tests/benchmarks/results/2026-10-21-macos-arm64/native-ingestion.json
```

## Remaining scope

No REST listener/authentication, streaming HTTP backpressure/abort acceptance,
projection/report coverage, distributed adapters, public operator retry endpoint,
metadata-history quota/purge, backup/restore or physical power-loss certification.
Workers retain bounded per-book caches; process memory and disk need explicit
production budgets. The local SQLite/JSON DOM reference runtime is a correctness
and measurement baseline. Scheduling, commit coalescing that preserves independent
outcomes, compact batches and storage contention still need workload-specific
profiling. No universal framework speed ranking or linear worker scaling is
claimed. Linux/GCC, Windows and network filesystems remain unverified. No platform
repository, server planning state or production deployment was changed.
