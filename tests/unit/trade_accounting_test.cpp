#include "luca/adapters/transaction.hpp"
#include "luca/reconciliation/trade_accounting.hpp"
#include <fstream>
#include <iostream>
#include <source_location>
#include <sstream>
using namespace luca;
using namespace luca::accounting;
void check(bool value, std::source_location at = std::source_location::current()) {
  if (!value) {
    std::cerr << "failed " << at.line() << '\n';
    std::abort();
  }
}
template <class T> auto take(T value, std::source_location at = std::source_location::current()) {
  check(bool(value), at);
  return *value;
}
const auto usd = take(Currency::from_code("USD"));
Money money(std::string_view s) { return take(Money::parse(s, usd)); }
std::string decimal(Money m) {
  auto n = trade_accounting_detail::magnitude(m.scaled_value());
  auto f = std::to_string(n % 1000000);
  return (m.scaled_value() < 0 ? "-" : "") + std::to_string(n / 1000000) + "." +
         std::string(6 - f.size(), '0') + f;
}
Timestamp stamp(int n) { return Timestamp{std::chrono::seconds{1700000000 + n}}; }
Provenance proof(std::string id) {
  return take(Provenance::create({SourceRecordId{"source:" + id}}, "fixture", "1"));
}
EquityTrade trade(std::string id, std::string q, std::string p, std::string c, std::string f,
                  int n) {
  auto h = take(EventHeader::create(EventId{id}, AccountId{"a"}, stamp(n), proof(id)));
  return take(
      EquityTrade::create(h, InstrumentId{"XYZ"}, take(Quantity::parse(q)), take(Price::parse(p)),
                          usd, take(SettlementDate::create(std::chrono::year{2023} / 11 / 15)),
                          TradeCharges{{"c", "commission", "broker", "", "reported", money(c)},
                                       {"f", "exchange", "exchange", "", "reported", money(f)}}));
}
std::vector<ResolvedPolicy> policies(RecognitionBasis basis, Timestamp at) {
  PolicyRevision r{"policy-1", "entity", "entity",           "test", PolicyScope::legal_entity,
                   stamp(-1),  1,        LotSelection::lifo, basis};
  return {take(resolve_policy({"book", "entity", "custody", "a"}, std::span{&r, 1}, at, 1))};
}
AccountingReport report(const LifecycleLedger &ledger, const std::vector<SettlementEvent> &events,
                        RecognitionBasis basis = RecognitionBasis::accrual, int n = 1000) {
  auto p = policies(basis, stamp(n));
  auto result = project_trade_accounting(
      ledger, p, events,
      {stamp(2000), stamp(n), ledger.records().size(), events.size(), "fixture/1"},
      {ChargeTreatment::capitalize_opening, CashRecognition::both_legs});
  if (!result) {
    std::cerr << result.error().code << ' ' << result.error().record_id << '\n';
    std::abort();
  }
  return *result;
}
Money balance(const AccountingReport &r, std::string account) {
  for (const auto &b : r.trial_balance.balances())
    if (b.key.account.value() == account)
      return b.net;
  return money("0");
}
void settle(std::vector<SettlementEvent> &events, const LifecycleLedger &ledger, std::string id,
            std::string record, std::string cash, int n, std::optional<std::string> reverses = {}) {
  const auto *t = ledger.find(EventId{record});
  check(t != nullptr);
  SettlementEvent e{id,
                    "a",
                    record,
                    t->economic_event_id().value(),
                    stamp(n),
                    stamp(200),
                    events.size() + 1,
                    ledger.records().size(),
                    money(cash),
                    {SourceRecordId{"bank:" + id}},
                    reverses};
  auto valid = validate_settlement(ledger, events, e);
  if (!valid)
    std::cerr << valid.error().code << '\n';
  check(bool(valid));
  events.push_back(e);
}
int main() {
  LifecycleLedger ledger;
  auto h =
      take(EventHeader::create(EventId{"deposit"}, AccountId{"a"}, stamp(0), proof("deposit")));
  take(ledger.accept(
      LifecycleRecordDraft::originate(EconomicEventId{"deposit"}, stamp(100),
                                      EconomicEvent{CashMovement::create(h, money("10000"))})));
  std::ifstream input(LUCA_TRADE_BOOK);
  check(input.good());
  std::string line;
  int n = 1;
  std::vector<SettlementEvent> events;
  while (std::getline(input, line)) {
    if (line.empty() || line.front() == '#')
      continue;
    std::istringstream in(line);
    std::string id, q, p, c, f, position, pnl, cash;
    check(bool(in >> id >> q >> p >> c >> f >> position >> pnl >> cash));
    auto t = trade(id, q, p, c, f, n++);
    take(ledger.accept(
        LifecycleRecordDraft::originate(EconomicEventId{id}, stamp(100), EconomicEvent{t})));
    auto before = report(ledger, events);
    if (events.empty())
      check(balance(before, "asset.cash") == money("10000"));
    settle(events, ledger, "settle:" + id, id, decimal(*trade_cash(t)), 100 + n);
    auto r = report(ledger, events);
    check(r.breaks.empty());
    check(balance(r, "asset.cash") == money(cash));
    check(balance(r, "income.realized-pnl") == take(money("0").subtract(money(pnl))));
    auto pos = Quantity::from_scaled(0);
    for (const auto &lot : r.lots.open_lots)
      pos = take(pos.add(Quantity::from_scaled(lot.remaining_quantity.scaled_value() *
                                               (lot.short_position ? -1 : 1))));
    check(pos == take(Quantity::parse(position)));
    for (const auto &o : r.obligations)
      check(o.outstanding_cash == money("0"));
    for (const auto &j : r.journals)
      check(j.debit_total() == j.credit_total());
    for (const auto &control :
         take(reconcile_trade_accounting(ledger.resolve(stamp(2000), stamp(1000)), r)))
      check(control.difference == money("0"));
    check(r == report(ledger, events));
    check(balance(report(ledger, events, RecognitionBasis::cash), "income.realized-pnl") ==
          balance(r, "income.realized-pnl"));
  }
  check(balance(report(ledger, {}), "asset.cash") ==
        money("10000")); // even well after scheduled settlement
  std::vector<SettlementEvent> partial;
  settle(partial, ledger, "partial-buy", "buy-a", "-51", 20);
  settle(partial, ledger, "partial-sale", "sale-c", "59.5", 21);
  auto r = report(ledger, partial);
  check(balance(r, "asset.cash") == money("10008.5"));
  check(r.obligations[0].outstanding_cash == money("-51"));
  auto cash = report(ledger, partial, RecognitionBasis::cash);
  check(balance(cash, "income.realized-pnl") == money("-7.0125"));
  check(balance(cash, "control.deferred-realized-pnl") == money("-55.2875"));
  for (const auto &j : cash.journals)
    if (j.journal_entry_id().value().starts_with("settlement:") ||
        j.journal_entry_id().value().starts_with("recognition:"))
      check(j.recorded_at() == stamp(200));
  auto knowledge_regression = partial;
  knowledge_regression.back().recorded_at = stamp(150);
  check(!validate_settlement_history(ledger, knowledge_regression));
  auto knowledge_policies = policies(RecognitionBasis::cash, stamp(1000));
  auto before_cash = project_trade_accounting(
      ledger, knowledge_policies, partial,
      {stamp(175), stamp(1000), ledger.records().size(), partial.size(), "m"},
      {ChargeTreatment::capitalize_opening, CashRecognition::both_legs});
  check(bool(before_cash));
  check(balance(*before_cash, "asset.cash") == money("10000"));
  auto malformed = partial;
  malformed.back().reverses = "missing";
  auto malformed_policies = policies(RecognitionBasis::cash, stamp(1000));
  auto invalid_report = project_trade_accounting(
      ledger, malformed_policies, malformed,
      {stamp(2000), stamp(1000), ledger.records().size(), malformed.size(), "m"},
      {ChargeTreatment::capitalize_opening, CashRecognition::both_legs});
  check(!invalid_report);
  malformed = partial;
  malformed.back().sequence = 3;
  check(
      !project_trade_accounting(ledger, malformed_policies, malformed,
                                {stamp(2000), stamp(1000), ledger.records().size(), 3, "m"},
                                {ChargeTreatment::capitalize_opening, CashRecognition::both_legs}));
  settle(partial, ledger, "refund", "buy-a", "51", 22, "partial-buy");
  check(balance(report(ledger, partial), "asset.cash") == money("10059.5"));
  check(balance(report(ledger, partial, RecognitionBasis::cash), "income.realized-pnl") ==
        money("0"));
  auto invalid = partial.front();
  invalid.id = "over";
  invalid.sequence = partial.size() + 1;
  invalid.cash = money("-1000");
  check(!validate_settlement(ledger, partial, invalid));
  // Correction changes lots/obligations while immutable actual cash survives.
  auto corrected = trade("buy-a-v2", "10", "11", "1", "1", 1);
  take(ledger.accept(LifecycleRecordDraft::correct(EconomicEventId{"buy-a"}, EventId{"buy-a"},
                                                   stamp(300), EconomicEvent{corrected})));
  auto revised = report(ledger, events);
  check(balance(revised, "asset.cash") == money("10053.1"));
  check(revised.obligations[0].outstanding_cash == money("-10"));
  bool original = false, replacement = false;
  for (const auto &j : revised.journals)
    for (const auto &id : j.lineage().record_ids()) {
      original = original || id.value() == "buy-a";
      replacement = replacement || id.value() == "buy-a-v2";
    }
  check(original && replacement);
  take(ledger.accept(LifecycleRecordDraft::cancel(EventId{"void-cover"}, EconomicEventId{"cover-f"},
                                                  EventId{"cover-f"}, AccountId{"a"}, stamp(301),
                                                  proof("void-cover"), stamp(500))));
  auto before_void = report(ledger, events, RecognitionBasis::accrual, 499);
  check(before_void.breaks.empty());
  auto after_void = report(ledger, events);
  check(after_void.breaks.size() == 1);
  check(after_void.breaks[0].code == "SETTLEMENT_ON_VOIDED_TRADE");
  check(balance(after_void, "asset.cash") == money("10053.1"));
  auto p = policies(RecognitionBasis::unspecified, stamp(1000));
  check(!project_trade_accounting(
      ledger, p, events, {stamp(2000), stamp(1000), ledger.records().size(), events.size(), "m"},
      {ChargeTreatment::capitalize_opening, CashRecognition::both_legs}));
  // Opposite-direction cash after correction never counts as payment.
  LifecycleLedger direction;
  auto a = trade("direction-buy", "1", "10", "0", "0", 1);
  auto z = trade("direction-sale", "-1", "20", "0", "0", 2);
  take(direction.accept(LifecycleRecordDraft::originate(EconomicEventId{"direction-buy"},
                                                        stamp(100), EconomicEvent{a})));
  take(direction.accept(LifecycleRecordDraft::originate(EconomicEventId{"direction-sale"},
                                                        stamp(100), EconomicEvent{z})));
  std::vector<SettlementEvent> paid;
  settle(paid, direction, "paid-buy", "direction-buy", "-10", 20);
  settle(paid, direction, "paid-sale", "direction-sale", "20", 21);
  auto cost = trade("direction-sale-v2", "-1", "20", "30", "0", 2);
  take(direction.accept(LifecycleRecordDraft::correct(EconomicEventId{"direction-sale"},
                                                      EventId{"direction-sale"}, stamp(300),
                                                      EconomicEvent{cost})));
  auto changed = report(direction, paid, RecognitionBasis::cash);
  check(balance(changed, "income.realized-pnl") == money("0"));
  check(balance(changed, "control.deferred-realized-pnl") == money("20"));
  check(changed.breaks.size() == 1);
  auto policy_a = policies(RecognitionBasis::accrual, stamp(1000));
  auto policy_b = policy_a.front();
  policy_b.target.book = "other";
  std::vector<ResolvedPolicy> mixed{policy_a.front(), policy_b};
  check(!project_trade_accounting(
      direction, mixed, paid,
      {stamp(2000), stamp(1000), direction.records().size(), paid.size(), "m"},
      {ChargeTreatment::capitalize_opening, CashRecognition::both_legs}));
  LifecycleLedger withdrawal;
  auto capital_header = take(
      EventHeader::create(EventId{"withdrawal"}, AccountId{"a"}, stamp(0), proof("withdrawal")));
  take(withdrawal.accept(LifecycleRecordDraft::originate(
      EconomicEventId{"withdrawal"}, stamp(100),
      EconomicEvent{CashMovement::create(capital_header, money("-1"))})));
  auto unsupported =
      project_trade_accounting(withdrawal, policy_a, {}, {stamp(2000), stamp(1000), 1, 0, "m"},
                               {ChargeTreatment::capitalize_opening, CashRecognition::both_legs});
  check(!unsupported && unsupported.error().code == "CASH_CONTRIBUTION_REQUIRED");
  std::puts("trade accounting deterministic book passed");
}
