#include <luca/accounting/balances.hpp>
#include <luca/accounting/settlement_date_projection.hpp>
#include <luca/accounting/trade_date_projection.hpp>
#include "../settlement-date-accounting/fixture.hpp"

#include <iostream>

namespace {
using namespace luca;
using namespace settlement_date_example;

template <class Projection>
void report(const Projection &projection, const Evaluation &evaluation) {
  const auto contract = require(JournalBalanceContract::create({
      BookId{"fund-a.book"}, "fixture-history-revision-1", std::string{projection.engine_version()},
      std::string{projection.projection_version()}, projection.policy(),
      evaluation.recorded_through, evaluation.economic_as_of, evaluation.settlement_as_of_date}));
  const auto balances = require(reduce_journal_balances(contract, projection.entries()));
  std::cout << "view=" << evaluation.name << " book=" << contract.definition().book.value()
            << " policy=" << projection.policy().id().value() << '\n';
  for (const auto &row : balances.balances()) {
    std::cout << "  account=" << row.key.account.value() << " currency=" << row.key.currency.code()
              << " debit_scaled=" << row.debits.scaled_value()
              << " credit_scaled=" << row.credits.scaled_value()
              << " net_scaled=" << row.net.scaled_value() << '\n';
    for (const auto &line : row.lines)
      std::cout << "    entry=" << line.entry.value() << " line=" << line.line.value() << '\n';
  }
  for (const auto &control : balances.controls())
    std::cout << "  trial_balance_currency=" << control.currency.code()
              << " debit_balances_scaled=" << control.debit_balances.scaled_value()
              << " credit_balances_scaled=" << control.credit_balances.scaled_value() << '\n';
  for (const auto &evidence : balances.evidence())
    for (const auto &source : evidence.lineage.source_record_ids())
      std::cout << "  entry=" << evidence.entry.value() << " source=" << source.value() << '\n';
}
} // namespace

int main() {
  const auto ledger = settlement_date_example::history();
  for (const auto &evaluation : settlement_date_example::evaluations()) {
    const auto resolution = ledger.resolve(evaluation.recorded_through, evaluation.economic_as_of);
    report(settlement_date_example::require(luca::project_trade_date_journals(resolution,
        {evaluation.recorded_through, evaluation.economic_as_of, evaluation.settlement_as_of_date})), evaluation);
    report(settlement_date_example::require(luca::project_settlement_date_journals(resolution,
        {evaluation.recorded_through, evaluation.economic_as_of, evaluation.settlement_as_of_date})), evaluation);
  }
}
