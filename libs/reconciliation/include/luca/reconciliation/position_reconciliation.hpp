#pragma once

#include "luca/reconciliation/position_observation.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace luca {

struct PositionReconciliationContext {
  Timestamp as_of;
};

enum class PositionBreakKind {
  missing_observation,
  unexpected_observation,
  quantity_mismatch,
};

enum class PositionReconciliationError {
  duplicate_observation,
  observation_time_mismatch,
  quantity_overflow,
  duplicate_projected_key,
};

class PositionBreak {
 public:
  [[nodiscard]] const PositionKey& key() const noexcept { return key_; }
  [[nodiscard]] PositionBreakKind kind() const noexcept { return kind_; }
  [[nodiscard]] const std::optional<Quantity>& expected() const noexcept {
    return expected_;
  }
  [[nodiscard]] const std::optional<Quantity>& observed() const noexcept {
    return observed_;
  }
  [[nodiscard]] const std::optional<Quantity>& difference() const noexcept {
    return difference_;
  }
  [[nodiscard]] const std::optional<Provenance>& observation_provenance() const noexcept {
    return observation_provenance_;
  }
  bool operator==(const PositionBreak&) const = default;

  [[nodiscard]] static PositionBreak missing(PositionKey key, Quantity expected) {
    return PositionBreak(std::move(key), PositionBreakKind::missing_observation,
                         expected, std::nullopt, std::nullopt, std::nullopt);
  }
  [[nodiscard]] static PositionBreak unexpected(const PositionObservation& observation) {
    return PositionBreak(observation.key(), PositionBreakKind::unexpected_observation,
                         std::nullopt, observation.quantity(), std::nullopt,
                         observation.provenance());
  }
  [[nodiscard]] static PositionBreak mismatch(const PositionObservation& observation,
                                              Quantity expected,
                                              Quantity difference) {
    return PositionBreak(observation.key(), PositionBreakKind::quantity_mismatch,
                         expected, observation.quantity(), difference,
                         observation.provenance());
  }

 private:
  PositionBreak(PositionKey key, PositionBreakKind kind,
                std::optional<Quantity> expected, std::optional<Quantity> observed,
                std::optional<Quantity> difference,
                std::optional<Provenance> observation_provenance)
      : key_(std::move(key)), kind_(kind), expected_(expected), observed_(observed),
        difference_(difference),
        observation_provenance_(std::move(observation_provenance)) {}

  PositionKey key_;
  PositionBreakKind kind_;
  std::optional<Quantity> expected_;
  std::optional<Quantity> observed_;
  std::optional<Quantity> difference_;
  std::optional<Provenance> observation_provenance_;
};

namespace detail {
struct PositionKeyHash {
  std::size_t operator()(const PositionKey& key) const noexcept {
    const auto first = std::hash<std::string>{}(key.account().value());
    const auto second = std::hash<std::string>{}(key.instrument().value());
    return first ^ (second + 0x9e3779b9U + (first << 6U) + (first >> 2U));
  }
};
}  // namespace detail

// Compares exact quantities at one shared timestamp. In particular, an observed
// zero is a present value and is unexpected when the sparse projection omits its key.
[[nodiscard]] inline std::expected<std::vector<PositionBreak>, PositionReconciliationError>
reconcile_positions(std::span<const Position> expected,
                    std::span<const PositionObservation> observed,
                    PositionReconciliationContext context) {
  std::unordered_map<PositionKey, std::size_t, detail::PositionKeyHash> observation_index;
  observation_index.reserve(observed.size());
  for (std::size_t index = 0; index < observed.size(); ++index) {
    if (observed[index].as_of() != context.as_of)
      return std::unexpected(PositionReconciliationError::observation_time_mismatch);
    if (!observation_index.emplace(observed[index].key(), index).second)
      return std::unexpected(PositionReconciliationError::duplicate_observation);
  }

  std::unordered_set<PositionKey, detail::PositionKeyHash> projected_keys;
  projected_keys.reserve(expected.size());
  for (const auto &row : expected)
    if (!projected_keys.insert(row.key()).second)
      return std::unexpected(PositionReconciliationError::duplicate_projected_key);

  std::vector<bool> seen(observed.size());
  std::vector<PositionBreak> breaks;
  breaks.reserve(expected.size() + observed.size());
  for (const auto& position : expected) {
    const auto found = observation_index.find(position.key());
    if (found == observation_index.end()) {
      breaks.push_back(PositionBreak::missing(position.key(), position.quantity()));
      continue;
    }
    seen[found->second] = true;
    const auto& observation = observed[found->second];
    if (position.quantity() == observation.quantity()) continue;
    const auto difference = observation.quantity().subtract(position.quantity());
    if (!difference)
      return std::unexpected(PositionReconciliationError::quantity_overflow);
    breaks.push_back(PositionBreak::mismatch(observation, position.quantity(), *difference));
  }
  for (std::size_t index = 0; index < observed.size(); ++index)
    if (!seen[index]) breaks.push_back(PositionBreak::unexpected(observed[index]));

  std::sort(breaks.begin(), breaks.end(), [](const PositionBreak& lhs,
                                             const PositionBreak& rhs) {
    if (lhs.key() != rhs.key()) return lhs.key() < rhs.key();
    return lhs.kind() < rhs.kind();
  });
  return breaks;
}

}  // namespace luca
