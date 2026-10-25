#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "luca/reconciliation/cash_observation.hpp"

namespace luca {

struct CashReconciliationContext {
  Timestamp as_of;
  std::chrono::year_month_day settlement_as_of_date;
};

enum class CashBreakKind {
  missing_observation,
  unexpected_observation,
  amount_mismatch,
};

enum class CashReconciliationError {
  duplicate_observation,
  observation_time_mismatch,
  observation_settlement_date_mismatch,
  amount_overflow,
  duplicate_projected_key,
  invalid_context,
};

class CashBreak {
 public:
  [[nodiscard]] const CashKey &key() const noexcept { return key_; }
  [[nodiscard]] CashBreakKind kind() const noexcept { return kind_; }
  [[nodiscard]] const std::optional<Money> &expected() const noexcept {
    return expected_;
  }
  [[nodiscard]] const std::optional<Money> &observed() const noexcept {
    return observed_;
  }
  [[nodiscard]] const std::optional<Money> &difference() const noexcept {
    return difference_;
  }
  [[nodiscard]] const std::optional<Provenance> &observation_provenance()
      const noexcept {
    return observation_provenance_;
  }
  bool operator==(const CashBreak &) const = default;

  [[nodiscard]] static CashBreak missing(const CashBalance &balance) {
    return CashBreak(balance.key(), CashBreakKind::missing_observation,
                     balance.amount(), std::nullopt, std::nullopt,
                     std::nullopt);
  }
  [[nodiscard]] static CashBreak unexpected(
      const CashObservation &observation) {
    return CashBreak(observation.key(), CashBreakKind::unexpected_observation,
                     std::nullopt, observation.amount(), std::nullopt,
                     observation.provenance());
  }
  [[nodiscard]] static CashBreak mismatch(const CashObservation &observation,
                                          Money expected, Money difference) {
    return CashBreak(observation.key(), CashBreakKind::amount_mismatch,
                     expected, observation.amount(), difference,
                     observation.provenance());
  }

 private:
  CashBreak(CashKey key, CashBreakKind kind, std::optional<Money> expected,
            std::optional<Money> observed, std::optional<Money> difference,
            std::optional<Provenance> observation_provenance)
      : key_(std::move(key)),
        kind_(kind),
        expected_(expected),
        observed_(observed),
        difference_(difference),
        observation_provenance_(std::move(observation_provenance)) {}

  CashKey key_;
  CashBreakKind kind_;
  std::optional<Money> expected_;
  std::optional<Money> observed_;
  std::optional<Money> difference_;
  std::optional<Provenance> observation_provenance_;
};

namespace detail {
struct CashReconciliationKeyHash {
  std::size_t operator()(const CashKey &key) const noexcept {
    const auto first = std::hash<std::string>{}(key.account().value());
    const auto second = std::hash<std::string_view>{}(key.currency().code());
    return first ^ (second + 0x9e3779b9U + (first << 6U) + (first >> 2U));
  }
};
}  // namespace detail

// Compares exact settled cash at one shared economic timestamp and settlement
// date. An observed zero remains present when the sparse projection omits its
// key.
[[nodiscard]] inline std::expected<std::vector<CashBreak>,
                                   CashReconciliationError>
reconcile_cash(std::span<const CashBalance> expected,
               std::span<const CashObservation> observed,
               CashReconciliationContext context) {
  if (!context.settlement_as_of_date.ok())
    return std::unexpected(CashReconciliationError::invalid_context);
  std::unordered_map<CashKey, std::size_t, detail::CashReconciliationKeyHash>
      observation_index;
  observation_index.reserve(observed.size());
  for (std::size_t index = 0; index < observed.size(); ++index) {
    if (observed[index].as_of() != context.as_of)
      return std::unexpected(
          CashReconciliationError::observation_time_mismatch);
    if (observed[index].settlement_as_of_date() !=
        context.settlement_as_of_date)
      return std::unexpected(
          CashReconciliationError::observation_settlement_date_mismatch);
    if (!observation_index.emplace(observed[index].key(), index).second)
      return std::unexpected(CashReconciliationError::duplicate_observation);
  }

  std::unordered_set<CashKey, detail::CashReconciliationKeyHash> projected_keys;
  projected_keys.reserve(expected.size());
  for (const auto &row : expected)
    if (!projected_keys.insert(row.key()).second)
      return std::unexpected(CashReconciliationError::duplicate_projected_key);

  std::vector<bool> seen(observed.size());
  std::vector<CashBreak> breaks;
  breaks.reserve(expected.size() + observed.size());
  for (const auto &balance : expected) {
    const auto found = observation_index.find(balance.key());
    if (found == observation_index.end()) {
      breaks.push_back(CashBreak::missing(balance));
      continue;
    }
    seen[found->second] = true;
    const auto &observation = observed[found->second];
    if (balance.amount() == observation.amount()) continue;
    const auto difference = observation.amount().subtract(balance.amount());
    if (!difference)
      return std::unexpected(CashReconciliationError::amount_overflow);
    breaks.push_back(
        CashBreak::mismatch(observation, balance.amount(), *difference));
  }
  for (std::size_t index = 0; index < observed.size(); ++index)
    if (!seen[index]) breaks.push_back(CashBreak::unexpected(observed[index]));

  std::sort(breaks.begin(), breaks.end(),
            [](const CashBreak &lhs, const CashBreak &rhs) {
              if (lhs.key() != rhs.key()) return lhs.key() < rhs.key();
              return lhs.kind() < rhs.kind();
            });
  return breaks;
}

}  // namespace luca
