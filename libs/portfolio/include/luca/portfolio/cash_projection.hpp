#pragma once

#include "luca/ledger.hpp"
#include "luca/portfolio/cash.hpp"

#include <algorithm>
#include <chrono>
#include <concepts>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace luca {

enum class CashProjectionError {
  amount_overflow,
  valuation_overflow,
  invalid_context,
  context_mismatch
};

struct CashProjectionContext {
  Timestamp as_of;
  std::chrono::year_month_day settlement_as_of_date;
};

namespace portfolio_detail {
class CashFold {
public:
  explicit CashFold(SettlementDate date, std::span<const CashBalance> initial = {}) : date_(date) {
    for (const auto &row : initial)
      amounts_.emplace(row.key(), row.amount());
  }
  [[nodiscard]] std::expected<std::optional<Money>, CashProjectionError>
  change(const EconomicEvent &event) const {
    if (const auto *cash = std::get_if<CashMovement>(&event))
      return cash->amount();
    const auto &trade = std::get<EquityTrade>(event);
    if (trade.settlement_date() > date_)
      return std::nullopt;
    const auto cash = trade_cash(trade);
    if (!cash)
      return std::unexpected(CashProjectionError::valuation_overflow);
    return *cash;
  }
  [[nodiscard]] std::expected<void, CashProjectionError> apply(const EconomicEvent &event) {
    const auto amount = change(event);
    if (!amount)
      return std::unexpected(amount.error());
    return *amount ? add(header(event).account(), **amount)
                   : std::expected<void, CashProjectionError>{};
  }
  [[nodiscard]] std::vector<CashBalance> finish() const {
    std::vector<CashBalance> balances;
    balances.reserve(amounts_.size());
    for (const auto &[key, amount] : amounts_)
      if (amount.scaled_value() != 0)
        balances.emplace_back(key.account(), amount);
    std::ranges::sort(balances, {}, &CashBalance::key);
    return balances;
  }
  [[nodiscard]] std::expected<void, CashProjectionError> add(const AccountId &account,
                                                             Money amount) {
    const auto [existing, inserted] =
        amounts_.try_emplace(CashKey{account, amount.currency()}, amount);
    if (inserted)
      return {};
    const auto sum = existing->second.add(amount);
    if (!sum)
      return std::unexpected(CashProjectionError::amount_overflow);
    existing->second = *sum;
    return {};
  }

private:
  struct KeyHash {
    std::size_t operator()(const CashKey& key) const noexcept {
      const auto first = std::hash<std::string>{}(key.account().value());
      const auto second = std::hash<std::string_view>{}(key.currency().code());
      return first ^ (second + 0x9e3779b9U + (first << 6U) + (first >> 2U));
    }
  };
  SettlementDate date_;
  std::unordered_map<CashKey, Money, KeyHash> amounts_;
};
} // namespace portfolio_detail

// Economic time and settlement eligibility are independent. Exactly-zero
// balances are omitted; public output is ordered by account then currency.
[[nodiscard]] inline std::expected<std::vector<CashBalance>, CashProjectionError>
project_cash(std::span<const LedgerEntry> entries, CashProjectionContext context) {
  const auto date = SettlementDate::create(context.settlement_as_of_date);
  if (!date)
    return std::unexpected(CashProjectionError::invalid_context);
  portfolio_detail::CashFold fold{*date};
  for (const auto entry : economic_entries_through(entries, context.as_of))
    if (auto applied = fold.apply(entry.get().event()); !applied)
      return std::unexpected(applied.error());
  return fold.finish();
}

} // namespace luca
