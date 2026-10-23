#include "luca/application/sqlite.hpp"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <source_location>
#include <unistd.h>
using namespace luca::application;
namespace {
void check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) {
    std::fprintf(stderr, "application test failed at %u\n", at.line());
    std::abort();
  }
}
template <class T> T take(luca::ingestion::Result<T> r) {
  if (!r) {
    std::fprintf(stderr, "unexpected %s\n", r.error().code.c_str());
    std::abort();
  }
  return std::move(*r);
}
} // namespace
int main() {
  char temp[] = "/tmp/luca-application-XXXXXX";
  check(mkdtemp(temp));
  Json config = {{"tenant_id", "t"},
                 {"book_id", "b"},
                 {"source_ids", {"s"}},
                 {"contexts",
                  {{"v1",
                    {{"adapter", "luca.transactions.jsonl/1"},
                     {"normalization", "source-book-identity/1"},
                     {"rules", "cash-equity/1"},
                     {"engine", "luca/0.1"},
                     {"reference_snapshot", "r"}}}}}};
  std::atomic<bool> fault = true;
  luca::ingestion::Hooks hooks;
  hooks.checkpoint = [&](std::string_view point) {
    if (point == "after_receipt_commit" && fault.exchange(false))
      throw std::runtime_error("injected uncertain outcome");
  };
  auto store = take(luca::ingestion::SqliteStore::open(temp, config, hooks));
  take(store.register_reference("r", {{"accounts", {"a"}}, {"instruments", Json::array()}}));
  SqliteApplication app({{store, {}, "", true, true, true, {"s"}}});
  Json request = {{"book_id", "b"},
                  {"idempotency_key", "k"},
                  {"metadata",
                   {{"source_id", "s"},
                    {"format", "luca.transactions.jsonl/1"},
                    {"validation_context_id", "v1"}}},
                  {"payload_utf8",
                   "{\"record_id\":\"r1\",\"economic_event_id\":\"e1\",\"action\":\"originate\","
                   "\"account_id\":\"a\",\"event\":{\"type\":\"cash_movement\",\"effective_at\":"
                   "\"2026-01-01T00:00:00Z\",\"currency\":\"USD\",\"amount\":\"100\"}}\n"}};
  auto uncertain = app.call(Operation::submit_import, request);
  check(!uncertain && uncertain.error().code == "OUTCOME_UNKNOWN" &&
        exit_code(uncertain.error()) == 9);
  auto receipt = take(app.call(Operation::submit_import, request));
  check(receipt.at("durable_acceptance") == true);
  check(take(app.call(Operation::submit_import, request)) == receipt);
  auto scope = Json{{"book_id", "b"}, {"import_id", receipt.at("import_id")}};
  check(take(app.call(Operation::import_status, scope)).at("enumeration").at("state") == "pending");
  take(app.drain("b"));
  check(take(app.call(Operation::import_status, scope)).at("all_records_committed") == true);
  SqliteApplication readonly({{store, {}, "", true, false, false, {}}});
  auto denied = readonly.call(Operation::submit_import, request);
  check(!denied && denied.error().code == "PERMISSION_DENIED");
  check(!readonly.drain("b"));
  check(take(readonly.call(Operation::import_status, scope)) ==
        take(app.call(Operation::import_status, scope)));
  Json target = {{"legal_entity", "firm"}, {"custodian_account", "custody"}, {"account", "a"}};
  take(store.put_policy({{"id", "p"},
                         {"scope", "legal_entity"},
                         {"legal_entity", "firm"},
                         {"subject", "firm"},
                         {"effective_from", "2026-01-01T00:00:00Z"},
                         {"lot_selection", "LIFO"},
                         {"recognition", "cash"}},
                        0, "admin"));
  auto policy = take(app.call(Operation::resolve_policy, {{"book_id", "b"},
                                                          {"target", target},
                                                          {"effective_at", "2026-02-01T00:00:00Z"},
                                                          {"through_sequence", "1"}}));
  auto evidence = take(store.retain_policy_result({{"projection", "economic-lifo/1"},
                                                   {"accepted_through", 1},
                                                   {"mapping_version", "m"},
                                                   {"policy", policy},
                                                   {"result_digest", std::string(64, 'a')}}));
  check(take(readonly.call(Operation::policy_result,
                           {{"book_id", "b"}, {"evidence_id", evidence.at("id")}})) == evidence);
  // Destroying adapter hosts never owns/deletes durable data; a reopened host
  // sees the exact same retained validation and policy evidence.
  auto reopened = take(luca::ingestion::SqliteStore::open(temp, config));
  SqliteApplication resumed({{reopened, {}, "", true, false, false, {}}});
  check(take(resumed.call(Operation::import_status, scope)) ==
        take(readonly.call(Operation::import_status, scope)));
  check(take(resumed.call(Operation::policy_result,
                          {{"book_id", "b"}, {"evidence_id", evidence.at("id")}})) == evidence);
  check(!app.call(static_cast<Operation>(999), Json::object()));
  check(document(static_cast<Operation>(999),
                 std::unexpected(luca::ingestion::Failure{"INVALID_ARGUMENTS"}))
            .at("ok") == false);
  std::filesystem::remove_all(temp);
  std::puts("shared application receipt, outcome recovery, scope and evidence checks passed");
}
