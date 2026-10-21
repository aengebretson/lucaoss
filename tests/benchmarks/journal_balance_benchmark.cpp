#include "benchmark.hpp"
#include <luca/accounting/balances.hpp>

#include <array>
#include <memory>

namespace luca::bench {
namespace {
template <class T, class E> T checked(std::expected<T, E> result) {
  if (!result) throw std::runtime_error("journal balance benchmark failed");
  return std::move(*result);
}
}

void add_journal_balance_benchmarks(std::vector<Benchmark> &benchmarks,
                                    const Configuration &configuration) {
  if (!configuration.filter.empty() &&
      std::string{"journal_balances.full"}.find(configuration.filter) == std::string::npos &&
      std::string{"journal_balances.merge_prepared_4"}.find(configuration.filter) == std::string::npos)
    return;
  using namespace std::chrono;
  const auto date = year{2026} / June / day{10};
  const Timestamp time{sys_days{date}};
  const auto policy = checked(AccountingPolicyIdentity::create(AccountingPolicyId{"benchmark.cash"}, "1"));
  const auto rule = checked(JournalBalanceContract::create({
      BookId{"benchmark.book"}, "synthetic-revision-1", "benchmark-engine-1", "benchmark-map-1",
      policy, time, time, date}));
  auto entries = std::make_shared<std::vector<JournalEntry>>();
  entries->reserve(configuration.events);
  for (std::size_t index = 0; index < configuration.events; ++index) {
    const auto id = std::to_string(index);
    const auto currency = checked(Currency::from_code(std::array{"USD", "EUR", "GBP"}[index % 3]));
    const auto amount = Money::from_scaled(1'000'000, currency);
    const JournalEntryId entry{"entry-" + id};
    const auto lineage = checked(JournalLineage::create({EventId{"record-" + id}},
        {EconomicEventId{"event-" + id}}, {SourceRecordId{"source-" + id}}));
    auto debit = checked(JournalLine::create(JournalLineId{id + ".debit"}, entry,
        AccountId{"asset.cash." + std::to_string(index % 100)}, JournalSide::debit, amount, lineage));
    auto credit = checked(JournalLine::create(JournalLineId{id + ".credit"}, entry,
        AccountId{"equity.capital"}, JournalSide::credit, amount, lineage));
    entries->push_back(checked(JournalEntry::create(entry, policy,
        JournalEventType::cash_movement, EventId{"record-" + id}, time, time,
        checked(JournalDate::create(date)), RecognitionPhase::immediate, 0,
        checked(JournalSettlementContext::create({}, {}, "immediate")), lineage,
        {std::move(debit), std::move(credit)})));
  }
  // Build partials outside timing. The merge case measures merging already
  // computed results, including all evidence; it is not end-to-end parallel speedup.
  auto partials = std::make_shared<std::vector<JournalBalanceResult>>();
  const std::span<const JournalEntry> all{*entries};
  for (std::size_t part = 0; part < 4; ++part) {
    const auto first = all.size() * part / 4;
    const auto last = all.size() * (part + 1) / 4;
    partials->push_back(checked(reduce_journal_balances(rule, all.subspan(first, last - first))));
  }
  const auto validate = [rule, entries, partials] {
    const auto full = checked(reduce_journal_balances(rule, *entries));
    const auto merged = checked(merge_journal_balances(rule, *partials));
    if (full != merged || full.evidence().size() != entries->size() || full.controls().size() != 3)
      throw std::runtime_error("journal balance benchmark result mismatch");
    std::int64_t total = 0;
    for (const auto &control : full.controls()) {
      if (control.debits != control.credits || control.debit_balances != control.credit_balances)
        throw std::runtime_error("journal balance benchmark control mismatch");
      total += control.debits.scaled_value();
    }
    if (total != static_cast<std::int64_t>(entries->size()) * 1'000'000)
      throw std::runtime_error("journal balance benchmark expected total mismatch");
    return full.balances().size();
  };
  benchmarks.push_back({"journal_balances.full", configuration.events, "journal_entries", validate,
      [rule, entries] { return checked(reduce_journal_balances(rule, *entries)).evidence().size(); }});
  benchmarks.push_back({"journal_balances.merge_prepared_4", configuration.events, "journal_entries", validate,
      [rule, partials] { return checked(merge_journal_balances(rule, *partials)).evidence().size(); }});
}
} // namespace luca::bench
