#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

#include "luca/portfolio.hpp"
#include "luca/reconciliation.hpp"

using namespace luca;

namespace {
const Currency usd = *Currency::from_code("USD");
const Currency eur = *Currency::from_code("EUR");
constexpr Timestamp as_of{std::chrono::nanoseconds{100}};
constexpr auto settlement_as_of = std::chrono::year{2026} / 1 / 8;
constexpr CashReconciliationContext context{as_of, settlement_as_of};

Money money(const char *value, Currency currency = usd) {
  const auto result = Money::parse(value, currency);
  assert(result);
  return *result;
}

Provenance provenance(const char *id) {
  const auto result =
      Provenance::create({SourceRecordId{id}}, "custodian.cash", "1");
  assert(result);
  return *result;
}

CashBalance balance(const char *account, const char *amount,
                    Currency currency = usd) {
  return {AccountId{account}, money(amount, currency)};
}

CashObservation observation(
    const char *account, const char *amount, Currency currency = usd,
    Timestamp time = as_of,
    std::chrono::year_month_day settlement_date = settlement_as_of,
    const char *source = "observation") {
  const auto result =
      CashObservation::create(AccountId{account}, money(amount, currency), time,
                              settlement_date, provenance(source));
  assert(result);
  return *result;
}

EventHeader header(const char *id, Timestamp time) {
  const auto result = EventHeader::create(EventId{id}, AccountId{"fund-a"},
                                          time, provenance(id));
  assert(result);
  return *result;
}
}  // namespace

int main() {
  static_assert(!std::is_convertible_v<CashObservation, EconomicEvent>);
  assert(!CashObservation::create(AccountId{""}, money("1"), as_of,
                                  settlement_as_of, provenance("invalid")));
  assert(!CashObservation::create(AccountId{"fund-a"}, money("1"), as_of,
                                  std::chrono::year{2026} / 2 / 30,
                                  provenance("invalid-date")));

  const CashBalance expected = balance("fund-a", "1000000");
  const auto exact = observation("fund-a", "1000000");
  assert(reconcile_cash(std::span{&expected, 1}, std::span{&exact, 1}, context)
             ->empty());

  const std::vector<CashBalance> duplicate_expected{expected, expected};
  const auto duplicate_projection =
      reconcile_cash(duplicate_expected, std::span{&exact, 1}, context);
  assert(!duplicate_projection &&
         duplicate_projection.error() == CashReconciliationError::duplicate_projected_key);
  const auto duplicate_without_observations = reconcile_cash(duplicate_expected, {}, context);
  assert(!duplicate_without_observations && duplicate_without_observations.error() ==
                                                CashReconciliationError::duplicate_projected_key);
  const auto invalid_context = reconcile_cash({}, {}, {as_of, std::chrono::year{2026} / 2 / 30});
  assert(!invalid_context && invalid_context.error() == CashReconciliationError::invalid_context);

  const auto mismatch =
      observation("fund-a", "999950", usd, as_of, settlement_as_of, "mismatch");
  const auto mismatch_result =
      reconcile_cash(std::span{&expected, 1}, std::span{&mismatch, 1}, context);
  assert(mismatch_result && mismatch_result->size() == 1);
  const auto &mismatch_break = mismatch_result->front();
  assert(mismatch_break.kind() == CashBreakKind::amount_mismatch);
  assert(mismatch_break.expected() == money("1000000"));
  assert(mismatch_break.observed() == money("999950"));
  assert(mismatch_break.difference() == money("-50"));
  assert(mismatch_break.observation_provenance() == provenance("mismatch"));

  const std::vector<CashObservation> no_observations;
  const auto missing =
      reconcile_cash(std::span{&expected, 1}, no_observations, context);
  assert(missing &&
         missing->front().kind() == CashBreakKind::missing_observation);
  assert(missing->front().expected() == money("1000000"));
  assert(!missing->front().observed() && !missing->front().difference());
  assert(!missing->front().observation_provenance());

  const std::vector<CashBalance> no_expected;
  const auto zero =
      observation("fund-a", "0", usd, as_of, settlement_as_of, "zero");
  const auto unexpected =
      reconcile_cash(no_expected, std::span{&zero, 1}, context);
  assert(unexpected &&
         unexpected->front().kind() == CashBreakKind::unexpected_observation);
  assert(!unexpected->front().expected() &&
         unexpected->front().observed() == money("0"));
  assert(unexpected->front().observation_provenance() == provenance("zero"));

  const std::vector<CashBalance> separated_expected{
      balance("fund-a", "10", usd), balance("fund-a", "20", eur),
      balance("fund-b", "30", usd)};
  const std::vector<CashObservation> separated_observed{
      observation("fund-b", "31", usd), observation("fund-a", "20", eur),
      observation("fund-a", "10", usd)};
  const auto separated =
      reconcile_cash(separated_expected, separated_observed, context);
  assert(separated && separated->size() == 1);
  assert(separated->front().key() == CashKey(AccountId{"fund-b"}, usd));

  const std::vector<CashObservation> duplicates{observation("fund-a", "1"),
                                                observation("fund-a", "2")};
  const auto duplicate = reconcile_cash(no_expected, duplicates, context);
  assert(!duplicate &&
         duplicate.error() == CashReconciliationError::duplicate_observation);

  for (const auto time : {Timestamp{std::chrono::nanoseconds{99}},
                          Timestamp{std::chrono::nanoseconds{101}}}) {
    const auto wrong_time = observation("fund-a", "1", usd, time);
    const auto result =
        reconcile_cash(no_expected, std::span{&wrong_time, 1}, context);
    assert(!result && result.error() ==
                          CashReconciliationError::observation_time_mismatch);
  }
  for (const auto date :
       {std::chrono::year{2026} / 1 / 7, std::chrono::year{2026} / 1 / 9}) {
    const auto wrong_date = observation("fund-a", "1", usd, as_of, date);
    const auto result =
        reconcile_cash(no_expected, std::span{&wrong_date, 1}, context);
    assert(!result &&
           result.error() ==
               CashReconciliationError::observation_settlement_date_mismatch);
  }

  const CashBalance negative = balance("fund-a", "-100");
  const auto negative_exact = observation("fund-a", "-100");
  assert(reconcile_cash(std::span{&negative, 1}, std::span{&negative_exact, 1},
                        context)
             ->empty());
  const auto negative_observed = observation("fund-a", "-90");
  const auto negative_result = reconcile_cash(
      std::span{&negative, 1}, std::span{&negative_observed, 1}, context);
  assert(negative_result &&
         negative_result->front().difference() == money("10"));

  const CashBalance minimum{
      AccountId{"fund-a"},
      Money::from_scaled(std::numeric_limits<std::int64_t>::min(), usd)};
  const auto maximum = CashObservation::create(
      AccountId{"fund-a"},
      Money::from_scaled(std::numeric_limits<std::int64_t>::max(), usd), as_of,
      settlement_as_of, provenance("maximum"));
  assert(maximum);
  const auto overflow =
      reconcile_cash(std::span{&minimum, 1}, std::span{&*maximum, 1}, context);
  assert(!overflow &&
         overflow.error() == CashReconciliationError::amount_overflow);

  const std::vector<CashBalance> unordered_expected{
      balance("fund-z", "1", usd), balance("fund-a", "1", usd)};
  const std::vector<CashObservation> unordered_observed{
      observation("fund-b", "0", usd), observation("fund-a", "2", eur)};
  const auto ordered =
      reconcile_cash(unordered_expected, unordered_observed, context);
  assert(ordered && ordered->size() == 4);
  assert((*ordered)[0].key() == CashKey(AccountId{"fund-a"}, eur));
  assert((*ordered)[1].key() == CashKey(AccountId{"fund-a"}, usd));
  assert((*ordered)[2].key() == CashKey(AccountId{"fund-b"}, usd));
  assert((*ordered)[3].key() == CashKey(AccountId{"fund-z"}, usd));

  Ledger ledger;
  const auto opening = CashMovement::create(
      header("opening", Timestamp{std::chrono::nanoseconds{1}}),
      money("1000000"));
  assert(ledger.append(EconomicEvent{opening}));
  const auto buy = EquityTrade::create(
      header("buy", Timestamp{std::chrono::nanoseconds{2}}),
      InstrumentId{"AAPL"}, *Quantity::parse("1000"), *Price::parse("200"), usd,
      *SettlementDate::create(settlement_as_of));
  assert(buy && ledger.append(EconomicEvent{*buy}));
  const auto projected = project_cash(
      ledger.entries(),
      CashProjectionContext{.as_of = as_of,
                            .settlement_as_of_date = settlement_as_of});
  assert(projected && projected->front().amount() == money("800000"));
  const auto external = observation("fund-a", "799950", usd, as_of,
                                    settlement_as_of, "custodian-row");
  const auto integrated =
      reconcile_cash(*projected, std::span{&external, 1}, context);
  assert(integrated && integrated->size() == 1);
  assert(integrated->front().kind() == CashBreakKind::amount_mismatch);
  assert(integrated->front().difference() == money("-50"));
  assert(integrated->front().observation_provenance() ==
         provenance("custodian-row"));
}
