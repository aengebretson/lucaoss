// Keep the behavioral checks active in Release builds, where CMake defines
// NDEBUG by default.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <limits>
#include <type_traits>
#include <variant>
#include <vector>

#include "luca/accounting/settlement_date_projection.hpp"
#include "luca/portfolio.hpp"

using namespace luca;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace {

const Currency usd = *Currency::from_code("USD");

year_month_day date(unsigned month_number, unsigned day_number) {
  return 2026y / month{month_number} / day{day_number};
}

Provenance provenance(const char *source) {
  auto value = Provenance::create({SourceRecordId{source}}, "lifecycle-projection.fixture", "1");
  assert(value);
  return *value;
}

EventHeader header_for(const char *record_id, Timestamp effective_at, const char *source) {
  auto value = EventHeader::create(EventId{record_id}, AccountId{"fund-a"}, effective_at,
                                   provenance(source));
  assert(value);
  return *value;
}

EconomicEvent cash(const char *record_id, Timestamp effective_at, const char *amount,
                   const char *source) {
  auto value = Money::parse(amount, usd);
  assert(value);
  return CashMovement::create(header_for(record_id, effective_at, source), *value);
}

EconomicEvent cash_scaled(const char *record_id, Timestamp effective_at, std::int64_t amount,
                          const char *source) {
  return CashMovement::create(header_for(record_id, effective_at, source),
                              Money::from_scaled(amount, usd));
}

EconomicEvent trade(const char *record_id, Timestamp effective_at, const char *quantity,
                    const char *price, year_month_day settlement_date, const char *source) {
  auto parsed_quantity = Quantity::parse(quantity);
  auto parsed_price = Price::parse(price);
  auto parsed_date = SettlementDate::create(settlement_date);
  assert(parsed_quantity && parsed_price && parsed_date);
  auto value =
      EquityTrade::create(header_for(record_id, effective_at, source), InstrumentId{"MSFT"},
                          *parsed_quantity, *parsed_price, usd, *parsed_date);
  assert(value);
  return *value;
}

void accept(LifecycleLedger &ledger, const LifecycleRecordDraft &draft) {
  if (!ledger.accept(draft))
    std::abort();
}

LifecycleProjectionResult project(const LifecycleResolution &resolution, Timestamp economic_as_of,
                                  year_month_day settlement_as_of_date) {
  return project_lifecycle(resolution,
                           LifecycleProjectionContext{economic_as_of, settlement_as_of_date});
}

void expect_no_positions(const LifecycleProjectionResult &result) {
  assert(result.positions && result.positions->empty());
}

void expect_no_cash(const LifecycleProjectionResult &result) {
  assert(result.settled_cash && result.settled_cash->empty());
}

void expect_no_obligations(const LifecycleProjectionResult &result) {
  assert(result.open_settlement_obligations && result.open_settlement_obligations->empty());
}

void expect_position(const LifecycleProjectionResult &result, const char *quantity) {
  assert(result.positions && result.positions->size() == 1);
  assert(
      (result.positions->front().key() == PositionKey{AccountId{"fund-a"}, InstrumentId{"MSFT"}}));
  assert(result.positions->front().quantity() == *Quantity::parse(quantity));
}

void expect_cash(const LifecycleProjectionResult &result, const char *amount) {
  assert(result.settled_cash && result.settled_cash->size() == 1);
  assert((result.settled_cash->front().key() == CashKey{AccountId{"fund-a"}, usd}));
  assert(result.settled_cash->front().amount() == *Money::parse(amount, usd));
}

void expect_obligation(const LifecycleProjectionResult &result, year_month_day settlement_date,
                       SettlementDirection direction, const char *amount) {
  assert(result.open_settlement_obligations && result.open_settlement_obligations->size() == 1);
  const auto &obligation = result.open_settlement_obligations->front();
  assert((obligation.key() == SettlementObligationKey{AccountId{"fund-a"},
                                                      *SettlementDate::create(settlement_date), usd,
                                                      direction}));
  assert(obligation.amount() == *Money::parse(amount, usd));
}

void late_cash_correction_and_cancellation() {
  constexpr Timestamp effective_at{24h};
  constexpr Timestamp recorded_original{2 * 24h};
  constexpr Timestamp recorded_correction{10 * 24h};
  constexpr Timestamp recorded_cancellation{12 * 24h};
  constexpr Timestamp economic_as_of{31 * 24h};

  LifecycleLedger ledger;
  accept(ledger, LifecycleRecordDraft::originate(
                     EconomicEventId{"cash-economic-1"}, recorded_original,
                     cash("cash-record-v1", effective_at, "1000", "cash-source-original")));
  accept(ledger,
         LifecycleRecordDraft::correct(
             EconomicEventId{"cash-economic-1"}, EventId{"cash-record-v1"}, recorded_correction,
             cash("cash-record-v2", effective_at, "1200", "cash-source-correction")));
  accept(ledger, LifecycleRecordDraft::cancel(
                     EventId{"cash-record-v3"}, EconomicEventId{"cash-economic-1"},
                     EventId{"cash-record-v2"}, AccountId{"fund-a"}, recorded_cancellation,
                     provenance("cash-source-cancellation")));

  const auto before_resolution = ledger.resolve(recorded_correction - 1ns, economic_as_of);
  const auto before = project(before_resolution, economic_as_of, date(1, 31));
  expect_no_positions(before);
  expect_cash(before, "1000");
  expect_no_obligations(before);
  assert(before_resolution.active_events().size() == 1);
  assert(before_resolution.active_events().front().record().record_id() ==
         EventId{"cash-record-v1"});

  const auto corrected_resolution = ledger.resolve(recorded_correction, economic_as_of);
  const auto corrected = project(corrected_resolution, economic_as_of, date(1, 31));
  expect_no_positions(corrected);
  expect_cash(corrected, "1200");
  expect_no_obligations(corrected);
  assert(corrected_resolution.active_events().size() == 1);
  const auto &corrected_event = corrected_resolution.active_events().front();
  assert(corrected_event.record().record_id() == EventId{"cash-record-v2"});
  assert(corrected_event.record().acceptance_sequence().value() == 2);
  assert(corrected_event.lineage().size() == 2);
  assert(corrected_event.lineage()[0].get().provenance() == provenance("cash-source-original"));
  assert(corrected_event.lineage()[1].get().provenance() == provenance("cash-source-correction"));

  const auto cancelled_resolution = ledger.resolve(recorded_cancellation, economic_as_of);
  const auto cancelled = project(cancelled_resolution, economic_as_of, date(1, 31));
  expect_no_positions(cancelled);
  expect_no_cash(cancelled);
  expect_no_obligations(cancelled);
  assert(cancelled_resolution.active_events().empty());
  assert(cancelled_resolution.chains().size() == 1);
  assert(!cancelled_resolution.chains().front().active());
  assert(cancelled_resolution.chains().front().lineage().size() == 3);
  assert(cancelled_resolution.chains().front().head().record_id() == EventId{"cash-record-v3"});

  // Projection does not mutate or replace any accepted lifecycle evidence.
  assert(ledger.records().size() == 3);
  assert(ledger.records()[0].record_id() == EventId{"cash-record-v1"});
  assert(std::get<CashMovement>(*ledger.records()[0].event()).amount() ==
         *Money::parse("1000", usd));
  assert(ledger.records()[2].provenance() == provenance("cash-source-cancellation"));
}

void equity_correction_and_reversal() {
  constexpr Timestamp opening_effective{24h};
  constexpr Timestamp trade_effective{4 * 24h + 14h};
  constexpr Timestamp reversal_effective{7 * 24h + 10h};
  constexpr Timestamp recorded_opening{2 * 24h};
  constexpr Timestamp recorded_trade{4 * 24h + 15h};
  constexpr Timestamp recorded_correction{5 * 24h + 9h};
  constexpr Timestamp recorded_reversal{9 * 24h};
  constexpr Timestamp trade_date_as_of{4 * 24h + 23h};
  constexpr Timestamp corrected_settlement_as_of{6 * 24h + 23h};
  constexpr Timestamp reversal_as_of{7 * 24h + 23h};
  constexpr Timestamp reversal_settlement_as_of{8 * 24h + 23h};

  LifecycleLedger ledger;
  accept(ledger,
         LifecycleRecordDraft::originate(
             EconomicEventId{"opening-cash-economic"}, recorded_opening,
             cash("opening-cash-record", opening_effective, "100000", "opening-cash-source")));
  accept(ledger,
         LifecycleRecordDraft::originate(EconomicEventId{"trade-economic-1"}, recorded_trade,
                                         trade("trade-record-v1", trade_effective, "100", "50",
                                               date(6, 4), "trade-source-original")));
  accept(ledger, LifecycleRecordDraft::correct(EconomicEventId{"trade-economic-1"},
                                               EventId{"trade-record-v1"}, recorded_correction,
                                               trade("trade-record-v2", trade_effective, "80", "55",
                                                     date(6, 4), "trade-source-correction")));
  accept(ledger,
         LifecycleRecordDraft::reverse(EconomicEventId{"trade-reversal-economic-1"},
                                       EventId{"trade-record-v2"}, recorded_reversal,
                                       trade("reversal-record-v1", reversal_effective, "-80", "55",
                                             date(6, 6), "trade-source-reversal")));

  const auto original_resolution = ledger.resolve(recorded_trade, trade_date_as_of);
  const auto original = project(original_resolution, trade_date_as_of, date(6, 2));
  expect_position(original, "100");
  expect_cash(original, "100000");
  expect_obligation(original, date(6, 4), SettlementDirection::payable, "5000");

  const auto corrected_resolution = ledger.resolve(recorded_correction, trade_date_as_of);
  const auto corrected = project(corrected_resolution, trade_date_as_of, date(6, 2));
  expect_position(corrected, "80");
  expect_cash(corrected, "100000");
  expect_obligation(corrected, date(6, 4), SettlementDirection::payable, "4400");
  assert(corrected_resolution.active_events()[1].record().record_id() ==
         EventId{"trade-record-v2"});
  assert(corrected_resolution.active_events()[1].lineage().size() == 2);

  const auto settled_resolution =
      ledger.resolve(recorded_reversal - 1ns, corrected_settlement_as_of);
  const auto settled = project(settled_resolution, corrected_settlement_as_of, date(6, 4));
  expect_position(settled, "80");
  expect_cash(settled, "95600");
  expect_no_obligations(settled);

  const auto reversed_resolution = ledger.resolve(recorded_reversal, reversal_as_of);
  const auto reversed = project(reversed_resolution, reversal_as_of, date(6, 5));
  expect_no_positions(reversed);
  expect_cash(reversed, "95600");
  expect_obligation(reversed, date(6, 6), SettlementDirection::receivable, "4400");
  assert(reversed_resolution.active_events().size() == 3);
  const auto &reversal = reversed_resolution.active_events()[2];
  assert(reversal.record().record_id() == EventId{"reversal-record-v1"});
  assert(reversal.record().acceptance_sequence().value() == 4);
  assert(reversal.reversed_record() != nullptr);
  assert(reversal.reversed_record()->record_id() == EventId{"trade-record-v2"});
  assert(reversal.record().provenance() == provenance("trade-source-reversal"));

  const auto final_resolution = ledger.resolve(recorded_reversal, reversal_settlement_as_of);
  const auto final = project(final_resolution, reversal_settlement_as_of, date(6, 6));
  expect_no_positions(final);
  expect_cash(final, "100000");
  expect_no_obligations(final);

  // The corrected target and its immutable predecessor remain inspectable
  // beside the exact offsetting reversal.
  assert(ledger.records()[1].record_id() == EventId{"trade-record-v1"});
  assert(std::get<EquityTrade>(*ledger.records()[1].event()).quantity() == *Quantity::parse("100"));
  assert(ledger.records()[2].record_id() == EventId{"trade-record-v2"});
  assert(std::get<EquityTrade>(*ledger.records()[2].event()).quantity() == *Quantity::parse("80"));
}

void deterministic_across_acceptance_paths() {
  constexpr Timestamp effective_at{4h};
  constexpr Timestamp recorded_at{8h};
  const std::array records{
      LifecycleRecordDraft::originate(EconomicEventId{"cash-a"}, recorded_at,
                                      cash("cash-a-v1", effective_at, "10", "cash-a-source-1")),
      LifecycleRecordDraft::correct(EconomicEventId{"cash-a"}, EventId{"cash-a-v1"}, recorded_at,
                                    cash("cash-a-v2", effective_at, "12", "cash-a-source-2")),
      LifecycleRecordDraft::originate(EconomicEventId{"cash-b"}, recorded_at,
                                      cash("cash-b-v1", effective_at, "20", "cash-b-source")),
  };

  LifecycleLedger individually_accepted;
  for (const auto &record : records)
    accept(individually_accepted, record);
  LifecycleLedger batch_accepted;
  const auto accepted = batch_accepted.accept_batch(records);
  assert(accepted && accepted->size() == records.size());

  const auto individual_resolution = individually_accepted.resolve(recorded_at, effective_at);
  const auto batch_resolution = batch_accepted.resolve(recorded_at, effective_at);
  const auto individual = project(individual_resolution, effective_at, date(1, 1));
  const auto batch = project(batch_resolution, effective_at, date(1, 1));
  const auto repeated = project(individual_resolution, effective_at, date(1, 1));

  assert(individual.positions == batch.positions);
  assert(individual.settled_cash == batch.settled_cash);
  assert(individual.open_settlement_obligations == batch.open_settlement_obligations);
  assert(individual.positions == repeated.positions);
  assert(individual.settled_cash == repeated.settled_cash);
  assert(individual.open_settlement_obligations == repeated.open_settlement_obligations);
  expect_cash(individual, "32");
}

void ordinary_and_lifecycle_replay_agree() {
  Ledger ordinary;
  LifecycleLedger lifecycle;
  const auto eur = *Currency::from_code("EUR");
  for (int i = 0; i < 120; ++i) {
    const auto id = "differential-" + std::to_string(i);
    const auto denomination = i % 2 ? usd : eur;
    const Timestamp effective{nanoseconds{(i * 17) % 11}};
    const Timestamp recorded{nanoseconds{100 + i}};
    const auto h = *EventHeader::create(EventId{id}, AccountId{i % 3 ? "fund-a" : "fund-b"},
                                        effective, provenance(id.c_str()));
    const EconomicEvent event = [&]() -> EconomicEvent {
      if (i % 4 == 0)
        return CashMovement::create(h,
                                    Money::from_scaled((i % 8 ? -1 : 1) * (i + 1), denomination));
      return *EquityTrade::create(h, InstrumentId{i % 5 ? "AAPL" : "MSFT"},
                                  Quantity::from_scaled((i % 2 ? -1 : 1) * 100000000),
                                  Price::from_scaled(123456789 + i), denomination,
                                  *SettlementDate::create(date(1, i % 3 + 1)),
                                  TradeCharges{{id, "commission", "broker", "", "reported",
                                                Money::from_scaled(1, denomination)}});
    }();
    assert(ordinary.append(event));
    accept(lifecycle, LifecycleRecordDraft::originate(EconomicEventId{id}, recorded, event));
  }
  for (const auto cutoff : {Timestamp{0ns}, Timestamp{5ns}, Timestamp{10ns}})
    for (const unsigned day : {1U, 2U, 3U}) {
      const auto settlement = date(1, day);
      const auto resolution = lifecycle.resolve({Timestamp::max(), cutoff});
      const auto actual = project_lifecycle(resolution, *SettlementDate::create(settlement));
      assert(actual.positions == project_positions(ordinary.entries(), cutoff));
      assert(actual.settled_cash == project_cash(ordinary.entries(), {cutoff, settlement}));
      assert(actual.open_settlement_obligations ==
             project_settlement_obligations(ordinary.entries(), {cutoff, settlement}));
    }
}

void journal_context_is_bound_to_resolution() {
  const LifecycleLedger empty;
  const auto resolution = empty.resolve({Timestamp{8h}, Timestamp{4h}});
  // Empty active sets cannot reveal missing inputs by inspecting events alone.
  for (const auto economic : {Timestamp{3h}, Timestamp{4h}, Timestamp{5h}})
    for (const auto known : {Timestamp{7h}, Timestamp{8h}, Timestamp{9h}}) {
      if (economic == Timestamp{4h} && known == Timestamp{8h})
        continue;
      const auto trade_date =
          project_trade_date_journals(resolution, {known, economic, date(1, 1)});
      const auto settlement_date =
          project_settlement_date_journals(resolution, {known, economic, date(1, 1)});
      assert(!trade_date && trade_date.error().category() ==
                                TradeDateProjectionDiagnosticCategory::invalid_context);
      assert(!settlement_date && settlement_date.error().category() ==
                                     SettlementDateProjectionDiagnosticCategory::invalid_context);
    }
}

void incompatible_context_is_rejected() {
  LifecycleLedger ledger;
  accept(ledger, LifecycleRecordDraft::originate(
                     EconomicEventId{"context-economic"}, Timestamp{8h},
                     cash("context-record", Timestamp{4h}, "10", "context-source")));
  const auto resolution = ledger.resolve(LifecycleEvaluationContext{
      .recorded_through = Timestamp{8h}, .economic_as_of = Timestamp{4h}});
  assert(resolution.evaluation_context().recorded_through == Timestamp{8h});
  assert(resolution.evaluation_context().economic_as_of == Timestamp{4h});
  const auto derived_context = project_lifecycle(resolution, *SettlementDate::create(date(1, 1)));
  const auto explicit_context = project(resolution, Timestamp{4h}, date(1, 1));
  assert(derived_context.positions == explicit_context.positions);
  assert(derived_context.settled_cash == explicit_context.settled_cash);
  assert(derived_context.open_settlement_obligations ==
         explicit_context.open_settlement_obligations);
  for (const auto cutoff : {Timestamp{3h}, Timestamp{5h}}) {
    const auto result = project(resolution, cutoff, date(1, 1));
    assert(!result.positions &&
           result.positions.error() == PositionProjectionError::context_mismatch);
    assert(!result.settled_cash &&
           result.settled_cash.error() == CashProjectionError::context_mismatch);
    assert(!result.open_settlement_obligations && result.open_settlement_obligations.error() ==
                                                      SettlementProjectionError::context_mismatch);
  }
  const auto invalid_date = project(resolution, Timestamp{4h}, date(2, 30));
  assert(invalid_date.positions && invalid_date.positions->empty());
  assert(!invalid_date.settled_cash &&
         invalid_date.settled_cash.error() == CashProjectionError::invalid_context);
  assert(!invalid_date.open_settlement_obligations &&
         invalid_date.open_settlement_obligations.error() ==
             SettlementProjectionError::invalid_context);
}

void projection_errors_remain_exact() {
  constexpr Timestamp effective_at{4h};
  constexpr Timestamp recorded_at{8h};

  LifecycleLedger cash_overflow;
  accept(cash_overflow,
         LifecycleRecordDraft::originate(EconomicEventId{"cash-max-economic"}, recorded_at,
                                         cash_scaled("cash-max", effective_at,
                                                     std::numeric_limits<std::int64_t>::max(),
                                                     "cash-max-source")));
  accept(cash_overflow, LifecycleRecordDraft::originate(
                            EconomicEventId{"cash-plus-economic"}, recorded_at,
                            cash_scaled("cash-plus", effective_at, 1, "cash-plus-source")));
  accept(cash_overflow, LifecycleRecordDraft::originate(
                            EconomicEventId{"cash-minus-economic"}, recorded_at,
                            cash_scaled("cash-minus", effective_at, -1, "cash-minus-source")));
  const auto cash_resolution = cash_overflow.resolve(recorded_at, effective_at);
  const auto cash_result = project(cash_resolution, effective_at, date(1, 1));
  assert(!cash_result.settled_cash);
  assert(cash_result.settled_cash.error() == CashProjectionError::amount_overflow);
  expect_no_positions(cash_result);
  expect_no_obligations(cash_result);

  LifecycleLedger settlement_overflow;
  accept(settlement_overflow, LifecycleRecordDraft::originate(
                                  EconomicEventId{"settlement-max-economic"}, recorded_at,
                                  trade("settlement-max", effective_at, "92233720368.54775807",
                                        "100", date(6, 30), "settlement-max-source")));
  accept(settlement_overflow,
         LifecycleRecordDraft::originate(EconomicEventId{"settlement-one-economic"}, recorded_at,
                                         trade("settlement-one", effective_at, "0.000001", "1",
                                               date(6, 30), "settlement-one-source")));
  const auto settlement_resolution = settlement_overflow.resolve(recorded_at, effective_at);
  const auto settlement_result = project(settlement_resolution, effective_at, date(6, 1));
  assert(!settlement_result.positions);
  assert(settlement_result.positions.error() == PositionProjectionError::quantity_overflow);
  assert(settlement_result.settled_cash && settlement_result.settled_cash->empty());
  assert(!settlement_result.open_settlement_obligations);
  assert(settlement_result.open_settlement_obligations.error() ==
         SettlementProjectionError::amount_overflow);

  LifecycleLedger valuation_overflow;
  accept(valuation_overflow, LifecycleRecordDraft::originate(
                                 EconomicEventId{"settled-huge-economic"}, recorded_at,
                                 trade("settled-huge", effective_at, "92233720368.54775807",
                                       "92233720368.54775807", date(6, 1), "settled-huge-source")));
  accept(valuation_overflow, LifecycleRecordDraft::originate(
                                 EconomicEventId{"open-huge-economic"}, recorded_at,
                                 trade("open-huge", effective_at, "-92233720368.54775807",
                                       "92233720368.54775807", date(6, 30), "open-huge-source")));
  const auto valuation_resolution = valuation_overflow.resolve(recorded_at, effective_at);
  const auto valuation_result = project(valuation_resolution, effective_at, date(6, 1));
  expect_no_positions(valuation_result);
  assert(!valuation_result.settled_cash);
  assert(valuation_result.settled_cash.error() == CashProjectionError::valuation_overflow);
  assert(!valuation_result.open_settlement_obligations);
  assert(valuation_result.open_settlement_obligations.error() ==
         SettlementProjectionError::valuation_overflow);
}

} // namespace

int main() {
  static_assert(std::is_same_v<decltype(LifecycleProjectionResult::positions),
                               std::expected<std::vector<Position>, PositionProjectionError>>);
  static_assert(std::is_same_v<decltype(LifecycleProjectionResult::settled_cash),
                               std::expected<std::vector<CashBalance>, CashProjectionError>>);
  static_assert(
      std::is_same_v<decltype(LifecycleProjectionResult::open_settlement_obligations),
                     std::expected<std::vector<SettlementObligation>, SettlementProjectionError>>);

  ordinary_and_lifecycle_replay_agree();
  journal_context_is_bound_to_resolution();
  incompatible_context_is_rejected();
  late_cash_correction_and_cancellation();
  equity_correction_and_reversal();
  deterministic_across_acceptance_paths();
  projection_errors_remain_exact();
}
