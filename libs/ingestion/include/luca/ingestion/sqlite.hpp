#pragma once

// Optional storage/runtime adapter. The financial targets do not include this
// header or depend on SQLite, JSON, threads, a web server, or an interpreter.
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <future>
#include <istream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stop_token>
#include <string>

namespace luca::ingestion {
using Json = nlohmann::json;
struct Failure {
  std::string code;
  bool retryable = false;
};
template <class T> using Result = std::expected<T, Failure>;
struct WorkerLimits {
  std::size_t batch_records = 256, max_records = 100000, max_record_bytes = 65536;
  std::size_t max_group_records = 1000, max_ledger_records = 1000000;
  std::uint32_t lease_seconds = 60, max_attempts = 3;
};
struct Hooks {
  std::function<std::int64_t()> clock;
  // Testing/observability hook. Must be thread safe when a store is shared.
  std::function<void(std::string_view)> checkpoint;
};
struct WorkResult {
  std::string work_id, kind, outcome, code;
};

// Thread/process-safe book-scoped durable adapter. JSON values belong to this
// external-format boundary; exact financial values remain C++ domain objects.
class SqliteStore {
public:
  struct Impl;
  static Result<SqliteStore> open(std::filesystem::path root, Json config, Hooks hooks = {});
  Result<Json> receive(Json metadata, std::string key, std::istream &input,
                       std::uint64_t bytes) const;
  Result<Json> create_upload(std::string key, std::uint64_t bytes, std::string sha256,
                             std::uint64_t chunk_bytes) const;
  Result<Json> put_chunk(std::string id, std::uint64_t index, std::string_view bytes,
                         std::string sha256) const;
  Result<Json> verify_upload(std::string id) const;
  Result<Json> receive_upload(Json metadata, std::string key, std::string upload) const;
  Result<Json> upload_status(std::string id) const;
  Result<Json> recover(std::string after = "", std::size_t limit = 100) const;
  Result<Json> register_reference(std::string id, Json definition) const;
  Result<Json> register_context(std::string id, Json definition) const;
  Result<Json> revalidate(std::string import_id, std::string key, Json request) const;
  Result<Json> status(std::string import_id) const;
  Result<Json> record(std::string import_id, std::string record_id, std::string run_id = "") const;
  Result<Json> group(std::string import_id, std::string group_id) const;
  // Bounded snapshot pages. filters are part of the cursor identity.
  Result<Json> page(std::string collection, std::string import_id, Json filters = Json::object(),
                    std::string cursor = "", std::size_t limit = 100) const;
  Result<Json> events(std::string import_id, std::uint64_t after = 0,
                      std::size_t limit = 100) const;
  Result<Json> payload(std::string import_id, std::ostream &output, std::uint64_t offset = 0,
                       std::optional<std::uint64_t> length = {}) const;
  Result<Json> capabilities() const;
  Result<Json> book_state() const;
  // Immutable, book-scoped policy revisions. expected_sequence fences stale edits.
  Result<Json> put_policy(Json revision, std::uint64_t expected_sequence, std::string actor) const;
  Result<Json> policy_history() const;
  Result<Json> resolve_policy(Json target, std::string effective_at,
                              std::optional<std::uint64_t> through_sequence = {}) const;
  // Content-addressed result evidence; repeated identical evidence is idempotent.
  Result<Json> retain_policy_result(Json evidence) const;
  Result<Json> policy_result(std::string id) const;
  Result<Json> periods() const;
  // Close all dates through an inclusive UTC boundary, fencing the reviewed
  // accepted sequence. Stable request identity makes retry safe.
  Result<Json> close_period(std::string request_id, std::string through,
                            std::uint64_t expected_sequence, std::string actor, bool allow_new = true,
                            std::optional<std::uint64_t> expected_policy_sequence = {}) const;
  Result<Json> ledger_records(std::uint64_t after = 0, std::size_t limit = 100) const;
  Result<Json> work_attempts(std::string import_id, std::uint64_t after = 0,
                             std::size_t limit = 100) const;
  [[nodiscard]] std::filesystem::path path() const;

private:
  explicit SqliteStore(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
  std::shared_ptr<Impl> impl_;
  friend class Worker;
};

// One worker owns one in-process financial cache. Different workers can execute
// concurrently; a book's canonical admission remains revision/lease fenced.
class Worker {
public:
  explicit Worker(SqliteStore store, WorkerLimits limits = {});
  ~Worker();
  Worker(const Worker &) = delete;
  Worker &operator=(const Worker &) = delete;
  Result<std::optional<WorkResult>> run_once();
  Result<std::size_t> drain(std::size_t max_jobs = 100000);
  void run(std::stop_token stop, std::chrono::milliseconds idle = std::chrono::milliseconds{25});

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Bounded native receipt executor plus background workers. Futures resolve only
// after durable receipt; financial validation is separate. No detached threads.
class AsyncHost {
public:
  explicit AsyncHost(SqliteStore store, std::size_t workers = 2, std::size_t max_submissions = 16,
                     std::size_t max_queued_bytes = 64 * 1024 * 1024);
  ~AsyncHost();
  AsyncHost(const AsyncHost &) = delete;
  AsyncHost &operator=(const AsyncHost &) = delete;
  Result<std::future<Result<Json>>> submit(Json metadata, std::string key, std::string payload);
  void stop(); // finish accepted receipt submissions, then join native workers
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace luca::ingestion
