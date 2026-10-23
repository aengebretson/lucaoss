#include "luca/accounting/period_accounting.hpp"
#include <charconv>
#include <chrono>
#include <iostream>
using namespace luca;
using namespace luca::accounting;
template <class T> auto take(T value) {
  if (!value)
    std::abort();
  return *value;
}
int main(int argc, char **argv) {
  std::size_t count = 100;
  if (argc > 2)
    return 2;
  if (argc == 2) {
    auto end = argv[1] + std::char_traits<char>::length(argv[1]);
    auto parsed = std::from_chars(argv[1], end, count);
    if (parsed.ec != std::errc{} || parsed.ptr != end)
      return 2;
  }
  if (!count || count % 2 || count > 500)
    return 2;
  const auto usd = take(Currency::from_code("USD"));
  auto money = [&](std::string_view text) { return take(Money::parse(text, usd)); };
  auto at = [](std::size_t n) {
    return Timestamp{std::chrono::sys_days{std::chrono::year{2026} / 1 / 1}} +
           std::chrono::seconds{n};
  };
  auto proof = [](std::string id) {
    return take(Provenance::create({SourceRecordId{"source:" + id}}, "synthetic-benchmark", "1"));
  };
  LifecycleLedger ledger;
  std::vector<SettlementEvent> cash;
  auto h = take(EventHeader::create(EventId{"capital"}, AccountId{"a"}, at(0), proof("capital")));
  take(ledger.accept(
      LifecycleRecordDraft::originate(EconomicEventId{"capital"}, at(0),
                                      EconomicEvent{CashMovement::create(h, money("1000000"))})));
  for (std::size_t i = 1; i <= count; ++i) {
    auto id = "trade:" + std::to_string(i);
    auto header = take(EventHeader::create(EventId{id}, AccountId{"a"}, at(i), proof(id)));
    auto t = take(EquityTrade::create(
        header, InstrumentId{"XYZ"}, take(Quantity::parse(i % 2 ? "1" : "-1")),
        take(Price::parse(i % 2 ? "10" : "12")), usd,
        take(SettlementDate::create(std::chrono::year{2026} / 1 / 31)),
        TradeCharges{{"c", "commission", "broker", "", "reported", money("0.01")},
                     {"f", "exchange", "exchange", "", "reported", money("0.01")}}));
    take(ledger.accept(
        LifecycleRecordDraft::originate(EconomicEventId{id}, at(i), EconomicEvent{t})));
    cash.push_back({"cash:" + std::to_string(i),
                    "a",
                    id,
                    id,
                    at(count + i),
                    at(count + i),
                    i,
                    count + 1,
                    take(trade_cash(t)),
                    {SourceRecordId{"bank:" + id}},
                    {}});
  }
  std::vector<PolicyTarget> targets{{"book", "entity", "custody", "a"}};
  std::vector<KnownPolicyRevision> policy{
      {{"policy", "entity", "entity", "operator", PolicyScope::legal_entity,
        at(0) - std::chrono::seconds{1}, 1, LotSelection::lifo, RecognitionBasis::cash},
       at(0) - std::chrono::seconds{1}}};
  auto start = std::chrono::steady_clock::now();
  auto report = project_period_accounting(
      ledger, targets, policy, cash,
      {at(1) - std::chrono::seconds{1}, at(3 * count + 1), at(3 * count), ledger.size(),
       policy.size(), cash.size(), "UTC", std::string(period_chart_version),
       CorrectionRecognition::recorded_time},
      {ChargeTreatment::capitalize_opening, CashRecognition::both_legs});
  const auto elapsed =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  if (!report) {
    std::cerr << report.error().code << '\n';
    return 1;
  }
  const auto expected = Money::from_scaled(static_cast<std::int64_t>(count / 2) * 1960000, usd);
  if (report->pnl.size() != 1 || report->pnl[0].movement.economic != expected ||
      report->pnl[0].movement.recognized != expected)
    return 1;
  for (const auto &row : report->balances)
    if (take(row.opening.add(row.movement)) != row.closing)
      return 1;
  for (const auto &effect : report->effects)
    if (effect.journal.debit_total() != effect.journal.credit_total())
      return 1;
  std::cout << "trades=" << count << " settlements=" << cash.size()
            << " effects=" << report->effects.size() << " elapsed_ms=" << elapsed
            << " report_id=" << report->report_id << '\n';
}
