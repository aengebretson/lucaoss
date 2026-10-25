#pragma once

#include "luca/lifecycle.hpp"
#include "luca/portfolio/cash_projection.hpp"
#include "luca/portfolio/position_projection.hpp"
#include "luca/portfolio/settlement_projection.hpp"

#include <chrono>
#include <expected>
#include <vector>

namespace luca {

struct LifecycleProjectionContext {
  Timestamp economic_as_of;
  std::chrono::year_month_day settlement_as_of_date;
};

// Every result is evaluated even when another projection reports an error, and
// each projection retains its existing public error type.
struct LifecycleProjectionResult {
  std::expected<std::vector<Position>, PositionProjectionError> positions;
  std::expected<std::vector<CashBalance>, CashProjectionError> settled_cash;
  std::expected<std::vector<SettlementObligation>, SettlementProjectionError>
      open_settlement_obligations;
};

// The validated date overload derives economic selection from the resolution.
// Active events are already ordered; every projection uses its ordinary checked
// transition without rebuilding a ledger or sorting the input again.
[[nodiscard]] inline LifecycleProjectionResult
project_lifecycle(const LifecycleResolution &resolution, SettlementDate settlement_as_of_date) {
  portfolio_detail::PositionFold positions;
  portfolio_detail::CashFold cash{settlement_as_of_date};
  portfolio_detail::SettlementFold obligations{settlement_as_of_date};
  std::expected<void, PositionProjectionError> position_status;
  std::expected<void, CashProjectionError> cash_status;
  std::expected<void, SettlementProjectionError> obligation_status;
  for (const auto &resolved : resolution.active_events()) {
    if (position_status)
      position_status = positions.apply(resolved.event());
    if (cash_status)
      cash_status = cash.apply(resolved.event());
    if (obligation_status)
      obligation_status = obligations.apply(resolved.event());
  }
  return {position_status
              ? std::expected<std::vector<Position>, PositionProjectionError>{positions.finish()}
              : std::unexpected(position_status.error()),
          cash_status ? std::expected<std::vector<CashBalance>, CashProjectionError>{cash.finish()}
                      : std::unexpected(cash_status.error()),
          obligation_status ? std::expected<std::vector<SettlementObligation>,
                                            SettlementProjectionError>{obligations.finish()}
                            : std::unexpected(obligation_status.error())};
}

// Compatibility entry point rejects mismatched cutoffs instead of silently
// widening or narrowing a lifecycle-resolved snapshot.
[[nodiscard]] inline LifecycleProjectionResult
project_lifecycle(const LifecycleResolution &resolution, LifecycleProjectionContext context) {
  if (context.economic_as_of != resolution.evaluation_context().economic_as_of)
    return {std::unexpected(PositionProjectionError::context_mismatch),
            std::unexpected(CashProjectionError::context_mismatch),
            std::unexpected(SettlementProjectionError::context_mismatch)};
  const auto date = SettlementDate::create(context.settlement_as_of_date);
  if (!date) {
    // Positions do not depend on settlement eligibility.
    portfolio_detail::PositionFold positions;
    for (const auto &resolved : resolution.active_events())
      if (const auto applied = positions.apply(resolved.event()); !applied)
        return {std::unexpected(applied.error()),
                std::unexpected(CashProjectionError::invalid_context),
                std::unexpected(SettlementProjectionError::invalid_context)};
    return {positions.finish(), std::unexpected(CashProjectionError::invalid_context),
            std::unexpected(SettlementProjectionError::invalid_context)};
  }
  return project_lifecycle(resolution, *date);
}

} // namespace luca
