#include "internal.hpp"
#include "luca/accounting/lots.hpp"
#include "luca/ingestion/policy.hpp"
#include "luca/serialization/canonical_decode.hpp"
#include <charconv>

namespace luca::application::detail {
namespace {
std::vector<std::byte> unhex(std::string_view hex) {
  require(hex.size() % 2 == 0, "LEDGER_CORRUPT");
  std::vector<std::byte> bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    unsigned value = 0;
    const auto [end, error] = std::from_chars(hex.data() + i, hex.data() + i + 2, value, 16);
    require(error == std::errc{} && end == hex.data() + i + 2, "LEDGER_CORRUPT");
    bytes.push_back(static_cast<std::byte>(value));
  }
  return bytes;
}
std::string decimal(std::int64_t value, unsigned scale) {
  // Format INT64_MIN without signed negation or binary floating point.
  auto magnitude = value < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(value)
                             : static_cast<std::uint64_t>(value);
  auto digits = std::to_string(magnitude);
  if (digits.size() <= scale)
    digits.insert(0, scale + 1 - digits.size(), '0');
  digits.insert(digits.size() - scale, 1, '.');
  if (value < 0)
    digits.insert(digits.begin(), '-');
  return digits;
}
Json money(Money m) {
  return {{"amount", decimal(m.scaled_value(), Money::scale)}, {"currency", m.currency().code()}};
}
Json amounts(const accounting::LotAmounts &a) {
  Json charges = Json::array();
  for (const auto &c : a.charges)
    charges.push_back({{"id", c.id},
                       {"category", c.category},
                       {"payee", c.payee},
                       {"amount", money(c.amount)},
                       {"state", c.state},
                       {"schedule_version", c.schedule_version}});
  return {{"principal", money(a.principal)}, {"charges", charges}};
}
Json pool(const accounting::LotPool &p) {
  return {{"book", p.book},
          {"legal_entity", p.legal_entity},
          {"custodian_account", p.custodian_account},
          {"instrument", p.instrument},
          {"currency", p.currency}};
}
Json transaction(Json row) {
  auto bytes = unhex(row.at("canonical").get<std::string>());
  auto decoded = serialization::decode_lifecycle_record(bytes);
  require(bool(decoded), "LEDGER_CORRUPT");
  require(decoded->acceptance_sequence == row.at("sequence").get<std::uint64_t>(),
          "LEDGER_CORRUPT");
  const auto &d = decoded->draft;
  Json sources = Json::array();
  for (const auto &id : d.provenance().source_records())
    sources.push_back(id.value());
  row["transaction"] = {
      {"record_id", d.record_id().value()},
      {"economic_event_id", d.economic_event_id().value()},
      {"account_id", d.account().value()},
      {"action", serialization::detail::lifecycle_action_name(d.action())},
      {"recorded_at", serialization::detail::canonical_timestamp(d.recorded_at())},
      {"source_record_ids", sources},
      {"causal_record_id", d.causal_record_id() ? Json(d.causal_record_id()->value()) : Json()},
      {"cancellation_at",
       d.cancellation_at() ? Json(serialization::detail::canonical_timestamp(*d.cancellation_at()))
                           : Json()},
      {"event", nullptr}};
  if (d.event())
    std::visit(
        [&](const auto &event) {
          Json value = {{"effective_at", serialization::detail::canonical_timestamp(
                                             event.header().effective_at())}};
          if constexpr (std::is_same_v<std::decay_t<decltype(event)>, CashMovement>) {
            value["type"] = "cash_movement";
            value["amount"] = money(event.amount());
          } else {
            value["type"] = "equity_trade";
            value["instrument_id"] = event.instrument().value();
            value["quantity"] = decimal(event.quantity().scaled_value(), Quantity::scale);
            value["price"] = decimal(event.price().scaled_value(), Price::scale);
            value["quote_currency"] = event.quote_currency().code();
            value["settlement_date"] =
                serialization::detail::canonical_date(event.settlement_date());
            if (event.charges()) {
              // Only serialization here: principal and charge arithmetic stay in core.
              Json charges = Json::array();
              for (const auto &c : *event.charges())
                charges.push_back({{"id", c.id},
                                   {"category", c.category},
                                   {"payee", c.payee},
                                   {"amount", money(c.amount)},
                                   {"state", c.state},
                                   {"schedule_version", c.schedule_version}});
              value["charges"] = charges;
            } else
              value["charges"] = nullptr;
          }
          row["transaction"]["event"] = std::move(value);
        },
        *d.event());
  // Native canonical bytes remain the lossless source of event and provenance.
  row["encoding"] = "luca.lifecycle.canonical";
  for (const auto *key : {"sequence", "recorded_ns"})
    if (row.contains(key) && row[key].is_number_integer())
      row[key] = std::to_string(row[key].get<std::int64_t>());
  return row;
}
} // namespace
Json transaction_row(Json row) { return transaction(std::move(row)); }
std::string mapping_digest(const BookBinding &b) {
  Json rows = Json::array();
  for (const auto &a : b.accounts)
    rows.push_back({a.account, a.legal_entity, a.custodian_account});
  auto scope = take(b.store.capabilities());
  const auto bytes = Json::array({"luca.mapping/1", scope.at("tenant_id"), scope.at("book_id"),
                                  b.mapping_version, rows})
                         .dump();
  return serialization::detail::sha256_hex(std::as_bytes(std::span{bytes}));
}
Json project(const BookBinding &b, const Json &a, bool lots) {
  auto state = take(b.store.book_state());
  const auto through = sequence(a.at("accepted_through"));
  require(through <= state.at("last_sequence").get<std::uint64_t>(), "RESULT_SEQUENCE_UNAVAILABLE");
  const auto after = a.contains("after") ? sequence(a.at("after")) : 0;
  const auto capabilities = take(b.store.capabilities());
  const auto maximum =
      std::min<std::size_t>(100, capabilities.at("limits").at("max_page_size").get<std::size_t>());
  const auto limit = a.value("limit", maximum);
  require(limit <= maximum, "LIMIT_EXCEEDED");
  if (!lots) {
    require(after <= through, "INVALID_CURSOR");
    auto raw = take(b.store.ledger_records(after, limit));
    Json rows = Json::array();
    auto last = after;
    for (auto &row : raw) {
      auto seq = row.at("sequence").get<std::uint64_t>();
      if (seq > through)
        break;
      require(seq == last + 1, "LEDGER_CORRUPT");
      rows.push_back(transaction(row));
      last = seq;
    }
    require(last == through || !rows.empty(), "LEDGER_CORRUPT");
    return {{"items", rows},
            {"accepted_through", std::to_string(through)},
            {"next_after", std::to_string(last)},
            {"has_more", last < through}};
  }
  require(a.at("mapping_digest") == mapping_digest(b), "MAPPING_CONFLICT");
  require(through <= 10000, "PROJECTION_LIMIT_EXCEEDED");
  require(!b.accounts.empty(), "LOT_ACCOUNT_REQUIRED");
  const auto at = adapters::parse_transaction_timestamp(a.at("effective_at").get<std::string>());
  const auto recorded =
      adapters::parse_transaction_timestamp(a.at("recorded_through").get<std::string>());
  require(bool(at) && bool(recorded), "INVALID_ARGUMENTS");
  const auto policy_through = sequence(a.at("policy_through"));
  // Read complete immutable history once, then resolve every binding at the same
  // explicit cutoff. Later policy or ledger appends cannot change this result.
  auto history = take(b.store.policy_history());
  require(policy_through <= history.at("through_sequence").get<std::uint64_t>(),
          "POLICY_SEQUENCE_UNAVAILABLE");
  std::vector<accounting::PolicyRevision> revisions;
  for (const auto &row : history.at("revisions"))
    revisions.push_back(take(ingestion::policy_revision(row)));
  std::vector<accounting::ResolvedPolicy> policies;
  const auto book = take(b.store.capabilities()).at("book_id").get<std::string>();
  for (const auto &binding : b.accounts) {
    auto p = accounting::resolve_policy(
        {book, binding.legal_entity, binding.custodian_account, binding.account}, revisions, *at,
        policy_through);
    if (!p)
      throw ingestion::Failure{p.error().code};
    policies.push_back(*p);
  }
  LifecycleLedger ledger;
  while (ledger.records().size() < through) {
    auto rows = take(b.store.ledger_records(ledger.records().size(), maximum));
    require(!rows.empty(), "LEDGER_CORRUPT");
    for (const auto &row : rows) {
      if (row.at("sequence").get<std::uint64_t>() > through)
        break;
      auto decoded =
          serialization::decode_lifecycle_record(unhex(row.at("canonical").get<std::string>()));
      require(decoded && decoded->acceptance_sequence == ledger.records().size() + 1,
              "LEDGER_CORRUPT");
      require(bool(ledger.accept(decoded->draft)), "LEDGER_CORRUPT");
    }
  }
  auto resolution = ledger.resolve(*recorded, *at);
  auto report = accounting::project_policy_lots(resolution, policies);
  if (!report)
    throw ingestion::Failure{report.error().code};
  Json rows = Json::array();
  const auto collection = a.at("collection").get<std::string>();
  if (collection == "trades")
    for (const auto &t : report->trades)
      rows.push_back({{"pool", pool(t.pool)},
                      {"record_id", t.record_id},
                      {"policy_version", t.policy_version},
                      {"quantity", decimal(t.signed_quantity.scaled_value(), Quantity::scale)},
                      {"amounts", amounts(t.amounts)},
                      {"commissions", money(t.commissions)},
                      {"fees", money(t.fees)},
                      {"cash_flow", money(t.cash_flow)},
                      {"charges_known", t.charges_known}});
  else if (collection == "open_lots")
    for (const auto &t : report->open_lots) {
      auto carrying = accounting::lot_carrying_amount(t);
      require(bool(carrying), "PROJECTION_AMOUNT_OVERFLOW");
      rows.push_back(
          {{"pool", pool(t.pool)},
           {"opening_record", t.opening_record},
           {"policy_version", t.policy_version},
           {"opened_at", serialization::detail::canonical_timestamp(t.opened_at)},
           {"opening_sequence", std::to_string(t.opening_sequence)},
           {"short_position", t.short_position},
           {"charges_known", t.charges_known},
           {"remaining_quantity", decimal(t.remaining_quantity.scaled_value(), Quantity::scale)},
           {"remaining", amounts(t.remaining)},
           {"carrying_amount", money(*carrying)}});
    }
  else
    for (const auto &t : report->reliefs)
      rows.push_back({{"pool", pool(t.pool)},
                      {"opening_record", t.opening_record},
                      {"closing_record", t.closing_record},
                      {"policy_version", t.policy_version},
                      {"short_position", t.short_position},
                      {"charges_known", t.charges_known},
                      {"quantity", decimal(t.quantity.scaled_value(), Quantity::scale)},
                      {"opening", amounts(t.opening)},
                      {"closing", amounts(t.closing)},
                      {"gross_pnl", money(t.gross_pnl)},
                      {"net_pnl", money(t.net_pnl)}});
  require(after <= rows.size(), "INVALID_CURSOR");
  Json page = Json::array();
  auto end = std::min<std::uint64_t>(rows.size(), after + limit);
  for (auto i = after; i < end; ++i)
    page.push_back(std::move(rows[i]));
  Json evidence = Json::array();
  for (const auto &p : report->resolved_policies)
    evidence.push_back(ingestion::policy_json(p));
  return {{"items", page},
          {"collection", collection},
          {"accepted_through", std::to_string(through)},
          {"policy_through", std::to_string(policy_through)},
          {"effective_at", a.at("effective_at")},
          {"recorded_through", a.at("recorded_through")},
          {"mapping_version", b.mapping_version},
          {"mapping_digest", mapping_digest(b)},
          {"resolved_policies", evidence},
          {"charges_complete", report->charges_complete},
          {"recognition_applied", false},
          {"projection", "economic-lifo/1"},
          {"total_rows", rows.size()},
          {"next_after", std::to_string(end)},
          {"has_more", end < rows.size()},
          {"coverage", "admitted_prefix_only; staged records excluded"}};
}
} // namespace luca::application::detail
