#pragma once

#include "luca/portfolio/checkpoint_resume.hpp"
#include "luca/portfolio/lifecycle_projection.hpp"

#include <expected>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace luca {

// Compatibility failures retain the complete CheckpointResumeError, including
// its stable category. Lifecycle and projection alternatives retain the
// existing engine error types; merge overflow is reported through the matching
// projection error because it has the same exact-arithmetic meaning.
using CheckpointApplyError =
    std::variant<CheckpointResumeError, LifecycleError, PositionProjectionError,
                 CashProjectionError, SettlementProjectionError>;

// Apply only an ordinary suffix that the integrated compatibility contract has
// accepted. Inputs are const and the result is built separately, so every
// failure exposes no partially updated state.
[[nodiscard]] inline std::expected<PortfolioState, CheckpointApplyError>
apply_checkpoint_suffix(const CheckpointResumeRequest &request, const CheckpointManifest &manifest,
                        const PortfolioState &checkpoint_state,
                        std::span<const LifecycleRecord> accepted_prefix,
                        std::span<const LifecycleRecord> proposed_suffix) {
  const auto compatible = check_checkpoint_resume_compatibility(request, manifest, checkpoint_state,
                                                                accepted_prefix, proposed_suffix);
  if (!compatible)
    return std::unexpected(CheckpointApplyError{compatible.error()});

  const auto &context = manifest.evaluation_context();
  const auto resolution = lifecycle_detail::Resolver::resolve(
      proposed_suffix, {context.recorded_through(), context.economic_as_of()});
  portfolio_detail::PositionFold positions{checkpoint_state.positions()};
  portfolio_detail::CashFold cash{context.settlement_as_of_date(), checkpoint_state.settled_cash()};
  portfolio_detail::SettlementFold obligations{context.settlement_as_of_date(),
                                               checkpoint_state.open_settlement_obligations()};
  for (const auto &resolved : resolution.active_events()) {
    // Preserve the established diagnostics: event valuation precedes aggregate
    // overflow checks, then accumulation follows position/cash/obligation order.
    const auto cash_change = cash.change(resolved.event());
    if (!cash_change)
      return std::unexpected(CheckpointApplyError{cash_change.error()});
    const auto obligation_change = obligations.change(resolved.event());
    if (!obligation_change)
      return std::unexpected(CheckpointApplyError{obligation_change.error()});
    if (auto applied = positions.apply(resolved.event()); !applied)
      return std::unexpected(CheckpointApplyError{applied.error()});
    if (*cash_change)
      if (auto applied = cash.add(resolved.record().account(), **cash_change); !applied)
        return std::unexpected(CheckpointApplyError{applied.error()});
    if (*obligation_change)
      if (auto applied = obligations.add(**obligation_change); !applied)
        return std::unexpected(CheckpointApplyError{applied.error()});
  }
  return PortfolioState{positions.finish(), cash.finish(), obligations.finish()};
}

} // namespace luca
