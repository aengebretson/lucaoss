# Corporate-action specification and fixture contract

Status: **design and validation only**, 2026-10-23. No corporate-action event,
entitlement or posting implementation is delivered. Baselines: OSS `391c91e` and
Platform `5112a9e`. This contract extends the proposed
[asset-servicing design](../oss-program-20260930/designs/08-corporate-actions.md)
without changing its production scope. It is not an accounting or tax compliance claim.

Read [cash dividends](cash-dividends.md), [eligibility calculation](eligibility.md), [other families](other-actions.md),
[policy decisions](decisions.md), [coverage](coverage.md), and
[implementation sequence](implementation.md). Portable synthetic inputs and expected
outputs live in [the fixture package](../../tests/conformance/corporate-actions/README.md).

## Architecture and ownership

The C++ kernel owns deterministic financial transformations. OSS has no UI.
Algebras operate on explicit typed values; reporting projects immutable evidence.
Source ingestion, normalized events, resolved entitlements and report results are
separate objects. Adapters retain external payloads and translate them without
putting vendor schemas in the kernel. Python here validates test contracts only.

The trade-accounting session owns journals, actual trade settlement, recognition
implementation and core interfaces. This work proposes corporate-action inputs
and expected effects, without editing those interfaces, adding migrations, or
claiming a trade settlement record can represent an entitlement payment. Its
approved opening-charge capitalization and proportional cash recognition of
matched trades do not automatically decide dividend recognition or tax treatment.

## Universal event mechanics

All identifiers below are opaque, namespace-qualified strings; examples use
`synthetic:`. Tenant/book scope is part of the identity, never inferred from a symbol.

| Record | Stable identity and required evidence |
|---|---|
| Source | Source system, qualified external reference, immutable bytes/hash/locator, received time, source revision and source status. Conflicting providers remain separate evidence. |
| Action | `action_id`, kind, stable security ID, issuer/agent reference, revision ID, predecessor, recorded time, economic dates and terms. Provider aliases map explicitly to the action. |
| Security | Stable instrument ID plus effective-dated name/symbol/CUSIP aliases and mapping version. Identity continuity is a reviewed reference decision. |
| Account | Book, legal entity, ledger account, custodian and custodian-account IDs, effective-dated bindings. Never fan out using a mutable account list. |
| Eligibility | Snapshot/revision ID, action revision, exact signed quantities and entitlement kinds per account, ledger/settlement/borrow/claim cutoffs, contributing records, eligibility and market-rule derivation versions. |
| Entitlement | Stable `(book, action, ledger account, entitlement kind, option)` identity; immutable revision links to action and eligibility revisions, components and resolved policy evidence. Revisions do not change the root identity. |
| Payment/delivery | Independent stable agent payment/allocation reference, actual value time, received/recorded times, account, signed amount/currency or delivered security/quantity, linked entitlement revision, optional refund/reversal reference. |
| Result | Engine/projection/mapping versions, economic and knowledge cutoffs, selected record IDs, policy history cutoff and resolved evidence, source lineage, prior result and exact effects. |

The fixture's `trace` is a compact assertion of this dependency chain. Production
records must retain full source/causal lineage for each effect, not just the action ID.
A result identity hashes its complete context and selected revisions; a payment
identity stays stable across re-evaluation. Content hashing uses versioned canonical
encoding, not implementation-dependent JSON serialization.

Receive first into durable staged ingestion; acknowledge persistence, then validate
asynchronously. Retain per-record pending, valid, invalid, blocked, conflict and
admitted statuses with machine codes, field paths, dependencies and retry evidence.
These are proposed semantic states, not new API enum values. No HTTP receipt proves
financial validity. Missing policy/reference data blocks the affected record; source
errors remain invalid. Dependency arrival schedules a versioned retry without
rewriting the earlier result. Reconcile external observations separately.

Exact money is bounded at the existing six-decimal kernel scale, quantity at eight.
Rates/ratios can retain rational precision beyond those scales until an explicitly
named rounding boundary. Cash settlement currency precision is a separate input;
never assume all currencies settle to two decimals. Overflow fails, never wraps.
No cross-currency addition or balancing. Zero entitlement is a valid no-op, not a
zero-value journal line. Principal/proceeds, commissions, fees, withholding, gross
and net totals remain separate typed components.

An action revision is an ordered dependency, not a commutative addition. Within a
book/custodian pool, order by effective boundary, explicit precedence and acceptance
sequence. Ambiguous simultaneous trade/action ordering blocks. Different complete
pools may run independently against a pinned action revision; a reorganization
joins its input/output securities atomically per account. A batch fan-out manifest
pins the selected accounts and snapshot, retains each result/error and supports
restart/idempotent retries. One account's error cannot disappear in an aggregate.

Lot pools remain `(book, legal entity, custodian account, security, currency)` with
LIFO initially, long or short. Current pools are net positions, not simultaneous
hedged sleeves. Eligibility is not lot selection. An action must not relieve lots
from another account pool; securities lending and market claims require distinct
entitlement kinds even when the trading pool is net.

Corrections append new facts and regenerate affected projections with explicit
causal links. Preserve earlier knowledge-cutoff results and dated void boundaries.
No cancellation fabricates a refund or undelivers securities. Protect closed
periods at admission and across dependency-driven recomputation; stale fan-out or
period previews fail. A new economic date cannot bypass a closed original.
