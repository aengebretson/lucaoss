#include "luca/ingestion/policy.hpp"
#include "luca/ingestion/settlement.hpp"
#include "luca/ingestion/sqlite.hpp"
#include <future>
#include <iostream>
#include <source_location>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
using namespace luca;
using namespace luca::ingestion;
void check(bool b, std::source_location at = std::source_location::current()) {
  if (!b) {
    std::cerr << "failed " << at.line() << '\n';
    std::abort();
  }
}
template <class T> T take(Result<T> r, std::source_location at = std::source_location::current()) {
  if (!r) {
    std::cerr << r.error().code << '\n';
    check(false, at);
  }
  return *r;
}
void error(const auto &r, std::string code) {
  check(!r);
  check(r.error().code == code);
}
int main() {
  char tmp[] = "/tmp/luca-settlement-XXXXXX";
  check(mkdtemp(tmp));
  Json config = {{"tenant_id", "t"},
                 {"book_id", "b"},
                 {"source_ids", Json::array({"s"})},
                 {"contexts",
                  {{"v2",
                    {{"adapter", "luca.transactions.jsonl/1"},
                     {"normalization", "source-book-identity/1"},
                     {"rules", "cash-equity/2"},
                     {"engine", "luca/0.1"},
                     {"reference_snapshot", "ref"}}}}}};
  auto store = take(SqliteStore::open(tmp, config));
  take(store.register_reference(
      "ref", {{"accounts", Json::array({"a"})}, {"instruments", Json::array({"XYZ"})}}));
  Json tx = {{"record_id", "buy"},
             {"economic_event_id", "buy"},
             {"action", "originate"},
             {"account_id", "a"},
             {"event",
              {{"type", "equity_trade"},
               {"effective_at", "2026-01-02T12:00:00Z"},
               {"quantity", "10"},
               {"price", "10"},
               {"instrument_id", "XYZ"},
               {"quote_currency", "USD"},
               {"settlement_date", "2026-01-05"},
               {"charges", Json::array()}}}};
  std::string payload = tx.dump() + "\n";
  std::istringstream input(payload);
  auto receipt = take(store.receive({{"source_id", "s"},
                                     {"format", "luca.transactions.jsonl/1"},
                                     {"validation_context_id", "v2"}},
                                    "buy", input, payload.size()));
  Worker worker(store);
  take(worker.drain());
  check(take(store.status(receipt["import_id"]))["all_records_committed"] == true);
  auto rows = take(store.ledger_records());
  check(rows.size() == 1);
  auto record = rows[0]["record_id"];
  Json request = {{"account", "a"},
                  {"trade_record", record},
                  {"cash", "-40"},
                  {"currency", "USD"},
                  {"effective_at", "2026-01-07T12:00:00Z"},
                  {"reverses", nullptr},
                  {"source", {{"bank_reference", "synthetic-1"}}}};
  auto received = take(store.receive_settlement("part", request, "tester"));
  check(take(store.settlement_status("part"))["state"] == "pending");
  check(take(store.receive_settlement("part", request, "tester")) == received);
  auto changed = request;
  changed["cash"] = "-41";
  error(store.receive_settlement("part", changed, "tester"), "SETTLEMENT_IDEMPOTENCY_CONFLICT");
  take(worker.drain());
  auto part = take(store.settlement_status("part"));
  check(part["state"] == "committed");
  check(part["event"]["sequence"] == 1);
  request["cash"] = "-61";
  take(store.receive_settlement("over", request, "tester"));
  take(worker.drain());
  check(take(store.settlement_status("over"))["code"] == "SETTLEMENT_EXCEEDS_OBLIGATION");
  request["cash"] = "-60";
  request["effective_at"] = "2026-02-02T12:00:00Z";
  take(store.receive_settlement("full", request, "tester"));
  error(store.close_period("close", "2026-01-31T23:59:59Z", 1, "tester", true, 0, 1),
        "PERIOD_WORK_PENDING");
  take(worker.drain());
  check(take(store.settlement_events()).size() == 2);
  error(store.close_period("close", "2026-01-31T23:59:59Z", 1, "tester", true, 0, 1),
        "SETTLEMENT_REVIEW_STALE");
  auto close = take(store.close_period("close", "2026-01-31T23:59:59Z", 1, "tester", true, 0, 2));
  check(close["settlement_through"] == 2);
  request["cash"] = "40";
  request["reverses"] = "part";
  request["effective_at"] = "2026-01-08T12:00:00Z";
  take(store.receive_settlement("closed-refund", request, "tester"));
  take(worker.drain());
  check(take(store.settlement_status("closed-refund"))["code"] == "PERIOD_CLOSED");
  request["effective_at"] = "2026-02-03T12:00:00Z";
  take(store.receive_settlement("refund", request, "tester"));
  take(worker.drain());
  check(take(store.settlement_status("refund"))["state"] == "committed");
  auto reopened = take(SqliteStore::open(tmp, config));
  check(take(reopened.settlement_events()).size() == 3);
  check(take(reopened.settlement_status("over"))["request"]["request"]["cash"] == "-61");
  check(take(reopened.receive_settlement(
            "part",
            part["event"]["source"].empty() ? Json()
                                            : Json{{"account", "a"},
                                                   {"trade_record", record},
                                                   {"cash", "-40"},
                                                   {"currency", "USD"},
                                                   {"effective_at", "2026-01-07T12:00:00Z"},
                                                   {"reverses", nullptr},
                                                   {"source", {{"bank_reference", "synthetic-1"}}}},
            "tester")) == received);
  request["cash"] = "-20";
  request["reverses"] = nullptr;
  request["effective_at"] = "2026-02-04T12:00:00Z";
  take(reopened.receive_settlement("concurrent-a", request, "tester"));
  take(reopened.receive_settlement("concurrent-b", request, "tester"));
  Worker one(reopened), two(reopened);
  auto first = std::async(std::launch::async, [&] { return take(one.drain()); });
  auto second = std::async(std::launch::async, [&] { return take(two.drain()); });
  first.get();
  second.get();
  check(take(reopened.settlement_events()).size() == 5);
  check(take(reopened.settlement_status("concurrent-a"))["state"] == "committed");
  check(take(reopened.settlement_status("concurrent-b"))["state"] == "committed");
  Json definition = {{"id", "entity-policy"},
                     {"scope", "legal_entity"},
                     {"subject", "entity"},
                     {"legal_entity", "entity"},
                     {"effective_from", "2026-02-01T00:00:00Z"},
                     {"lot_selection", "LIFO"},
                     {"recognition", "cash"}};
  take(reopened.put_policy(definition, 0, "tester"));
  auto policy = take(reopened.resolve_policy(
      {{"legal_entity", "entity"}, {"custodian_account", "custody"}, {"account", "a"}},
      "2026-02-05T00:00:00Z", 1));
  Json evidence = {{"projection", "equity-journals/1"},
                   {"accepted_through", 1},
                   {"settlement_through", 5},
                   {"mapping_version", "fixture/1"},
                   {"policy", policy},
                   {"result_digest", std::string(64, 'a')}};
  auto retained = take(reopened.retain_policy_result(evidence));
  check(take(reopened.retain_policy_result(evidence)) == retained);
  evidence["settlement_through"] = 6;
  error(reopened.retain_policy_result(evidence), "SETTLEMENT_SEQUENCE_UNAVAILABLE");
  auto backwards = take(SqliteStore::open(tmp, config, Hooks{[] { return std::int64_t{1}; }, {}}));
  request["cash"] = "20";
  request["reverses"] = "concurrent-a";
  request["effective_at"] = "2026-02-05T12:00:00Z";
  take(backwards.receive_settlement("clock-refund", request, "tester"));
  Worker rollback_clock(backwards);
  take(rollback_clock.drain());
  auto refund = take(backwards.settlement_status("clock-refund"));
  check(refund["state"] == "committed");
  auto history = take(backwards.settlement_events());
  check(history[5]["recorded_at"] == history[4]["recorded_at"]);
  request["cash"] = "-10";
  request["reverses"] = nullptr;
  request["effective_at"] = "2026-02-06T12:00:00Z";
  for (const auto &phase :
       {std::string("settlement_before_commit"), std::string("settlement_after_commit")}) {
    take(reopened.receive_settlement(phase, request, "tester"));
    auto pid = fork();
    check(pid >= 0);
    if (pid == 0) {
      auto crashed = take(SqliteStore::open(tmp, config, Hooks{{}, [&](std::string_view value) {
                                                                 if (value == phase)
                                                                   _exit(77);
                                                               }}));
      Worker child(crashed);
      take(child.drain());
      _exit(1);
    }
    int status = 0;
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 77);
    check(take(reopened.settlement_status(phase))["state"] ==
          (phase == "settlement_before_commit" ? "pending" : "committed"));
    take(worker.drain());
    check(take(reopened.settlement_status(phase))["state"] == "committed");
  }
  check(take(reopened.settlement_events()).size() == 8);
  std::filesystem::remove_all(tmp);
  std::puts("actual settlement durable tests passed");
}
