#pragma once

#include "luca/ledger.hpp"
#include "luca/portfolio/settlement.hpp"

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

enum class SettlementProjectionError { valuation_overflow, amount_overflow };

struct SettlementProjectionContext {
  Timestamp as_of;
  std::chrono::year_month_day settlement_as_of_date;
};

// Open obligations are positive magnitudes. Economic selection and ordering are
// delegated to the ledger; results are sorted explicitly for public output.
[[nodiscard]] inline
std::expected<std::vector<SettlementObligation>, SettlementProjectionError>
project_settlement_obligations(std::span<const LedgerEntry> entries,
                               SettlementProjectionContext context) {
  const auto ordered = economic_entries_through(entries, context.as_of);
  struct KeyHash {
    std::size_t operator()(const SettlementObligationKey& key) const noexcept {
      auto seed = std::hash<std::string>{}(key.account().value());
      const auto combine = [&seed](std::size_t value) {
        seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
      };
      combine(std::hash<long long>{}(
          std::chrono::sys_days{key.settlement_date().value()}.time_since_epoch().count()));
      combine(std::hash<std::string_view>{}(key.currency().code()));
      combine(std::hash<int>{}(static_cast<int>(key.direction())));
      return seed;
    }
  };
  std::unordered_map<SettlementObligationKey, Money, KeyHash> amounts;

  for (const auto entry : ordered) {
    const auto result = std::visit(
        [&amounts, context](const auto& event)
            -> std::expected<void, SettlementProjectionError> {
          using Event = std::remove_cvref_t<decltype(event)>;
          if constexpr (std::same_as<Event, EquityTrade>) {
            if (event.settlement_date().value() <= context.settlement_as_of_date)
              return {};

            const auto gross = trade_cash(event);
            if (!gross)
              return std::unexpected(SettlementProjectionError::valuation_overflow);

            const auto direction = gross->scaled_value() < 0
                                       ? SettlementDirection::payable
                                       : SettlementDirection::receivable;
            Money magnitude = *gross;
            if (magnitude.scaled_value() < 0) {
              const auto positive = Money::from_scaled(0, magnitude.currency())
                                        .subtract(magnitude);
              if (!positive)
                return std::unexpected(SettlementProjectionError::valuation_overflow);
              magnitude = *positive;
            }

            SettlementObligationKey key{event.header().account(),
                                        event.settlement_date(),
                                        event.quote_currency(), direction};
            const auto existing = amounts.find(key);
            if (existing == amounts.end()) {
              amounts.emplace(std::move(key), magnitude);
            } else {
              const auto sum = existing->second.add(magnitude);
              if (!sum)
                return std::unexpected(SettlementProjectionError::amount_overflow);
              existing->second = *sum;
            }
          }
          return {};
        }, entry.get().event());
    if (!result) return std::unexpected(result.error());
  }

  std::vector<SettlementObligation> obligations;
  obligations.reserve(amounts.size());
  for (const auto& [key, amount] : amounts)
    if (amount.scaled_value() != 0)
      obligations.emplace_back(key.account(), key.settlement_date(), key.direction(), amount);
  std::sort(obligations.begin(), obligations.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.key() < rhs.key();
  });
  return obligations;
}

}  // namespace luca
