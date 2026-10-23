#pragma once
#include "luca/accounting/trade_accounting.hpp"
namespace luca::accounting {
struct AccountingControl {
  std::string control;
  Money expected, journal, difference;
  bool operator==(const AccountingControl &) const = default;
};
// Reporting cross-checks against independently derived economic/actual-cash
// state. External observations remain separate; no journal or ledger is edited.
inline std::expected<std::vector<AccountingControl>, AccountingError>
reconcile_trade_accounting(const LifecycleResolution &resolution, const AccountingReport &report) {
  using namespace trade_accounting_detail;
  try {
    std::map<std::pair<std::string, std::string>, Money> expected, journal;
    auto accumulate = [](auto &map, std::string name, Money value) {
      auto [it, inserted] =
          map.emplace(std::pair{name, std::string(value.currency().code())}, value);
      if (!inserted)
        it->second = add(it->second, value);
    };
    std::set<std::string> accounts;
    for (const auto &p : report.lots.resolved_policies)
      accounts.insert(p.target.account);
    for (const auto &r : resolution.active_events())
      if (accounts.contains(r.record().account().value()))
        if (const auto *cash = std::get_if<CashMovement>(&r.event()))
          accumulate(expected, "cash", cash->amount());
    for (const auto &e : report.settlements)
      accumulate(expected, "cash", e.cash);
    for (const auto &o : report.obligations)
      accumulate(expected, o.contractual_cash.scaled_value() >= 0 ? "receivables" : "payables",
                 o.outstanding_cash);
    for (const auto &lot : report.lots.open_lots)
      accumulate(expected, lot.short_position ? "short_inventory" : "long_inventory",
                 lot.short_position ? neg(carrying(lot.remaining, true))
                                    : carrying(lot.remaining, false));
    for (const auto &relief : report.lots.reliefs)
      accumulate(expected, "economic_realized_pnl", neg(relief.net_pnl));
    for (const auto &b : report.trial_balance.balances()) {
      const auto &name = b.key.account.value();
      if (name == "asset.cash")
        accumulate(journal, "cash", b.net);
      else if (name == "asset.trade-receivable")
        accumulate(journal, "receivables", b.net);
      else if (name == "liability.trade-payable")
        accumulate(journal, "payables", b.net);
      else if (name == "asset.equity-securities")
        accumulate(journal, "long_inventory", b.net);
      else if (name == "liability.short-equities")
        accumulate(journal, "short_inventory", b.net);
      else if (name == "income.realized-pnl" || name == "control.deferred-realized-pnl")
        accumulate(journal, "economic_realized_pnl", b.net);
    }
    for (const auto &[key, value] : journal)
      expected.try_emplace(key, Money::from_scaled(0, value.currency()));
    std::vector<AccountingControl> result;
    for (const auto &[key, value] : expected) {
      auto it = journal.find(key);
      const auto actual =
          it == journal.end() ? Money::from_scaled(0, value.currency()) : it->second;
      result.push_back({key.first, value, actual, sub(actual, value)});
    }
    return result;
  } catch (const ArithmeticFailure &) {
    return std::unexpected(AccountingError{"RECONCILIATION_AMOUNT_OVERFLOW", ""});
  }
}
} // namespace luca::accounting
