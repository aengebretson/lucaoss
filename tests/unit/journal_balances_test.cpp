#include <luca/accounting/balances.hpp>
#include <luca/accounting/settlement_date_projection.hpp>
#include <luca/accounting/trade_date_projection.hpp>
#include "../../examples/settlement-date-accounting/fixture.hpp"

#include <array>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <sstream>
#include <source_location>
#include <type_traits>

namespace {
using namespace luca;
using namespace settlement_date_example;

void check(bool condition, std::source_location location = std::source_location::current()) {
  if (!condition) {
    std::cerr << "journal balance check failed at " << location.file_name() << ':'
              << location.line() << '\n';
    std::abort();
  }
}

JournalBalanceContract contract(const char *policy = "fixture.trade-date.v1") {
  return require(JournalBalanceContract::create({
      BookId{"fund-a.book"}, "fixture-history-revision-1", "fixture-accounting-engine-1",
      "fixture-journal-projection-1",
      require(AccountingPolicyIdentity::create(AccountingPolicyId{policy}, "1")),
      at(2026y / June / 10d), at(2026y / June / 10d), 2026y / June / 10d}));
}

JournalEntry posting(std::string id, std::int64_t amount, const char *currency = "USD",
                     const char *policy = "fixture.trade-date.v1",
                     std::string debit_line = {}, const char *debit_account = "asset.cash",
                     const char *credit_account = "equity.contributed-capital") {
  const auto value = Money::from_scaled(amount, require(Currency::from_code(currency)));
  const JournalEntryId entry{id};
  auto lineage = require(JournalLineage::create({EventId{"record-" + id}},
      {EconomicEventId{"event-" + id}}, {SourceRecordId{"source-" + id}}));
  auto debit = require(JournalLine::create(JournalLineId{debit_line.empty() ? id + ".debit" : debit_line},
      entry, AccountId{debit_account}, JournalSide::debit, value, lineage));
  auto credit = require(JournalLine::create(JournalLineId{id + ".credit"}, entry,
      AccountId{credit_account}, JournalSide::credit, value, lineage));
  return require(JournalEntry::create(entry,
      require(AccountingPolicyIdentity::create(AccountingPolicyId{policy}, "1")),
      JournalEventType::cash_movement, EventId{"record-" + id}, at(2026y / June / 1d),
      at(2026y / June / 2d), require(JournalDate::create(2026y / June / 1d)),
      RecognitionPhase::immediate, 0,
      require(JournalSettlementContext::create({}, {}, "immediate")), lineage,
      {std::move(debit), std::move(credit)}));
}

std::int64_t net(const JournalBalanceResult &result, std::string_view account) {
  for (const auto &row : result.balances())
    if (row.key.account.value() == account) return row.net.scaled_value();
  return 0;
}

template <class T>
void expect_error(const std::expected<T, JournalBalanceError> &result, JournalBalanceError error) {
  check(!result && result.error() == error);
  check(!category_name(error).empty());
}

template <class Projection>
void walkthrough(const Projection &projected, const Evaluation &evaluation, std::istringstream &row) {
  auto definition = contract(projected.policy().id().value().c_str()).definition();
  definition.recorded_through = evaluation.recorded_through;
  definition.economic_as_of = evaluation.economic_as_of;
  definition.settlement_as_of_date = evaluation.settlement_as_of_date;
  const auto rule = require(JournalBalanceContract::create(std::move(definition)));
  const auto result = require(reduce_journal_balances(rule, projected.entries()));
  std::size_t count{};
  std::int64_t gross{}, closing{}, cash{}, securities{}, payable{}, receivable{}, capital{};
  check(static_cast<bool>(row >> count >> gross >> closing >> cash >> securities >> payable >> receivable >> capital));
  std::string extra;
  check(!(row >> extra));
  check(result.balances().size() == count && result.controls().size() == 1);
  const auto &control = result.controls().front();
  check(control.debits.scaled_value() == gross && control.debits == control.credits);
  check(control.debit_balances.scaled_value() == closing &&
        control.debit_balances == control.credit_balances);
  check(net(result, "asset.cash") == cash);
  check(net(result, "asset.equity-securities") == securities);
  check(net(result, "liability.trade-payable") == payable);
  check(net(result, "asset.trade-receivable") == receivable);
  check(net(result, "equity.contributed-capital") == capital);
  check(result.evidence().size() == projected.entries().size());
  for (const auto &balance : result.balances()) {
    check(balance.key.book == BookId{"fund-a.book"});
    check(balance.net == require(balance.debits.subtract(balance.credits)));
    check(!balance.lines.empty() && std::ranges::is_sorted(balance.lines));
    for (const auto &reference : balance.lines) {
      const auto entry = std::ranges::find(projected.entries(), reference.entry, &JournalEntry::journal_entry_id);
      check(entry != projected.entries().end());
      const auto line = std::ranges::find(entry->lines(), reference.line, &JournalLine::journal_line_id);
      check(line != entry->lines().end());
      check(balance.key.account.value() == line->account_id().value());
      const auto evidence = std::ranges::find(result.evidence(), reference.entry, &JournalEntryEvidence::entry);
      check(evidence != result.evidence().end() && evidence->lineage == entry->lineage());
    }
  }

  // Real independent workers reduce disjoint whole entries under one context.
  const auto entries = projected.entries();
  const auto split = entries.size() / 2;
  auto first = std::async(std::launch::async, [&] {
    return require(reduce_journal_balances(rule, entries.first(split)));
  });
  auto second = std::async(std::launch::async, [&] {
    return require(reduce_journal_balances(rule, entries.subspan(split)));
  });
  const auto identity = require(reduce_journal_balances(rule, {}));
  const std::array partials{second.get(), identity, first.get()};
  check(require(merge_journal_balances(rule, partials)) == result);
  const auto prefix = require(merge_journal_balances(rule, std::span{partials}.first(2)));
  const std::array nested{prefix, partials.back()};
  check(require(merge_journal_balances(rule, nested)) == result);
  check(require(merge_journal_balances(rule, {})) == identity);
  std::vector<JournalEntry> reversed{entries.rbegin(), entries.rend()};
  check(require(reduce_journal_balances(rule, reversed)) == result);
}

void fixture_walkthrough() {
  auto ledger = history();
  const auto evaluations_list = evaluations();
  std::ifstream fixture{LUCA_JOURNAL_BALANCE_FIXTURE};
  check(fixture.good());
  std::string text;
  std::set<std::pair<std::size_t, std::string>> seen;
  while (std::getline(fixture, text)) {
    if (text.empty() || text.front() == '#') continue;
    std::istringstream row{text};
    std::size_t view{};
    std::string policy;
    check(static_cast<bool>(row >> view >> policy));
    check(view < evaluations_list.size() && (policy == "td" || policy == "sd"));
    check(seen.emplace(view, policy).second);
    const auto &evaluation = evaluations_list[view];
    const auto resolved = ledger.resolve(evaluation.recorded_through, evaluation.economic_as_of);
    if (policy == "td")
      walkthrough(require(project_trade_date_journals(resolved,
          {evaluation.recorded_through, evaluation.economic_as_of, evaluation.settlement_as_of_date})), evaluation, row);
    else
      walkthrough(require(project_settlement_date_journals(resolved,
          {evaluation.recorded_through, evaluation.economic_as_of, evaluation.settlement_as_of_date})), evaluation, row);
  }
  check(seen.size() == 10);
}

void boundaries() {
  static_assert(!std::is_same_v<BookId, ChartAccountId>);
  static_assert(!std::is_same_v<ChartAccountId, AccountId>);
  const auto rule = contract();
  const std::array entries{posting("a", 10), posting("b", 20)};
  const auto result = require(reduce_journal_balances(rule, entries));
  check(net(result, "asset.cash") == 30);

  const std::array duplicate{entries[0], entries[0]};
  expect_error(reduce_journal_balances(rule, duplicate), JournalBalanceError::duplicate_entry);
  const std::array shared_line{posting("a", 1, "USD", "fixture.trade-date.v1", "shared"),
                               posting("b", 1, "USD", "fixture.trade-date.v1", "shared")};
  expect_error(reduce_journal_balances(rule, shared_line), JournalBalanceError::duplicate_line);
  const auto a = require(reduce_journal_balances(rule, std::span{shared_line}.first(1)));
  const auto b = require(reduce_journal_balances(rule, std::span{shared_line}.last(1)));
  const std::array shared_partials{a, b};
  expect_error(merge_journal_balances(rule, shared_partials), JournalBalanceError::duplicate_line);
  const std::array repeated{result, result};
  expect_error(merge_journal_balances(rule, repeated), JournalBalanceError::duplicate_entry);
  const std::array other_policy{posting("c", 1, "USD", "other-policy")};
  expect_error(reduce_journal_balances(rule, other_policy), JournalBalanceError::policy_mismatch);

  // Each contract field participates in merge compatibility, even for identity.
  for (int field = 0; field != 9; ++field) {
    auto changed = rule.definition();
    switch (field) {
    case 0: changed.book = BookId{"other-book"}; break;
    case 1: changed.input_revision = "other-revision"; break;
    case 2: changed.engine_version = "other-engine"; break;
    case 3: changed.journal_projection_version = "other-projection"; break;
    case 4: changed.accounting_policy = require(AccountingPolicyIdentity::create(AccountingPolicyId{"other-policy"}, "2")); break;
    case 5: changed.recorded_through += 1s; break;
    case 6: changed.economic_as_of += 1s; break;
    case 7: changed.settlement_as_of_date = 2026y / June / 11d; break;
    case 8: changed.accounting_policy = require(AccountingPolicyIdentity::create(AccountingPolicyId{"fixture.trade-date.v1"}, "2")); break;
    }
    const auto other = require(JournalBalanceContract::create(std::move(changed)));
    const std::array partials{require(reduce_journal_balances(other, {}))};
    expect_error(merge_journal_balances(rule, partials), JournalBalanceError::incompatible_contract);
  }
  auto changed = rule.definition();
  changed.book = BookId{""};
  expect_error(JournalBalanceContract::create(changed), JournalBalanceError::invalid_contract);
  changed = rule.definition();
  changed.input_revision = "bad revision";
  expect_error(JournalBalanceContract::create(changed), JournalBalanceError::invalid_contract);
  changed = rule.definition();
  changed.settlement_as_of_date = 2026y / February / 30d;
  expect_error(JournalBalanceContract::create(changed), JournalBalanceError::invalid_contract);
  changed = rule.definition();
  changed.recorded_through = at(2026y / June / 1d);
  expect_error(reduce_journal_balances(require(JournalBalanceContract::create(changed)), entries), JournalBalanceError::outside_cutoff);
  changed = rule.definition();
  changed.economic_as_of = at(2026y / May / 31d);
  expect_error(reduce_journal_balances(require(JournalBalanceContract::create(changed)), entries), JournalBalanceError::outside_cutoff);
  changed = rule.definition();
  changed.accounting_policy = require(AccountingPolicyIdentity::create(AccountingPolicyId{"fixture.trade-date.v1"}, "2"));
  expect_error(reduce_journal_balances(require(JournalBalanceContract::create(changed)), entries), JournalBalanceError::policy_mismatch);

  auto ledger = history();
  const auto view = evaluations()[2];
  const auto journals = require(project_trade_date_journals(
      ledger.resolve(view.recorded_through, view.economic_as_of),
      {view.recorded_through, view.economic_as_of, view.settlement_as_of_date}));
  changed = rule.definition();
  changed.settlement_as_of_date = 2026y / June / 3d;
  expect_error(reduce_journal_balances(require(JournalBalanceContract::create(changed)), journals.entries()), JournalBalanceError::outside_cutoff);

  // Independent currency controls; values are never converted or netted together.
  const std::array currencies{entries[0], posting("eur", 100, "EUR")};
  const auto multi = require(reduce_journal_balances(rule, currencies));
  check(multi.controls().size() == 2 && multi.balances().size() == 4);
  check(multi.controls()[0].currency.code() == "EUR" && multi.controls()[1].currency.code() == "USD");

  // Monotone gross turnover, including currency totals across different accounts,
  // defines the representable domain for every partition/grouping.
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  const std::array large{posting("max", maximum), posting("one", 1)};
  expect_error(reduce_journal_balances(rule, large), JournalBalanceError::amount_overflow);
  const std::array large_partials{
      require(reduce_journal_balances(rule, std::span{large}.first(1))),
      require(reduce_journal_balances(rule, std::span{large}.last(1)))};
  expect_error(merge_journal_balances(rule, large_partials), JournalBalanceError::amount_overflow);
  const std::array safe{posting("almost", maximum - 1), posting("last", 1)};
  check(require(reduce_journal_balances(rule, safe)).controls().front().debits.scaled_value() == maximum);
  const std::array zero_net{posting("up", maximum), posting("down", maximum, "USD",
      "fixture.trade-date.v1", {}, "equity.contributed-capital", "asset.cash")};
  expect_error(reduce_journal_balances(rule, zero_net), JournalBalanceError::amount_overflow);

  const std::array three{entries[0], entries[1], posting("c", 30)};
  const auto full = require(reduce_journal_balances(rule, three));
  const auto one = require(reduce_journal_balances(rule, std::span{three}.first(1)));
  const auto two = require(reduce_journal_balances(rule, std::span{three}.subspan(1, 1)));
  const auto third = require(reduce_journal_balances(rule, std::span{three}.last(1)));
  const std::array left_pair{one, two};
  const std::array right_pair{two, third};
  const std::array left{require(merge_journal_balances(rule, left_pair)), third};
  const std::array right{one, require(merge_journal_balances(rule, right_pair))};
  check(require(merge_journal_balances(rule, left)) == full);
  check(require(merge_journal_balances(rule, right)) == full);
  const auto unchanged = full;
  const std::array repeated_full{full, full};
  expect_error(merge_journal_balances(rule, repeated_full), JournalBalanceError::duplicate_entry);
  check(full == unchanged);
  const auto owned = [&] { const std::array local{posting("owned", 7)};
    return require(reduce_journal_balances(rule, local)); }();
  check(owned.evidence().front().lineage.source_record_ids().front().value() == "source-owned");
  check(owned.balances().front().lines.front().entry == JournalEntryId{"owned"});
}
} // namespace

int main() {
  fixture_walkthrough();
  boundaries();
  std::cout << "journal_balances: ten policy views, partition equivalence and boundaries passed\n";
}
