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
                     {"rules", "cash-equity/2"},
                     {"engine", "luca/0.1"},
                     {"reference_snapshot", "r"}}}}}};
  std::atomic<bool> fault = true, settlement_fault = true, version_fault = false;
  luca::ingestion::Hooks hooks;
  hooks.checkpoint = [&](std::string_view point) {
    if (point == "after_receipt_commit" && fault.exchange(false))
      throw std::runtime_error("injected uncertain outcome");
    if (point == "after_receipt_commit" && version_fault.exchange(false))
      throw std::runtime_error("injected uncertain version receipt");
    if (point == "settlement_receipt_after_commit" && settlement_fault.exchange(false))
      throw std::runtime_error("injected uncertain settlement receipt");
  };
  auto store = take(luca::ingestion::SqliteStore::open(temp, config, hooks));
  take(store.register_reference("r", {{"accounts", {"a"}}, {"instruments", {"ABC"}}}));
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
  auto edit_request = request;
  edit_request["idempotency_key"] = "atomic-edit";
  edit_request["payload_utf8"] =
      Json{{"record_id", "void"}, {"economic_event_id", "e1"}, {"account_id", "a"},
           {"action", "cancel"},  {"causal_record_id", "r1"},  {"void_at", "2026-01-02T00:00:00Z"}}
          .dump() +
      "\n" +
      Json{{"record_id", "r2"},
           {"economic_event_id", "e1"},
           {"account_id", "a"},
           {"action", "correct"},
           {"causal_record_id", "void"},
           {"event",
            {{"type", "cash_movement"},
             {"effective_at", "2026-01-01T00:00:00Z"},
             {"currency", "USD"},
             {"amount", "150"}}}}
          .dump() +
      "\n";
  take(app.call(Operation::submit_import, edit_request));
  take(app.drain("b"));
  auto restored_store = take(luca::ingestion::SqliteStore::open(temp, config));
  SqliteApplication restored_app(
      {{restored_store, {{"a", "firm", "custody"}}, "m", true, false, false, {}}});
  const auto capabilities = take(restored_app.call(Operation::capabilities, {{"book_id", "b"}}));
  Json lots_request = {{"book_id", "b"},
                       {"accepted_through", "3"},
                       {"policy_through", "1"},
                       {"effective_at", "2026-02-01T00:00:00Z"},
                       {"recorded_through", "2099-01-01T00:00:00Z"},
                       {"collection", "trades"},
                       {"mapping_digest", capabilities.at("mapping_digest")}};
  auto lots = take(restored_app.call(Operation::lots, lots_request));
  check(lots.at("items").empty());
  lots_request["accepted_through"] = "2";
  auto split = restored_app.call(Operation::lots, lots_request);
  check(!split &&
        split.error().code ==
            "INPUT_REVISION_SPLITS_COMMIT"); // financial replay reached the paired replacement
  SqliteApplication financial({{store, {{"a", "firm", "custody"}}, "m", true, true, true, {"s"}}});
  auto buy = request;
  buy["idempotency_key"] = "buy";
  buy["payload_utf8"] = Json{{"record_id", "buy"},
                             {"economic_event_id", "buy-event"},
                             {"action", "originate"},
                             {"account_id", "a"},
                             {"event",
                              {{"type", "equity_trade"},
                               {"effective_at", "2026-01-02T00:00:00Z"},
                               {"settlement_date", "2026-01-05"},
                               {"quote_currency", "USD"},
                               {"instrument_id", "ABC"},
                               {"quantity", "1"},
                               {"price", "100"},
                               {"charges", Json::array()}}}}
                            .dump() +
                        "\n";
  take(financial.call(Operation::submit_import, buy));
  take(financial.drain("b"));
  auto accepted = take(store.ledger_records());
  check(accepted.size() == 4);
  Json settlement = {{"book_id", "b"},
                     {"idempotency_key", "payment"},
                     {"request",
                      {{"account", "a"},
                       {"trade_record", accepted[3]["record_id"]},
                       {"cash", "-40"},
                       {"currency", "USD"},
                       {"effective_at", "2026-01-07T00:00:00Z"},
                       {"source", {{"source_id", "s"}, {"reference", "synthetic"}}}}}};
  auto unknown = financial.call(Operation::submit_settlement, settlement);
  check(!unknown && unknown.error().code == "OUTCOME_UNKNOWN");
  auto settlement_receipt = take(financial.call(Operation::submit_settlement, settlement));
  check(take(financial.call(Operation::submit_settlement, settlement)) == settlement_receipt);
  take(financial.drain("b"));
  auto payment = take(financial.call(Operation::settlement_status,
                                     {{"book_id", "b"}, {"idempotency_key", "payment"}}));
  check(payment["state"] == "committed");
  const auto closed_period =
      take(store.close_period("close", "2026-01-31T23:59:59Z", 4, "tester", true, 1, 1));
  Json period_query = {
      {"book_id", "b"},
      {"accepted_through", "4"},
      {"policy_through", "1"},
      {"settlement_through", "1"},
      {"recorded_through", "2200-01-01T00:00:00Z"},
      {"period_from", "2026-02-01T00:00:00Z"},
      {"period_until", "2026-03-01T00:00:00Z"},
      {"mapping_digest",
       take(financial.call(Operation::capabilities, {{"book_id", "b"}}))["mapping_digest"]},
      {"reporting_timezone", "UTC"},
      {"correction_recognition", "recorded_time"},
      {"collection", "rows"}};
  auto unavailable_period = financial.call(Operation::query_pnl, period_query);
  check(!unavailable_period && unavailable_period.error().code == "PERIOD_RETENTION_UNAVAILABLE");
  period_query["period_from"] = "2026-01-02T00:00:00Z";
  unavailable_period = financial.call(Operation::query_pnl, period_query);
  check(!unavailable_period && unavailable_period.error().code == "PERIOD_RETENTION_UNAVAILABLE");
  auto refund = settlement;
  refund["idempotency_key"] = "closed-refund";
  refund["request"]["cash"] = "40";
  refund["request"]["reverses"] = "payment";
  take(financial.call(Operation::submit_refund, refund));
  take(financial.drain("b"));
  auto rejected = take(financial.call(Operation::settlement_status,
                                      {{"book_id", "b"}, {"idempotency_key", "closed-refund"}}));
  check(rejected["state"] == "invalid" && rejected["code"] == "PERIOD_CLOSED");
  refund["idempotency_key"] = "open-refund";
  refund["request"]["effective_at"] = "2026-02-03T00:00:00Z";
  take(financial.call(Operation::submit_refund, refund));
  take(financial.drain("b"));
  check(take(financial.call(Operation::settlement_status,
                            {{"book_id", "b"}, {"idempotency_key", "open-refund"}}))["state"] ==
        "committed");
  auto envelope_context = config["contexts"]["v1"];
  envelope_context["adapter"] = "luca.transactions.envelope.jsonl/1";
  take(store.register_context("versions", envelope_context));
  Json edit = {{"book_id", "b"},
               {"idempotency_key", "closed-edit"},
               {"original_record_id", accepted[2]["record_id"]},
               {"validation_context_id", "versions"},
               {"void_at", "2026-01-15T00:00:00Z"},
               {"reason", "synthetic correction"},
               {"replacement_event_utf8", Json{{"type", "cash_movement"},
                                               {"effective_at", "2026-01-01T00:00:00Z"},
                                               {"currency", "USD"},
                                               {"amount", "175"}}
                                              .dump()}};
  auto closed_receipt = take(financial.call(Operation::edit_transaction, edit));
  take(financial.drain("b"));
  check(take(store.book_state())["last_sequence"] == 4);
  auto closed_diagnostics = take(financial.call(
      Operation::read_diagnostics, {{"book_id", "b"}, {"import_id", closed_receipt["import_id"]}}));
  bool found_closed = false;
  for (const auto &d : closed_diagnostics["items"])
    found_closed = found_closed || d["code"] == "PERIOD_CLOSED";
  check(found_closed);
  edit["idempotency_key"] = "open-edit";
  edit["void_at"] = "2026-02-05T00:00:00Z";
  version_fault = true;
  auto uncertain_edit = financial.call(Operation::edit_transaction, edit);
  check(!uncertain_edit && uncertain_edit.error().code == "OUTCOME_UNKNOWN");
  auto recovered_edit = take(financial.call(Operation::edit_transaction, edit));
  check(take(financial.call(Operation::edit_transaction, edit)) == recovered_edit);
  take(financial.drain("b"));
  check(take(store.book_state())["last_sequence"] == 6);
  check(take(store.periods())["closes"][0] == closed_period);
  auto actor_binding =
      BookBinding{store, {{"a", "firm", "custody"}}, "m", true, true, false, {"s"}};
  actor_binding.transaction_actor = "another-actor";
  SqliteApplication changed_actor({actor_binding});
  auto actor_conflict = changed_actor.call(Operation::edit_transaction, edit);
  check(!actor_conflict && actor_conflict.error().code == "IDEMPOTENCY_CONFLICT");
  check(!app.call(static_cast<Operation>(999), Json::object()));
  check(document(static_cast<Operation>(999),
                 std::unexpected(luca::ingestion::Failure{"INVALID_ARGUMENTS"}))
            .at("ok") == false);
  std::filesystem::remove_all(temp);
  std::puts("shared application receipt, outcome recovery, scope and evidence checks passed");
}
