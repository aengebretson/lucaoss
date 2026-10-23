#pragma once

// Implemented bounded SQLite application facade, distinct from the broader
// storage-independent v1 design. Reuses the existing optional JSON storage API.
// Enterprise embeds this object directly; CLI and MCP translate transport only.
#include "luca/ingestion/sqlite.hpp"
#include <vector>

namespace luca::application {
using ingestion::Json;
using ingestion::Result;
struct AccountBinding {
  std::string account, legal_entity, custodian_account;
};
struct BookBinding {
  ingestion::SqliteStore store;
  std::vector<AccountBinding> accounts;
  std::string mapping_version;
  bool inspect = true, submit = false, process = false;
  std::vector<std::string> submit_sources;
  std::string settlement_actor = "local-profile";
};
enum class Operation {
  books,
  capabilities,
  accounts,
  submit_import,
  import_status,
  read_records,
  read_diagnostics,
  read_record,
  policy_history,
  resolve_policy,
  policy_result,
  transactions,
  lots,
  accounting_report,
  submit_settlement,
  submit_refund,
  settlement_status,
  query_transactions
};
// All responses are owned documents in the existing optional storage boundary.
// The pure financial targets acquire no JSON/storage/transport dependencies.
// Bindings are trusted host configuration, never tool-supplied grants or paths.
class SqliteApplication {
public:
  explicit SqliteApplication(std::vector<BookBinding> bindings);
  ~SqliteApplication();
  SqliteApplication(const SqliteApplication &) = delete;
  SqliteApplication &operator=(const SqliteApplication &) = delete;
  // Thread safe. Operational failures are values; startup/allocation may throw.
  Result<Json> call(Operation operation, Json arguments) const;
  Json tools() const; // only permitted MCP tools; exact shared input/output schemas
  // Bounded native processing, explicit host ownership; pending work is durable.
  Result<std::size_t> drain(std::string book, std::size_t max_jobs = 10000) const;
  Result<void> start_workers();
  void stop() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
std::optional<Operation> operation(std::string_view name);
std::string_view name(Operation operation);
int exit_code(const ingestion::Failure &failure);
Json document(Operation operation, Result<Json> result);
} // namespace luca::application
