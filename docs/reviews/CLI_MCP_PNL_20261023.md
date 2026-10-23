# Bounded period P&L adapter review — 2026-10-23

Branch `work/cli-mcp-period-pnl-20261023`, base `bc40ed2` (PR32 adapter,
PR29 reporting and PR30 immutable financial versions). Financial prerequisite
PR31 exact `fbbda2f5a9c85458b5f498ceea8939fb1d24aec0` is consumed unchanged
as `1add6f5`. Pure financial/storage/canonical implementation paths have no
adapter diff against that reviewed prerequisite. Current origin/main remains
`e47a2d74376c5cbce4f3f01f10b25344352fa05c`; dependent PRs remain unmerged.

## Delivered scope

One shared optional C++ `query_pnl` operation, thin CLI `pnl query`, and stdio MCP
`luca_query_pnl` expose period-accounting/1 economic/recognized/deferred realized
P&L with opening/activity/closing distinctions. Complete native prefixes and
independent policy knowledge feed the reviewed full-boundary replay. Account and
instrument filtering uses the pure post-fold selection; totals cover all selected
rows before paging. Whole-book UTC daily activity and complete pinned financial
input evidence have separate collection schemas. Unknown/unsupported values,
financial diagnostics and admitted-only staging limitations remain explicit.

Stateless JSON/JSONL pages bind full input evidence, source/admission context,
cutoffs, mappings, rules, budgets and selection. Synchronous bounded execution
reuses the existing report mutex and native immutable reads/workers. No financial
rule, native schema migration, new worker, remote service, report job, close
mutation, deployment, live policy/data change or PR merge is introduced.

The period financial owner reviewed the adapter's two explicit capability fences:
all period P&L queries on books with any native close fail until complete retained
period-result/exception storage exists, including later open intervals; missing
legacy policy knowledge fails rather than being inferred. Availability is checked
before and after replay. Existing snapshots remain available under their older
accurate as-of labels. Full details in [the guide](../cli-mcp-pnl.md).

## Verification commands and cases

Configured Unix Makefiles with `LUCA_BUILD_INGESTION=ON`,
`LUCA_BUILD_BENCHMARKS=ON`, `CMAKE_PREFIX_PATH=/opt/homebrew`, Debug and Release.

```sh
cmake --build ../pnl-output/debug --parallel 2
ctest --test-dir ../pnl-output/debug --output-on-failure
cmake --build ../pnl-output/release --parallel 2
ctest --test-dir ../pnl-output/release --output-on-failure
python3 -B tests/benchmarks/application_period_benchmark.py ../pnl-output/release 20 100 250 450
python3 -B tests/benchmarks/application_period_benchmark.py ../pnl-output/release 440
../pnl-output/release/luca_period_accounting_benchmark 100
```

Seven adapter integration cases compare direct C++, CLI and a real initialized
stdio MCP peer and validate their outputs against concrete executable schemas.
They cover period/balance meaning, UTC daily activity, exact signed amounts,
full-pool filter totals before pagination, source/canonical/policy/cash evidence,
all-page JSONL export, mismatched cursors, unchanged results after append/restart,
partial cash recognition and explicit full refunds, retained invalid refund,
knowledge cutoffs, future dated-void/replacement gates and split-commit rejection.
They also verify unavailable valuation remains null, diagnostics survive empty
display selection, unknown charges, bounds, genuine replay-work-limit rejection,
read-only/book/mapping scope and concurrent native ingestion with pinned readers.
Legacy policy knowledge absence is simulated only in a disposable database.

The direct C++ application test additionally checks that both closed-overlap and
later-open P&L requests fail on a book with a native close; the close receipt and
admission protections remain intact. Earlier native crash/restart, uncertain
receipt, immutable edit, settlement retry and packaging/conformance checks remain
part of the combined suite. No Python product financial calculations are added.

## Measured budget

[Raw application samples](../../tests/benchmarks/results/2026-10-23-macos-arm64/application-period-pnl.jsonl)
contain three full Release calls for each pinned synthetic book; all successful
calls reproduce their book's report identity. Complete CLI call median at
20/100/250/440 distinct trade boundaries: 16/102/578/1820 ms. At 450 boundaries,
the explicit 200,000-work-unit budget rejects without partial result in 63 ms.
Inputs are charged-complete, no actual settlements; setup is excluded. The
[pure core sample](../../tests/benchmarks/results/2026-10-23-macos-arm64/application-period-core.jsonl)
includes 100 trades and 100 actual settlements, 251 derived effects, about 226 ms,
excluding native storage/transport. These justify a small synchronous surface,
not an SLA, unbounded throughput, or an improvement claim.

## Review and remaining limits

Independent read-only review found no actionable implementation, schema, scope
or evidence blockers, independently ran the initial adapter cases, direct
application and pure period tests, and requested final workload/docs/full-suite
evidence before exact final approval. Final combined Debug: 49/49 passed (91.25 seconds). Release: 49/49 passed
(99.08 seconds). The independent reviewer reran all seven final integration cases
and approved the current adapter diff without implementation blockers; the minor
guide-banner placement finding was corrected. `git diff --check` passes. Changed
integration bases require fresh checks/review.

UTC only; cash/equity cost-based coverage only; no valuation or complete financial
statements. Native close anchor/result/exception retention, durable report jobs,
CSV/Excel export and affected-pool incremental replay remain future work. Local
AppleClang17/macOSarm64 evidence does not imply production, broader compiler or
remote CI acceptance. Existing native admitted-only diagnostics are preserved.

The focused adapter PR targets a frozen composite prerequisite branch at
`1add6f5`, containing reviewed PR32 plus unchanged PR31. It deliberately excludes
those already-reviewed financial patches from the adapter diff; integration must
reconcile dependencies and retarget after their normal review process.
