#pragma once

// DESIGN ONLY: initial adapter construction, not installed or implemented.
// Only this optional adapter header exposes the existing native SQLite store.
#include "application_api.hpp"
#include "luca/ingestion/sqlite.hpp"

namespace luca::application::v1 {
struct ExecutionLimits {
  std::uint32_t validation_workers = 2, report_workers = 1;
  std::uint32_t max_pending_calls = 16;
  std::uint64_t max_queued_bytes = 64 * 1024 * 1024;
};
struct CalculationContextDefinition { CalculationIdentity identity; ReportKind kind; };
struct SqliteBookBinding {
  BookScope scope;
  ingestion::SqliteStore store;
  std::vector<CalculationContextDefinition> calculation_contexts;
};
struct DrainResult { std::uint64_t jobs; bool more_ready; };
class ApplicationHost {
public:
  // Destruction stops intake, resolves queued calls and joins owned workers.
  virtual ~ApplicationHost() = default;
  virtual Application &application() noexcept = 0;
  virtual Result<void> start_workers() = 0;
  virtual Result<DrainResult> run_ready_work(std::uint64_t max_jobs) = 0;
  virtual void stop() noexcept = 0;
};
// Validate bindings, contexts, non-null authorizer and finite limits up front.
// Receipts/reads have an owned executor; validation/report workers start only
// when explicitly requested. Sessions retain shared closed state after host
// shutdown/destruction; later calls fail rather than dereference destroyed state.
Result<std::unique_ptr<ApplicationHost>> make_sqlite_host(
    std::vector<SqliteBookBinding>, std::shared_ptr<Authorizer>, ExecutionLimits);
} // namespace luca::application::v1
