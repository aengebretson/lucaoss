#pragma once

#include "luca/ledger.hpp"
#include "luca/portfolio/settlement.hpp"

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

enum class SettlementProjectionError {
  valuation_overflow,
  amount_overflow,
  invalid_context,
  context_mismatch
};

struct SettlementProjectionContext {
  Timestamp as_of;
  std::chrono::year_month_day settlement_as_of_date;
};

namespace portfolio_detail {
class SettlementFold {
public:
  explicit SettlementFold(SettlementDate date, std::span<const SettlementObligation> initial = {})
      : date_(date) {
    for (const auto &row : initial)
      amounts_.emplace(row.key(), row.amount());
  }
  [[nodiscard]] std::expected<std::optional<SettlementObligation>, SettlementProjectionError>
  change(const EconomicEvent &event) const {
    const auto *trade = std::get_if<EquityTrade>(&event);
    if (!trade || trade->settlement_date() <= date_)
      return std::nullopt;
    const auto cash = trade_cash(*trade);
    if (!cash)
      return std::unexpected(SettlementProjectionError::valuation_overflow);
    const auto direction =
        cash->scaled_value() < 0 ? SettlementDirection::payable : SettlementDirection::receivable;
    auto magnitude = *cash;
    if (magnitude.scaled_value() < 0) {
      const auto positive = Money::from_scaled(0, magnitude.currency()).subtract(magnitude);
      if (!positive)
        return std::unexpected(SettlementProjectionError::valuation_overflow);
      magnitude = *positive;
    }
    return SettlementObligation{trade->header().account(), trade->settlement_date(), direction,
                                magnitude};
  }
  [[nodiscard]] std::expected<void, SettlementProjectionError> apply(const EconomicEvent &event) {
    const auto obligation = change(event);
    if (!obligation)
      return std::unexpected(obligation.error());
    return *obligation ? add(**obligation) : std::expected<void, SettlementProjectionError>{};
  }
  [[nodiscard]] std::expected<void, SettlementProjectionError>
  add(const SettlementObligation &row) {
    const auto &key = row.key();
    const auto magnitude = row.amount();
    const auto [existing, inserted] = amounts_.try_emplace(key, magnitude);
    if (inserted)
      return {};
    const auto sum = existing->second.add(magnitude);
    if (!sum)
      return std::unexpected(SettlementProjectionError::amount_overflow);
    existing->second = *sum;
    return {};
  }
  [[nodiscard]] std::vector<SettlementObligation> finish() const {
    std::vector<SettlementObligation> obligations;
    obligations.reserve(amounts_.size());
    for (const auto &[key, amount] : amounts_)
      if (amount.scaled_value() != 0)
        obligations.emplace_back(key.account(), key.settlement_date(), key.direction(), amount);
    std::ranges::sort(obligations, {}, &SettlementObligation::key);
    return obligations;
  }

private:
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
  SettlementDate date_;
  std::unordered_map<SettlementObligationKey, Money, KeyHash> amounts_;
};
} // namespace portfolio_detail

// Open obligations are positive magnitudes, ordered by their public keys.
[[nodiscard]] inline std::expected<std::vector<SettlementObligation>, SettlementProjectionError>
project_settlement_obligations(std::span<const LedgerEntry> entries,
                               SettlementProjectionContext context) {
  const auto date = SettlementDate::create(context.settlement_as_of_date);
  if (!date)
    return std::unexpected(SettlementProjectionError::invalid_context);
  portfolio_detail::SettlementFold fold{*date};
  for (const auto entry : economic_entries_through(entries, context.as_of))
    if (auto applied = fold.apply(entry.get().event()); !applied)
      return std::unexpected(applied.error());
  return fold.finish();
}

} // namespace luca
