#pragma once

#include "luca/ledger.hpp"
#include "luca/portfolio/cash.hpp"

#include <algorithm>
#include <chrono>
#include <concepts>
#include <expected>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace luca {

enum class CashProjectionError { amount_overflow, valuation_overflow };

struct CashProjectionContext {
  Timestamp as_of;
  std::chrono::year_month_day settlement_as_of_date;
};

// Economic time and settlement eligibility are independent. Results omit
// exactly-zero balances and are ordered by account, then currency.
[[nodiscard]] inline std::expected<std::vector<CashBalance>, CashProjectionError>
project_cash(std::span<const LedgerEntry> entries, CashProjectionContext context) {
  const auto ordered = economic_entries_through(entries, context.as_of);
  struct KeyHash {
    std::size_t operator()(const CashKey& key) const noexcept {
      const auto first = std::hash<std::string>{}(key.account().value());
      const auto second = std::hash<std::string_view>{}(key.currency().code());
      return first ^ (second + 0x9e3779b9U + (first << 6U) + (first >> 2U));
    }
  };
  std::unordered_map<CashKey, Money, KeyHash> amounts;
  const auto apply = [&amounts](const AccountId& account, Money amount)
      -> std::expected<void, CashProjectionError> {
    CashKey key{account, amount.currency()};
    const auto existing = amounts.find(key);
    if (existing == amounts.end()) {
      amounts.emplace(std::move(key), amount);
      return {};
    }
    const auto sum = existing->second.add(amount);
    if (!sum) return std::unexpected(CashProjectionError::amount_overflow);
    existing->second = *sum;
    return {};
  };

  for (const auto entry : ordered) {
    const auto result = std::visit(
        [&apply, context](const auto& event) -> std::expected<void, CashProjectionError> {
          using Event = std::remove_cvref_t<decltype(event)>;
          if constexpr (std::same_as<Event, CashMovement>) {
            return apply(event.header().account(), event.amount());
          } else {
            if (event.settlement_date().value() > context.settlement_as_of_date) return {};
            const auto cash = trade_cash(event);
            if (!cash) return std::unexpected(CashProjectionError::valuation_overflow);
            return apply(event.header().account(), *cash);
          }
        }, entry.get().event());
    if (!result) return std::unexpected(result.error());
  }

  std::vector<CashBalance> balances;
  balances.reserve(amounts.size());
  for (const auto& [key, amount] : amounts)
    if (amount.scaled_value() != 0) balances.emplace_back(key.account(), amount);
  std::sort(balances.begin(), balances.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.key() < rhs.key();
  });
  return balances;
}

}  // namespace luca
