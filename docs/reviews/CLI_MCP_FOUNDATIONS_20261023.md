# CLI and stdio MCP foundations verification — 2026-10-23

Base: `391c91edbd48a816c48a0c247e1b27df4dc79edc` (`origin/main`).
Branch: `work/cli-mcp-foundations-20261023`.
Direct user assignment: bounded CLI/MCP foundations alongside a separate trade
accounting session. No shared planning journal or Platform source edits.

## Delivered boundary

The optional installed `luca::application_sqlite` facade, `luca` CLI and `luca-mcp`
stdio server share scope checks, schemas, operation execution and error mapping.
They reuse native receipts/status/errors/admission, policy history/resolution/
retained evidence, canonical lifecycle decoding and economic LIFO lot projection.
No authoritative accounting, validation or persistence rule is duplicated.
No new dependency, network service or SQLite migration is introduced.

Read-only grants are the default. Submission requires explicit book/source grants;
worker ownership requires a separate explicit process grant. Durable acceptance
is distinguished from enumeration/validation/admission. Exact decimal/time formats,
opaque identifiers, cursor snapshots, pinned ledger/policy/knowledge cutoffs,
binding digests, idempotency, uncertain outcomes and retries are documented in
[the implementation guide](../cli-mcp-foundations.md).
The broader typed/storage-independent application contract and report jobs remain
proposals. This increment is an optional SQLite facade over the existing JSON
storage API, directly embeddable without CLI/MCP.

## Executed checks

Environment: macOS arm64, AppleClang 17, CMake 3.29.2, Unix Makefiles,
SDK SQLite 3.51.0 (native selected DELETE durability profile), nlohmann JSON 3.12.0,
Python 3.12.2 for test/demo orchestration. Ninja was unavailable, so the equivalent
explicit CMake Debug/Release configurations were used. No product Python runtime.

From the worktree root, build outputs were isolated in `../output/`:

```sh
cmake -S . -B ../output/build-debug -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=Debug -DLUCA_BUILD_INGESTION=ON -DCMAKE_PREFIX_PATH=/opt/homebrew
cmake --build ../output/build-debug --parallel 2
ctest --test-dir ../output/build-debug --output-on-failure
cmake -S . -B ../output/build-release -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=Release -DLUCA_BUILD_INGESTION=ON -DCMAKE_PREFIX_PATH=/opt/homebrew
cmake --build ../output/build-release --parallel 2
ctest --test-dir ../output/build-release --output-on-failure
python3 -B examples/cli-mcp/demo.py --build ../output/build-debug
```

Debug: all 42 CTest entries passed. Release: all 42 CTest entries passed.
This includes conformance, migration/recovery and package/parent consumer checks.
The installed external consumer now links `luca::application_sqlite` and proves
its status result equals the directly embedded native result. Header-only package
and option tests remain passing without the optional storage/application runtime.

The shared application test injects an after-receipt-commit failure: the facade
returns `OUTCOME_UNKNOWN`, and identical retry recovers the original receipt.
It also tests permissions, pending-vs-complete state, reopened durable status and
retained policy result evidence.

Seven real CLI/stdio-peer integration tests cover equivalent documents/schemas,
retained invalid records and errors, record history access, transport-key conflict,
exact prefix transactions, pinned economic lots, policy reads, scope denial,
read-only tool discovery, native worker restart, concurrent CLI submissions/readers
while native MCP processing runs, configured page limits, mapping drift, protocol
framing/initialization, omitted tool arguments, CLI help without storage, and
schema boundary regressions. Existing native tests additionally retain concurrent
admission/fencing and fourteen SIGKILL boundaries; the new adapter restart test
kills after receipt with processing disabled, then starts a processing-enabled
host to recover pending intent. In-flight lease reclamation remains governed by
the existing native lease contract.

The disposable demo accepts four records: before processing enumeration is pending
with unknown record count; after restart two records are committed and two invalid.
Four retained diagnostic entries include enumeration/validation evidence for the
two invalid rows. An explicit retry recovers the same receipt. CLI and MCP agree
on status/errors and one economic open lot of `7.00000000` units with
`700.000000 USD` carrying amount. The temporary store is removed on exit. No live
or shared demo store was accessed. No throughput or new benchmark claim is made.

## Independent review

A separate read-only reviewer inspected source, contracts, ownership, packaging,
protocol and tests. Six findings were corrected and regression-tested:
configured storage page limits; invalid-policy-date exit mapping; concrete output
schemas; native 128-byte idempotency-key limit; native 512-byte charge metadata;
and long retained diagnostic field paths. The reviewer independently reproduced
the relevant schema failures before correction, then reran shared application and
all seven adapter tests. Final review reported no remaining blockers, including
standard optional `tools/call.arguments` and initialization clientInfo checks.
`git diff --check` passed.

Review evidence applies to this branch's source, not a future merge candidate.
Any integration with changed main must receive combined verification/review under
the repository's integration protocol. No integration receipt, merge or deployment
is claimed here.

## Remaining limitations

- Actual settlement, recognition and journals belong to the separate accounting
  session; no tools/report operations are invented for its unfinished contracts.
- This binary is based on schema 4 and refuses newer databases. The accounting
  branch owns schema 5. Rebuild with the integrated storage code before using it.
- Lot replay is bounded to 10,000 admitted records and explicit complete configured
  custody pools. It does not assert staging completeness or run a durable report.
- MCP inline payloads are 64 KiB; larger submissions use existing `luca-ingest`
  upload/receive. No model-supplied paths are opened.
- Read-only grants are financial permissions; operator startup preserves native
  creation/migration behavior and does not open SQLite in OS-enforced read-only mode.
- Generic authorizers, live grant revocation, typed storage-independent sessions,
  durable report jobs, exports and network MCP remain later design work.
- Stdio protocol interoperability is automated; manual Codex UI/LLM-host integration,
  broader compiler/OS matrix, network filesystems and live backup/restore are unverified.
