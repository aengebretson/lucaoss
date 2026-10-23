#pragma once
#include "luca/core.hpp"
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace luca {
// Amounts are separate from execution price. Empty known charges means confirmed
// zero; absent charges on a legacy trade means unreported, never confirmed zero.
struct TradeCharge {
  std::string id, category, payee, schedule_version, state;
  Money amount;
  bool operator==(const TradeCharge &) const = default;
};
using TradeCharges = std::vector<TradeCharge>;
inline std::expected<void, ValueError> validate_charges(const TradeCharges &charges,
                                                        Currency currency) {
  if (charges.size() > 64)
    return std::unexpected(ValueError::metadata_too_large);
  std::set<std::string> ids;
  auto total = Money::from_scaled(0, currency);
  for (const auto &c : charges) {
    if (c.id.empty() || c.payee.empty() || !ids.insert(c.id).second ||
        (c.category != "commission" && c.category != "exchange" && c.category != "regulatory" &&
         c.category != "other") ||
        (c.state != "reported" && c.state != "scheduled" && c.state != "unreported") ||
        (c.state == "scheduled" && c.schedule_version.empty()))
      return std::unexpected(ValueError::empty_required_field);
    for (const auto *s : {&c.id, &c.payee, &c.schedule_version})
      if (s->size() > 512 || s->find('\0') != std::string::npos)
        return std::unexpected(ValueError::metadata_too_large);
    if (c.amount.currency() != currency)
      return std::unexpected(ValueError::currency_mismatch);
    if (c.amount.scaled_value() < 0 || (c.state == "unreported" && c.amount.scaled_value() != 0))
      return std::unexpected(ValueError::invalid_decimal);
    auto sum = total.add(c.amount);
    if (!sum)
      return std::unexpected(sum.error());
    total = *sum;
  }
  return {};
}
inline std::expected<Money, ValueError> charge_total(const TradeCharges &charges, Currency currency,
                                                     bool commissions) {
  auto total = Money::from_scaled(0, currency);
  for (const auto &c : charges)
    if ((c.category == "commission") == commissions) {
      auto sum = total.add(c.amount);
      if (!sum)
        return std::unexpected(sum.error());
      total = *sum;
    }
  return total;
}
} // namespace luca
