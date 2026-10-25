#pragma once

#include "luca/ledger.hpp"
#include "luca/portfolio/position.hpp"

#include <algorithm>
#include <concepts>
#include <expected>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace luca {

enum class PositionProjectionError { quantity_overflow, context_mismatch };

namespace portfolio_detail {
class PositionFold {
public:
  explicit PositionFold(std::span<const Position> initial = {}) {
    for (const auto &row : initial)
      quantities_.emplace(row.key(), row.quantity());
  }
  [[nodiscard]] std::expected<void, PositionProjectionError> apply(const EconomicEvent &event) {
    const auto *trade = std::get_if<EquityTrade>(&event);
    if (!trade)
      return {};
    PositionKey key{trade->header().account(), trade->instrument()};
    const auto [existing, inserted] = quantities_.try_emplace(std::move(key), trade->quantity());
    if (inserted)
      return {};
    const auto sum = existing->second.add(trade->quantity());
    if (!sum)
      return std::unexpected(PositionProjectionError::quantity_overflow);
    existing->second = *sum;
    return {};
  }
  [[nodiscard]] std::vector<Position> finish() const {
    std::vector<Position> positions;
    positions.reserve(quantities_.size());
    for (const auto &[key, quantity] : quantities_)
      if (quantity.scaled_value() != 0)
        positions.emplace_back(key, quantity);
    std::ranges::sort(positions, {}, &Position::key);
    return positions;
  }

private:
  struct KeyHash {
    std::size_t operator()(const PositionKey& key) const noexcept {
      const auto first = std::hash<std::string>{}(key.account().value());
      const auto second = std::hash<std::string>{}(key.instrument().value());
      return first ^ (second + 0x9e3779b9U + (first << 6U) + (first >> 2U));
    }
  };
  std::unordered_map<PositionKey, Quantity, KeyHash> quantities_;
};
} // namespace portfolio_detail

// Accepts any input order; checked transitions follow effective_at then sequence.
[[nodiscard]] inline std::expected<std::vector<Position>, PositionProjectionError>
project_positions(std::span<const LedgerEntry> entries, Timestamp as_of) {
  portfolio_detail::PositionFold fold;
  for (const auto entry : economic_entries_through(entries, as_of))
    if (auto applied = fold.apply(entry.get().event()); !applied)
      return std::unexpected(applied.error());
  return fold.finish();
}

} // namespace luca
