# Durable intake — executed verification, 2026-10-21

Base financial main: `6dbf9da86bacf8d8a0f290c081a7da3c531114ed` (PR #19).
Architecture proposal: `534a570609118c4705b12c53be6090cfd0a3ab96` (PR #20).
Implementation branch: `feat/durable-intake-20261021`.

## Delivered boundary

The optional [local host](../../apps/ingestion-host/README.md) durably retains
original bytes and atomically writes import receipt, scoped idempotency identity,
validation context metadata, parse work intent and initial status event. It
supports bounded streaming, range reads, resumable checked chunks, full upload
verification, recovery of interrupted transfers, expiry of unattached sessions
and logical capacity reservations. A local API and CLI expose these operations.

This is step 1 of the [staged-ingestion design](../staged-ingestion.md), not the
complete staged-ingestion feature. Original invalid/unparseable bytes are retained
without transaction parsing. Status remains enumeration pending, work queued and
not committed. Validation workers, structured per-transaction diagnostics,
dependency groups, durable ledger admission and HTTP remain unimplemented. No
C++ financial code, canonical bytes, installed target or package dependency changed.

The Python host uses standard SQLite/filesystem adapters. Its storage profile is
durably pinned before opening SQLite: patched runtimes can create WAL/FULL stores;
older runtimes create DELETE/EXTRA stores. A newer runtime does not automatically
change an existing store's mode; an unpatched runtime cannot open a host WAL store.
The explicit fallback addresses the upstream
[WAL-reset issue](https://sqlite.org/wal.html#walreset). Profile/configuration
migrations are outside this increment.

## Environment and correctness evidence

Apple M3 Max, 16 logical CPUs, 48 GiB RAM; macOS 15.7.8 build 24G824, arm64.
AppleClang 17.0.0.17000604, CMake 3.29.2, Unix Makefiles. CMake selected Python
3.12.2 (Anaconda), SQLite 3.41.2, using DELETE/EXTRA. Additional execution used
Python 3.12.14 with SQLite 3.53.1 (WAL/FULL) and Homebrew Python 3.12.3 with
SQLite 3.45.3 (DELETE/EXTRA).

| Check | Actual result |
| --- | --- |
| Complete Debug root suite | 35/35 passed, including financial/conformance, CLI/Python and installed/embedded consumers. |
| Complete Release root suite | 35/35 passed, with C++ test assertions active. |
| Final focused durable-intake suite | 29 tests passed on WAL/FULL and DELETE/EXTRA; rerun after final store hardening. |
| Initial additional Homebrew execution | Intake suite passed on SQLite 3.45.3; final rollback benchmark also exercises this runtime. |
| Financial semantics | No changes; full root checks remain passing. |
| Source/static checks | Python syntax/configuration/fingerprint JSON, documentation links and whitespace checked. |

The full root runs preceded the last two focused host regression cases; the final
focused reruns cover all 29 current host tests. No financial source/build change
followed the full runs. Pre-existing unused lambda capture/missing initializer
warnings remain outside this slice; builds used `-Wall -Wextra`, not `-Werror`.

Tests include concurrent same-key threads/processes; different content/options
under one key; tenant/book/source separation; pinned context/limits; retry while
capacity is full; bounded reads/partial output writes; out-of-order chunks; exact
chunk retry/conflict; full checksum failure retained in status; session expiry
without deletion of attached evidence; active-writer exclusion during recovery;
atomic event/receipt/work rollback on an injected SQL error; and runtime/profile
pinning before database access. Published submission fingerprint vectors are
included for reuse by another host implementation.

Real child processes receive `SIGKILL` at these boundaries:

- After capacity reservation, after payload flush and after immutable installation.
- Before receipt transaction commit and after it commits but before a response.
- After chunk flush, after chunk metadata commit and after upload installation.

Reopen/recovery verifies receipt/work/event agreement, exact retained bytes,
idempotent retry, quota accounting, SQLite integrity and foreign keys. File-flush
failure prevents acknowledgement. These are process-crash and adapter-call tests,
not physical power-loss, full-disk-loss, backup/restore or replicated durability
certification. Linux/other filesystems and production host acceptance remain open.

## Commands

From the implementation checkout:

```sh
cmake -S . -B build/review-debug -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DLUCA_BUILD_TESTS=ON -DLUCA_BUILD_BENCHMARKS=ON \
  -DCMAKE_CXX_FLAGS="-Wall -Wextra"
cmake --build build/review-debug --parallel 2
ctest --test-dir build/review-debug --output-on-failure --parallel 2

cmake -S . -B build/review-release -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DLUCA_BUILD_TESTS=ON -DCMAKE_CXX_FLAGS="-Wall -Wextra"
cmake --build build/review-release --parallel 2
ctest --test-dir build/review-release --output-on-failure --parallel 2

python3 -B -m unittest discover -s tests/ingestion -v
ctest --test-dir build/review-release -R luca_durable_intake_test --output-on-failure
```

The final WAL run used the app's bundled Python executable:
`/Users/andrew/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3`.
Use a patched SQLite runtime for WAL; the same command with an older supported
runtime verifies the disclosed rollback profile. No runtime was installed or upgraded.

## Intake-only benchmark

Raw results are in
[WAL](../../tests/benchmarks/results/2026-10-21-durable-intake/macos-arm64-wal.json)
and [rollback](../../tests/benchmarks/results/2026-10-21-durable-intake/macos-arm64-rollback.json).
They include exact host/benchmark source hashes, runtime/profile, individual
samples and concurrency cases. Three measured fresh imports per size; no separate
warmup. Fixture creation, initial host setup and post-receipt full hash verification
are outside individual receipt timings. Source files are local and cache-warm;
there is no network upload. Timings include streaming source reads/hash, durable
payload writes/flushes/rename, quota reservation and atomic receipt/work/event
storage. Every retained output is hash-checked against the generated fixture.

| Profile | Source records | Payload bytes | Median receipt seconds | Median last-byte-to-receipt ms |
| --- | ---: | ---: | ---: | ---: |
| WAL/FULL, SQLite 3.53.1 | 10,000 | 2,167,780 | 0.038988 | 23.131 |
| WAL/FULL, SQLite 3.53.1 | 100,000 | 21,877,780 | 0.053250 | 26.287 |
| WAL/FULL, SQLite 3.53.1 | 1,000,000 | 220,777,780 | 0.161671 | 27.593 |
| DELETE/EXTRA, SQLite 3.45.3 | 10,000 | 2,167,780 | 0.037489 | 22.831 |
| DELETE/EXTRA, SQLite 3.45.3 | 100,000 | 21,877,780 | 0.050955 | 23.643 |
| DELETE/EXTRA, SQLite 3.45.3 | 1,000,000 | 220,777,780 | 0.154258 | 26.010 |

Peak process RSS over the entire benchmark was 66,748,416 bytes for the WAL
runtime and 73,367,552 bytes for the rollback runtime, including fixture generation
and verification. This is not per-record memory or OS page-cache accounting.

There are also eight-import runs at 1/2/4 threads for a shared book and independent
books. Their wall time includes post-receipt output verification, unlike each
receipt sample. Small samples, filesystem caching and different Python/SQLite
runtimes prevent a controlled WAL-versus-rollback comparison or scaling claim.
No p99/SLO, Geneva advantage, validated transaction rate, ledger admission rate or
report throughput is established. Million-record inputs are just opaque bytes to
this intake host. Python peak RSS excludes the OS page cache.

```sh
python3 -B tests/benchmarks/intake_benchmark.py --records 10000 100000 1000000 \
  --repetitions 3 --output /tmp/luca-intake-benchmark.json
```

## Remaining acceptance

Keep metadata/context migrations explicit. Current queue/payload limits are
bounded local defaults, not workload-derived service budgets; receipts and
idempotency identities are retained without automatic purge. Add validation
workers with fenced result publication and retained transaction diagnostics next,
then integrate strict lifecycle admission through an atomic durable commit receipt.
Representative concurrent import/report, recovery, latency and cost budgets remain
roadmap checkpoint 8. Shared batches and efficient incremental evidence remain
their own checkpoints.
