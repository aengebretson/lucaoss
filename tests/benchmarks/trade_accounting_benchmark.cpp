#include "luca/reconciliation/trade_accounting.hpp"
#include <chrono>
#include <iostream>
using namespace luca;
using namespace luca::accounting;
template <class T> auto take(T r) {
  if (!r)
    std::abort();
  return *r;
}
Timestamp at(std::int64_t n) { return Timestamp{std::chrono::seconds{1700000000 + n}}; }
int main(int argc, char **argv) {
  const auto count = argc > 1 ? std::stoull(argv[1]) : 10000;
  if (count < 2 || count > 100000 || count % 2)
    return 2;
  const auto usd = take(Currency::from_code("USD"));
  LifecycleLedger ledger;
  for (std::size_t i = 0; i < count; ++i) {
    auto id = "t" + std::to_string(i);
    auto h = take(
        EventHeader::create(EventId{id}, AccountId{"a"}, at(i),
                            take(Provenance::create({SourceRecordId{"s" + id}}, "fixture", "1"))));
    auto trade =
        take(EquityTrade::create(h, InstrumentId{"XYZ"}, take(Quantity::parse(i % 2 ? "-1" : "1")),
                                 take(Price::parse(i % 2 ? "11" : "10")), usd,
                                 take(SettlementDate::create(std::chrono::year{2023} / 11 / 16)),
                                 TradeCharges{{"c", "commission", "broker", "", "reported",
                                               take(Money::parse("0.01", usd))}}));
    take(ledger.accept(
        LifecycleRecordDraft::originate(EconomicEventId{id}, at(100000), EconomicEvent{trade})));
  }
  std::vector<SettlementEvent> events;
  for (std::size_t i = 0; i < count; ++i) {
    auto id = "t" + std::to_string(i);
    events.push_back({"pay" + id,
                      "a",
                      id,
                      id,
                      at(110000 + i),
                      at(250000),
                      i + 1,
                      count,
                      take(Money::parse(i % 2 ? "10.99" : "-10.01", usd)),
                      {SourceRecordId{"bank" + id}},
                      std::nullopt});
  }
  PolicyRevision revision{
      "p",    "entity", "entity",           "benchmark",           PolicyScope::legal_entity,
      at(-1), 1,        LotSelection::lifo, RecognitionBasis::cash};
  auto policy = take(
      resolve_policy({"book", "entity", "custody", "a"}, std::span{&revision, 1}, at(300000), 1));
  auto start = std::chrono::steady_clock::now();
  auto report = project_trade_accounting(
      ledger, std::span{&policy, 1}, events, {at(300000), at(300000), count, count, "benchmark/1"},
      {ChargeTreatment::capitalize_opening, CashRecognition::both_legs});
  if (!report) {
    std::cerr << report.error().code << '\n';
    return 1;
  }
  auto end = std::chrono::steady_clock::now();
  auto controls = take(reconcile_trade_accounting(ledger.resolve(at(300000), at(300000)), *report));
  for (const auto &c : controls)
    if (c.difference.scaled_value())
      return 1;
  std::cout << "{\"trades\":" << count << ",\"settlements\":" << events.size()
            << ",\"journals\":" << report->journals.size()
            << ",\"reliefs\":" << report->lots.reliefs.size() << ",\"projection_ms\":"
            << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() << "}\n";
}
