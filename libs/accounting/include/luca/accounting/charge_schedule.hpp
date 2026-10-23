#pragma once
#include "luca/event.hpp"
#include <algorithm>
#include <span>
namespace luca::accounting {
enum class ChargeBasis { flat, per_share, notional };
enum class ChargeSide { both, buy, sell };
struct ChargeRule {
  std::string id, version, category, payee;
  Timestamp effective_from, effective_until; // half-open interval
  Currency currency;
  ChargeBasis basis;
  ChargeSide side;
  Money flat;
  Price per_share;
  Rate notional_rate;
  Money minimum, maximum;
  // Positive currency quantum in Money's 1e-6 units, e.g. 10000 for cents.
  std::int64_t quantum;
  RoundingMode rounding;
};
// Rules are explicit immutable inputs. Caller selects broker/venue/jurisdiction;
// this function applies active rules, never fetches or guesses rates. Returned
// scheduled amounts remain distinguishable from reported actual amounts.
inline std::expected<TradeCharges, ValueError>
calculate_charges(Quantity signed_quantity, Price price, Currency currency, Timestamp effective_at,
                  std::span<const ChargeRule> rules) {
  auto q = signed_quantity.scaled_value();
  if (q == 0 || q == std::numeric_limits<std::int64_t>::min() || price.scaled_value() <= 0)
    return std::unexpected(ValueError::invalid_decimal);
  bool sell = q < 0;
  if (sell)
    q = -q;
  TradeCharges out;
  for (const auto &rule : rules) {
    if ((rule.side != ChargeSide::buy && rule.side != ChargeSide::sell &&
         rule.side != ChargeSide::both) ||
        (rule.rounding != RoundingMode::half_even && rule.rounding != RoundingMode::half_up &&
         rule.rounding != RoundingMode::floor && rule.rounding != RoundingMode::ceiling &&
         rule.rounding != RoundingMode::toward_zero) ||
        rule.effective_from >= rule.effective_until || rule.quantum <= 0 || rule.id.empty() ||
        rule.version.empty() || rule.minimum.currency() != currency ||
        rule.maximum.currency() != currency || rule.flat.currency() != currency ||
        rule.currency != currency || rule.minimum.scaled_value() < 0 ||
        rule.maximum.scaled_value() < rule.minimum.scaled_value() || rule.flat.scaled_value() < 0 ||
        rule.per_share.scaled_value() < 0 || rule.notional_rate.scaled_value() < 0 ||
        rule.minimum.scaled_value() % rule.quantum || rule.maximum.scaled_value() % rule.quantum)
      return std::unexpected(ValueError::invalid_decimal);
    if (effective_at < rule.effective_from || effective_at >= rule.effective_until ||
        (rule.side == ChargeSide::buy && sell) || (rule.side == ChargeSide::sell && !sell))
      continue;
    detail::Wide numerator = 0, denominator = 1;
    switch (rule.basis) {
    case ChargeBasis::flat:
      numerator = rule.flat.scaled_value();
      break;
    case ChargeBasis::per_share:
      numerator = detail::Wide(q) * rule.per_share.scaled_value();
      denominator = detail::pow10(10);
      break;
    case ChargeBasis::notional: {
      auto principal = value(Quantity::from_scaled(q), price, currency);
      if (!principal)
        return std::unexpected(principal.error());
      numerator = detail::Wide(principal->scaled_value()) * rule.notional_rate.scaled_value();
      denominator = detail::pow10(12);
      break;
    }
    default:
      return std::unexpected(ValueError::invalid_decimal);
    }
    auto rounded =
        detail::divide_rounded(numerator, denominator * rule.quantum, rule.rounding) * rule.quantum;
    rounded = std::clamp(rounded, detail::Wide(rule.minimum.scaled_value()),
                         detail::Wide(rule.maximum.scaled_value()));
    auto amount = detail::narrow(rounded);
    if (!amount)
      return std::unexpected(amount.error());
    out.push_back({rule.id, rule.category, rule.payee, rule.version, "scheduled",
                   Money::from_scaled(*amount, currency)});
  }
  auto valid = validate_charges(out, currency);
  if (!valid)
    return std::unexpected(valid.error());
  return out;
}
} // namespace luca::accounting
