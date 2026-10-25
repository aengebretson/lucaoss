#include "luca/portfolio.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>

using namespace luca;

namespace {
const Currency usd = *Currency::from_code("USD");
const Currency eur = *Currency::from_code("EUR");
constexpr Timestamp t1{std::chrono::nanoseconds{1}};
constexpr Timestamp t2{std::chrono::nanoseconds{2}};
constexpr Timestamp t3{std::chrono::nanoseconds{3}};

std::chrono::year_month_day date(unsigned month, unsigned day) {
  return std::chrono::year{2026} / std::chrono::month{month} / std::chrono::day{day};
}
Provenance provenance(const char* id) {
  auto value = Provenance::create({SourceRecordId{id}}, "settlement.fixture", "1");
  assert(value);
  return *value;
}
EventHeader header_for(const char* id, const char* account, Timestamp time) {
  auto value = EventHeader::create(EventId{id}, AccountId{account}, time, provenance(id));
  assert(value);
  return *value;
}
EconomicEvent trade(const char* id, const char* account, const char* quantity,
                    const char* price, Currency currency, Timestamp time,
                    std::chrono::year_month_day settlement) {
  auto value = EquityTrade::create(
      header_for(id, account, time), InstrumentId{"AAPL"}, *Quantity::parse(quantity),
      *Price::parse(price), currency, *SettlementDate::create(settlement));
  assert(value);
  return *value;
}
SettlementProjectionContext context(Timestamp as_of, unsigned month, unsigned day) {
  return {.as_of = as_of, .settlement_as_of_date = date(month, day)};
}
void expect_one(const std::expected<std::vector<SettlementObligation>,
                                    SettlementProjectionError>& result,
                SettlementDirection direction, const char* amount) {
  assert(result && result->size() == 1);
  assert(result->front().key().direction() == direction);
  assert(result->front().amount() == *Money::parse(amount, usd));
  assert(result->front().amount().scaled_value() > 0);
}
}  // namespace

int main() {
  Ledger empty;
  const auto invalid_context = project_settlement_obligations(empty.entries(), {t3, date(2, 30)});
  assert(!invalid_context && invalid_context.error() == SettlementProjectionError::invalid_context);
  assert(project_settlement_obligations(empty.entries(), context(t3, 1, 1))->empty());

  Ledger cash;
  assert(cash.append(EconomicEvent{CashMovement::create(
      header_for("deposit", "fund-a", t1), *Money::parse("100", usd))}));
  assert(project_settlement_obligations(cash.entries(), context(t3, 1, 1))->empty());

  // Buy, sell, effective-time, and settlement-date boundary semantics.
  Ledger buy;
  assert(buy.append(trade("buy", "fund-a", "1000", "200", usd, t2, date(1, 8))));
  expect_one(project_settlement_obligations(buy.entries(), context(t2, 1, 7)),
             SettlementDirection::payable, "200000");
  assert(project_settlement_obligations(buy.entries(), context(t2, 1, 8))->empty());
  assert(project_settlement_obligations(buy.entries(), context(t3, 1, 9))->empty());
  assert(project_settlement_obligations(buy.entries(), context(t1, 1, 7))->empty());

  Ledger sell;
  assert(sell.append(trade("sell", "fund-a", "-400", "250", usd, t2, date(2, 12))));
  expect_one(project_settlement_obligations(sell.entries(), context(t2, 2, 11)),
             SettlementDirection::receivable, "100000");
  assert(project_settlement_obligations(sell.entries(), context(t3, 2, 12))->empty());

  // Same-key aggregation retains separate directions. Other key dimensions remain separate.
  Ledger aggregate;
  assert(aggregate.append(trade("buy-1", "fund-b", "2", "10", usd, t2, date(8, 13))));
  assert(aggregate.append(trade("sell-1", "fund-a", "-4", "20", usd, t1, date(8, 12))));
  assert(aggregate.append(trade("buy-2", "fund-a", "3", "10", usd, t2, date(8, 12))));
  assert(aggregate.append(trade("buy-3", "fund-a", "7", "10", usd, t1, date(8, 12))));
  assert(aggregate.append(trade("sell-2", "fund-a", "-2", "10", usd, t2, date(8, 12))));
  assert(aggregate.append(trade("eur", "fund-a", "5", "10", eur, t2, date(8, 12))));
  assert(aggregate.append(trade("later", "fund-a", "1", "10", usd, t2, date(8, 13))));
  const auto obligations = project_settlement_obligations(aggregate.entries(), context(t3, 8, 11));
  assert(obligations && obligations->size() == 5);
  // account, date, currency, direction ordering (receivable precedes payable).
  assert((*obligations)[0].key().currency() == eur);
  assert((*obligations)[1].key() == SettlementObligationKey(
      AccountId{"fund-a"}, *SettlementDate::create(date(8, 12)), usd,
      SettlementDirection::receivable));
  assert((*obligations)[1].amount() == *Money::parse("100", usd));
  assert((*obligations)[2].key().direction() == SettlementDirection::payable);
  assert((*obligations)[2].amount() == *Money::parse("100", usd));
  assert((*obligations)[3].key().settlement_date() == *SettlementDate::create(date(8, 13)));
  assert((*obligations)[4].key().account() == AccountId{"fund-b"});

  // Out-of-order append and equal timestamps are selected and ordered by the ledger helper.
  Ledger history;
  assert(history.append(trade("late", "fund-a", "1", "3", usd, t2, date(9, 1))));
  assert(history.append(trade("early", "fund-a", "1", "2", usd, t1, date(9, 1))));
  assert(history.append(trade("tied", "fund-a", "1", "5", usd, t2, date(9, 1))));
  expect_one(project_settlement_obligations(history.entries(), context(t3, 8, 31)),
             SettlementDirection::payable, "10");

  Ledger valuation;
  assert(valuation.append(trade("huge", "fund-a", "92233720368.54775807",
                                "92233720368.54775807", usd, t1, date(9, 1))));
  const auto valuation_error =
      project_settlement_obligations(valuation.entries(), context(t3, 8, 31));
  assert(!valuation_error && valuation_error.error() ==
      SettlementProjectionError::valuation_overflow);

  Ledger overflow;
  assert(overflow.append(trade("max", "fund-a", "92233720368.54775807", "100",
                               usd, t1, date(9, 1))));
  assert(overflow.append(trade("one", "fund-a", "0.000001", "1",
                               usd, t2, date(9, 1))));
  const auto amount_error =
      project_settlement_obligations(overflow.entries(), context(t3, 8, 31));
  assert(!amount_error && amount_error.error() ==
      SettlementProjectionError::amount_overflow);

  // Trade lifecycle: position exists, settled cash is unchanged, and the payable closes.
  Ledger lifecycle;
  assert(lifecycle.append(EconomicEvent{CashMovement::create(
      header_for("opening", "fund-a", t1), *Money::parse("500000", usd))}));
  assert(lifecycle.append(trade("life-buy", "fund-a", "200", "300", usd,
                                t2, date(3, 18))));
  assert(project_positions(lifecycle.entries(), t3)->front().quantity() ==
         *Quantity::parse("200"));
  assert(project_cash(lifecycle.entries(), CashProjectionContext{
      .as_of = t3, .settlement_as_of_date = date(3, 17)})->front().amount() ==
         *Money::parse("500000", usd));
  expect_one(project_settlement_obligations(lifecycle.entries(), context(t3, 3, 17)),
             SettlementDirection::payable, "60000");
  assert(project_settlement_obligations(lifecycle.entries(), context(t3, 3, 18))->empty());
}
