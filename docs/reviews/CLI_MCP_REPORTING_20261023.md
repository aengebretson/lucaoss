# CLI/MCP bounded accounting reporting review — 2026-10-23

Base: `e47a2d74376c5cbce4f3f01f10b25344352fa05c` (`origin/main`, PRs 26/27 merged).
Branch: `work/cli-mcp-reporting-20261023`. Direct user assignment; isolated local
worktree; no shared planning, Enterprise, live-data, policy or deployment changes.

## Delivered behavior

The shared optional C++ SQLite application facade, thin CLI and stdio MCP now
expose existing journal lines, trial balances, trade obligations, actual cash and
internal accounting reconciliation. They also expose durable settlement/full
refund receipt and retained status, plus accepted transaction history with
period/account/instrument/source/type/status filters and historical cutoffs.
Financial rules and asynchronous processing stay in existing C++ libraries and
workers. One additive immutable evidence read and one receipt fault-injection
checkpoint extend the existing optional storage adapter; no migration is added.

Bounded synchronous reports retain ledger/policy/settlement prefixes, economic
and recorded cutoffs, complete configured account bindings, mapping identity,
selected policy revisions/resolutions and content digests. Cursors and JSON/JSONL
pages bind the result identity. Byte/record/page limits fail explicitly; staging
completeness remains unknown. Reports are regenerated as-of projections.
Period P&L, derived period adjustments and immutable dated edit/delete adapters
remain conditional on the separately reviewed accounting contracts.

Full schemas, identifiers, grants, exact values, times, exports, idempotency and
uncertain-response recovery are in [the implementation guide](../cli-mcp-reporting.md).
The broader durable report-job API remains a proposal.

## Verification

Environment: macOS arm64, AppleClang 17, SDK SQLite 3.51.0, nlohmann JSON 3.12.0,
CMake Unix Makefiles. Python orchestrates disposable test books; it performs no
product processing or financial calculations.

Commands from the isolated OSS worktree:

```sh
cmake -S . -B ../output/debug -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Debug \
  -DLUCA_BUILD_INGESTION=ON -DCMAKE_PREFIX_PATH=/opt/homebrew
cmake --build ../output/debug --parallel 2
ctest --test-dir ../output/debug --output-on-failure
cmake -S . -B ../output/release -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Release \
  -DLUCA_BUILD_INGESTION=ON -DLUCA_BUILD_BENCHMARKS=ON -DCMAKE_PREFIX_PATH=/opt/homebrew
cmake --build ../output/release --parallel 2
ctest --test-dir ../output/release --output-on-failure
python3 -B tests/benchmarks/application_reporting_benchmark.py ../output/release 100 1000 5000 10000
../output/release/luca_trade_accounting_benchmark 1000
../output/release/luca_trade_accounting_benchmark 10000
```

Final Debug: 45/45 CTest entries passed (76.05 seconds).
Final Release: 45/45 passed (92.46 seconds). The existing conformance, recovery,
kill-boundary, native settlement, packaging/installed-consumer and parent-build
checks remain passing. `git diff --check` passed.

Eight new integration cases use the shared operation directly in a C++ embedding
peer, the CLI and a genuine initialized stdio MCP peer, validating identical
result documents against the executable schemas. They cover all five report
collections, exact amounts, whole-journal controls, filters, cursor rejection,
exports, pinned input results after later appends and restart, partial/delayed
settlement, refunds, retained invalid requests, concurrent retry identity,
read-only discovery, source/account/book denial, cash recognition requiring both
trade legs, instrument-scoped cancellation history, actual revalidation context,
negative schema cases, atomic admission boundaries, currency separation,
incomplete charges and reports while native ingestion runs. Existing adapter
coverage still verifies imports, retained diagnostics and worker restart.

The direct application test additionally injects failure after durable settlement
receipt commitment: the facade returns `OUTCOME_UNKNOWN` and identical retry
recovers the same receipt. It verifies closed-period refund rejection remains
retained, and an open-period refund can subsequently commit. Native tests retain
before/after actual-settlement admission crash/restart checks.

## Measured reporting bound

[Raw adapter measurements](../../tests/benchmarks/results/2026-10-23-macos-arm64/application-reporting.jsonl)
include process startup, immutable SQLite reads, complete projection/row
materialization/hash, and first-page serialization. Each is three Release calls
on a disposable charged-complete cash-equity fixture with no actual settlements.
Native receipt/admission setup is excluded. Existing atomic groups of at most
1,000 records keep setup bounded without bypassing native validation/admission.

| Trades | Journal lines | Median complete first-page call | Outcome |
| --- | --- | --- | --- |
| 100 | 250 | 24 ms | available |
| 1,000 | 2,500 | 159 ms | available |
| 5,000 | 12,500 | 756 ms | available |
| 10,000 | — | 481 ms | explicit 32 MiB evidence-limit rejection |

This supports bounded synchronous execution; it does not claim 10,000-record
availability for all evidence sizes. The byte bound can reject a prefix below
the count bound. Each continuation recomputes the same bounded result.
[Typed-core measurements](../../tests/benchmarks/results/2026-10-23-macos-arm64/application-accounting-core.jsonl)
include full actual-settlement trace evidence: 1,000 trades + 1,000 settlements
projected in 16 ms; 10,000 + 10,000 in 209 ms. Those exclude storage/transport and
must not be presented as complete adapter throughput or an SLA. No before/after
performance improvement is claimed. The initial independent-group 10,000-record
setup was stopped; that ingestion cost is not report latency evidence.

## Independent review

A separate read-only reviewer inspected the financial, persistence, transport,
permission, history and schema boundaries. Three P2 findings were fixed with
regressions: instrument filters dropping cancellation evidence, original import
context mislabeled as actual revalidation context, and permissive financial
output schemas. Journal trace serialization was also indexed once instead of
scanning all traces for each line. Re-review independently passed the eight new
integration cases, direct application test, pure trade-accounting test and durable
actual-settlement test, then approved the current bounded increment with no
remaining actionable blockers. Full-suite/Release results above are separate
coordinator evidence. Any integration against changed main requires fresh checks.

## Remaining limits

- Current output is as-of snapshot accounting. No period activity, financial
  statements, valuation-based unrealized P&L or conventional posted adjustment
  journal is inferred. P&L/edit/delete tool exposure awaits reviewed shared core.
- Reports are synchronous with explicit count/byte budgets and one in-process
  report owner. There are no durable report jobs or retained report artifacts.
  Save the manifest/export; existing immutable input history remains durable.
- Historical policy knowledge requires an explicit policy sequence. Legacy policy
  revisions do not provide recorded timestamps for automatic timestamp selection.
- Actual refunds are exact full reversals of one prior non-refund payment.
  Partial refunds, arbitrary cash adjustments and external bank reconciliation
  are outside the supported native contract.
- Existing static host grants remain. Dynamic revocation, OS-enforced read-only
  storage, remote MCP, full backup/restore certification, other compiler/OS
  matrices and production workloads are unverified.

No merge, deployment, live financial mutation or complete-feature acceptance is
claimed by this PR evidence.

### Native metadata schema follow-up

A dependent version-adapter review identified that a native import may contain
supported `metadata.replaces` even though the thin submission schema does not
expose that optional native feature. Accepted-history output now describes that
field concretely and preserves it. A ninth reporting integration case creates a
native import with this metadata and verifies lossless, schema-valid query output
through direct C++, CLI and stdio MCP. All three application CTest entries passed
again (11.88 seconds); financial code and the previously verified full-suite scope
are unchanged by this schema/test-only follow-up.
