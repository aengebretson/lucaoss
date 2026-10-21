# Native asynchronous ingestion boundary

Decision, 2026-10-21: the user requires Luca's asynchronous ingestion APIs and
execution to be C++. The earlier Python orchestration in PR #22 is superseded.
The optional native runtime now directly calls the existing financial library.

| Responsibility | Implementation |
| --- | --- |
| Financial types, exact arithmetic, lifecycle preparation/acceptance, canonical encoding | Existing pure C++ library targets |
| JSONL shape/identity adapter and reference checks | Native C++ ingestion adapter |
| Durable receipt, immutable payloads, transfer, status/errors/history, leases and admission coordination | Optional compiled `luca::ingestion_sqlite` library |
| Owned receipt queue and background execution | C++ futures, bounded queue, `std::jthread` and stop tokens |
| Local reference application | Native `luca-ingest` |
| REST transport, when added | Optional µWebSockets/µSockets adapter |
| Legacy store generation and cross-language migration assertions | Python test fixtures only |

`SqliteStore` has an explicit book scope and owns no global mutable state. It
provides synchronous storage operations returning `std::expected`. A caller can
embed it directly or use `AsyncHost` for owned submissions. Submission futures
resolve after durable receipt. They do not authorize ledger acceptance. Workers
consume durable jobs with leased generations and retain outcomes/diagnostics;
strict admission is ordered per book. Cache reconstruction never becomes the
source of truth. All resource limits and ownership rules are explicit.

The optional component depends on SQLite, nlohmann/json 3.12.0 and native threads.
The standard library does not supply a JSON parser or a transactional durable
store, so these dependencies remain at the external/storage adapter boundary.
Its public JSON objects represent versioned external documents; authoritative
financial values remain typed C++ objects. The separate package component export
keeps dependencies out of financial-only consumers. No generic scheduler,
interpreter, cloud framework or broker is added to financial targets.

## REST choice

Use µWebSockets over its µSockets foundation when implementing the REST contract.
The library supports HTTP routes as well as WebSockets and scales through isolated
applications/event loops per thread. Its documentation requires socket access on
the owning loop and provides `Loop::defer` for cross-thread completion. µSockets
is the lower-level eventing/network/crypto foundation, not a separate REST router.
Sources: [µWebSockets architecture](https://github.com/uNetworking/uWebSockets),
[threading/request lifetime documentation](https://github.com/uNetworking/uWebSockets/blob/master/misc/READMORE.md),
[µSockets](https://github.com/uNetworking/uSockets).

No workload-equivalent experiment establishes a faster alternative for Luca.
Framework marketing benchmarks cannot establish durable ingestion latency. Use
this transport choice and compare end-to-end receipt p50/p95/p99, concurrent
uploads, status polling, slow/disconnected clients, bounded memory, TLS behavior
and worker/report contention when the adapter is implemented.

The transport must:

1. Authorize tenant/book/source and enforce transport limits before storage work.
2. Copy required request metadata before the request callback returns; never pass
   borrowed request objects or socket pointers to financial/storage workers.
3. Stream large bodies through bounded storage handoffs with read backpressure;
   avoid buffering every upload on the event-loop thread.
4. Run SQLite, fsync, parsing, validation and report work on bounded native workers.
5. Return `202` only after durable receipt/work intent commits; marshal completion
   to the owning loop and handle an aborted response without dereferencing it.
6. Preserve idempotency after disconnection: a lost response can still correspond
   to a committed receipt, and the client retries the same key.
7. Keep polling/status durable and notifications optional. WebSocket delivery
   never substitutes for stored status or transactional work intent.

No listener, REST framework dependency, authentication deployment or HTTP
performance acceptance is included in the current C++ runtime slice. The existing
HTTP specification remains the target for that next adapter.

## Verification and limits

The native tests exercise direct C++ calls, native threads/futures, concurrent
admission, lease/context fencing and fourteen real SIGKILL boundaries. Separate
migration tests create original schema-1 stores with frozen Python fixtures, then
exercise only the native CLI. An installed consumer explicitly imports the native
component and completes ingestion without an interpreter. Financial-only package
consumers remain independent of the adapter dependencies.

See [the host guide](../apps/ingestion-host/README.md) and
[current executed verification](reviews/NATIVE_INGESTION_20261021.md). JSON DOM
allocation, per-group durable transaction costs, worker cache memory and storage
contention still need profiling and representative budgets. C++ alone does not
guarantee linear scaling or establish reporting/GPU performance.
