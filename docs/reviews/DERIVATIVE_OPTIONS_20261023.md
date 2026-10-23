# SPX/SPXW derivative increment review — 2026-10-23

Base: `ae820869d4b617fdb8ac9fdcefd37de2bbcabaf4` (current main inspected).
Scope: additive typed contract economics, observations, premium option lots,
explicit official-fixing terminal events, exact balances/period effects and full
retained evidence. ES contract economics are validated but accounting explicitly
returns `UNSUPPORTED_FUTURES_ACCOUNTING`; ES is a separate review increment.

Period-accounting, Enterprise accounting UI and CLI/MCP owners confirmed the
existing equity chart, APIs, null valuation fields and exclusions remain frozen.
This change owns only the new financial headers and the appended validated
`JournalEventType::derivative_projection`. No canonical financial-event serializer,
native admission/schema, adapter, live instrument or policy is changed. No merge,
deployment or live-data action was performed.

Independent read-only review identified and verified repairs for predecessor
recognition before correction recording, ambiguous observation lookup/selection,
closed-period interior marks, missing marked valuations, collateral before trades,
row-fold preflight and stable causal journal lineage. A final option-only review
compiled the focused suite and independently proved ES rejection. No blocking
financial correctness findings remained. Removed ES branches were not treated as
reviewed option functionality.

Validation on macOS arm64 / Apple clang17:

* `cmake -S . -B ../output/debug -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Debug -DLUCA_BUILD_TESTS=ON -DLUCA_BUILD_INGESTION=ON`
* `cmake --build ../output/debug -j 4`: passed.
* `ctest --test-dir ../output/debug --output-on-failure -j 4`: 50/50 passed,
  including canonical golden vectors, native immutable versions/settlement,
  CLI/MCP report peers, package/parent consumers and derivative fixtures.
* `cmake --install ../output/debug --prefix ../output/option-install`; independent
  `examples/derivatives` configure/build against that prefix: passed.

The repository's Ninja preset could not configure because Ninja was unavailable;
the equivalent supported Makefiles configuration above used the same Debug/test
options, additionally enabling native integration checks. No dependency was added.

Synthetic results: SPX buy2@10/sell2@12.50 = gross500; charges and partial relief
conserve exact micro-money; official zero terminal value expires positions once;
call/put/short cash settlement and dated-history replay reconcile to actual cash.
Versioned policies are explicit accrual only. Intraperiod policy changes and
cash-basis derivative recognition reject with diagnostics. Missing observations,
unsupported products and unavailable option notional never silently become zero.

Full replay and independent complete pools are supported. Append-and-replay is
continuation; partial-pool/checkpoint merges are not. Hosts must retain complete
report inputs, outputs/anchors and rejected requests; native derivative admission
and adapter adoption require separately reviewed versioned integration.
