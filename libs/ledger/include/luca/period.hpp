#pragma once
#include "luca/lifecycle.hpp"

namespace luca {
// Pure admission rule. A host serializes closing with canonical admission and
// retains the accepted prefix in its close receipt. No persistence in this rule.
[[nodiscard]] inline bool changes_closed_period(
    const LifecycleRecord& candidate, const LifecycleRecord* target,
    std::optional<Timestamp> closed_through) {
  if (!closed_through) return false;
  if (candidate.cancellation_at()) return *candidate.cancellation_at() <= *closed_through;
  if (candidate.effective_at() && *candidate.effective_at() <= *closed_through) return true;
  if (candidate.action() == LifecycleAction::correct || candidate.action() == LifecycleAction::cancel)
    return target && target->effective_at() && *target->effective_at() <= *closed_through;
  return false;
}
} // namespace luca
