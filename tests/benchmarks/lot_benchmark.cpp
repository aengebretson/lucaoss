#include "luca/accounting/lots.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
using namespace luca;
using namespace luca::accounting;
template <class T> auto need(T value) {
  if (!value)
    std::abort();
  return *value;
}
void run(const char *name, int count, int pools, bool deep_short) {
  const auto currency = need(Currency::from_code("USD"));
  const auto date = need(SettlementDate::create(std::chrono::year{2026} / 10 / 23));
  std::vector<LotAccount> accounts;
  for (int i = 0; i < pools; ++i)
    accounts.push_back({std::to_string(i), "book", "firm", std::to_string(i), "LIFO/1"});
  LifecycleLedger ledger;
  const auto begin = std::chrono::steady_clock::now();
  for (int i = 0; i < count; ++i) {
    int side = deep_short ? (i < count / 2 ? -1 : 1)
                          : ((i / pools) % 8 >= 2 && (i / pools) % 8 < 6 ? -1 : 1);
    const auto id = std::to_string(i);
    auto header =
        need(EventHeader::create(EventId{id}, AccountId{std::to_string(i % pools)},
                                 Timestamp{std::chrono::seconds{1700000000 + i}},
                                 need(Provenance::create({SourceRecordId{id}}, "benchmark", "1"))));
    auto trade = need(
        EquityTrade::create(header, InstrumentId{"XYZ"}, Quantity::from_scaled(side * 100000000),
                            Price::from_scaled(10000000000), currency, date,
                            TradeCharges{{"broker", "commission", "broker", "", "reported",
                                          Money::from_scaled(5000, currency)},
                                         {"exchange", "exchange", "exchange", "", "reported",
                                          Money::from_scaled(100, currency)}}));
    need(ledger.accept(LifecycleRecordDraft::originate(
        EconomicEventId{id}, Timestamp{std::chrono::seconds{1800000000}}, EconomicEvent{trade})));
  }
  const auto admitted = std::chrono::steady_clock::now();
  auto resolution = ledger.resolve(Timestamp::max(), Timestamp::max());
  const auto resolved = std::chrono::steady_clock::now();
  auto report = need(project_lifo_lots(resolution, accounts));
  const auto projected = std::chrono::steady_clock::now();
  if (report.reliefs.size() != static_cast<std::size_t>(count / 2))
    std::abort();
  auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
  std::cout << name << " trades=" << count << " pools=" << pools
            << " admit_ms=" << ms(begin, admitted) << " resolve_ms=" << ms(admitted, resolved)
            << " fold_ms=" << ms(resolved, projected) << " reliefs=" << report.reliefs.size()
            << '\n';
}
int main() {
  run("long_short_crossing", 100000, 2, false);
  run("many_custodians", 104000, 1000, false);
  run("deep_short_inventory", 100000, 1, true);
}
