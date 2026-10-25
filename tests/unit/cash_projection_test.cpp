#include "luca/portfolio.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>
#include <type_traits>

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
  auto result = Provenance::create({SourceRecordId{id}}, "cash.fixture", "1");
  assert(result);
  return *result;
}

EventHeader header_for(const char* id, const char* account, Timestamp time) {
  auto result = EventHeader::create(EventId{id}, AccountId{account}, time, provenance(id));
  assert(result);
  return *result;
}

EconomicEvent movement(const char* id, const char* account, const char* amount,
                       Currency currency, Timestamp time) {
  return CashMovement::create(header_for(id, account, time),
                              *Money::parse(amount, currency));
}

EconomicEvent trade(const char* id, const char* account, const char* quantity,
                    const char* price, Currency currency, Timestamp time,
                    std::chrono::year_month_day settlement) {
  auto result = EquityTrade::create(
      header_for(id, account, time), InstrumentId{"AAPL"}, *Quantity::parse(quantity),
      *Price::parse(price), currency, *SettlementDate::create(settlement));
  assert(result);
  return *result;
}

const CashBalance& only(const std::expected<std::vector<CashBalance>, CashProjectionError>& result) {
  assert(result && result->size() == 1);
  return result->front();
}

void expect_amount(const std::expected<std::vector<CashBalance>, CashProjectionError>& result,
                   const char* amount, Currency currency = usd) {
  assert(only(result).amount() == *Money::parse(amount, currency));
}

}  // namespace

int main() {
  static_assert(std::is_same_v<decltype(std::declval<const CashBalance&>().key()),
                               const CashKey&>);
  static_assert(std::is_same_v<decltype(std::declval<const CashBalance&>().amount()), Money>);

  Ledger empty;
  const auto invalid_context = project_cash(empty.entries(), {t3, date(2, 30)});
  assert(!invalid_context && invalid_context.error() == CashProjectionError::invalid_context);
  assert(project_cash(empty.entries(), CashProjectionContext{
      .as_of = t3,
      .settlement_as_of_date = date(12, 31)})->empty());

  Ledger movements;
  assert(movements.append(movement("deposit", "fund-a", "1000000", usd, t1)));
  assert(movements.append(movement("withdrawal", "fund-a", "-50000", usd, t2)));
  expect_amount(project_cash(movements.entries(), CashProjectionContext{
      .as_of = t2,
      .settlement_as_of_date = date(1, 1)}), "950000");
  expect_amount(project_cash(movements.entries(), CashProjectionContext{
      .as_of = t1,
      .settlement_as_of_date = date(1, 1)}), "1000000");

  Ledger separated;
  assert(separated.append(movement("b-usd", "fund-b", "20", usd, t1)));
  assert(separated.append(movement("a-eur", "fund-a", "30", eur, t1)));
  assert(separated.append(movement("a-usd", "fund-a", "10", usd, t1)));
  const auto ordered = project_cash(separated.entries(), CashProjectionContext{
      .as_of = t3,
      .settlement_as_of_date = date(1, 1)});
  assert(ordered && ordered->size() == 3);
  assert((*ordered)[0].key() == CashKey(AccountId{"fund-a"}, eur));
  assert((*ordered)[1].key() == CashKey(AccountId{"fund-a"}, usd));
  assert((*ordered)[2].key() == CashKey(AccountId{"fund-b"}, usd));

  Ledger zero;
  assert(zero.append(movement("plus", "fund-a", "10", usd, t1)));
  assert(zero.append(movement("minus", "fund-a", "-10", usd, t2)));
  assert(project_cash(zero.entries(), CashProjectionContext{
      .as_of = t3,
      .settlement_as_of_date = date(1, 1)})->empty());

  // Equity-buy conformance semantics and settlement-date boundaries.
  Ledger buy;
  assert(buy.append(movement("opening", "fund-a", "1000000", usd, t1)));
  assert(buy.append(trade("buy", "fund-a", "1000", "200", usd, t2, date(1, 8))));
  expect_amount(project_cash(buy.entries(), CashProjectionContext{
      .as_of = t3,
      .settlement_as_of_date = date(1, 7)}), "1000000");
  expect_amount(project_cash(buy.entries(), CashProjectionContext{
      .as_of = t3,
      .settlement_as_of_date = date(1, 8)}), "800000");
  expect_amount(project_cash(buy.entries(), CashProjectionContext{
      .as_of = t3,
      .settlement_as_of_date = date(1, 9)}), "800000");

  // Equity-sell and trade-settlement-lifecycle conformance semantics.
  Ledger sell;
  assert(sell.append(movement("sell-opening", "fund-a", "100000", usd, t1)));
  assert(sell.append(trade("sell", "fund-a", "-400", "250", usd, t2, date(2, 12))));
  expect_amount(project_cash(sell.entries(), CashProjectionContext{
      .as_of = t3,
      .settlement_as_of_date = date(2, 11)}), "100000");
  expect_amount(project_cash(sell.entries(), CashProjectionContext{
      .as_of = t3,
      .settlement_as_of_date = date(2, 12)}), "200000");
  Ledger lifecycle;
  assert(lifecycle.append(movement("life-opening", "fund-a", "500000", usd, t1)));
  assert(lifecycle.append(trade("life-buy", "fund-a", "200", "300", usd, t2, date(3, 18))));
  expect_amount(project_cash(lifecycle.entries(), CashProjectionContext{
      .as_of = t3,
      .settlement_as_of_date = date(3, 17)}), "500000");
  expect_amount(project_cash(lifecycle.entries(), CashProjectionContext{
      .as_of = t3,
      .settlement_as_of_date = date(3, 18)}), "440000");

  // Settlement eligibility cannot bypass economic-time selection.
  Ledger future;
  assert(future.append(trade("future", "fund-a", "10", "2", usd, t3, date(1, 1))));
  assert(project_cash(future.entries(), CashProjectionContext{
      .as_of = t2,
      .settlement_as_of_date = date(12, 31)})->empty());

  // Out-of-order append history must replay the early offset before the maximum.
  Ledger out_of_order;
  assert(out_of_order.append(EconomicEvent{CashMovement::create(
      header_for("late-max", "fund-a", t2),
      Money::from_scaled(std::numeric_limits<std::int64_t>::max(), usd))}));
  assert(out_of_order.append(EconomicEvent{CashMovement::create(
      header_for("early-minus", "fund-a", t1), Money::from_scaled(-1, usd))}));
  assert(out_of_order.append(EconomicEvent{CashMovement::create(
      header_for("late-plus", "fund-a", t2), Money::from_scaled(1, usd))}));
  assert(only(project_cash(out_of_order.entries(), CashProjectionContext{
      .as_of = t3,
      .settlement_as_of_date = date(1, 1)})).amount() ==
         Money::from_scaled(std::numeric_limits<std::int64_t>::max(), usd));

  // Equal effective timestamps consume ledger sequence as the tie-breaker.
  Ledger tied;
  assert(tied.append(EconomicEvent{CashMovement::create(
      header_for("max", "fund-a", t2),
      Money::from_scaled(std::numeric_limits<std::int64_t>::max(), usd))}));
  assert(tied.append(EconomicEvent{CashMovement::create(
      header_for("overflow", "fund-a", t2), Money::from_scaled(1, usd))}));
  assert(tied.append(EconomicEvent{CashMovement::create(
      header_for("compensate", "fund-a", t2), Money::from_scaled(-1, usd))}));
  const auto amount_overflow = project_cash(tied.entries(), CashProjectionContext{
      .as_of = t3,
      .settlement_as_of_date = date(1, 1)});
  assert(!amount_overflow && amount_overflow.error() == CashProjectionError::amount_overflow);

  Ledger valuation;
  assert(valuation.append(trade("huge", "fund-a", "92233720368.54775807",
                                "92233720368.54775807", usd, t1, date(1, 1))));
  const auto valuation_overflow = project_cash(valuation.entries(), CashProjectionContext{
      .as_of = t3,
      .settlement_as_of_date = date(1, 1)});
  assert(!valuation_overflow &&
         valuation_overflow.error() == CashProjectionError::valuation_overflow);
}
