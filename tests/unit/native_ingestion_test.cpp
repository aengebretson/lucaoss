#include "luca/ingestion/sqlite.hpp"
#include "luca/adapters/transaction.hpp"
#include "luca/serialization/canonical.hpp"
#include "luca/serialization/canonical_decode.hpp"
#include <atomic>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <latch>
#include <source_location>
#include <sqlite3.h>
#include <sstream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
using namespace luca::ingestion;
namespace {
void check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) {
    std::fprintf(stderr, "failed %s:%u\n", at.file_name(), at.line());
    std::abort();
  }
}
template <class T>
T take(Result<T> result, std::source_location at = std::source_location::current()) {
  if (!result) {
    std::fprintf(stderr, "%s at %s:%u\n", result.error().code.c_str(), at.file_name(), at.line());
    std::abort();
  }
  return std::move(*result);
}
void error(const auto &result, const char *code) {
  check(!result);
  check(result.error().code == code);
}
struct Temp {
  std::filesystem::path path;
  Temp() {
    char pattern[] = "/tmp/luca-native-test-XXXXXX";
    const char *result = ::mkdtemp(pattern);
    check(result);
    path = result;
  }
  ~Temp() { std::filesystem::remove_all(path); }
};
Json config(std::string book = "fund-a") {
  return {{"tenant_id", "example-tenant"},
          {"book_id", book},
          {"source_ids", Json::array({"custodian-a"})},
          {"contexts",
           {{"cash-equity-v1-ref42",
             {{"adapter", "luca.transactions.jsonl/1"},
              {"normalization", "source-book-identity/1"},
              {"rules", "cash-equity/1"},
              {"engine", "luca/0.1"},
              {"reference_snapshot", "ref42"}}}}}};
}
Json meta() {
  return {{"source_id", "custodian-a"},
          {"format", "luca.transactions.jsonl/1"},
          {"validation_context_id", "cash-equity-v1-ref42"}};
}
Json cash(std::string id = "r1", std::string economic = "e1", std::string amount = "100") {
  return {{"record_id", id},
          {"economic_event_id", economic},
          {"action", "originate"},
          {"account_id", "portfolio-a"},
          {"event",
           {{"type", "cash_movement"},
            {"effective_at", "2026-10-20T00:00:00Z"},
            {"currency", "USD"},
            {"amount", amount}}}};
}
std::string encode(const Json &rows) {
  std::string body;
  for (const auto &row : rows)
    body += row.dump() + "\n";
  return body;
}
std::string receive(SqliteStore &store, std::string body, std::string key = "key",
                    Json metadata = meta()) {
  std::istringstream input(body);
  return take(store.receive(metadata, key, input, body.size()))["import_id"];
}
std::string receive(SqliteStore &store, Json rows, std::string key = "key",
                    Json metadata = meta()) {
  return receive(store, encode(rows), key, metadata);
}
SqliteStore opened(Temp &root, std::string book = "fund-a", Hooks hooks = {}) {
  auto s = take(SqliteStore::open(root.path, config(book), hooks));
  take(s.register_reference("ref42", {{"accounts", Json::array({"portfolio-a", "other"})},
                                      {"instruments", Json::array({"ABC"})}}));
  return s;
}
void drain(SqliteStore &store) {
  Worker worker(store);
  take(worker.drain());
}
Json items(SqliteStore &s, std::string collection, std::string id, Json filters = Json::object()) {
  return take(s.page(collection, id, filters))["items"];
}
void sql(SqliteStore &store, std::string statement) {
  sqlite3 *db = nullptr;
  check(sqlite3_open((store.path() / "intake.sqlite3").c_str(), &db) == SQLITE_OK);
  sqlite3_busy_timeout(db, 10000);
  if (sqlite3_exec(db, statement.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) {
    std::fprintf(stderr, "SQL failed: %s\n", sqlite3_errmsg(db));
    check(false);
  }
  sqlite3_close(db);
}
void receipt_errors_and_replay() {
  Temp root;
  auto store = opened(root);
  const auto body = encode(Json::array({cash(), cash("bad", "ebad", "0.0000001")})) + "not json\n";
  auto id = receive(store, body);
  auto receipt = take(store.status(id))["receipt"];
  check(take(store.status(id))["enumeration"]["state"] == "pending");
  check(take(store.book_state())["last_sequence"] == 0);
  check(receive(store, body) == id);
  std::istringstream different("different");
  error(store.receive(meta(), "key", different, 9), "IDEMPOTENCY_CONFLICT");
  Worker worker(store);
  take(worker.drain());
  auto status = take(store.status(id));
  check(status["validation_counts"]["invalid"] == 2);
  check(status["commitment_record_counts"]["committed"] == 1);
  check(!status["all_records_committed"].get<bool>());
  check(items(store, "diagnostics", id).size() >= 2);
  auto reopened = take(SqliteStore::open(root.path, config()));
  check(take(reopened.status(id))["receipt"] == receipt);
  std::ostringstream raw;
  take(reopened.payload(id, raw));
  check(raw.str() == body);
  check(take(store.capabilities())["usage"]["slots"] == 0);
  auto second = receive(reopened, Json::array({cash("second", "second-e")}), "second");
  drain(reopened);
  check(take(reopened.status(second))["all_records_committed"]);
  check(take(reopened.book_state())["last_sequence"] == 2);
  const auto original = take(store.ledger_records());
  check(original.size() == 2);
  check(original[0]["digest"].get<std::string>().size() == 64);
}
void atomicity_and_causal_order() {
  Temp root;
  auto store = opened(root);
  auto good = cash("a", "ea"), bad = cash("b", "eb", "bad");
  good["atomic_group"] = "group";
  bad["atomic_group"] = "group";
  auto id = receive(store, Json::array({good, bad}));
  drain(store);
  check(items(store, "commits", id).empty());
  check(items(store, "records", id)[0]["validation"]["state"] == "valid");
  auto parent = cash("parent", "econ"), child = cash("child", "econ", "80");
  child["action"] = "correct";
  child["causal_record_id"] = "parent";
  auto causal = receive(store, Json::array({child, parent}), "causal");
  drain(store);
  check(take(store.status(causal))["all_records_committed"]);
  auto records = items(store, "records", causal), log = take(store.ledger_records());
  check(log[0]["record_id"] == records[1]["canonical_record_id"]);
  check(items(store, "commits", causal).size() == 1);
  auto whole_meta = meta();
  whole_meta["atomicity"] = "whole_import";
  auto whole = receive(store, Json::array({cash("w1", "we1"), cash("w2", "we2", "nan")}), "whole",
                       whole_meta);
  drain(store);
  check(items(store, "commits", whole).empty());
  auto unknown = receive(store, encode(Json::array({good})) + "broken\n", "unknown");
  drain(store);
  check(take(store.status(unknown))["enumeration"]["state"] == "failed");
}
void dependencies_history_and_revalidation() {
  Temp root;
  auto store = opened(root);
  auto child = cash("child", "econ", "80");
  child["action"] = "correct";
  child["causal_record_id"] = "parent";
  auto id = receive(store, Json::array({child}));
  drain(store);
  auto old = items(store, "records", id)[0];
  check(old["validation"]["state"] == "blocked");
  check(take(store.capabilities())["usage"]["slots"] == 0);
  receive(store, Json::array({cash("parent", "econ")}), "parent");
  drain(store);
  check(take(store.status(id))["all_records_committed"]);
  check(take(store.record(id, old["record_id"],
                          old["validation"]["run_id"]))["historical_validation"]["state"] ==
        "blocked");
  auto row = cash("new-account", "new-economic");
  row["account_id"] = "new-account";
  auto reference = receive(store, Json::array({row}), "ref");
  drain(store);
  auto prior = items(store, "records", reference)[0];
  auto context = config()["contexts"]["cash-equity-v1-ref42"];
  context["reference_snapshot"] = "ref43";
  take(store.register_context("context43", context));
  take(store.register_reference(
      "ref43", {{"accounts", Json::array({"new-account"})}, {"instruments", Json::array()}}));
  auto group = take(store.group(reference, prior["group_id"]));
  Json request = {
      {"validation_context_id", "context43"},
      {"groups",
       Json::array({{{"group_id", group["id"]},
                     {"expected_revision", std::to_string(group["revision"].get<int>())}}})}};
  auto retry = take(store.revalidate(reference, "retry", request));
  check(take(store.revalidate(reference, "retry", request)) == retry);
  drain(store);
  check(take(store.status(reference))["all_records_committed"]);
  error(store.revalidate(reference, "new-key", request), "STALE_SELECTION");
  error(store.register_reference(
            "ref43", {{"accounts", Json::array({"different"})}, {"instruments", Json::array()}}),
        "REFERENCE_CONFLICT");
  check(items(store, "diagnostics", reference).size() >= 1);
}
void cycles_duplicates_and_reversals() {
  Temp root;
  auto store = opened(root);
  auto a = cash("a", "ea"), b = cash("b", "eb");
  a["action"] = b["action"] = "reverse";
  a["causal_record_id"] = "b";
  b["causal_record_id"] = "a";
  auto cycle = receive(store, Json::array({a, b}));
  drain(store);
  check(items(store, "commits", cycle).empty());
  bool has_cycle = false;
  for (const auto &e : items(store, "diagnostics", cycle))
    has_cycle = has_cycle || e["code"] == "CAUSAL_CYCLE";
  check(has_cycle);
  auto duplicate = receive(store, Json::array({cash("d1", "de"), cash("d2", "de")}), "duplicate");
  drain(store);
  check(items(store, "commits", duplicate).empty());
  receive(store, Json::array({cash("original", "original-e")}), "original");
  drain(store);
  auto reversal = cash("reverse", "reversal-e", "-99");
  reversal["action"] = "reverse";
  reversal["causal_record_id"] = "original";
  auto invalid = receive(store, Json::array({reversal}), "reverse");
  drain(store);
  check(items(store, "commits", invalid).empty());
  reversal["record_id"] = "exact-reverse";
  reversal["event"]["amount"] = "-100";
  auto exact = receive(store, Json::array({reversal}), "exact");
  drain(store);
  check(take(store.status(exact))["all_records_committed"]);
}
void competing_admissions() {
  Temp root;
  auto store = opened(root);
  receive(store, Json::array({cash("parent", "econ")}));
  drain(store);
  std::vector<std::string> imports;
  for (const auto *name : {"child1", "child2"}) {
    auto row = cash(name, "econ", "80");
    row["action"] = "correct";
    row["causal_record_id"] = "parent";
    imports.push_back(receive(store, Json::array({row}), name));
  }
  Worker staging(store);
  for (int i = 0; i < 2; ++i) {
    check(take(staging.run_once())->kind == "parse_import");
    sql(store, "UPDATE work SET ready_ns=4611686018427387904 WHERE kind='validate_group'");
  }
  sql(store, "UPDATE work SET ready_ns=0 WHERE kind='validate_group'");
  for (int i = 0; i < 2; ++i) {
    check(take(staging.run_once())->kind == "validate_group");
    sql(store, "UPDATE work SET ready_ns=4611686018427387904 WHERE kind='admit_group'");
  }
  auto before = items(store, "validation_runs", imports[1])[0];
  auto old_errors = items(store, "diagnostics", imports[1], {{"run_id", before["run_id"]}});
  sql(store, "UPDATE work SET ready_ns=0 WHERE kind='admit_group'");
  std::jthread first([&] { drain(store); }), second([&] { drain(store); });
  first.join();
  second.join();
  check(take(store.book_state())["last_sequence"] == 2);
  check(items(store, "commits", imports[0]).size() + items(store, "commits", imports[1]).size() ==
        1);
  check(items(store, "diagnostics", imports[1], {{"run_id", before["run_id"]}}) == old_errors);
  bool conflict = false;
  for (auto &id : imports)
    for (auto &row : items(store, "records", id))
      conflict = conflict || row["commitment"]["state"] == "conflict";
  check(conflict);
}
void snapshot_and_fencing() {
  Temp root;
  std::atomic<std::int64_t> clock{1792540800000000000LL};
  std::string inflight;
  bool change = false;
  Hooks hooks;
  hooks.clock = [&] { return clock.load(); };
  hooks.checkpoint = [&](std::string_view phase) {
    if (change && phase == "after_admission_prepare") {
      change = false;
      auto active = take(SqliteStore::open(root.path, config(), hooks));
      auto row = items(active, "records", inflight)[0];
      auto group = take(active.group(inflight, row["group_id"]));
      take(active.revalidate(
          inflight, "inflight",
          {{"validation_context_id", "cash-equity-v1-ref42"},
           {"groups", Json::array({{{"group_id", group["id"]},
                                    {"expected_revision",
                                     std::to_string(group["revision"].get<int>())}}})}}));
    }
  };
  auto store = opened(root, "fund-a", hooks);
  auto id = receive(store, Json::array({cash("a", "ea"), cash("b", "eb")}));
  Worker worker(store);
  take(worker.run_once());
  auto page = take(store.page("records", id, Json::object(), "", 1));
  take(worker.drain());
  auto older = take(store.page("records", id, Json::object(), page["next_cursor"], 1));
  check(older["items"][0]["validation"]["state"] == "pending");
  error(store.page("records", id, {{"validation_state", "valid"}}, page["next_cursor"], 1),
        "INVALID_CURSOR");
  inflight = receive(store, Json::array({cash("inflight", "inflight-e")}), "inflight");
  change = true;
  take(worker.drain());
  check(take(store.status(inflight))["all_records_committed"]);
  check(items(store, "commits", inflight).size() == 1);
  // Advance the clock after claim but before parse, expire the original lease,
  // and let another worker reclaim. The old generation cannot publish records.
  Temp other;
  bool expired = false;
  std::optional<SqliteStore> competing;
  Hooks lease_hooks;
  lease_hooks.clock = hooks.clock;
  lease_hooks.checkpoint = [&](std::string_view phase) {
    if (phase == "after_claim" && !expired) {
      expired = true;
      clock += 2LL * 1000000000;
      Worker next(*competing);
      take(next.drain());
    }
  };
  auto leased = opened(other, "fund-a", lease_hooks);
  competing = leased;
  auto pending = receive(leased, Json::array({cash()}));
  WorkerLimits limits;
  limits.lease_seconds = 1;
  Worker stale(leased, limits);
  auto result = take(stale.run_once());
  check(result->code == "LEASE_LOST");
  check(take(leased.status(pending))["all_records_committed"]);
  check(take(leased.book_state())["last_sequence"] == 1);
}
void resumable_and_fingerprints() {
  Temp root;
  auto store = opened(root);
  const std::string bytes = "abcdefgh";
  auto sha = [](std::string_view value) {
    return luca::serialization::detail::sha256_hex(std::as_bytes(std::span{value}));
  };
  auto id = take(store.create_upload("upload", 8, sha(bytes), 4))["upload_id"].get<std::string>();
  take(store.put_chunk(id, 1, "efgh", sha("efgh")));
  check(take(store.upload_status(id))["state"] == "uploading");
  take(store.put_chunk(id, 0, "abcd", sha("abcd")));
  take(store.put_chunk(id, 0, "abcd", sha("abcd")));
  take(store.verify_upload(id));
  auto receipt = take(store.receive_upload(meta(), "upload-receipt", id));
  check(take(store.receive_upload(meta(), "upload-receipt", id)) == receipt);
  std::ostringstream output;
  take(store.payload(receipt["import_id"], output));
  check(output.str() == bytes);
  error(store.put_chunk(id, 0, "zzzz", sha("zzzz")), "IDEMPOTENCY_CONFLICT");
  // Published byte/framing vectors are checked through the persisted binding.
  std::ifstream input(LUCA_NATIVE_VECTORS);
  auto vectors = Json::parse(input);
  int index = 0;
  for (const auto &vector : vectors["vectors"]) {
    auto raw = vector["payload_hex"].get<std::string>();
    std::string body;
    for (std::size_t i = 0; i < raw.size(); i += 2)
      body += static_cast<char>(std::stoul(raw.substr(i, 2), nullptr, 16));
    auto key = "vector-" + std::to_string(index++);
    receive(store, body, key, vector["metadata"]);
    sqlite3 *db;
    check(sqlite3_open((store.path() / "intake.sqlite3").c_str(), &db) == SQLITE_OK);
    sqlite3_stmt *statement = nullptr;
    check(sqlite3_prepare_v2(db, "SELECT fingerprint FROM imports WHERE request_key=?", -1,
                             &statement, nullptr) == SQLITE_OK);
    sqlite3_bind_text(statement, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(statement) == SQLITE_ROW);
    check(std::string(reinterpret_cast<const char *>(sqlite3_column_text(statement, 0))) ==
          vector["expected_submission_sha256"].get<std::string>());
    sqlite3_finalize(statement);
    sqlite3_close(db);
  }
}
void async_executor() {
  Temp root;
  std::latch entered{1}, release{1};
  std::atomic<bool> once{true};
  Hooks hooks;
  hooks.checkpoint = [&](std::string_view phase) {
    if (phase == "after_reservation" && once.exchange(false)) {
      entered.count_down();
      release.wait();
    }
  };
  auto store = opened(root, "fund-a", hooks);
  AsyncHost host(store, 2, 1, 65536);
  auto future = take(host.submit(meta(), "async", encode(Json::array({cash()}))));
  entered.wait();
  error(host.submit(meta(), "full", encode(Json::array({cash("next", "ne")}))),
        "CAPACITY_UNAVAILABLE");
  release.count_down();
  auto receipt = take(future.get());
  const auto id = receipt["import_id"].get<std::string>();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
  while (!take(store.status(id))["all_records_committed"]) {
    check(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  host.stop();
  error(host.submit(meta(), "stopped", ""), "EXECUTOR_STOPPED");
}
void crash_recovery() {
  for (const auto *phase :
       {"after_reservation", "after_payload_flush", "after_payload_install",
        "before_receipt_commit", "after_receipt_commit", "after_parse_batch",
        "before_enumeration_commit", "before_validation_publish", "before_validation_commit",
        "after_validation_commit", "after_admission_prepare", "before_admission_commit",
        "after_admission_commit", "after_admission_publish"}) {
    Temp root;
    auto config_data = config();
    auto phase_text = std::string(phase);
    const auto pid = ::fork();
    check(pid >= 0);
    if (pid == 0) {
      Hooks hooks;
      hooks.checkpoint = [&](std::string_view at) {
        if (at == phase_text)
          ::kill(::getpid(), SIGKILL);
      };
      auto store = take(SqliteStore::open(root.path, config_data, hooks));
      take(store.register_reference(
          "ref42", {{"accounts", Json::array({"portfolio-a"})}, {"instruments", Json::array()}}));
      receive(store, Json::array({cash()}));
      WorkerLimits limits;
      limits.lease_seconds = 1;
      Worker worker(store, limits);
      take(worker.drain());
      ::_exit(9);
    }
    int status;
    check(::waitpid(pid, &status, 0) == pid);
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    Hooks hooks;
    hooks.clock = [] {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count() +
             3LL * 1000000000;
    };
    auto store = take(SqliteStore::open(root.path, config_data, hooks));
    take(store.recover());
    auto id = receive(store, Json::array({cash()}));
    drain(store);
    check(take(store.status(id))["all_records_committed"]);
    check(take(store.book_state())["last_sequence"] == 1);
    check(items(store, "commits", id).size() == 1);
  }
}
void missing_snapshot_empty_clock_and_capacity() {
  {
    Temp missing;
    auto store = take(SqliteStore::open(missing.path, config()));
    auto metadata = meta();
    metadata["atomicity"] = "whole_import";
    auto id = receive(
        store, Json::array({cash("valid-shape", "shape-e"), cash("invalid-field", "bad-e", "bad")}),
        "missing", metadata);
    drain(store);
    auto records = items(store, "records", id);
    check(records[0]["validation"]["state"] == "blocked");
    check(records[1]["validation"]["state"] == "invalid");
    check(items(store, "commits", id).empty());
  }

  Temp root;
  auto cfg = config();
  cfg["limits"] = {{"max_pending_imports", 1}};
  auto store = take(SqliteStore::open(root.path, cfg));
  auto child = cash("child", "economic", "80");
  child["action"] = "correct";
  child["causal_record_id"] = "parent";
  auto id = receive(store, Json::array({child}));
  drain(store);
  check(take(store.status(id))["validation_counts"]["blocked"] == 1);
  take(store.register_reference(
      "ref42", {{"accounts", Json::array({"portfolio-a"})}, {"instruments", Json::array()}}));
  drain(store);
  check(take(store.status(id))["validation_counts"]["blocked"] == 1);
  auto parent = receive(store, Json::array({cash("parent", "economic")}), "parent");
  drain(store);
  check(take(store.status(parent))["all_records_committed"]);
  check(take(store.status(id))["all_records_committed"]);
  auto empty = receive(store, std::string(), "empty");
  drain(store);
  check(take(store.status(empty))["all_records_committed"]);
  check(take(store.book_state())["last_sequence"] == 2);
  auto prior = take(store.book_state())["recorded_ns"].get<std::int64_t>();
  Hooks clock;
  clock.clock = [&] { return prior - 1000000000; };
  auto regressed = take(SqliteStore::open(root.path, cfg, clock));
  auto later = receive(regressed, Json::array({cash("later", "later-e")}), "later");
  drain(regressed);
  check(take(regressed.status(later))["all_records_committed"]);
  check(take(regressed.book_state())["recorded_ns"] == prior);
}
void operational_failures_and_group_bounds() {
  Temp root;
  std::atomic<std::int64_t> now{1792540800000000000LL};
  Hooks hooks;
  hooks.clock = [&] { return now.load(); };
  hooks.checkpoint = [](std::string_view phase) {
    if (phase == "before_validation_publish")
      throw Failure{"TEMPORARY_FAILURE", true};
  };
  auto store = opened(root, "fund-a", hooks);
  auto id = receive(store, Json::array({cash()}));
  WorkerLimits limits;
  limits.max_attempts = 2;
  Worker worker(store, limits);
  take(worker.run_once());
  auto first = take(worker.run_once());
  check(first->outcome == "deferred");
  now += 10LL * 1000000000;
  take(worker.run_once());
  check(take(store.status(id))["work_health"] == "attention_required");
  check(take(store.status(id))["validation_counts"]["invalid"] == 0);
  check(items(store, "diagnostics", id).empty());
  check(take(store.work_attempts(id))["items"].size() == 2);
  Temp grouped;
  auto bounded = opened(grouped);
  auto metadata = meta();
  metadata["atomicity"] = "whole_import";
  auto batch = receive(bounded, Json::array({cash("a", "ea"), cash("b", "eb")}), "group", metadata);
  limits = WorkerLimits{};
  limits.max_group_records = 1;
  Worker limited(bounded, limits);
  take(limited.drain());
  check(items(bounded, "commits", batch).empty());
  check(items(bounded, "diagnostics", batch, {{"code", "GROUP_LIMIT_EXCEEDED"}}).size() == 1);
}
void malformed_limits_and_corruption() {
  {
    Temp long_lines;
    auto store = opened(long_lines);
    const auto first = cash("long-line", "long-e").dump() + std::string(10000, ' ') + "\n";
    const auto body = first + std::string(70000, ' ') + "\n";
    auto id = receive(store, body);
    drain(store);
    auto status = take(store.status(id));
    check(status["validation_counts"]["invalid"] == 1);
    check(status["commitment_record_counts"]["committed"] == 1);
    const auto digest = luca::serialization::detail::sha256_hex(std::as_bytes(std::span{body}));
    check(status["receipt"]["payload"]["digest"].get<std::string>() == digest);
    auto records = items(store, "records", id);
    check(records[0]["locator"]["sha256"].get<std::string>() ==
          luca::serialization::detail::sha256_hex(std::as_bytes(std::span{first})));
    check(items(store, "diagnostics", id, {{"code", "RECORD_LIMIT_EXCEEDED"}}).size() >= 1);
  }

  {
    Temp metadata_root;
    auto metadata_store = opened(metadata_root);
    auto historical = cash("historical", "historical-e");
    historical["source_event_at"] = "1600-01-01T00:00:00.123456789Z";
    auto invalid = cash("bad-source-time", "bad-source-e");
    invalid["source_event_at"] = "2026-02-30T00:00:00Z";
    auto id = receive(metadata_store, Json::array({historical, invalid}));
    drain(metadata_store);
    check(take(metadata_store.status(id))["validation_counts"]["invalid"] == 1);
    check(take(metadata_store.status(id))["commitment_record_counts"]["committed"] == 1);
  }

  Temp root;
  auto store = opened(root);
  auto number = cash("number", "number-e");
  number["event"]["amount"] = 1.5;
  auto unicode = cash("unicode", "unicode-e");
  unicode["account_id"] = "e\xcc\x81";
  auto id = receive(store, encode(Json::array({number, unicode})) +
                               "{\"record_id\":\"a\",\"record_id\":\"b\"}\n");
  drain(store);
  check(take(store.status(id))["validation_counts"]["invalid"] == 3);
  check(take(store.book_state())["last_sequence"] == 0);
  Temp bounded;
  auto limited = opened(bounded);
  auto input = receive(limited, Json::array({cash("a", "ea"), cash("b", "eb")}));
  WorkerLimits limits;
  limits.max_records = 1;
  Worker worker(limited, limits);
  take(worker.drain());
  check(take(limited.status(input))["enumeration"]["state"] == "failed");
  check(items(limited, "commits", input).empty());
  receive(store, Json::array({cash("original", "original-e")}), "original");
  drain(store);
  sql(store, "UPDATE ledger_records SET canonical=x'00' WHERE sequence=1");
  auto broken = receive(store, Json::array({cash("after-corrupt", "after-e")}), "after");
  drain(store);
  check(take(store.status(broken))["work_health"] == "attention_required");
  check(!take(store.work_attempts(broken))["items"].empty());
}
void source_envelopes() {
  check(luca::adapters::parse_transaction_date("2026-10-22").has_value());
  check(!luca::adapters::parse_transaction_date("2026-02-30"));
  check(luca::adapters::parse_transaction_timestamp("2026-10-22T15:00:00.123456789Z").has_value());
  check(!luca::adapters::parse_transaction_timestamp("2026-10-22T25:00:00Z"));
  Temp root;
  auto cfg = config();
  auto context = cfg["contexts"]["cash-equity-v1-ref42"];
  context["adapter"] = "luca.transactions.envelope.jsonl/1";
  cfg["contexts"]["envelope-ref42"] = context;
  auto store = take(SqliteStore::open(root.path, cfg));
  take(store.register_reference("ref42", {{"accounts", Json::array({"portfolio-a"})},
                                         {"instruments", Json::array({"ABC"})}}));
  auto metadata = meta();
  metadata["format"] = "luca.transactions.envelope.jsonl/1";
  metadata["validation_context_id"] = "envelope-ref42";
  Json origin = {{"transaction", cash("original", "econ")},
                 {"source", {{"wire", "8=FIX.4.2\00135=8\00117=E1\001"},
                             {"account_id", "cannot-override-the-transaction"}}}};
  auto correction = origin;
  correction["transaction"] = cash("correction", "econ", "125");
  correction["transaction"]["action"] = "correct";
  correction["transaction"]["causal_record_id"] = "original";
  auto invalid = origin;
  invalid["transaction"] = cash("bad", "bad-econ", "nan");
  const auto bytes = encode(Json::array({origin, correction, invalid}));
  const auto id = receive(store, bytes, "envelopes", metadata);
  drain(store);
  auto status = take(store.status(id));
  check(status["commitment_record_counts"]["committed"] == 2);
  check(status["validation_counts"]["invalid"] == 1);
  std::ostringstream retained;
  take(store.payload(id, retained));
  check(retained.str() == bytes);
  check(receive(store, bytes, "envelopes", metadata) == id);
  auto malformed = receive(store, Json::array({{{"transaction", cash("other", "other-e")},
                                               {"source", Json::object()}}}), "empty-source", metadata);
  drain(store);
  check(take(store.status(malformed))["enumeration"]["state"] == "failed");
  check(take(store.book_state())["last_sequence"] == 2);
}
void charged_transactions() {
  Temp root;
  auto cfg=config();
  cfg["contexts"]["cash-equity-v1-ref42"]["rules"]="cash-equity/2";
  auto store=take(SqliteStore::open(root.path,cfg));
  take(store.register_reference("ref42",{{"accounts",Json::array({"portfolio-a"})},{"instruments",Json::array({"ABC"})}}));
  auto row=cash("charged","charged");
  row["event"]={{"type","equity_trade"},{"effective_at","2026-10-20T00:00:00Z"},{"instrument_id","ABC"},
    {"quantity","-2"},{"price","100"},{"quote_currency","USD"},{"settlement_date","2026-10-21"},
    {"charges",Json::array({{{"id","commission"},{"category","commission"},{"payee","broker"},{"state","reported"},
      {"schedule_version",""},{"currency","USD"},{"amount","1.25"}}})}};
  auto bad=row;bad["record_id"]="bad";bad["economic_event_id"]="bad";bad["event"]["charges"][0]["currency"]="EUR";
  auto imported=receive(store,Json::array({row,bad}));
  check(take(store.book_state())["last_sequence"]==0);
  // Restart before validation; original envelope and pending intent are durable.
  store=take(SqliteStore::open(root.path,cfg));drain(store);
  check(take(store.status(imported))["validation_counts"]["invalid"]==1);
  check(take(store.book_state())["last_sequence"]==1);
  auto records=take(store.ledger_records());auto hex=records[0]["canonical"].get<std::string>();
  std::vector<std::byte> bytes;
  for(std::size_t i=0;i<hex.size();i+=2)bytes.push_back(static_cast<std::byte>(std::stoul(hex.substr(i,2),nullptr,16)));
  auto decoded=luca::serialization::decode_lifecycle_record(bytes);check(bool(decoded));
  luca::LifecycleLedger history;check(bool(history.accept(decoded->draft)));
  const auto& trade=std::get<luca::EquityTrade>(*history.records()[0].event());
  check(trade.charges() && trade.charges()->front().amount.scaled_value()==1250000);
  check(luca::trade_cash(trade)->scaled_value()==198750000);
  auto replay=receive(store,Json::array({row,bad}));check(replay==imported);drain(store);
  check(take(store.book_state())["last_sequence"]==1);
  Temp old;auto legacy=opened(old);auto rejected=receive(legacy,Json::array({row}));drain(legacy);
  check(take(legacy.status(rejected))["validation_counts"]["invalid"]==1);
}
} // namespace
void period_controls() {
  Temp root;
  auto store=opened(root);
  auto first=receive(store,Json::array({cash()}));
  error(store.close_period("close-a","2026-10-20T23:59:59.999999999Z",0,"operator"),"PERIOD_WORK_PENDING");
  drain(store);
  error(store.close_period("close-a","2026-10-20T23:59:59.999999999Z",0,"operator"),"PERIOD_REVIEW_STALE");
  auto receipt=take(store.close_period("close-a","2026-10-20T23:59:59.999999999Z",1,"operator"));
  check(take(store.close_period("close-a","2026-10-20T23:59:59.999999999Z",1,"operator"))==receipt);
  error(store.close_period("close-a","2026-10-21T23:59:59.999999999Z",1,"operator"),"PERIOD_IDEMPOTENCY_CONFLICT");
  auto late=receive(store,Json::array({cash("late","late")}),"late");
  check(take(store.close_period("close-a","2026-10-20T23:59:59.999999999Z",1,"operator",false))==receipt);
  error(store.close_period("close-b","2026-10-21T00:00:00Z",1,"operator",false),"PERIOD_WORK_PENDING");
  drain(store);
  check(take(store.book_state())["last_sequence"]==1);
  auto diagnostics=items(store,"diagnostics",late);bool closed=false;
  for(auto d:diagnostics) if(d.value("code","")=="PERIOD_CLOSED") closed=true;
  check(closed);
  Json cancel={{"record_id","delete"},{"economic_event_id","e1"},{"action","cancel"},
               {"account_id","portfolio-a"},{"causal_record_id","r1"}};
  receive(store,Json::array({cancel}),"delete");drain(store);check(take(store.book_state())["last_sequence"]==1);
  auto malformed=cancel;malformed["record_id"]="bad-void";malformed["void_at"]=42;
  auto invalid=receive(store,Json::array({malformed}),"bad-void");drain(store);
  check(take(store.status(invalid))["validation_counts"]["invalid"]==1);
  auto replacement=cash("closed-correction","e1");replacement["action"]="correct";
  replacement["causal_record_id"]="r1";replacement["event"]["effective_at"]="2026-10-21T00:00:00Z";
  auto corrected=receive(store,Json::array({replacement}),"closed-correction");drain(store);
  check(take(store.status(corrected))["commitment_record_counts"]["conflict"]==1);
  cancel["record_id"]="void";cancel["void_at"]="2026-10-21T00:00:00Z";
  receive(store,Json::array({cancel}),"void");drain(store);check(take(store.book_state())["last_sequence"]==2);
  auto reopened=take(SqliteStore::open(root.path,config()));
  check(take(reopened.periods())["closes"][0]==receipt);
  auto records=take(reopened.ledger_records());
  luca::LifecycleLedger history;
  for(auto record:records){
    std::vector<std::byte> bytes;auto hex=record["canonical"].get<std::string>();
    for(std::size_t j=0;j<hex.size();j+=2)bytes.push_back(static_cast<std::byte>(std::stoul(hex.substr(j,2),nullptr,16)));
    auto decoded=luca::serialization::decode_lifecycle_record(bytes);check(bool(decoded));check(bool(history.accept(decoded->draft)));
  }
  check(history.resolve(luca::Timestamp::max(),*luca::adapters::parse_transaction_timestamp("2026-10-20T23:59:59Z")).active_events().size()==1);
  check(history.resolve(luca::Timestamp::max(),*luca::adapters::parse_transaction_timestamp("2026-10-21T00:00:00Z")).active_events().empty());
}
void immutable_replacement() {
  Temp root;auto store=opened(root);receive(store,Json::array({cash()}),"origin");drain(store);
  Json removal={{"record_id","void-r1"},{"economic_event_id","e1"},{"account_id","portfolio-a"},
    {"action","cancel"},{"causal_record_id","r1"},{"void_at","2026-10-21T12:00:00Z"},{"atomic_group","edit"}};
  auto replacement=cash("r2","e1","150");replacement["action"]="correct";replacement["causal_record_id"]="void-r1";replacement["atomic_group"]="edit";
  auto invalid=replacement;invalid["event"]["amount"]="bad";
  auto failed=receive(store,Json::array({removal,invalid}),"invalid-edit");drain(store);
  check(take(store.book_state())["last_sequence"]==1);
  check(!items(store,"diagnostics",failed).empty());
  const auto bytes=encode(Json::array({removal,replacement}));auto saved=receive(store,bytes,"edit");drain(store);
  check(take(store.status(saved))["all_records_committed"]==true);
  check(take(store.book_state())["last_sequence"]==3);
  // Lost-response retry is the same durable import, including after reopening.
  auto reopened=take(SqliteStore::open(root.path,config()));check(receive(reopened,bytes,"edit")==saved);drain(reopened);
  check(take(reopened.book_state())["last_sequence"]==3);
  Json deletion={{"record_id","delete-r2"},{"economic_event_id","e1"},{"account_id","portfolio-a"},
    {"action","cancel"},{"causal_record_id","r2"},{"void_at","2026-10-22T15:00:00Z"}};
  receive(reopened,Json::array({deletion}),"delete");drain(reopened);
  auto resurrect=cash("r3","e1","200");resurrect["action"]="correct";resurrect["causal_record_id"]="delete-r2";
  auto forbidden=receive(reopened,Json::array({resurrect}),"resurrect");drain(reopened);
  check(!take(reopened.status(forbidden))["all_records_committed"].get<bool>());
  check(take(reopened.book_state())["last_sequence"]==4);
}
int main() {
  immutable_replacement();
  period_controls();
  charged_transactions();
  source_envelopes();
  receipt_errors_and_replay();
  atomicity_and_causal_order();
  dependencies_history_and_revalidation();
  cycles_duplicates_and_reversals();
  competing_admissions();
  snapshot_and_fencing();
  resumable_and_fingerprints();
  async_executor();
  crash_recovery();
  malformed_limits_and_corruption();
  missing_snapshot_empty_clock_and_capacity();
  operational_failures_and_group_bounds();
  std::puts("14 native ingestion scenarios passed, including 14 SIGKILL boundaries");
}
