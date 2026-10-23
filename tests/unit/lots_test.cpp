#include "luca/accounting/charge_schedule.hpp"
#include "luca/accounting/lots.hpp"
#include "luca/adapters/transaction.hpp"
#include "luca/portfolio/lifecycle_projection.hpp"
#include "luca/serialization/canonical_decode.hpp"
#include <fstream>
#include <future>
#include <iostream>
#include <source_location>
#include <sstream>
using namespace luca;
using namespace luca::accounting;
void check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) {
    std::cerr << at.line() << " failed\n";
    std::abort();
  }
}
template <class T> auto require(T value) {
  check(bool(value));
  return *value;
}
const auto usd = require(Currency::from_code("USD"));
Money money(std::string_view s) { return require(Money::parse(s, usd)); }
Timestamp time_at(int n) { return Timestamp{std::chrono::seconds{1700000000 + n}}; }
TradeCharges charges(std::string commission, std::string fee) {
  return {{"broker", "commission", "fixture-broker", "", "reported", money(commission)},
          {"exchange", "exchange", "fixture-exchange", "", "reported", money(fee)}};
}
EquityTrade trade(std::string id, std::string q, std::string p, int n,
                  std::optional<TradeCharges> c, std::string account = "custody-a") {
  auto h = require(EventHeader::create(
      EventId{id}, AccountId{account}, time_at(n),
      require(Provenance::create({SourceRecordId{"source-" + id}}, "fixture", "1"))));
  return require(EquityTrade::create(
      h, InstrumentId{"XYZ"}, require(Quantity::parse(q)), require(Price::parse(p)), usd,
      require(SettlementDate::create(std::chrono::year{2026} / 10 / 23)), c));
}
void add(LifecycleLedger &ledger, EquityTrade t, std::string action = "originate",
         std::string target = "") {
  const auto id = t.header().id();
  auto draft = action == "correct"
                   ? LifecycleRecordDraft::correct(EconomicEventId{target}, EventId{target},
                                                   time_at(100), EconomicEvent{t})
                   : LifecycleRecordDraft::originate(EconomicEventId{id.value()}, time_at(100),
                                                     EconomicEvent{t});
  require(ledger.accept(draft));
}
std::vector<LotAccount> bindings{{"custody-a", "book", "firm", "bank-a", "LIFO/1"},
                                 {"custody-b", "book", "firm", "bank-b", "LIFO/1"}};
LotReport project(const LifecycleLedger &ledger) {
  return require(project_lifo_lots(ledger.resolve(Timestamp::max(), Timestamp::max()), bindings));
}
void fixtures() {
  LifecycleLedger ledger;
  std::ifstream file(LUCA_LOT_FIXTURE);
  check(file.good());
  std::string line;
  int n = 0;
  while (std::getline(file, line)) {
    if (line.empty() || line.front() == '#')
      continue;
    std::string id, q, p, c, f, position, principal, pnl;
    std::istringstream in(line);
    check(bool(in >> id >> q >> p >> c >> f >> position >> principal >> pnl));
    add(ledger, trade(id, q, p, n++, charges(c, f)));
    const auto result = project(ledger);
    auto pos = Quantity::from_scaled(0);
    auto basis = money("0");
    auto net = money("0");
    for (const auto &lot : result.open_lots) {
      pos = require(pos.add(Quantity::from_scaled(lot.remaining_quantity.scaled_value() *
                                                  (lot.short_position ? -1 : 1))));
      basis = require(basis.add(lot.remaining.principal));
    }
    for (const auto &relief : result.reliefs)
      net = require(net.add(relief.net_pnl));
    check(pos == require(Quantity::parse(position)));
    check(basis == money(principal));
    check(net == money(pnl));
    check(result == project(ledger));
    auto event = EconomicEvent{trade("roundtrip", q, p, n, charges(c, f))};
    LifecycleLedger roundtrip;
    add(roundtrip, std::get<EquityTrade>(event));
    auto encoded = serialization::canonical_bytes(roundtrip.records().front());
    auto decoded = require(serialization::decode_lifecycle_record(encoded));
    LifecycleLedger rebuilt;
    require(rebuilt.accept(decoded.draft));
    check(*rebuilt.records().front().event() == *roundtrip.records().front().event());
  }
  auto result = project(ledger);
  check(require(lot_carrying_amount(result.open_lots[0])) == money("25.12"));
  check(result.reliefs[0].opening_record == "buy-b");
  check(result.reliefs[1].opening_record == "buy-a");
  auto cash = project_lifecycle(ledger.resolve(Timestamp::max(), Timestamp::max()),
                                {Timestamp::max(), std::chrono::year{2026} / 10 / 30});
  check(cash.settled_cash && cash.settled_cash->size() == 1 &&
        cash.settled_cash->front().amount() == money("275.94"));
  add(ledger, trade("other-custody", "-3", "20", 10, TradeCharges{}, "custody-b"));
  result = project(ledger);
  check(result.open_lots.size() == 2);
  auto firm = require(firm_lot_balances(result));
  check(firm.size() == 1);
  check(firm[0].long_quantity == Quantity::from_scaled(100000000));
  check(firm[0].short_quantity == Quantity::from_scaled(300000000));
  // Independent pools can run in parallel and merge without changing relief.
  auto resolved = ledger.resolve(Timestamp::max(), Timestamp::max());
  std::vector<ResolvedLifecycleEvent> a, b;
  for (const auto &record : resolved.active_events()) {
    auto &partition = header(record.event()).account().value() == "custody-a" ? a : b;
    partition.push_back(record);
  }
  auto other =
      std::async(std::launch::async, [&] { return require(project_lifo_lots(b, bindings)); });
  check(require(project_lifo_lots(a, bindings)).reliefs == result.reliefs);
  check(other.get().open_lots.back() == result.open_lots.back());
  // Replacement of old effective-time input replays all affected relief.
  add(ledger, trade("buy-a-corrected", "10", "11", 0, charges("1", "0")), "correct", "buy-a");
  auto corrected = project(ledger);
  check(corrected.reliefs[1].opening_record == "buy-a-corrected");
  check(corrected.reliefs[1].net_pnl == money("131.425"));
  auto cancel = LifecycleRecordDraft::cancel(
      EventId{"cancel"}, EconomicEventId{"cover-cross"}, EventId{"cover-cross"},
      AccountId{"custody-a"}, time_at(101),
      require(Provenance::create({SourceRecordId{"cancel-source"}}, "fixture", "1")));
  require(ledger.accept(cancel));
  check(project(ledger).open_lots.front().short_position);
}
void edge_cases() {
  LifecycleLedger ledger;
  add(ledger, trade("tiny", "3", "0.000001", 0, charges("0.000001", "0.000001")));
  for (int i = 0; i < 3; ++i)
    add(ledger, trade("sell" + std::to_string(i), "-1", "0.000002", i + 1, TradeCharges{}));
  auto r = project(ledger);
  check(r.open_lots.empty());
  auto commissions = money("0"), fees = money("0");
  for (auto &relief : r.reliefs) {
    commissions = require(commissions.add(relief.opening.charges[0].amount));
    fees = require(fees.add(relief.opening.charges[1].amount));
  }
  check(commissions == money("0.000001") && fees == money("0.000001"));
  LifecycleLedger legacy;
  add(legacy, trade("legacy", "1", "1", 0, std::nullopt));
  check(!project(legacy).charges_complete);
  auto invalid = charges("1", "0");
  invalid.push_back(invalid.front());
  check(!validate_charges(invalid, usd));
  invalid = charges("-1", "0");
  check(!validate_charges(invalid, usd));
  invalid = charges("1", "0");
  invalid[0].amount = require(Money::parse("1", require(Currency::from_code("EUR"))));
  check(!validate_charges(invalid, usd));
  check(!project_lifo_lots(legacy.resolve(Timestamp::max(), Timestamp::max()), {}));
  LifecycleLedger shorts;
  add(shorts, trade("s1", "-3", "10", 0, TradeCharges{}));
  add(shorts, trade("s2", "-2", "20", 1, TradeCharges{}));
  add(shorts, trade("cover", "4", "5", 2, TradeCharges{}));
  auto s = project(shorts);
  check(s.reliefs[0].opening_record == "s2" && s.reliefs[1].opening_record == "s1");
  check(s.open_lots[0].remaining.principal == money("10"));
}
void additional_contracts() {
  LifecycleLedger history;
  add(history,trade("first","-1","10",0,TradeCharges{}));
  add(history,trade("second","-1","20",0,TradeCharges{}));
  add(history,trade("cover-tied","1","5",1,TradeCharges{}));
  check(project(history).reliefs.front().opening_record=="second");
  auto resolution=history.resolve(Timestamp::max(),Timestamp::max());
  std::vector<ResolvedLifecycleEvent> reversed(resolution.active_events().begin(),resolution.active_events().end());
  std::reverse(reversed.begin(),reversed.end());
  check(!project_lifo_lots(reversed,bindings));
  LifecycleLedger huge;
  add(huge,trade("overflow","92233720368","92233720368",0,TradeCharges{}));
  auto failed=project_lifo_lots(huge.resolve(Timestamp::max(),Timestamp::max()),bindings);
  check(!failed && failed.error().code=="LOT_AMOUNT_OVERFLOW");
  auto estimated=charges("1","0");estimated[0].state="scheduled";estimated[0].schedule_version="v1";
  LifecycleLedger provisional;add(provisional,trade("estimated","1","10",0,estimated));
  check(!project(provisional).charges_complete);
}
void schedules() {
  ChargeRule rule{"broker",
                  "v1",
                  "commission",
                  "broker",
                  time_at(0),
                  time_at(10),
                  usd,
                  ChargeBasis::per_share,
                  ChargeSide::both,
                  money("0"),
                  require(Price::parse("0.005")),
                  Rate::from_scaled(0),
                  money("0.01"),
                  money("100"),
                  10000,
                  RoundingMode::half_even};
  auto calc = [&](int q, int t) {
    return calculate_charges(Quantity::from_scaled(q * 100000000LL), require(Price::parse("10")),
                             usd, time_at(t), std::span{&rule, 1});
  };
  check(require(calc(3, 0))[0].amount == money("0.02"));
  check(require(calc(5, 9))[0].amount == money("0.02"));
  check(require(calc(5, 10)).empty());
  rule.side = ChargeSide::sell;
  check(require(calc(5, 0)).empty());
  check(require(calc(-5, 0)).size() == 1);
  rule.basis = ChargeBasis::notional;
  rule.notional_rate = require(Rate::parse("0.001"));
  check(require(calc(-5, 0))[0].amount == money("0.05"));
  rule.basis = ChargeBasis::flat;
  rule.flat = money("200");
  check(require(calc(-5, 0))[0].amount == money("100"));
  rule.quantum = 0;
  check(!calc(-5, 0));
  rule.quantum = 10000;
  rule.side = static_cast<ChargeSide>(99);
  check(!calc(-5, 0));
}
void dated_void() {
  LifecycleLedger ledger;
  add(ledger, trade("dated-buy", "5", "20", 0, charges("1", "0")));
  const auto provenance = require(Provenance::create({SourceRecordId{"void-evidence"}},"fixture","1"));
  auto draft = LifecycleRecordDraft::cancel(EventId{"dated-void"},EconomicEventId{"dated-buy"},
      EventId{"dated-buy"},AccountId{"custody-a"},time_at(110),provenance,time_at(10));
  require(ledger.accept(draft));
  auto before = require(project_lifo_lots(ledger.resolve(time_at(200),time_at(9)),bindings));
  check(before.open_lots.size()==1 && before.open_lots[0].remaining_quantity==require(Quantity::parse("5")));
  check(require(project_lifo_lots(ledger.resolve(time_at(200),time_at(10)),bindings)).open_lots.empty());
  check(require(project_lifo_lots(ledger.resolve(time_at(105),time_at(20)),bindings)).open_lots.size()==1);
  std::ifstream fixture(LUCA_DATED_VOID_FIXTURE);check(bool(fixture));
  std::string header;std::getline(fixture,header);
  int recorded,economic;std::size_t count;std::size_t queries=0;
  while(fixture>>recorded>>economic>>count) {
    check(require(project_lifo_lots(ledger.resolve(time_at(recorded),time_at(economic)),bindings)).open_lots.size()==count);
    ++queries;
  }
  check(queries==4);
  LifecycleLedger replay;
  for (const auto& record:ledger.records()) {
    auto decoded = require(serialization::decode_lifecycle_record(serialization::canonical_bytes(record)));
    require(replay.accept(decoded.draft));
  }
  check(require(project_lifo_lots(replay.resolve(time_at(200),time_at(9)),bindings))==before);
  check(replay.records().back().cancellation_at()==time_at(10));
  LifecycleLedger invalid; add(invalid,trade("invalid-buy","1","10",10,TradeCharges{}));
  check(!invalid.accept(LifecycleRecordDraft::cancel(EventId{"early-void"},EconomicEventId{"invalid-buy"},
       EventId{"invalid-buy"},AccountId{"custody-a"},time_at(110),provenance,time_at(9))));
}
int main() {
  {
    LifecycleLedger ledger;add(ledger,trade("policy-buy","2","10",1,TradeCharges{},"custody-a"));
    add(ledger,trade("policy-sell","-1","12",2,TradeCharges{},"custody-b"));
    PolicyRevision entity{"e","firm","firm","admin",PolicyScope::legal_entity,time_at(0),1,LotSelection::lifo,RecognitionBasis::unspecified};
    std::vector<ResolvedPolicy> policies;
    for(const auto* account:{"custody-a","custody-b"})
      policies.push_back(require(resolve_policy({"book","firm","bank-a",account},std::span{&entity,1},time_at(10),1)));
    check(policies[0].digest!=policies[1].digest);
    auto result=require(project_policy_lots(ledger.resolve(Timestamp::max(),Timestamp::max()),policies));
    check(result.reliefs.size()==1 && result.open_lots.size()==1 && result.resolved_policies.size()==2);
    check(result.reliefs[0].gross_pnl==money("2"));
  }
  dated_void();
  fixtures();
  edge_cases();
  schedules();
  additional_contracts();
  std::cout
      << "LIFO long/short, charges, replay, pool isolation, canonical v2 and schedules passed\n";
}
