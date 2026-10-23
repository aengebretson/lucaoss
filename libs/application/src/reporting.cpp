#include "internal.hpp"
#include "luca/ingestion/policy.hpp"
#include "luca/ingestion/settlement.hpp"
#include "luca/reconciliation/trade_accounting.hpp"
#include "luca/serialization/canonical_decode.hpp"
#include <charconv>

namespace luca::application::detail {
namespace {
std::string digest(const Json &j) {
  const auto text = j.dump();
  return serialization::detail::sha256_hex(std::as_bytes(std::span{text}));
}
Timestamp stamp(const Json &j) {
  auto parsed = adapters::parse_transaction_timestamp(j.get<std::string>());
  require(bool(parsed), "INVALID_ARGUMENTS");
  return *parsed;
}
std::string decimal(std::int64_t n, unsigned scale) {
  auto magnitude =
      n < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(n) : static_cast<std::uint64_t>(n);
  auto s = std::to_string(magnitude);
  if (s.size() <= scale)
    s.insert(0, scale + 1 - s.size(), '0');
  s.insert(s.size() - scale, 1, '.');
  return n < 0 ? "-" + s : s;
}
Json money(Money m) {
  return {{"amount", decimal(m.scaled_value(), Money::scale)}, {"currency", m.currency().code()}};
}
template <class T> Json identifiers(T values) {
  auto out = Json::array();
  for (const auto &v : values)
    out.push_back(v.value());
  return out;
}
Json lineage(const JournalLineage &l) {
  return {{"record_ids", identifiers(l.record_ids())},
          {"economic_event_ids", identifiers(l.economic_event_ids())},
          {"source_record_ids", identifiers(l.source_record_ids())},
          {"reverses_record_id",
           l.reverses_record_id() ? Json(l.reverses_record_id()->value()) : Json()}};
}
struct Snapshot {
  LifecycleLedger ledger;
  Json rows = Json::array(), input;
  Timestamp recorded, economic;
};
Snapshot snapshot(const BookBinding &b, const Json &a) {
  require(a.at("mapping_digest") == mapping_digest(b), "MAPPING_CONFLICT");
  const auto through = sequence(a.at("accepted_through"));
  require(through <= 10000, "PROJECTION_LIMIT_EXCEEDED");
  const auto state = take(b.store.book_state());
  require(through <= state.at("last_sequence").get<std::uint64_t>(), "RESULT_SEQUENCE_UNAVAILABLE");
  Snapshot s;
  s.recorded = stamp(a.at("recorded_through"));
  s.economic = stamp(a.at("effective_at"));
  const auto caps = take(b.store.capabilities());
  const auto limit = std::min<std::size_t>(100, caps.at("limits").at("max_page_size"));
  std::size_t bytes = 0;
  while (s.ledger.records().size() < through) {
    auto rows = take(b.store.ledger_evidence(s.ledger.records().size(), limit));
    require(!rows.empty(), "LEDGER_CORRUPT");
    for (const auto &row : rows) {
      if (row.at("sequence").get<std::uint64_t>() > through)
        break;
      const auto hex = row.at("canonical").get<std::string>();
      bytes += row.dump().size();
      require(bytes <= 32 * 1024 * 1024, "PROJECTION_LIMIT_EXCEEDED");
      require(hex.size() % 2 == 0, "LEDGER_CORRUPT");
      std::vector<std::byte> raw;
      for (std::size_t i = 0; i < hex.size(); i += 2) {
        unsigned n = 0;
        auto [end, ec] = std::from_chars(hex.data() + i, hex.data() + i + 2, n, 16);
        require(ec == std::errc{} && end == hex.data() + i + 2, "LEDGER_CORRUPT");
        raw.push_back(static_cast<std::byte>(n));
      }
      auto decoded = serialization::decode_lifecycle_record(raw);
      require(decoded && decoded->acceptance_sequence == s.ledger.records().size() + 1,
              "LEDGER_CORRUPT");
      require(bool(s.ledger.accept(decoded->draft)), "LEDGER_CORRUPT");
      s.rows.push_back(row);
    }
  }
  if (through)
    require(s.rows.back().at("admission").at("last_sequence") == std::to_string(through),
            "INPUT_REVISION_SPLITS_COMMIT");
  Json bindings = Json::array();
  for (const auto &v : b.accounts)
    bindings.push_back({{"account", v.account},
                        {"legal_entity", v.legal_entity},
                        {"custodian_account", v.custodian_account}});
  s.input = {{"tenant_id", caps.at("tenant_id")},
             {"book_id", caps.at("book_id")},
             {"accepted_through", a.at("accepted_through")},
             {"recorded_through", a.at("recorded_through")},
             {"effective_at", a.at("effective_at")},
             {"mapping_digest", mapping_digest(b)},
             {"mapping_version", b.mapping_version},
             {"account_bindings", bindings},
             {"ledger_digest", digest(s.rows)},
             {"engine", "luca/0.1"}};
  return s;
}
Json page(const BookBinding &b, const Json &a, Json rows, Json manifest) {
  Json selection = a;
  for (auto key : {"cursor", "limit", "format"})
    selection.erase(key);
  manifest["selection"] = selection;
  manifest["rows_digest"] = digest(rows);
  manifest["total_rows"] = std::to_string(rows.size());
  manifest["result_id"] = digest(manifest);
  std::uint64_t after = 0;
  if (a.contains("cursor")) {
    try {
      const auto cursor = Json::parse(a.at("cursor").get<std::string>());
      require(cursor.is_array() && cursor.size() == 2 && cursor[0] == manifest.at("result_id"),
              "INVALID_CURSOR");
      after = sequence(cursor[1]);
    } catch (const Json::exception &) {
      throw ingestion::Failure{"INVALID_CURSOR"};
    }
  }
  require(after <= rows.size(), "INVALID_CURSOR");
  auto maximum =
      std::min<std::size_t>(100, take(b.store.capabilities()).at("limits").at("max_page_size"));
  auto limit = a.value("limit", maximum);
  require(limit <= maximum, "LIMIT_EXCEEDED");
  auto end = std::min<std::uint64_t>(rows.size(), after + limit);
  Json items = Json::array();
  for (auto i = after; i < end; ++i)
    items.push_back(std::move(rows[i]));
  Json result = {{"manifest", manifest},
                 {"items", items},
                 {"total_rows", std::to_string(rows.size())},
                 {"has_more", end < rows.size()},
                 {"next_cursor",
                  end < rows.size()
                      ? Json(Json::array({manifest.at("result_id"), std::to_string(end)}).dump())
                      : Json()}};
  if (a.value("format", std::string{"json"}) == "jsonl") {
    std::string content =
        Json{{"kind", "manifest"}, {"manifest", manifest}, {"offset", std::to_string(after)}}
            .dump() +
        "\n";
    for (const auto &row : items)
      content += Json{{"kind", "row"}, {"row", row}}.dump() + "\n";
    content += Json{{"kind", "page"},
                    {"next_cursor", result.at("next_cursor")},
                    {"has_more", result.at("has_more")}}
                   .dump() +
               "\n";
    result["export"] = {
        {"media_type", "application/x-ndjson"}, {"encoding", "UTF-8"}, {"content", content}};
  }
  require(result.dump().size() <= 1024 * 1024, "PAGE_BYTE_LIMIT_EXCEEDED");
  return result;
}
Json coverage() {
  return {
      {"selection", "admitted_prefix_only"},
      {"staged_inputs_included", false},
      {"staging_completeness", "unknown"},
      {"diagnostics",
       "Inspect import status, records and errors separately; no staging snapshot is asserted."}};
}
} // namespace
Json accounting_report(const BookBinding &b, const Json &a) {
  auto s = snapshot(b, a);
  require(!b.accounts.empty(), "LOT_ACCOUNT_REQUIRED");
  const auto policy_through = sequence(a.at("policy_through"));
  const auto settlement_through = sequence(a.at("settlement_through"));
  require(settlement_through <= 10000, "PROJECTION_LIMIT_EXCEEDED");
  const auto state = take(b.store.book_state());
  require(settlement_through <= state.at("settlement_sequence").get<std::uint64_t>(),
          "SETTLEMENT_SEQUENCE_UNAVAILABLE");
  auto history = take(b.store.policy_history());
  require(policy_through <= history.at("through_sequence").get<std::uint64_t>(),
          "POLICY_SEQUENCE_UNAVAILABLE");
  std::vector<accounting::PolicyRevision> revisions;
  Json selected_revisions = Json::array();
  for (const auto &r : history.at("revisions")) {
    auto revision = take(ingestion::policy_revision(r));
    if (revision.sequence <= policy_through) {
      revisions.push_back(revision);
      selected_revisions.push_back(r);
    }
  }
  std::vector<accounting::ResolvedPolicy> policies;
  Json evidence = Json::array();
  for (const auto &v : b.accounts) {
    auto p = accounting::resolve_policy(
        {s.input.at("book_id"), v.legal_entity, v.custodian_account, v.account}, revisions,
        s.economic, policy_through);
    if (!p)
      throw ingestion::Failure{p.error().code};
    policies.push_back(*p);
    evidence.push_back(ingestion::policy_json(*p));
  }
  require(selected_revisions.dump().size() <= 4 * 1024 * 1024, "PROJECTION_LIMIT_EXCEEDED");
  std::size_t settlement_bytes = 0;
  std::vector<accounting::SettlementEvent> settlements;
  Json raw_settlements = Json::array();
  while (settlements.size() < settlement_through) {
    auto rows = take(b.store.settlement_events(settlements.size(), 100));
    require(!rows.empty(), "SETTLEMENT_SEQUENCE_UNAVAILABLE");
    for (auto row : rows) {
      if (row.at("sequence").get<std::uint64_t>() > settlement_through)
        break;
      settlement_bytes += row.dump().size();
      require(settlement_bytes <= 32 * 1024 * 1024, "PROJECTION_LIMIT_EXCEEDED");
      auto e = take(ingestion::settlement_event(row));
      require(e.sequence == settlements.size() + 1, "INVALID_SETTLEMENT_PREFIX");
      require(e.ledger_through <= s.ledger.records().size(), "INPUT_REVISION_INCOMPATIBLE");
      settlements.push_back(e);
      raw_settlements.push_back(std::move(row));
    }
  }
  s.input["policy_through"] = a.at("policy_through");
  s.input["settlement_through"] = a.at("settlement_through");
  s.input["policy_revisions"] = selected_revisions;
  s.input["resolved_policies"] = evidence;
  s.input["settlement_digest"] = digest(raw_settlements);
  s.input["projection"] = "equity-journals/1";
  s.input["recognition_rules"] = {{"charges", "capitalize_opening"}, {"cash", "both_legs"}};
  auto report = accounting::project_trade_accounting(
      s.ledger, policies, settlements,
      {s.recorded, s.economic, s.ledger.records().size(), settlement_through, b.mapping_version},
      {accounting::ChargeTreatment::capitalize_opening, accounting::CashRecognition::both_legs});
  if (!report)
    throw ingestion::Failure{report.error().code};
  auto controls =
      accounting::reconcile_trade_accounting(s.ledger.resolve(s.recorded, s.economic), *report);
  if (!controls)
    throw ingestion::Failure{controls.error().code};
  Json rows = Json::array(), currency_controls = Json::array(), breaks = Json::array();
  for (const auto &c : report->trial_balance.controls())
    currency_controls.push_back({{"currency", c.currency.code()},
                                 {"debits", money(c.debits)},
                                 {"credits", money(c.credits)},
                                 {"debit_balances", money(c.debit_balances)},
                                 {"credit_balances", money(c.credit_balances)}});
  for (const auto &v : report->breaks)
    breaks.push_back({{"code", v.code},
                      {"record_id", v.record_id},
                      {"amount", money(v.amount)},
                      {"settlement_ids", v.settlement_ids}});
  const auto collection = a.at("collection").get<std::string>();
  require(!a.contains("chart_account") || collection == "journals" || collection == "trial_balance",
          "INVALID_ARGUMENTS");
  std::map<std::string, Json> traces;
  for (const auto &v : report->trace) {
    auto [it, inserted] = traces.try_emplace(v.entry_id, Json::array());
    it->second.push_back({{"account", v.ledger_account},
                          {"policy_digest", v.policy_digest},
                          {"relief_records", v.relief_records},
                          {"settlement_ids", v.settlement_ids}});
  }
  if (collection == "journals") {
    for (const auto &entry : report->journals)
      for (const auto &line : entry.lines()) {
        const auto it = traces.find(entry.journal_entry_id().value());
        const auto trace = it == traces.end() ? Json::array() : it->second;
        rows.push_back(
            {{"entry_id", entry.journal_entry_id().value()},
             {"line_id", line.journal_line_id().value()},
             {"chart_account", line.account_id().value()},
             {"side", line.side() == JournalSide::debit ? "debit" : "credit"},
             {"amount", money(line.amount())},
             {"effective_at", serialization::detail::canonical_timestamp(entry.effective_at())},
             {"recorded_at", serialization::detail::canonical_timestamp(entry.recorded_at())},
             {"recognized_on", serialization::detail::canonical_date(
                                   *SettlementDate::create(entry.recognized_on().value()))},
             {"lineage", lineage(entry.lineage())},
             {"trace", trace}});
      }
  } else if (collection == "trial_balance") {
    for (const auto &v : report->trial_balance.balances()) {
      Json lines = Json::array();
      for (const auto &l : v.lines)
        lines.push_back({{"entry_id", l.entry.value()}, {"line_id", l.line.value()}});
      rows.push_back({{"book_id", v.key.book.value()},
                      {"chart_account", v.key.account.value()},
                      {"debits", money(v.debits)},
                      {"credits", money(v.credits)},
                      {"net", money(v.net)},
                      {"lines", lines}});
    }
  } else if (collection == "obligations") {
    for (const auto &v : report->obligations)
      rows.push_back({{"record_id", v.record_id},
                      {"economic_event_id", v.economic_event},
                      {"account", v.account},
                      {"contractual_cash", money(v.contractual_cash)},
                      {"actual_cash", money(v.actual_cash)},
                      {"outstanding_cash", money(v.outstanding_cash)}});
  } else if (collection == "actual_settlements") {
    for (const auto &e : report->settlements) {
      auto row = raw_settlements.at(e.sequence - 1);
      row["sequence"] = std::to_string(e.sequence);
      row["ledger_through"] = std::to_string(e.ledger_through);
      row["amount"] = money(e.cash);
      rows.push_back(row);
    }
  } else
    for (const auto &v : *controls)
      rows.push_back({{"control", v.control},
                      {"expected", money(v.expected)},
                      {"journal", money(v.journal)},
                      {"difference", money(v.difference)}});
  Json filtered = Json::array();
  for (auto &row : rows) {
    if (a.contains("chart_account") && row.at("chart_account") != a.at("chart_account"))
      continue;
    auto currency = row.contains("amount")             ? row.at("amount").at("currency")
                    : row.contains("net")              ? row.at("net").at("currency")
                    : row.contains("contractual_cash") ? row.at("contractual_cash").at("currency")
                                                       : row.at("expected").at("currency");
    if (a.contains("currency") && currency != a.at("currency"))
      continue;
    filtered.push_back(std::move(row));
  }
  return page(b, a, std::move(filtered),
              {{"input", s.input},
               {"meaning", "as_of_balance; regenerated_snapshot_journals; not_period_activity"},
               {"collection", collection},
               {"coverage", coverage()},
               {"charges_complete", report->lots.charges_complete},
               {"currency_controls", currency_controls},
               {"control_scope", "whole_journals_before_display_filters"},
               {"breaks", breaks},
               {"unrealized_pnl", "unavailable; no valuation inputs"}});
}
Json query_transactions(const BookBinding &b, const Json &a) {
  auto s = snapshot(b, a);
  const auto from = a.contains("period_from") ? stamp(a.at("period_from")) : Timestamp::min();
  const auto to = a.contains("period_to") ? stamp(a.at("period_to")) : Timestamp::max();
  require(from < to, "INVALID_ARGUMENTS");
  const auto filters = a.value("filters", Json::object());
  const auto resolved = s.ledger.resolve(s.recorded, s.economic);
  std::map<std::string, const LifecycleRecord *> heads, successors;
  std::set<std::string> active;
  for (const auto &c : resolved.chains())
    heads[c.economic_event_id().value()] = &c.head();
  for (const auto &e : resolved.active_events())
    active.insert(e.record().record_id().value());
  for (const auto &r : s.ledger.records())
    if (r.recorded_at() <= s.recorded && r.causal_record_id() &&
        (!r.cancellation_at() || *r.cancellation_at() <= s.economic))
      successors[r.causal_record_id()->value()] = &r;
  Json rows = Json::array();
  for (std::size_t i = 0; i < s.ledger.records().size(); ++i) {
    const auto &r = s.ledger.records()[i];
    if (r.recorded_at() > s.recorded)
      continue;
    auto head = heads.find(r.economic_event_id().value());
    if (head == heads.end())
      continue;
    const auto when = r.event() ? r.effective_at() : r.cancellation_at();
    if (!when) {
      if (a.contains("period_from") || a.contains("period_to"))
        continue;
    } else if (*when > s.economic || *when < from || *when >= to)
      continue;
    const auto status = r.action() == LifecycleAction::cancel               ? "cancellation"
                        : active.contains(r.record_id().value())            ? "active"
                        : head->second->action() == LifecycleAction::cancel ? "voided"
                                                                            : "superseded";
    auto row = transaction_row(s.rows.at(i));
    auto &tx = row["transaction"];
    const auto type = tx.at("event").is_null() ? Json("cancellation") : tx.at("event").at("type");
    const LifecycleRecord *instrument_record = &r;
    while (!instrument_record->event() && instrument_record->causal_record_id())
      instrument_record = s.ledger.find(*instrument_record->causal_record_id());
    Json instrument;
    if (instrument_record && instrument_record->event())
      if (const auto *trade = std::get_if<EquityTrade>(instrument_record->event()))
        instrument = trade->instrument().value();
    tx["instrument_id"] = instrument;
    if (filters.contains("account") && filters.at("account") != r.account().value())
      continue;
    if (filters.contains("instrument") && filters.at("instrument") != instrument)
      continue;
    if (filters.contains("source") &&
        filters.at("source") != row.at("source_metadata").at("source_id"))
      continue;
    if (filters.contains("status") && filters.at("status") != status)
      continue;
    if (filters.contains("type") && filters.at("type") != type)
      continue;
    tx["status"] = status;
    tx["processing_status"] = "committed";
    const auto successor = successors.find(r.record_id().value());
    tx["next_record_id"] =
        successor == successors.end() ? Json() : Json(successor->second->record_id().value());
    tx["void_at"] = successor != successors.end() && successor->second->cancellation_at()
                        ? Json(serialization::detail::canonical_timestamp(
                              *successor->second->cancellation_at()))
                        : Json();
    tx["version_contract"] = "immutable_lifecycle_history; retrospective_correct_is_not_dated_edit";
    rows.push_back(std::move(row));
  }
  return page(b, a, std::move(rows),
              {{"input", s.input},
               {"meaning",
                "accepted_transaction_history_at_economic_and_knowledge_cutoffs; period_half_open"},
               {"coverage", coverage()}});
}
} // namespace luca::application::detail
