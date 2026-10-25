#include "luca/portfolio.hpp"
#include "luca/reconciliation.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

using namespace luca;

namespace {
constexpr Timestamp as_of{std::chrono::nanoseconds{100}};

Quantity quantity(const char* value) {
  const auto result = Quantity::parse(value);
  assert(result);
  return *result;
}

Provenance provenance(const char* id) {
  const auto result = Provenance::create({SourceRecordId{id}}, "custodian.positions", "1");
  assert(result);
  return *result;
}

Position position(const char* account, const char* instrument, Quantity value) {
  return {PositionKey{AccountId{account}, InstrumentId{instrument}}, value};
}

PositionObservation observation(const char* account, const char* instrument,
                                Quantity value, Timestamp time = as_of,
                                const char* source = "observation") {
  const auto result = PositionObservation::create(
      AccountId{account}, InstrumentId{instrument}, value, time, provenance(source));
  assert(result);
  return *result;
}

EventHeader header(const char* id, Timestamp time) {
  const auto result = EventHeader::create(EventId{id}, AccountId{"fund-a"}, time,
                                          provenance(id));
  assert(result);
  return *result;
}

EconomicEvent trade(const char* id, Quantity value, Timestamp time) {
  const auto result = EquityTrade::create(
      header(id, time), InstrumentId{"AAPL"}, value, *Price::parse("10"),
      *Currency::from_code("USD"),
      *SettlementDate::create(std::chrono::year{2026} / 1 / 2));
  assert(result);
  return *result;
}
}  // namespace

int main() {
  static_assert(!std::is_convertible_v<PositionObservation, EconomicEvent>);
  assert(!PositionObservation::create(AccountId{""}, InstrumentId{"AAPL"}, quantity("1"),
                                      as_of, provenance("invalid")));
  assert(!PositionObservation::create(AccountId{"fund-a"}, InstrumentId{""}, quantity("1"),
                                      as_of, provenance("invalid")));

  const Position expected = position("fund-a", "AAPL", quantity("100"));
  const auto exact = observation("fund-a", "AAPL", quantity("100"));
  assert(reconcile_positions(std::span{&expected, 1}, std::span{&exact, 1}, {as_of})->empty());

  const auto mismatch = observation("fund-a", "AAPL", quantity("95"), as_of, "mismatch");
  const auto mismatch_result = reconcile_positions(
      std::span{&expected, 1}, std::span{&mismatch, 1}, {as_of});
  assert(mismatch_result && mismatch_result->size() == 1);
  const auto& mismatch_break = mismatch_result->front();
  assert(mismatch_break.kind() == PositionBreakKind::quantity_mismatch);
  assert(mismatch_break.expected() == quantity("100"));
  assert(mismatch_break.observed() == quantity("95"));
  assert(mismatch_break.difference() == quantity("-5"));
  assert(mismatch_break.observation_provenance() == provenance("mismatch"));

  const std::vector<PositionObservation> none;
  const auto missing = reconcile_positions(std::span{&expected, 1}, none, {as_of});
  assert(missing && missing->front().kind() == PositionBreakKind::missing_observation);
  assert(missing->front().expected() == quantity("100"));
  assert(!missing->front().observed() && !missing->front().difference());
  assert(!missing->front().observation_provenance());

  const std::vector<Position> no_expected;
  const auto zero = observation("fund-a", "AAPL", quantity("0"), as_of, "zero");
  const auto unexpected = reconcile_positions(no_expected, std::span{&zero, 1}, {as_of});
  assert(unexpected && unexpected->front().kind() == PositionBreakKind::unexpected_observation);
  assert(!unexpected->front().expected() && unexpected->front().observed() == quantity("0"));
  assert(unexpected->front().observation_provenance() == provenance("zero"));

  const std::vector<Position> multiple_expected{
      position("fund-b", "AAPL", quantity("5")),
      position("fund-a", "MSFT", quantity("7")),
      position("fund-a", "AAPL", quantity("10"))};
  const std::vector<PositionObservation> multiple_observed{
      observation("fund-a", "MSFT", quantity("8")),
      observation("fund-b", "AAPL", quantity("5")),
      observation("fund-a", "AAPL", quantity("10"))};
  const auto multiple = reconcile_positions(multiple_expected, multiple_observed, {as_of});
  assert(multiple && multiple->size() == 1);
  assert(multiple->front().key() == PositionKey(AccountId{"fund-a"}, InstrumentId{"MSFT"}));

  const std::vector<PositionObservation> duplicates{
      observation("fund-a", "AAPL", quantity("1")),
      observation("fund-a", "AAPL", quantity("2"))};
  const auto duplicate = reconcile_positions(no_expected, duplicates, {as_of});
  assert(!duplicate && duplicate.error() == PositionReconciliationError::duplicate_observation);
  for (const auto time : {Timestamp{std::chrono::nanoseconds{99}},
                          Timestamp{std::chrono::nanoseconds{101}}}) {
    const auto wrong_time = observation("fund-a", "AAPL", quantity("1"), time);
    const auto result = reconcile_positions(no_expected, std::span{&wrong_time, 1}, {as_of});
    assert(!result && result.error() == PositionReconciliationError::observation_time_mismatch);
  }

  const Position short_expected = position("fund-a", "AAPL", quantity("-100"));
  const auto short_exact = observation("fund-a", "AAPL", quantity("-100"));
  assert(reconcile_positions(std::span{&short_expected, 1}, std::span{&short_exact, 1},
                             {as_of})->empty());
  const std::vector<Position> duplicate_expected{short_expected, short_expected};
  const auto duplicate_projection =
      reconcile_positions(duplicate_expected, std::span{&short_exact, 1}, {as_of});
  assert(!duplicate_projection &&
         duplicate_projection.error() == PositionReconciliationError::duplicate_projected_key);
  const auto duplicate_without_observations = reconcile_positions(duplicate_expected, {}, {as_of});
  assert(!duplicate_without_observations &&
         duplicate_without_observations.error() ==
             PositionReconciliationError::duplicate_projected_key);

  const auto short_observed = observation("fund-a", "AAPL", quantity("-90"));
  const auto short_break = reconcile_positions(
      std::span{&short_expected, 1}, std::span{&short_observed, 1}, {as_of});
  assert(short_break && short_break->front().difference() == quantity("10"));

  const Position minimum = position(
      "fund-a", "AAPL", Quantity::from_scaled(std::numeric_limits<std::int64_t>::min()));
  const auto maximum = observation(
      "fund-a", "AAPL", Quantity::from_scaled(std::numeric_limits<std::int64_t>::max()));
  const auto overflow = reconcile_positions(std::span{&minimum, 1}, std::span{&maximum, 1},
                                            {as_of});
  assert(!overflow && overflow.error() == PositionReconciliationError::quantity_overflow);

  const std::vector<Position> unordered_expected{
      position("fund-z", "ZZZ", quantity("1")),
      position("fund-a", "MSFT", quantity("1"))};
  const std::vector<PositionObservation> unordered_observed{
      observation("fund-b", "AAPL", quantity("0")),
      observation("fund-a", "AAPL", quantity("2"))};
  const auto ordered = reconcile_positions(unordered_expected, unordered_observed, {as_of});
  assert(ordered && ordered->size() == 4);
  assert((*ordered)[0].key() == PositionKey(AccountId{"fund-a"}, InstrumentId{"AAPL"}));
  assert((*ordered)[1].key() == PositionKey(AccountId{"fund-a"}, InstrumentId{"MSFT"}));
  assert((*ordered)[2].key() == PositionKey(AccountId{"fund-b"}, InstrumentId{"AAPL"}));
  assert((*ordered)[3].key() == PositionKey(AccountId{"fund-z"}, InstrumentId{"ZZZ"}));

  Ledger ledger;
  assert(ledger.append(trade("buy", quantity("100"), as_of)));
  const auto projected = project_positions(ledger.entries(), as_of);
  assert(projected);
  const auto external = observation("fund-a", "AAPL", quantity("98"), as_of, "custodian-row");
  const auto integrated = reconcile_positions(*projected, std::span{&external, 1}, {as_of});
  assert(integrated && integrated->size() == 1);
  assert(integrated->front().kind() == PositionBreakKind::quantity_mismatch);
  assert(integrated->front().difference() == quantity("-2"));
}
