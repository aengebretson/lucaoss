# Immutable transaction version adapters — 2026-10-23

Base reporting commit: `1edfdf887bce951d263271d05876a2f5fbfa1f15` (PR29).
Financial prerequisite: PR30 exact `22fd8e5607abf7dfd6549294e2f8f1f1b21055a8`,
consumed as three unchanged financial patches; application-test conflict resolution
retains both suites. Dedicated branch `work/cli-mcp-version-compat-20261023`.

Shared C++ application operations expose durable edit/delete submission and source
evidence through thin CLI and stdio MCP adapters. Native admission owns paired
replacement atomicity, immutable versions, dated voids and closed-period protection.
Queries restore canonical version pairs and expose own void versus inherited
activation evidence. Policy reports retain independent policy knowledge timestamps.
Actor identity comes from the trusted book profile. No application financial rule,
source mutation, merge, deployment or live-data/policy change is introduced.

The complete operation schemas and recovery rules are in
[cli-mcp-versions.md](../cli-mcp-versions.md).

## Verification

Configured Unix Makefiles Debug and Release with LUCA_BUILD_INGESTION=ON and
CMAKE_PREFIX_PATH=/opt/homebrew; built with `cmake --build ../output/{debug,release}
--parallel 2` and ran `ctest --test-dir ../output/{debug,release} --output-on-failure`.
Debug: 47/47 passed, 79.56 seconds. Release: 47/47 passed, 90.58 seconds.

Five additional integration cases compare direct C++, CLI and a real initialized
stdio MCP peer using disposable synthetic books. They verify historical knowledge
and void gates, restart after durable receipt, original source/hash/actor/reason
retention, atomic edit admission, native replaces metadata, legacy undated voids,
late actual cash, explicit refunds, invalid replacement retention, concurrent retry
identity and source/account/book/read-only scope isolation. Malformed, duplicate-key
and deeply nested replacement input is retained for native rejection, never reduced
to a cancellation. Direct C++ fault injection verifies postreceipt uncertain response
recovery, closed-period rejection and actor-bound retry identity. Existing native
crash/recovery, packaging and conformance suites pass. `git diff --check` passes.

Independent read-only adapter review cleared the current production diff and
independently passed five version cases and the direct application test. Both adapter P2
findings (legacy undated cancellation status and strict source replacement metadata)
were fixed with regressions. Inherited activation evidence was a separate core
prerequisite. The financial owner separately cleared exact PR30
22fd8e5, including activation through reverse descendants. Integration against
changed main still requires fresh combined review/checks.

## Limits

PR30 schema6 is a separate prerequisite; this branch does not invent a competing
migration. No period P&L adapter is claimed here; the newly reviewed PR31 contract
will be consumed in a separate bounded increment. Existing snapshot reports remain
synchronous and bounded, with explicit staging completeness and retention limits.
No remote CI, production or broader compiler acceptance is claimed.
