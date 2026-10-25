#pragma once

#include "luca/portfolio/checkpoint_apply.hpp"

#include <cstdint>
#include <exception>
#include <expected>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace luca {

// Both values own their data. Publication occurs only after state application
// and every refreshed manifest factory have succeeded.
struct CheckpointApplicationResult {
  PortfolioState state;
  CheckpointManifest manifest;
};

enum class CheckpointResultConstructionCategory { count_overflow, canonical_encoding };

struct CheckpointResultConstructionError {
  CheckpointResultConstructionCategory category;
  std::string message;
};

// Original application diagnostics are retained without translating categories
// or messages. CheckpointError is the original validating-factory diagnostic.
using CheckpointApplicationError =
    std::variant<CheckpointResumeError, LifecycleError, PositionProjectionError,
                 CashProjectionError, SettlementProjectionError, CheckpointError,
                 CheckpointResultConstructionError>;


// Continue the existing ordered financial fold under exactly the same context.
// Reconstituting the full history below derives evidence only; the authoritative
// portfolio calculation and compatibility decision remain apply_checkpoint_suffix.
[[nodiscard]] inline std::expected<CheckpointApplicationResult, CheckpointApplicationError>
apply_checkpoint_suffix_with_manifest(const CheckpointResumeRequest &request,
                                      const CheckpointManifest &manifest,
                                      const PortfolioState &checkpoint_state,
                                      std::span<const LifecycleRecord> accepted_prefix,
                                      std::span<const LifecycleRecord> proposed_suffix) {
  auto state = apply_checkpoint_suffix(request, manifest, checkpoint_state, accepted_prefix,
                                       proposed_suffix);
  if (!state) {
    return std::unexpected(std::visit(
        [](const auto &error) -> CheckpointApplicationError { return error; }, state.error()));
  }

  std::vector<LifecycleRecordDraft> drafts;
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  // Compatibility has already checked the sequence boundary. Explicitly check
  // both the wire count and the host container size before adding or narrowing.
  if (accepted_prefix.size() > maximum || proposed_suffix.size() > maximum ||
      proposed_suffix.size() > maximum - accepted_prefix.size() ||
      accepted_prefix.size() > drafts.max_size() ||
      proposed_suffix.size() > drafts.max_size() - accepted_prefix.size()) {
    return std::unexpected(CheckpointApplicationError{CheckpointResultConstructionError{
        CheckpointResultConstructionCategory::count_overflow,
        "full accepted record count is not representable"}});
  }
  const auto count = accepted_prefix.size() + proposed_suffix.size();
  drafts.reserve(count);
  for (const auto records : {accepted_prefix, proposed_suffix}) {
    for (const auto &record : records)
      drafts.push_back(lifecycle_detail::draft_from(record));
  }

  LifecycleLedger full_ledger;
  if (const auto accepted = full_ledger.accept_batch(drafts); !accepted)
    return std::unexpected(CheckpointApplicationError{accepted.error()});

  const auto &context = manifest.evaluation_context();
  const auto resolution = full_ledger.resolve(context.recorded_through(), context.economic_as_of());
  std::vector<EventId> lifecycle_ids;
  std::vector<EventId> active_ids;
  std::vector<SourceRecordId> source_ids;
  std::unordered_set<std::string_view> seen_sources;
  lifecycle_ids.reserve(count);
  active_ids.reserve(resolution.active_events().size());
  for (const auto &record : full_ledger.records()) {
    lifecycle_ids.push_back(record.record_id());
    for (const auto &source : record.provenance().source_records()) {
      if (seen_sources.emplace(source.value()).second)
        source_ids.push_back(source);
    }
  }
  for (const auto &active : resolution.active_events())
    active_ids.push_back(active.record().record_id());

  // v1 requires nonempty active lineage and an actual resolved watermark.
  // Keep that factory diagnostic rather than inventing an empty-state sentinel.
  const auto lineage = CheckpointLineage::create(lifecycle_ids, active_ids, source_ids);
  if (!lineage)
    return std::unexpected(CheckpointApplicationError{lineage.error()});
  const auto &last_active = resolution.active_events().back().record();
  const auto watermark = ResolvedEventWatermark::create(
      *last_active.effective_at(), last_active.acceptance_sequence().value(), last_active.record_id());
  if (!watermark)
    return std::unexpected(CheckpointApplicationError{watermark.error()});

  // Existing canonical encoders throw for nonrepresentable typed values. Only
  // those logical encoding failures are translated; resource exhaustion follows
  // the standard containers' exception behavior, as in the existing APIs.
  try {
    const auto input_digest = Sha256Digest::create(serialization::canonical_digest(full_ledger));
    if (!input_digest)
      return std::unexpected(CheckpointApplicationError{input_digest.error()});
    const auto state_digest = Sha256Digest::create(serialization::canonical_digest(*state));
    if (!state_digest)
      return std::unexpected(CheckpointApplicationError{state_digest.error()});
    const auto &last_record = full_ledger.records().back();
    const auto prefix = CheckpointEventPrefix::create(
        1, last_record.acceptance_sequence().value(), static_cast<std::uint64_t>(count),
        last_record.record_id(), *input_digest);
    if (!prefix)
      return std::unexpected(CheckpointApplicationError{prefix.error()});
    auto refreshed = CheckpointManifest::create(
        manifest.projection(), manifest.engine_version(), manifest.policy(), manifest.partition(),
        *prefix, context, *state_digest, *watermark, *lineage);
    if (!refreshed)
      return std::unexpected(CheckpointApplicationError{refreshed.error()});
    return CheckpointApplicationResult{std::move(*state), std::move(*refreshed)};
  } catch (const std::logic_error &error) {
    return std::unexpected(CheckpointApplicationError{CheckpointResultConstructionError{
        CheckpointResultConstructionCategory::canonical_encoding, error.what()}});
  }
}

} // namespace luca
