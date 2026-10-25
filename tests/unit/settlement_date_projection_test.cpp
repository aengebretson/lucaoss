// Keep financial-semantic assertions enabled in release builds.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "luca/accounting/settlement_date_projection.hpp"
#include "luca/accounting/trade_date_projection.hpp"
#include "luca/portfolio/lifecycle_projection.hpp"
#include "../../examples/settlement-date-accounting/fixture.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <string_view>
#include <type_traits>

using namespace luca;
using namespace settlement_date_example;

namespace {
using Category = SettlementDateProjectionDiagnosticCategory;
using Outcome = std::expected<SettlementDateProjectionResult, SettlementDateProjectionError>;

void accept(LifecycleLedger &ledger, LifecycleRecordDraft draft) {
  require(ledger.accept(draft));
}

SettlementDateProjectionContext context(const Evaluation &e) {
  return {e.recorded_through, e.economic_as_of, e.settlement_as_of_date};
}

void error(const Outcome &result, Category category, std::string_view record = {}) {
  assert(!result);
  assert(result.error().category() == category);
  assert(result.error().category_name() == luca::category_name(category));
  assert(!result.error().message().empty());
  if (!record.empty()) {
    assert(result.error().record_id());
    assert(result.error().record_id()->value() == record);
  }
}

template <class Result>
Money balance(const Result &result, std::string_view account) {
  auto total = Money::from_scaled(0, require(Currency::from_code("USD")));
  for (const auto &entry : result.entries()) {
    assert(entry.debit_total() == entry.credit_total());
    for (const auto &line : entry.lines()) {
      assert(line.lineage() == entry.lineage());
      assert(line.amount().scaled_value() > 0);
      if (line.account_id().value() == account) {
        auto signed_amount = Money::from_scaled(
            line.side() == JournalSide::debit ? line.amount().scaled_value()
                                             : -line.amount().scaled_value(),
            line.amount().currency());
        total = require(total.add(signed_amount));
      }
    }
  }
  return total;
}

void entry(const JournalEntry &value, const char *id, const char *amount,
           const char *debit, const char *credit, year_month_day recognized) {
  assert(value.journal_entry_id() == JournalEntryId{id});
  assert(value.policy().id() == AccountingPolicyId{"fixture.settlement-date.v1"});
  assert(value.policy().version() == "1");
  assert(value.recognized_on() == require(JournalDate::create(recognized)));
  assert(value.debit_total() == require(Money::parse(amount, require(Currency::from_code("USD")))));
  assert(value.debit_total() == value.credit_total());
  assert(value.lines().size() == 2);
  assert(value.lines()[0].account_id() == AccountId{debit});
  assert(value.lines()[1].account_id() == AccountId{credit});
  for (std::size_t i = 0; i < 2; ++i) {
    const auto &line = value.lines()[i];
    assert(line.side() == (i == 0 ? JournalSide::debit : JournalSide::credit));
    assert(line.amount() == value.debit_total());
    assert(line.journal_entry_id() == value.journal_entry_id());
    assert(line.journal_line_id().value() == std::string{id} + (i == 0 ? ".debit" : ".credit"));
    assert(line.lineage() == value.lineage());
  }
}

void walkthrough() {
  auto ledger = history();
  LifecycleLedger individually_accepted;
  for (const auto &draft : drafts())
    accept(individually_accepted, draft);
  const std::array<std::size_t, 5> counts{1, 1, 2, 2, 3};
  const std::array<std::int64_t, 5> cash_values{
      100'000'000'000LL, 100'000'000'000LL, 95'600'000'000LL, 95'600'000'000LL, 100'000'000'000LL};
  const std::array<std::int64_t, 5> security_values{0, 0, 4'400'000'000LL, 4'400'000'000LL, 0};
  const std::array<std::int64_t, 5> quantities{10'000'000'000LL, 8'000'000'000LL, 8'000'000'000LL, 0, 0};
  const std::array<std::int64_t, 5> open_values{5'000'000'000LL, 4'400'000'000LL, 0, 4'400'000'000LL, 0};
  const std::array<std::int64_t, 5> totals{
      100'000'000'000LL, 100'000'000'000LL, 104'400'000'000LL, 104'400'000'000LL, 108'800'000'000LL};
  const auto cases = evaluations();
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const auto c = context(cases[i]);
    const auto resolved = ledger.resolve(c.recorded_through, c.economic_as_of);
    const auto journals = project_settlement_date_journals(resolved, c);
    assert(journals && journals->entries().size() == counts[i]);
    assert(journals->evaluation_context() == c);
    assert(journals->engine_version() == "fixture-accounting-engine-1");
    assert(journals->projection_version() == "fixture-journal-projection-1");
    assert(journals->lifecycle_contract_version() == "luca.event-lifecycle.v1");
    assert(journals->active_record_ids().size() == (i < 3 ? 2 : 3));
    assert(journals->economic_event_ids().size() == journals->active_record_ids().size());
    assert(journals->lifecycle_record_ids().size() == (i == 0 ? 2 : (i < 3 ? 3 : 4)));
    assert(journals->source_record_ids().size() == journals->lifecycle_record_ids().size());
    assert(*journals == require(project_settlement_date_journals(resolved, c)));
    assert(*journals == require(project_settlement_date_journals(
        individually_accepted.resolve(c.recorded_through, c.economic_as_of), c)));
    entry(journals->entries()[0], "sd.opening-cash.immediate", "100000", "asset.cash",
          "equity.contributed-capital", 2026y / May / 29d);
    assert(journals->entries()[0].phase_ordinal() == 0);
    assert(journals->entries()[0].recognition_phase() == RecognitionPhase::immediate);
    assert(journals->entries()[0].effective_at() == at(2026y / May / 29d));
    assert(!journals->entries()[0].settlement_context().trade_date());
    assert(journals->entries()[0].settlement_context().recognition_rule() == "immediate");
    if (i >= 2) {
      const auto &purchase = journals->entries()[1];
      entry(purchase, "sd.trade-v2.settlement", "4400", "asset.equity-securities", "asset.cash",
            2026y / June / 4d);
      assert(purchase.effective_at() == at(2026y / June / 2d, 14h));
      assert(purchase.recorded_at() == at(2026y / June / 3d, 9h + 1min));
      assert(purchase.recognition_phase() == RecognitionPhase::settlement_date);
      assert(purchase.phase_ordinal() == 1);
      assert(purchase.settlement_context().recognition_rule() == "settlement_date");
      assert(purchase.settlement_context().trade_date() == require(JournalDate::create(2026y / June / 2d)));
      assert(purchase.lineage().record_ids().size() == 2);
      assert(purchase.lineage().record_ids()[0] == EventId{"trade-record-v1"});
      assert(purchase.lineage().record_ids()[1] == EventId{"trade-record-v2"});
      assert(purchase.lineage().source_record_ids()[0] == SourceRecordId{"trade-source-original"});
      assert(purchase.lineage().source_record_ids()[1] == SourceRecordId{"trade-source-correction"});
    }
    if (i == 4) {
      const auto &reversal = journals->entries()[2];
      entry(reversal, "sd.reversal.settlement", "4400", "asset.cash", "asset.equity-securities",
            2026y / June / 6d);
      assert(reversal.effective_at() == at(2026y / June / 5d, 10h));
      assert(reversal.recorded_at() == at(2026y / June / 7d, 9h + 1min));
      assert(reversal.lineage().reverses_record_id() == EventId{"trade-record-v2"});
      assert(reversal.lineage().record_ids().size() == 1);
      assert(reversal.lineage().source_record_ids()[0] == SourceRecordId{"trade-source-reversal"});
    }
    assert(balance(*journals, "asset.cash").scaled_value() == cash_values[i]);
    assert(balance(*journals, "asset.equity-securities").scaled_value() == security_values[i]);
    assert(balance(*journals, "liability.trade-payable").scaled_value() == 0);
    assert(balance(*journals, "asset.trade-receivable").scaled_value() == 0);
    std::int64_t total = 0; // Bounded fixture values, far below int64 maximum.
    for (const auto &value : journals->entries()) total += value.debit_total().scaled_value();
    assert(total == totals[i]);
    const auto td = require(project_trade_date_journals(
        resolved, {c.recorded_through, c.economic_as_of, c.settlement_as_of_date}));
    assert(balance(td, "asset.cash") == balance(*journals, "asset.cash"));
    const auto portfolio = project_lifecycle(resolved, {c.economic_as_of, c.settlement_as_of_date});
    assert(portfolio.positions && portfolio.settled_cash && portfolio.open_settlement_obligations);
    assert(portfolio.settled_cash->front().amount() == balance(*journals, "asset.cash"));
    if (quantities[i] == 0) assert(portfolio.positions->empty());
    else assert(portfolio.positions->front().quantity().scaled_value() == quantities[i]);
    if (open_values[i] == 0) assert(portfolio.open_settlement_obligations->empty());
    else {
      assert(portfolio.open_settlement_obligations->size() == 1);
      const auto &obligation = portfolio.open_settlement_obligations->front();
      assert(obligation.amount().scaled_value() == open_values[i]);
      assert(obligation.key().direction() == (i == 3 ? SettlementDirection::receivable
                                                    : SettlementDirection::payable));
    }
  }
  // An independently advanced settlement cutoff retains the historical original.
  auto c = context(cases[0]);
  c.settlement_as_of_date = 2026y / June / 4d;
  const auto original = require(project_settlement_date_journals(
      ledger.resolve(c.recorded_through, c.economic_as_of), c));
  entry(original.entries()[1], "sd.trade-v1.settlement", "5000", "asset.equity-securities",
        "asset.cash", 2026y / June / 4d);
  assert(original.entries()[1].recorded_at() == at(2026y / June / 2d, 14h + 2min));
  // Accepted payloads remain immutable after every policy and cutoff evaluation.
  assert(ledger.records().size() == 4);
  assert(std::get<EquityTrade>(*ledger.records()[1].event()).quantity() == require(Quantity::parse("100")));
  assert(std::get<EquityTrade>(*ledger.records()[2].event()).quantity() == require(Quantity::parse("80")));
}

void boundaries_and_diagnostics() {
  const auto effective = at(2026y / June / 2d, 10h);
  const auto recorded = effective + 1h;
  const auto cutoff = effective + 2h;
  const auto settle = 2026y / June / 4d;
  const SettlementDateProjectionContext unsettled{cutoff, cutoff, 2026y / June / 2d};
  const SettlementDateProjectionContext settled{cutoff, cutoff, settle};
  const auto project_one = [&](EconomicEvent event, SettlementDateProjectionContext c) {
    LifecycleLedger ledger;
    accept(ledger, LifecycleRecordDraft::originate(EconomicEventId{"economic"}, recorded,
                                                   std::move(event)));
    return project_settlement_date_journals(ledger.resolve(cutoff, cutoff), c);
  };
  // Half-even ties in either direction; rounding happens once at the Money boundary.
  for (const auto *price : {"150", "250"}) {
    const auto event = trade("rounded", effective, "source", "0.00000001", price, settle);
    const auto before = project_one(event, unsettled);
    assert(before && before->entries().empty() && before->active_record_ids().size() == 1);
    const auto after = project_one(event, settled);
    assert(after && after->entries().size() == 1);
    assert(after->entries()[0].debit_total().scaled_value() == 2);
  }
  for (const auto c : {unsettled, settled}) {
    error(project_one(cash("withdrawal", effective, "source", "-1"), c),
          Category::unsupported_event, "withdrawal");
    error(project_one(cash("zero", effective, "source", "0"), c), Category::unsupported_event, "zero");
    error(project_one(trade("sell", effective, "source", "-1", "50", settle), c),
          Category::unsupported_event, "sell");
    error(project_one(trade("zero-price", effective, "source", "1", "0", settle), c),
          Category::unsupported_event, "zero-price");
    error(project_one(trade("rounds-zero", effective, "source", "0.00000001", "50", settle), c),
          Category::unsupported_event, "rounds-zero");
    error(project_one(trade("bad-date", effective, "source", "1", "50", 2026y / June / 1d), c),
          Category::invalid_context, "bad-date");
    error(project_one(cash("eur-cash", effective, "source", "1", "EUR"), c),
          Category::unsupported_currency, "eur-cash");
    error(project_one(trade("eur-trade", effective, "source", "1", "50", settle, "EUR"), c),
          Category::unsupported_currency, "eur-trade");
    error(project_one(trade("overflow", effective, "source", "92233720368.54775807",
                           "92233720368.54775807", settle), c),
          Category::arithmetic_overflow, "overflow");
  }
  // Cutoffs are inclusive and independent; cash is immediate even with an earlier settlement date.
  const auto contribution = cash("cash", effective, "source", "1");
  LifecycleLedger inclusive_ledger;
  accept(inclusive_ledger,
         LifecycleRecordDraft::originate(EconomicEventId{"inclusive"}, recorded, contribution));
  const auto inclusive = project_settlement_date_journals(
      inclusive_ledger.resolve(recorded, effective), {recorded, effective, 2026y / May / 1d});
  assert(inclusive && inclusive->entries().size() == 1);
  error(project_one(contribution, {recorded - 1ns, cutoff, settle}), Category::invalid_context, "cash");
  error(project_one(contribution, {cutoff, effective - 1ns, settle}), Category::invalid_context, "cash");
  error(project_one(contribution, {cutoff, cutoff, 2026y / February / 30d}), Category::invalid_context);

  // Cash reversal is accepted by lifecycle, but excluded by this accounting policy.
  LifecycleLedger cash_reversal;
  accept(cash_reversal, LifecycleRecordDraft::originate(EconomicEventId{"cash"}, recorded,
      cash("cash-target", effective, "source", "10")));
  accept(cash_reversal, LifecycleRecordDraft::reverse(EconomicEventId{"reverse"},
      EventId{"cash-target"}, recorded + 1s, cash("cash-reverse", effective + 1s, "reverse-source", "-10")));
  error(project_settlement_date_journals(cash_reversal.resolve(cutoff, cutoff), unsettled),
        Category::invalid_reversal_treatment, "cash-reverse");

  // Alias collisions fail even if neither entry (or only one) is recognized yet.
  for (const auto second_settlement : {settle, 2026y / June / 2d}) {
    LifecycleLedger collisions;
    accept(collisions, LifecycleRecordDraft::originate(EconomicEventId{"one"}, recorded,
        trade("trade-record-v1", effective, "source", "1", "50", settle)));
    accept(collisions, LifecycleRecordDraft::originate(EconomicEventId{"two"}, recorded,
        trade("trade-v1", effective, "source-two", "1", "50", second_settlement)));
    for (const auto c : {unsettled, settled})
      error(project_settlement_date_journals(collisions.resolve(cutoff, cutoff), c),
            Category::journal_invariant, "trade-v1");
  }
  // Invalid partial reversal cannot reach a public LifecycleResolution.
  LifecycleLedger partial;
  accept(partial, LifecycleRecordDraft::originate(EconomicEventId{"purchase"}, recorded,
      trade("target", effective, "source", "10", "50", settle)));
  const auto rejected = partial.accept(LifecycleRecordDraft::reverse(EconomicEventId{"reverse"},
      EventId{"target"}, recorded + 1s, trade("partial", effective + 1s, "reverse-source", "-9", "50", settle)));
  assert(!rejected);
  assert(rejected.error().category() == LifecycleDiagnosticCategory::incompatible_event_relationship);

  // Reversing a sell is an exact lifecycle offset but is not a purchase reversal.
  LifecycleLedger reverse_sell;
  accept(reverse_sell, LifecycleRecordDraft::originate(EconomicEventId{"sell"}, recorded,
      trade("sell-target", effective, "source", "-10", "50", settle)));
  accept(reverse_sell, LifecycleRecordDraft::reverse(EconomicEventId{"reverse-sell"},
      EventId{"sell-target"}, recorded + 1s,
      trade("positive-reversal", effective + 1s, "reverse-source", "10", "50", settle)));
  // The unsupported original sell rejects the complete result, before any journal is returned.
  error(project_settlement_date_journals(reverse_sell.resolve(cutoff, cutoff), unsettled),
        Category::unsupported_event, "sell-target");
}

void order_ownership_and_cancellation() {
  const auto effective = at(2026y / June / 2d);
  const auto recorded = effective + 1h;
  const auto end = at(2026y / June / 9d);
  LifecycleLedger ledger;
  // Acceptance order and economic order differ from settlement presentation order.
  accept(ledger, LifecycleRecordDraft::originate(EconomicEventId{"later"}, recorded,
      trade("a-later", effective, "shared-source", "1", "1", 2026y / June / 5d)));
  accept(ledger, LifecycleRecordDraft::originate(EconomicEventId{"first-tie"}, recorded,
      trade("z-first-tie", effective + 1s, "shared-source", "1", "1", 2026y / June / 4d)));
  accept(ledger, LifecycleRecordDraft::originate(EconomicEventId{"second-tie"}, recorded,
      trade("a-second-tie", effective, "third-source", "1", "1", 2026y / June / 4d)));
  const auto result = require(project_settlement_date_journals(
      ledger.resolve(end, end), {end, end, 2026y / June / 9d}));
  assert(result.entries()[0].active_record_id() == EventId{"z-first-tie"});
  assert(result.entries()[1].active_record_id() == EventId{"a-second-tie"});
  assert(result.entries()[2].active_record_id() == EventId{"a-later"});
  assert(result.active_record_ids()[1] == EventId{"a-second-tie"});
  assert(result.source_record_ids().size() == 2);
  assert(result.source_record_ids()[0] == SourceRecordId{"shared-source"});

  const auto owned = [] {
    auto temporary = history();
    const auto e = evaluations().back();
    return require(project_settlement_date_journals(
        temporary.resolve(e.recorded_through, e.economic_as_of), context(e)));
  }(); // No ledger or resolution survives; entries/lineage/context are owned.
  assert(owned.entries()[1].lineage().record_ids()[0] == EventId{"trade-record-v1"});

  const auto cancelled = ledger.accept(LifecycleRecordDraft::cancel(EventId{"cancel-later"},
      EconomicEventId{"later"}, EventId{"a-later"}, AccountId{"fund-a"}, recorded + 1s,
      require(Provenance::create({SourceRecordId{"cancel-source"}}, "fixture", "1"))));
  assert(cancelled);
  const auto without = require(project_settlement_date_journals(
      ledger.resolve(end, end), {end, end, 2026y / June / 9d}));
  assert(without.entries().size() == 2); // No compensating entry inferred for cancellation.
  assert(result.entries().size() == 3); // Previously returned value unchanged.
  LifecycleLedger empty;
  const auto identity = require(project_settlement_date_journals(
      empty.resolve(end, end), {end, end, 2026y / June / 9d}));
  assert(identity.entries().empty() && identity.active_record_ids().empty());
}
} // namespace

int main() {
  static_assert(std::is_same_v<decltype(std::declval<const SettlementDateProjectionResult &>().entries()),
                               std::span<const JournalEntry>>);
  walkthrough();
  boundaries_and_diagnostics();
  order_ownership_and_cancellation();
}
