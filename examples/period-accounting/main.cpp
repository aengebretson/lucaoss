#include <cstdlib>
#include <iostream>
#include <luca/accounting/period_pnl_rows.hpp>
using namespace luca;
using namespace luca::accounting;
template <class T> auto take(T value) {
  if (!value)
    std::abort();
  return *value;
}
int main() {
  const auto usd = take(Currency::from_code("USD"));
  auto money = [&](std::string_view text) { return take(Money::parse(text, usd)); };
  auto at = [](int n) {
    return Timestamp{std::chrono::sys_days{std::chrono::year{2026} / 1 / 1}} + std::chrono::days{n};
  };
  auto proof = [](std::string id) {
    return take(Provenance::create({SourceRecordId{"source:" + id}}, "period-walkthrough", "1"));
  };
  auto header_for = [&](std::string id, int n) {
    return take(EventHeader::create(EventId{id}, AccountId{"a"}, at(n), proof(id)));
  };
  auto make_trade = [&](std::string id, std::string q, std::string p, std::string charge, int n) {
    return take(EquityTrade::create(
        header_for(id, n), InstrumentId{"XYZ"}, take(Quantity::parse(q)), take(Price::parse(p)),
        usd, take(SettlementDate::create(std::chrono::year{2026} / 1 / 31)),
        TradeCharges{{"c", "commission", "broker", "", "reported", money(charge)}}));
  };
  LifecycleLedger ledger;
  take(ledger.accept(LifecycleRecordDraft::originate(
      EconomicEventId{"capital"}, at(0),
      EconomicEvent{CashMovement::create(header_for("capital", 0), money("1000"))})));
  take(ledger.accept(LifecycleRecordDraft::originate(
      EconomicEventId{"buy"}, at(1), EconomicEvent{make_trade("buy", "10", "10", "2", 1)})));
  take(ledger.accept(LifecycleRecordDraft::originate(
      EconomicEventId{"sale"}, at(2), EconomicEvent{make_trade("sale", "-4", "15", "1", 2)})));
  std::vector<SettlementEvent> cash{{"bank-buy",
                                     "a",
                                     "buy",
                                     "buy",
                                     at(3),
                                     at(3),
                                     1,
                                     3,
                                     money("-102"),
                                     {SourceRecordId{"bank:buy"}},
                                     {}},
                                    {"bank-sale",
                                     "a",
                                     "sale",
                                     "sale",
                                     at(4),
                                     at(4),
                                     2,
                                     3,
                                     money("59"),
                                     {SourceRecordId{"bank:sale"}},
                                     {}}};
  std::vector<PolicyTarget> bindings{{"book", "entity", "custody", "a"}};
  std::vector<KnownPolicyRevision> policies{
      {{"policy", "entity", "entity", "operator", PolicyScope::legal_entity, at(-1), 1,
        LotSelection::lifo, RecognitionBasis::cash},
       at(-1)}};
  auto report = [&](int from, int until, std::optional<ClosedPeriodAnchor> closed = {}) {
    return take(project_period_accounting(
        ledger, bindings, policies, cash,
        {at(from), at(until), at(20), ledger.size(), policies.size(), cash.size(), "UTC",
         std::string(period_chart_version), CorrectionRecognition::recorded_time},
        {ChargeTreatment::capitalize_opening, CashRecognition::both_legs}, closed));
  };
  auto first = report(0, 10);
  auto retained = retain_period_anchor(first);
  if (first.pnl.size() != 1 || first.pnl[0].movement.recognized != money("18.2"))
    return 1;
  take(ledger.accept(
      LifecycleRecordDraft::correct(EconomicEventId{"buy"}, EventId{"buy"}, at(11),
                                    EconomicEvent{make_trade("buy-v2", "10", "11", "2", 1)})));
  auto correction = report(10, 12, retained);
  if (correction.pnl[0].movement.economic != money("-4"))
    return 2;
  for (const auto &row : correction.balances)
    if (take(row.opening.add(row.movement)) != row.closing)
      return 3;
  auto rows = take(select_period_pnl(correction, {"a", "XYZ"}));
  if (rows.rows.size() != 1 || rows.totals != correction.pnl)
    return 4;
  std::cout << "period_pnl_scaled=" << first.pnl[0].movement.recognized.scaled_value()
            << " correction_economic_scaled=" << correction.pnl[0].movement.economic.scaled_value()
            << " currency=USD\n";
}
