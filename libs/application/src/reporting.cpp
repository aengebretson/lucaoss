#include "internal.hpp"
#include "luca/accounting/period_pnl_rows.hpp"
#include "luca/ingestion/policy.hpp"
#include "luca/ingestion/settlement.hpp"
#include "luca/reconciliation/trade_accounting.hpp"
#include "luca/serialization/canonical_decode.hpp"
#include "luca/transaction_versions.hpp"
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
      require(bool(s.ledger.restore_record(decoded->draft, decoded->acceptance_sequence)),
              "LEDGER_CORRUPT");
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
  s.input["policy_selection"] = "explicit_sequence";
  s.input["policy_knowledge"] = Json::array();
  for (auto row : history.at("knowledge"))
    if (row.at("sequence").get<std::uint64_t>() <= policy_through) {
      row["sequence"] = std::to_string(row.at("sequence").get<std::uint64_t>());
      s.input["policy_knowledge"].push_back(std::move(row));
    }
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
Json query_pnl(const BookBinding &b, const Json &a) {
  // Full native immutable prefixes are loaded before any display selection.
  // A host without a period result store cannot verify retained close anchors.
  require(take(b.store.periods()).at("closed_through").is_null(), "PERIOD_RETENTION_UNAVAILABLE");
  require(sequence(a.at("accepted_through")) <= 1000 &&
              sequence(a.at("settlement_through")) <= 1000 &&
              sequence(a.at("policy_through")) <= 256 && b.accounts.size() <= 256,
          "PERIOD_REPLAY_LIMIT_EXCEEDED");
  require(!b.accounts.empty(), "LOT_ACCOUNT_REQUIRED");
  const auto from = stamp(a.at("period_from")), until = stamp(a.at("period_until"));
  require(from != Timestamp::min() && from < until, "INVALID_PERIOD_CONTEXT");
  const auto collection = a.at("collection").get<std::string>();
  require(collection == "rows" || (!a.contains("account") && !a.contains("instrument")),
          "INVALID_ARGUMENTS");
  Json selection = a;
  selection["effective_at"] =
      serialization::detail::canonical_timestamp(until - std::chrono::nanoseconds{1});
  auto s = snapshot(b, selection);
  const auto policy_through = sequence(a.at("policy_through"));
  const auto cash_through = sequence(a.at("settlement_through"));
  const auto history = take(b.store.policy_history());
  require(policy_through <= history.at("through_sequence").get<std::uint64_t>(),
          "POLICY_SEQUENCE_UNAVAILABLE");
  std::map<std::uint64_t, Timestamp> knowledge;
  Json policy_knowledge = Json::array(), raw_policy = Json::array();
  for (const auto &row : history.at("knowledge")) {
    auto n = row.at("sequence").get<std::uint64_t>();
    if (n > policy_through)
      continue;
    require(!row.at("recorded_at").is_null(), "POLICY_KNOWLEDGE_UNAVAILABLE");
    require(knowledge.emplace(n, stamp(row.at("recorded_at"))).second, "INVALID_POLICY_HISTORY");
    policy_knowledge.push_back(
        {{"sequence", std::to_string(n)}, {"recorded_at", row.at("recorded_at")}});
  }
  std::vector<accounting::KnownPolicyRevision> policies;
  for (const auto &row : history.at("revisions")) {
    auto revision = take(ingestion::policy_revision(row));
    if (revision.sequence > policy_through)
      continue;
    require(knowledge.contains(revision.sequence), "POLICY_KNOWLEDGE_UNAVAILABLE");
    policies.push_back({revision, knowledge.at(revision.sequence)});
    raw_policy.push_back(row);
  }
  require(raw_policy.dump().size() <= 4 * 1024 * 1024, "PROJECTION_LIMIT_EXCEEDED");
  require(cash_through <= take(b.store.book_state()).at("settlement_sequence").get<std::uint64_t>(),
          "SETTLEMENT_SEQUENCE_UNAVAILABLE");
  std::vector<accounting::SettlementEvent> cash;
  Json raw_cash = Json::array();
  std::size_t cash_bytes = 0;
  while (cash.size() < cash_through) {
    auto batch = take(b.store.settlement_events(cash.size(), 100));
    require(!batch.empty(), "SETTLEMENT_SEQUENCE_UNAVAILABLE");
    for (auto row : batch) {
      if (row.at("sequence").get<std::uint64_t>() > cash_through)
        break;
      cash_bytes += row.dump().size();
      require(cash_bytes <= 32 * 1024 * 1024, "PROJECTION_LIMIT_EXCEEDED");
      auto e = take(ingestion::settlement_event(row));
      require(e.sequence == cash.size() + 1, "INVALID_SETTLEMENT_PREFIX");
      require(e.ledger_through <= s.ledger.size(), "INPUT_REVISION_INCOMPATIBLE");
      cash.push_back(e);
      raw_cash.push_back(std::move(row));
    }
  }
  std::vector<accounting::PolicyTarget> targets;
  for (const auto &v : b.accounts)
    targets.push_back({s.input.at("book_id"), v.legal_entity, v.custodian_account, v.account});
  const accounting::PeriodLimits limits{512, 200000};
  const auto report = accounting::project_period_accounting(
      s.ledger, targets, policies, cash,
      {from, until, s.recorded, s.ledger.size(), policy_through, cash_through,
       a.at("reporting_timezone"), std::string(accounting::period_chart_version),
       accounting::CorrectionRecognition::recorded_time},
      {accounting::ChargeTreatment::capitalize_opening, accounting::CashRecognition::both_legs}, {},
      limits);
  if (!report)
    throw ingestion::Failure{report.error().code};
  const auto selected = accounting::select_period_pnl(
      *report, {a.value("account", std::string{}), a.value("instrument", std::string{})});
  if (!selected)
    throw ingestion::Failure{selected.error().code};
  const auto amounts = [](const accounting::PeriodPnlAmounts &v) {
    return Json{{"economic_realized", money(v.economic)},
                {"recognized_realized", money(v.recognized)},
                {"deferred_realized", money(v.deferred)}};
  };
  Json rows = Json::array(), totals = Json::array(), diagnostics = Json::array();
  for (const auto &v : collection == "rows" ? selected->totals : report->pnl)
    totals.push_back({{"currency", v.currency.code()},
                      {"opening", amounts(v.opening)},
                      {"activity", amounts(v.movement)},
                      {"closing", amounts(v.closing)}});
  if (collection == "rows") {
    for (const auto &v : selected->rows) {
      const auto &pool = v.key.pool;
      rows.push_back({{"book_id", pool.book},
                      {"legal_entity", pool.legal_entity},
                      {"custodian_account", pool.custodian_account},
                      {"account", v.key.account},
                      {"instrument", pool.instrument},
                      {"currency", pool.currency},
                      {"opening", amounts(v.opening)},
                      {"activity", amounts(v.movement)},
                      {"closing", amounts(v.closing)},
                      {"opening_charges_complete", v.opening_charges_complete},
                      {"closing_charges_complete", v.closing_charges_complete},
                      {"record_ids", identifiers(v.records)},
                      {"settlement_ids", v.settlements},
                      {"entry_ids", identifiers(v.entries)}});
    }
  } else if (collection == "daily") {
    for (const auto &v : report->daily_pnl)
      rows.push_back(
          {{"day", serialization::detail::canonical_date(*SettlementDate::create(v.day))},
           {"currency", v.currency.code()},
           {"activity", amounts(v.movement)},
           {"entry_ids", identifiers(v.entries)}});
  } else {
    // Exportable full canonical/source evidence, including inputs excluded by the
    // knowledge cutoff; those are evidence, never asserted to be period activity.
    for (auto row : s.rows)
      rows.push_back({{"kind", "ledger"}, {"ledger", transaction_row(std::move(row))}});
    for (std::size_t i = 0; i < policies.size(); ++i)
      rows.push_back(
          {{"kind", "policy"},
           {"revision", raw_policy.at(i)},
           {"recorded_at", serialization::detail::canonical_timestamp(policies[i].recorded_at)}});
    for (auto row : raw_cash) {
      row["sequence"] = std::to_string(row.at("sequence").get<std::uint64_t>());
      row["ledger_through"] = std::to_string(row.at("ledger_through").get<std::uint64_t>());
      rows.push_back({{"kind", "settlement"}, {"settlement", std::move(row)}});
    }
  }
  for (const auto &v : report->diagnostics)
    diagnostics.push_back({{"code", v.code},
                           {"id", v.id},
                           {"affects_supported_coverage", v.affects_supported_coverage}});
  s.input["policy_through"] = a.at("policy_through");
  s.input["settlement_through"] = a.at("settlement_through");
  s.input["policy_revisions"] = raw_policy;
  s.input["policy_knowledge"] = policy_knowledge;
  s.input["settlement_digest"] = digest(raw_cash);
  s.input["projection"] = std::string(accounting::period_accounting_version);
  s.input["chart_version"] = std::string(accounting::period_chart_version);
  s.input["correction_recognition"] = "recorded_time";
  s.input["reporting_timezone"] = a.at("reporting_timezone");
  s.input["period_from"] = a.at("period_from");
  s.input["period_until"] = a.at("period_until");
  s.input["recognition_rules"] = {{"charges", "capitalize_opening"}, {"cash", "both_legs"}};
  s.input["replay_limits"] = {{"max_boundaries", limits.max_boundaries},
                              {"max_replay_work", limits.max_replay_work}};
  s.input["core_evidence"] = {{"input_digest", report->inputs.input_digest},
                              {"ledger_digest", report->inputs.ledger_digest},
                              {"policy_digest", report->inputs.policy_digest},
                              {"settlement_digest", report->inputs.settlement_digest},
                              {"history_digest", report->history_digest}};
  // A concurrent close invalidates availability, rather than returning an
  // unanchored open-history result for a book that acquired a protected baseline.
  require(take(b.store.periods()).at("closed_through").is_null(), "PERIOD_RETENTION_UNAVAILABLE");
  return page(b, a, std::move(rows),
              {{"input", s.input},
               {"collection", collection},
               {"report_id", report->report_id},
               {"meaning", "activity=period_realized_pnl; opening/closing=as_of_realized_pnl; "
                           "gains_positive; no_valuation"},
               {"coverage", coverage()},
               {"supported_complete", report->supported_complete},
               {"coverage_scope",
                "supported_cash_equity_cost_based_slice_only; diagnostics_before_display_filters"},
               {"diagnostics", diagnostics},
               {"totals", totals},
               {"totals_scope", collection == "rows" ? "all_filtered_rows_before_pagination"
                                                     : "whole_book_by_currency"},
               {"retention_available", false},
               {"unrealized_pnl", nullptr},
               {"other_income", nullptr},
               {"expenses", nullptr},
               {"fx", nullptr},
               {"cash_flow", nullptr}});
}
Json query_transactions(const BookBinding &b, const Json &a) {
  auto s = snapshot(b, a);
  const auto from = a.contains("period_from") ? stamp(a.at("period_from")) : Timestamp::min();
  const auto to = a.contains("period_to") ? stamp(a.at("period_to")) : Timestamp::max();
  require(from < to, "INVALID_ARGUMENTS");
  const auto filters = a.value("filters", Json::object());
  const auto resolved = s.ledger.resolve(s.recorded, s.economic);
  const auto versions = transaction_versions(s.ledger, s.recorded);
  std::map<std::string, const TransactionVersion *> version_index;
  for (const auto &v : versions)
    version_index.emplace(v.version_id, &v);
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
    const auto &version = *version_index.at(r.record_id().value());
    const auto status =
        r.action() == LifecycleAction::cancel    ? "cancellation"
        : active.contains(r.record_id().value()) ? "active"
        : version.activation_effective_at && *version.activation_effective_at > s.economic
            ? "pending_activation"
        : version.void_version_id &&
                (!version.void_effective_at || *version.void_effective_at <= s.economic)
            ? "voided"
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
    const auto optional_id = [](const auto &value) { return value ? Json(*value) : Json(); };
    const auto optional_time = [](const auto &value) {
      return value ? Json(serialization::detail::canonical_timestamp(*value)) : Json();
    };
    tx["predecessor_id"] = optional_id(version.predecessor_id);
    tx["void_version_id"] = optional_id(version.void_version_id);
    tx["replacement_version_id"] = optional_id(version.replacement_version_id);
    tx["void_effective_at"] = optional_time(version.void_effective_at);
    tx["void_recorded_at"] = optional_time(version.void_recorded_at);
    tx["activation_effective_at"] = optional_time(version.activation_effective_at);
    tx["version_contract"] = "immutable-dated-versions/1";
    rows.push_back(std::move(row));
  }
  return page(b, a, std::move(rows),
              {{"input", s.input},
               {"meaning",
                "accepted_transaction_history_at_economic_and_knowledge_cutoffs; period_half_open"},
               {"coverage", coverage()}});
}
} // namespace luca::application::detail
