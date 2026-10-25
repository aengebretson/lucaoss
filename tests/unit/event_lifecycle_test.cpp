#include "luca/lifecycle.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <span>
#include <type_traits>
#include <variant>

using namespace luca;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace {

Provenance provenance(const char* source) {
  auto result = Provenance::create({SourceRecordId{source}}, "lifecycle.fixture", "1");
  assert(result);
  return *result;
}

Currency currency(const char* code = "USD") {
  auto result = Currency::from_code(code);
  assert(result);
  return *result;
}

EconomicEvent cash(const char* record_id, const char* account, Timestamp effective_at,
                   const char* amount, const char* source,
                   const char* currency_code = "USD") {
  auto event_header = EventHeader::create(EventId{record_id}, AccountId{account},
                                          effective_at, provenance(source));
  const auto denomination = currency(currency_code);
  auto value = Money::parse(amount, denomination);
  assert(event_header && value);
  return CashMovement::create(*event_header, *value);
}

EconomicEvent trade(const char* record_id, const char* account,
                    Timestamp effective_at, const char* instrument,
                    const char* quantity, const char* price, const char* source,
                    year_month_day settlement_date,
                    const char* currency_code = "USD") {
  auto event_header = EventHeader::create(EventId{record_id}, AccountId{account},
                                          effective_at, provenance(source));
  auto parsed_quantity = Quantity::parse(quantity);
  auto parsed_price = Price::parse(price);
  auto parsed_date = SettlementDate::create(settlement_date);
  assert(event_header && parsed_quantity && parsed_price && parsed_date);
  auto result = EquityTrade::create(
      *event_header, InstrumentId{instrument}, *parsed_quantity, *parsed_price,
      currency(currency_code), *parsed_date);
  assert(result);
  return *result;
}

template <class Result>
void expect_error(const Result& result, LifecycleDiagnosticCategory category,
                  const char* record_id) {
  assert(!result);
  assert(result.error().category() == category);
  assert(result.error().category_name() == category_name(category));
  assert(result.error().record_id() == EventId{record_id});
  assert(!result.error().message().empty());
}

const CashMovement& resolved_cash(const ResolvedLifecycleEvent& event) {
  return std::get<CashMovement>(event.event());
}

const EquityTrade& resolved_trade(const ResolvedLifecycleEvent& event) {
  return std::get<EquityTrade>(event.event());
}

void accept_successfully(LifecycleLedger& ledger,
                         const LifecycleRecordDraft& draft) {
  const auto accepted = ledger.accept(draft);
  if (!accepted) std::abort();
}

void cash_correction_and_cancellation() {
  constexpr Timestamp effective{24h};
  constexpr Timestamp recorded_original{48h};
  constexpr Timestamp recorded_correction{10 * 24h};
  constexpr Timestamp recorded_cancellation{12 * 24h};

  LifecycleLedger ledger;
  const auto original = LifecycleRecordDraft::originate(
      EconomicEventId{"cash-economic-1"}, recorded_original,
      cash("cash-record-v1", "fund-a", effective, "1000", "cash-source-original"));
  const auto first = ledger.accept(original);
  assert(first);
  assert(first->get().record_id() == EventId{"cash-record-v1"});
  assert(first->get().economic_event_id() == EconomicEventId{"cash-economic-1"});
  assert(first->get().action() == LifecycleAction::originate);
  assert(first->get().acceptance_sequence().value() == 1);
  assert(first->get().recorded_at() == recorded_original);
  assert(first->get().provenance() == provenance("cash-source-original"));
  assert(first->get().event() != nullptr);
  assert(!first->get().causal_record_id());
  const auto immutable_original = first->get();

  const auto before_correction = ledger.resolve(recorded_original, effective);
  assert(before_correction.chains().size() == 1);
  assert(before_correction.chains()[0].active());
  assert(before_correction.chains()[0].lineage().size() == 1);
  assert(before_correction.active_events().size() == 1);
  assert(before_correction.active_events()[0].record().record_id() ==
         EventId{"cash-record-v1"});
  assert(resolved_cash(before_correction.active_events()[0]).amount() ==
         *Money::parse("1000", currency()));

  // Both cutoffs are inclusive, and knowledge selection occurs before the
  // independently supplied economic cutoff.
  assert(ledger.resolve(recorded_original, effective - 1ns).active_events().empty());
  assert(ledger.resolve(recorded_original - 1ns, effective).chains().empty());

  const auto correction = LifecycleRecordDraft::correct(
      EconomicEventId{"cash-economic-1"}, EventId{"cash-record-v1"},
      recorded_correction,
      cash("cash-record-v2", "fund-a", effective, "1200",
           "cash-source-correction"));
  const auto second = ledger.accept(correction);
  assert(second && second->get().acceptance_sequence().value() == 2);
  assert(second->get().supersedes_record_id() &&
         *second->get().supersedes_record_id() == EventId{"cash-record-v1"});
  assert(second->get().reverses_record_id() == nullptr);
  assert(ledger.records()[0] == immutable_original);

  const auto corrected = ledger.resolve(recorded_correction, effective);
  assert(corrected.chains().size() == 1 && corrected.chains()[0].active());
  assert(corrected.active_events().size() == 1);
  assert(corrected.active_events()[0].record().record_id() ==
         EventId{"cash-record-v2"});
  assert(corrected.active_events()[0].lineage().size() == 2);
  assert(corrected.active_events()[0].lineage()[0].get().provenance() ==
         provenance("cash-source-original"));
  assert(corrected.active_events()[0].lineage()[1].get().provenance() ==
         provenance("cash-source-correction"));
  assert(resolved_cash(corrected.active_events()[0]).amount() ==
         *Money::parse("1200", currency()));
  assert(ledger.resolve(recorded_correction - 1ns, effective)
             .active_events()[0]
             .record()
             .record_id() == EventId{"cash-record-v1"});

  const auto cancellation = LifecycleRecordDraft::cancel(
      EventId{"cash-record-v3"}, EconomicEventId{"cash-economic-1"},
      EventId{"cash-record-v2"}, AccountId{"fund-a"}, recorded_cancellation,
      provenance("cash-source-cancellation"));
  const auto third = ledger.accept(cancellation);
  assert(third && third->get().acceptance_sequence().value() == 3);
  assert(third->get().action() == LifecycleAction::cancel);
  assert(third->get().event() == nullptr && !third->get().effective_at());

  const auto cancelled = ledger.resolve(recorded_cancellation, effective);
  assert(cancelled.active_events().empty());
  assert(cancelled.chains().size() == 1);
  assert(!cancelled.chains()[0].active());
  assert(cancelled.chains()[0].lineage().size() == 3);
  assert(cancelled.chains()[0].head().record_id() == EventId{"cash-record-v3"});
  assert(cancelled.chains()[0].head().provenance() ==
         provenance("cash-source-cancellation"));
  assert(ledger.records()[0] == immutable_original);
}

void equity_correction_and_reversal() {
  constexpr Timestamp opening_effective{24h};
  constexpr Timestamp trade_effective{4 * 24h + 14h};
  constexpr Timestamp reversal_effective{7 * 24h + 10h};
  constexpr Timestamp recorded_opening{2 * 24h};
  constexpr Timestamp recorded_trade{4 * 24h + 15h};
  constexpr Timestamp recorded_correction{5 * 24h + 9h};
  constexpr Timestamp recorded_reversal{9 * 24h};

  LifecycleLedger ledger;
  accept_successfully(ledger, LifecycleRecordDraft::originate(
      EconomicEventId{"opening-cash-economic"}, recorded_opening,
      cash("opening-cash-record", "fund-a", opening_effective, "100000",
           "opening-cash-source")));
  accept_successfully(ledger, LifecycleRecordDraft::originate(
      EconomicEventId{"trade-economic-1"}, recorded_trade,
      trade("trade-record-v1", "fund-a", trade_effective, "MSFT", "100", "50",
            "trade-source-original", 2026y / June / 4d)));

  const auto original_view = ledger.resolve(recorded_trade, reversal_effective);
  assert(original_view.active_events().size() == 2);
  assert(original_view.active_events()[0].record().record_id() ==
         EventId{"opening-cash-record"});
  assert(resolved_trade(original_view.active_events()[1]).quantity() ==
         *Quantity::parse("100"));

  accept_successfully(ledger, LifecycleRecordDraft::correct(
      EconomicEventId{"trade-economic-1"}, EventId{"trade-record-v1"},
      recorded_correction,
      trade("trade-record-v2", "fund-a", trade_effective, "MSFT", "80", "55",
            "trade-source-correction", 2026y / June / 4d)));
  const auto corrected_view = ledger.resolve(recorded_correction, reversal_effective);
  assert(corrected_view.active_events().size() == 2);
  assert(corrected_view.active_events()[1].record().record_id() ==
         EventId{"trade-record-v2"});
  assert(corrected_view.active_events()[1].lineage().size() == 2);
  assert(resolved_trade(corrected_view.active_events()[1]).quantity() ==
         *Quantity::parse("80"));
  assert(resolved_trade(corrected_view.active_events()[1]).price() ==
         *Price::parse("55"));

  accept_successfully(ledger, LifecycleRecordDraft::reverse(
      EconomicEventId{"trade-reversal-economic"}, EventId{"trade-record-v2"},
      recorded_reversal,
      trade("trade-reversal-record", "fund-a", reversal_effective, "MSFT", "-80",
            "55", "trade-source-reversal", 2026y / June / 8d)));

  const auto before_reversal_knowledge =
      ledger.resolve(recorded_reversal - 1ns, reversal_effective);
  assert(before_reversal_knowledge.active_events().size() == 2);
  const auto before_reversal_economic =
      ledger.resolve(recorded_reversal, reversal_effective - 1ns);
  assert(before_reversal_economic.active_events().size() == 2);

  const auto reversed_view = ledger.resolve(recorded_reversal, reversal_effective);
  assert(reversed_view.chains().size() == 3);
  assert(reversed_view.active_events().size() == 3);
  assert(reversed_view.active_events()[0].record().record_id() ==
         EventId{"opening-cash-record"});
  assert(reversed_view.active_events()[1].record().record_id() ==
         EventId{"trade-record-v2"});
  assert(reversed_view.active_events()[2].record().record_id() ==
         EventId{"trade-reversal-record"});
  assert(resolved_trade(reversed_view.active_events()[2]).quantity() ==
         *Quantity::parse("-80"));
  assert(reversed_view.active_events()[2].lineage().size() == 1);
  assert(reversed_view.active_events()[2].reversed_record() != nullptr);
  assert(reversed_view.active_events()[2].reversed_record()->record_id() ==
         EventId{"trade-record-v2"});
  assert(ledger.records()[1].record_id() == EventId{"trade-record-v1"});
  assert(std::get<EquityTrade>(*ledger.records()[1].event()).quantity() ==
         *Quantity::parse("100"));
}

void deterministic_replay_across_acceptance_paths() {
  constexpr Timestamp effective{4h};
  constexpr Timestamp recorded{8h};
  const std::array records{
      LifecycleRecordDraft::originate(
          EconomicEventId{"economic-a"}, recorded,
          cash("record-a1", "fund-a", effective - 1ns, "10", "source-a1")),
      LifecycleRecordDraft::correct(
          EconomicEventId{"economic-a"}, EventId{"record-a1"}, recorded,
          cash("record-a2", "fund-a", effective, "12", "source-a2")),
      LifecycleRecordDraft::originate(
          EconomicEventId{"economic-b"}, recorded,
          cash("record-b1", "fund-a", effective, "20", "source-b1")),
  };

  LifecycleLedger individually_accepted;
  for (const auto& record : records)
    accept_successfully(individually_accepted, record);
  LifecycleLedger batch_accepted;
  const auto batch = batch_accepted.accept_batch(records);
  assert(batch && batch->size() == records.size());

  assert(individually_accepted.records().size() == batch_accepted.records().size());
  for (std::size_t index = 0; index < individually_accepted.size(); ++index)
    assert(individually_accepted.records()[index] == batch_accepted.records()[index]);

  const auto individual_view = individually_accepted.resolve(recorded, effective);
  const auto batch_view = batch_accepted.resolve(recorded, effective);
  assert(individual_view.active_events().size() == 2);
  assert(batch_view.active_events().size() == 2);
  for (std::size_t index = 0; index < individual_view.active_events().size(); ++index) {
    assert(individual_view.active_events()[index].record().record_id() ==
           batch_view.active_events()[index].record().record_id());
    assert(individual_view.active_events()[index].lineage().size() ==
           batch_view.active_events()[index].lineage().size());
  }
  assert(individual_view.active_events()[0].record().record_id() ==
         EventId{"record-a2"});
  assert(individual_view.active_events()[1].record().record_id() ==
         EventId{"record-b1"});

  const auto repeated_view = individually_accepted.resolve(recorded, effective);
  assert(repeated_view.active_events()[0].record().record_id() ==
         EventId{"record-a2"});
  assert(repeated_view.active_events()[1].record().record_id() ==
         EventId{"record-b1"});
}

void stable_causal_diagnostics() {
  constexpr Timestamp effective{1h};
  constexpr Timestamp recorded{2h};

  {
    LifecycleLedger ledger;
    const auto result = ledger.accept(LifecycleRecordDraft::correct(
        EconomicEventId{"cash-economic"}, EventId{"not-accepted"}, recorded,
        cash("cash-v2", "fund-a", effective, "12", "source")));
    expect_error(result, LifecycleDiagnosticCategory::causal_reference_missing,
                 "cash-v2");
    assert(ledger.empty());
  }
  {
    LifecycleLedger ledger;
    const auto result = ledger.accept(LifecycleRecordDraft::correct(
        EconomicEventId{"cash-economic"}, EventId{"cash-self"}, recorded,
        cash("cash-self", "fund-a", effective, "12", "source")));
    expect_error(result, LifecycleDiagnosticCategory::causal_self_reference,
                 "cash-self");
    assert(ledger.empty());
  }
  {
    LifecycleLedger ledger;
    const std::array cycle{
        LifecycleRecordDraft::correct(
            EconomicEventId{"cash-economic"}, EventId{"cash-cycle-b"}, recorded,
            cash("cash-cycle-a", "fund-a", effective, "10", "source-a")),
        LifecycleRecordDraft::correct(
            EconomicEventId{"cash-economic"}, EventId{"cash-cycle-a"}, recorded + 1ns,
            cash("cash-cycle-b", "fund-a", effective, "12", "source-b")),
    };
    const auto result = ledger.accept_batch(cycle);
    expect_error(result, LifecycleDiagnosticCategory::causal_cycle, "cash-cycle-a");
    assert(ledger.empty());
    const auto valid = ledger.accept(LifecycleRecordDraft::originate(
        EconomicEventId{"valid-economic"}, recorded,
        cash("valid-record", "fund-a", effective, "1", "valid-source")));
    assert(valid && valid->get().acceptance_sequence().value() == 1);
  }
  {
    LifecycleLedger ledger;
    const std::array forward_reference{
        LifecycleRecordDraft::correct(
            EconomicEventId{"cash-economic"}, EventId{"cash-origin"}, recorded,
            cash("cash-correction", "fund-a", effective, "12", "source-a")),
        LifecycleRecordDraft::originate(
            EconomicEventId{"cash-economic"}, recorded + 1ns,
            cash("cash-origin", "fund-a", effective, "10", "source-b")),
    };
    const auto result = ledger.accept_batch(forward_reference);
    expect_error(result, LifecycleDiagnosticCategory::causal_reference_unavailable,
                 "cash-correction");
    assert(ledger.empty());
  }
  {
    LifecycleLedger ledger;
    accept_successfully(ledger, LifecycleRecordDraft::originate(
        EconomicEventId{"cash-economic"}, recorded,
        cash("cash-v1", "fund-a", effective, "10", "source-a")));
    const auto before = ledger.records()[0];
    const auto result = ledger.accept(LifecycleRecordDraft::correct(
        EconomicEventId{"cash-economic"}, EventId{"cash-v1"}, recorded + 1ns,
        cash("cash-v2", "fund-b", effective, "12", "source-b")));
    expect_error(result, LifecycleDiagnosticCategory::incompatible_account, "cash-v2");
    assert(ledger.size() == 1 && ledger.records()[0] == before);
  }
  {
    LifecycleLedger ledger;
    accept_successfully(ledger, LifecycleRecordDraft::originate(
        EconomicEventId{"cash-economic"}, recorded,
        cash("event-v1", "fund-a", effective, "10", "source-a")));
    const auto result = ledger.accept(LifecycleRecordDraft::correct(
        EconomicEventId{"cash-economic"}, EventId{"event-v1"}, recorded + 1ns,
        trade("event-v2", "fund-a", effective, "MSFT", "1", "10", "source-b",
              2026y / January / 5d)));
    expect_error(result,
                 LifecycleDiagnosticCategory::incompatible_event_relationship,
                 "event-v2");
    assert(ledger.size() == 1);
  }
  {
    LifecycleLedger ledger;
    accept_successfully(ledger, LifecycleRecordDraft::originate(
        EconomicEventId{"cash-economic"}, recorded,
        cash("cash-v1", "fund-a", effective, "10", "source-a")));
    accept_successfully(ledger, LifecycleRecordDraft::correct(
        EconomicEventId{"cash-economic"}, EventId{"cash-v1"}, recorded + 1ns,
        cash("cash-v2", "fund-a", effective, "12", "source-b")));
    const auto result = ledger.accept(LifecycleRecordDraft::cancel(
        EventId{"cash-cancel"}, EconomicEventId{"cash-economic"},
        EventId{"cash-v1"}, AccountId{"fund-a"}, recorded + 2ns,
        provenance("source-c")));
    expect_error(result,
                 LifecycleDiagnosticCategory::conflicting_lifecycle_successor,
                 "cash-cancel");
    const auto cross_account = ledger.accept(LifecycleRecordDraft::cancel(
        EventId{"cross-account-cancel"}, EconomicEventId{"cash-economic"},
        EventId{"cash-v1"}, AccountId{"fund-b"}, recorded + 2ns,
        provenance("source-c")));
    expect_error(cross_account, LifecycleDiagnosticCategory::incompatible_account,
                 "cross-account-cancel");
    assert(ledger.size() == 2);
  }
  {
    LifecycleLedger ledger;
    accept_successfully(ledger, LifecycleRecordDraft::originate(
        EconomicEventId{"cash-economic"}, recorded,
        cash("cash-v1", "fund-a", effective, "10", "source-a")));
    accept_successfully(ledger, LifecycleRecordDraft::cancel(
        EventId{"cash-void"}, EconomicEventId{"cash-economic"},
        EventId{"cash-v1"}, AccountId{"fund-a"}, recorded + 1ns,
        provenance("source-b")));
    const auto result = ledger.accept(LifecycleRecordDraft::correct(
        EconomicEventId{"cash-economic"}, EventId{"cash-void"}, recorded + 2ns,
        cash("cross-account-replacement", "fund-b", effective, "12", "source-c")));
    expect_error(result, LifecycleDiagnosticCategory::incompatible_account,
                 "cross-account-replacement");
    assert(ledger.size() == 2);
  }
}

void relationship_and_identity_validation() {
  constexpr Timestamp effective{1h};
  constexpr Timestamp recorded{2h};

  {
    LifecycleLedger ledger;
    accept_successfully(ledger, LifecycleRecordDraft::originate(
        EconomicEventId{"cash-economic"}, recorded,
        cash("cash-origin", "fund-a", effective, "100", "source-a")));
    const auto wrong_offset = ledger.accept(LifecycleRecordDraft::reverse(
        EconomicEventId{"cash-reversal-economic"}, EventId{"cash-origin"},
        recorded + 1ns,
        cash("cash-reversal", "fund-a", effective + 1ns, "-99", "source-b")));
    expect_error(wrong_offset,
                 LifecycleDiagnosticCategory::incompatible_event_relationship,
                 "cash-reversal");
    assert(ledger.size() == 1);
  }
  {
    LifecycleLedger ledger;
    accept_successfully(ledger, LifecycleRecordDraft::originate(
        EconomicEventId{"trade-economic"}, recorded,
        trade("trade-origin", "fund-a", effective, "MSFT", "10", "50", "source-a",
              2026y / January / 5d)));
    const auto changed_key = ledger.accept(LifecycleRecordDraft::correct(
        EconomicEventId{"trade-economic"}, EventId{"trade-origin"}, recorded + 1ns,
        trade("trade-correction", "fund-a", effective, "AAPL", "10", "50",
              "source-b", 2026y / January / 5d)));
    expect_error(changed_key,
                 LifecycleDiagnosticCategory::incompatible_event_relationship,
                 "trade-correction");
    assert(ledger.size() == 1);
  }
  {
    LifecycleLedger ledger;
    accept_successfully(ledger, LifecycleRecordDraft::originate(
        EconomicEventId{"trade-economic"}, recorded,
        trade("trade-origin", "fund-a", effective, "MSFT", "10", "50", "source-a",
              2026y / January / 5d)));
    const auto wrong_terms = ledger.accept(LifecycleRecordDraft::reverse(
        EconomicEventId{"trade-reversal-economic"}, EventId{"trade-origin"},
        recorded + 1ns,
        trade("trade-reversal", "fund-a", effective + 1ns, "MSFT", "-10", "51",
              "source-b", 2026y / January / 6d)));
    expect_error(wrong_terms,
                 LifecycleDiagnosticCategory::incompatible_event_relationship,
                 "trade-reversal");
    assert(ledger.size() == 1);
  }
  {
    LifecycleLedger ledger;
    accept_successfully(ledger, LifecycleRecordDraft::originate(
        EconomicEventId{"cash-economic"}, recorded,
        cash("cash-origin", "fund-a", effective, "100", "source-a")));
    accept_successfully(ledger, LifecycleRecordDraft::reverse(
        EconomicEventId{"cash-reversal-economic"}, EventId{"cash-origin"},
        recorded + 1ns,
        cash("cash-reversal", "fund-a", effective + 1ns, "-100", "source-b")));
    const auto terminal = ledger.accept(LifecycleRecordDraft::correct(
        EconomicEventId{"cash-reversal-economic"}, EventId{"cash-reversal"},
        recorded + 2ns,
        cash("reversal-correction", "fund-a", effective + 1ns, "-100",
             "source-c")));
    expect_error(terminal,
                 LifecycleDiagnosticCategory::incompatible_event_relationship,
                 "reversal-correction");
    assert(ledger.size() == 2);
  }
  {
    LifecycleLedger ledger;
    accept_successfully(ledger, LifecycleRecordDraft::originate(
        EconomicEventId{"economic-1"}, recorded,
        cash("record-1", "fund-a", effective, "1", "source-a")));
    const auto duplicate_record = ledger.accept(LifecycleRecordDraft::originate(
        EconomicEventId{"economic-2"}, recorded + 1ns,
        cash("record-1", "fund-a", effective, "2", "source-b")));
    expect_error(duplicate_record, LifecycleDiagnosticCategory::duplicate_identity,
                 "record-1");
    const auto duplicate_origin = ledger.accept(LifecycleRecordDraft::originate(
        EconomicEventId{"economic-1"}, recorded + 1ns,
        cash("record-2", "fund-a", effective, "2", "source-b")));
    expect_error(duplicate_origin, LifecycleDiagnosticCategory::duplicate_identity,
                 "record-2");
    const auto time_regression = ledger.accept(LifecycleRecordDraft::originate(
        EconomicEventId{"economic-3"}, recorded - 1ns,
        cash("record-3", "fund-a", effective, "3", "source-c")));
    expect_error(time_regression,
                 LifecycleDiagnosticCategory::deterministic_ordering, "record-3");
    assert(ledger.size() == 1);
    assert(ledger.find(EventId{"record-1"}) != nullptr);
    assert(ledger.find(EventId{"missing"}) == nullptr);
  }
}

}  // namespace

int main() {
  static_assert(LifecycleSequence::first_value == 1);
  static_assert(!std::is_convertible_v<EventId, EconomicEventId>);
  static_assert(std::is_same_v<decltype(std::declval<const LifecycleLedger&>().records()),
                               std::span<const LifecycleRecord>>);
  static_assert(std::variant_size_v<EconomicEvent> == 2);

  cash_correction_and_cancellation();
  equity_correction_and_reversal();
  deterministic_replay_across_acceptance_paths();
  stable_causal_diagnostics();
  relationship_and_identity_validation();
}
