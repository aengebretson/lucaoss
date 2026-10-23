#pragma once
#include "luca/lifecycle.hpp"
#include <map>
namespace luca {
struct TransactionVersion {
  std::string economic_id, version_id;
  std::optional<std::string> predecessor_id, void_version_id, replacement_version_id;
  Timestamp recorded_at;
  std::optional<Timestamp> economic_at, void_effective_at, void_recorded_at;
  LifecycleAction action;
  std::optional<Timestamp> activation_effective_at;
};
// Immutable lineage metadata. Knowledge selection excludes unknown successors;
// effective selection is deliberately separate from displaying version history.
inline std::vector<TransactionVersion> transaction_versions(
    const LifecycleLedger& ledger, Timestamp recorded_through) {
  std::vector<TransactionVersion> out;
  std::map<std::string, std::size_t> index;
  for (const auto& r : ledger.records()) {
    if (r.recorded_at() > recorded_through) continue;
    out.push_back({r.economic_event_id().value(), r.record_id().value(),
        r.causal_record_id() ? std::optional{r.causal_record_id()->value()} : std::nullopt,
        {}, {}, r.recorded_at(), r.effective_at(), {}, {}, r.action(), {}});
    index[r.record_id().value()] = out.size()-1;
    if (r.action() == LifecycleAction::cancel && r.causal_record_id()) {
      auto& original = out.at(index.at(r.causal_record_id()->value()));
      original.void_version_id = r.record_id().value();
      original.void_effective_at = r.cancellation_at();
      original.void_recorded_at = r.recorded_at();
      out.back().void_effective_at = r.cancellation_at();
      out.back().void_recorded_at = r.recorded_at();
      out.back().activation_effective_at = original.activation_effective_at;
    } else if (r.action() == LifecycleAction::correct && r.causal_record_id()) {
      auto& predecessor = out.at(index.at(r.causal_record_id()->value()));
      predecessor.replacement_version_id = r.record_id().value();
      out.back().activation_effective_at = predecessor.activation_effective_at;
      if (predecessor.action == LifecycleAction::cancel && predecessor.predecessor_id) {
        auto& original = out.at(index.at(*predecessor.predecessor_id));
        original.replacement_version_id = r.record_id().value();
        if(predecessor.void_effective_at)
          out.back().activation_effective_at = out.back().activation_effective_at
              ? std::max(*out.back().activation_effective_at,*predecessor.void_effective_at)
              : predecessor.void_effective_at;
      }
    }
  }
  return out;
}
} // namespace luca
