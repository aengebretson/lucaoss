#pragma once

#include "luca/lifecycle.hpp"
#include "luca/portfolio/checkpoint_serialization.hpp"
#include "luca/portfolio/serialization.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace luca {

namespace checkpoint_resume_detail {
struct CheckpointResumeValidation;
}

enum class CheckpointResumeDiagnosticCategory {
  schema_shape,
  unsupported_version,
  digest_mismatch,
  incompatible_projection,
  incompatible_engine,
  incompatible_policy,
  incompatible_partition,
  incompatible_context,
  incompatible_prefix,
  prefix_continuity,
  late_lifecycle_knowledge,
  incompatible_account,
  incompatible_event_relationship,
  conflicting_lifecycle_successor,
};

[[nodiscard]] constexpr std::string_view
category_name(CheckpointResumeDiagnosticCategory category) noexcept {
  switch (category) {
  case CheckpointResumeDiagnosticCategory::schema_shape:
    return "schema_shape";
  case CheckpointResumeDiagnosticCategory::unsupported_version:
    return "unsupported_version";
  case CheckpointResumeDiagnosticCategory::digest_mismatch:
    return "digest_mismatch";
  case CheckpointResumeDiagnosticCategory::incompatible_projection:
    return "incompatible_projection";
  case CheckpointResumeDiagnosticCategory::incompatible_engine:
    return "incompatible_engine";
  case CheckpointResumeDiagnosticCategory::incompatible_policy:
    return "incompatible_policy";
  case CheckpointResumeDiagnosticCategory::incompatible_partition:
    return "incompatible_partition";
  case CheckpointResumeDiagnosticCategory::incompatible_context:
    return "incompatible_context";
  case CheckpointResumeDiagnosticCategory::incompatible_prefix:
    return "incompatible_prefix";
  case CheckpointResumeDiagnosticCategory::prefix_continuity:
    return "prefix_continuity";
  case CheckpointResumeDiagnosticCategory::late_lifecycle_knowledge:
    return "late_lifecycle_knowledge";
  case CheckpointResumeDiagnosticCategory::incompatible_account:
    return "incompatible_account";
  case CheckpointResumeDiagnosticCategory::incompatible_event_relationship:
    return "incompatible_event_relationship";
  case CheckpointResumeDiagnosticCategory::conflicting_lifecycle_successor:
    return "conflicting_lifecycle_successor";
  }
  return "schema_shape";
}

class CheckpointResumeError {
public:
  [[nodiscard]] CheckpointResumeDiagnosticCategory category() const noexcept { return category_; }
  [[nodiscard]] std::string_view category_name() const noexcept {
    return luca::category_name(category_);
  }
  [[nodiscard]] const std::string &message() const noexcept { return message_; }

private:
  friend struct checkpoint_resume_detail::CheckpointResumeValidation;

  CheckpointResumeError(CheckpointResumeDiagnosticCategory category, std::string message)
      : category_(category), message_(std::move(message)) {}

  CheckpointResumeDiagnosticCategory category_;
  std::string message_;
};

namespace checkpoint_resume_detail {

struct CheckpointResumeValidation {
  [[nodiscard]] static CheckpointResumeError error(CheckpointResumeDiagnosticCategory category,
                                                   std::string message) {
    return CheckpointResumeError{category, std::move(message)};
  }
};

[[nodiscard]] inline std::unexpected<CheckpointResumeError>
failure(CheckpointResumeDiagnosticCategory category, std::string message) {
  return std::unexpected(CheckpointResumeValidation::error(category, std::move(message)));
}

} // namespace checkpoint_resume_detail

class CheckpointResumeRequest {
public:
  static constexpr std::string_view schema_version = "luca.checkpoint-resume.v1";
  static constexpr std::string_view serialization_version = "luca.canonical-bytes.v1";

  [[nodiscard]] static std::expected<CheckpointResumeRequest, CheckpointResumeError> create(
      std::string_view requested_schema_version, std::string_view requested_serialization_version,
      const Sha256Digest &checkpoint_manifest_digest, const CheckpointIdentity &projection,
      std::string_view engine_version, const CheckpointIdentity &policy,
      const AccountSetPartition &partition, const CheckpointEvaluationContext &evaluation_context,
      const CheckpointEventPrefix &checkpoint_event_prefix,
      const Sha256Digest &checkpoint_state_digest) {
    if (requested_schema_version != schema_version) {
      return checkpoint_resume_detail::failure(
          CheckpointResumeDiagnosticCategory::unsupported_version,
          "checkpoint-resume schema version is not supported");
    }
    if (requested_serialization_version != serialization_version) {
      return checkpoint_resume_detail::failure(
          CheckpointResumeDiagnosticCategory::unsupported_version,
          "checkpoint-resume serialization version is not supported");
    }
    if (auto valid = checkpoint_detail::validate_text(engine_version, "engine version"); !valid) {
      return checkpoint_resume_detail::failure(CheckpointResumeDiagnosticCategory::schema_shape,
                                               valid.error().message());
    }

    return CheckpointResumeRequest{checkpoint_manifest_digest,
                                   projection,
                                   std::string{engine_version},
                                   policy,
                                   partition,
                                   evaluation_context,
                                   checkpoint_event_prefix,
                                   checkpoint_state_digest};
  }

  [[nodiscard]] const Sha256Digest &checkpoint_manifest_digest() const noexcept {
    return checkpoint_manifest_digest_;
  }
  [[nodiscard]] const CheckpointIdentity &projection() const noexcept { return projection_; }
  [[nodiscard]] const std::string &engine_version() const noexcept { return engine_version_; }
  [[nodiscard]] const CheckpointIdentity &policy() const noexcept { return policy_; }
  [[nodiscard]] const AccountSetPartition &partition() const noexcept { return partition_; }
  [[nodiscard]] const CheckpointEvaluationContext &evaluation_context() const noexcept {
    return evaluation_context_;
  }
  [[nodiscard]] const CheckpointEventPrefix &checkpoint_event_prefix() const noexcept {
    return checkpoint_event_prefix_;
  }
  [[nodiscard]] const Sha256Digest &checkpoint_state_digest() const noexcept {
    return checkpoint_state_digest_;
  }
  bool operator==(const CheckpointResumeRequest &) const = default;

private:
  CheckpointResumeRequest(Sha256Digest checkpoint_manifest_digest, CheckpointIdentity projection,
                          std::string engine_version, CheckpointIdentity policy,
                          AccountSetPartition partition,
                          CheckpointEvaluationContext evaluation_context,
                          CheckpointEventPrefix checkpoint_event_prefix,
                          Sha256Digest checkpoint_state_digest)
      : checkpoint_manifest_digest_(std::move(checkpoint_manifest_digest)),
        projection_(std::move(projection)), engine_version_(std::move(engine_version)),
        policy_(std::move(policy)), partition_(std::move(partition)),
        evaluation_context_(std::move(evaluation_context)),
        checkpoint_event_prefix_(std::move(checkpoint_event_prefix)),
        checkpoint_state_digest_(std::move(checkpoint_state_digest)) {}

  Sha256Digest checkpoint_manifest_digest_;
  CheckpointIdentity projection_;
  std::string engine_version_;
  CheckpointIdentity policy_;
  AccountSetPartition partition_;
  CheckpointEvaluationContext evaluation_context_;
  CheckpointEventPrefix checkpoint_event_prefix_;
  Sha256Digest checkpoint_state_digest_;
};

} // namespace luca

namespace luca::serialization {

[[nodiscard]] inline CanonicalBytes canonical_bytes(const CheckpointResumeRequest &value) {
  auto output = detail::top_level_bytes();
  detail::append_map(output, 10);
  detail::append_key(output, "checkpoint_event_prefix");
  detail::append_checkpoint_prefix(output, value.checkpoint_event_prefix());
  detail::append_key(output, "checkpoint_manifest_digest");
  detail::append_text(output, value.checkpoint_manifest_digest().value());
  detail::append_key(output, "checkpoint_state_digest");
  detail::append_text(output, value.checkpoint_state_digest().value());
  detail::append_key(output, "engine_version");
  detail::append_text(output, value.engine_version());
  detail::append_key(output, "evaluation_context");
  detail::append_checkpoint_context(output, value.evaluation_context());
  detail::append_key(output, "partition");
  detail::append_checkpoint_partition(output, value.partition());
  detail::append_key(output, "policy");
  detail::append_checkpoint_identity(output, value.policy());
  detail::append_key(output, "projection");
  detail::append_checkpoint_identity(output, value.projection());
  detail::append_key(output, "schema_version");
  detail::append_text(output, CheckpointResumeRequest::schema_version);
  detail::append_key(output, "serialization_version");
  detail::append_text(output, CheckpointResumeRequest::serialization_version);
  return output;
}

[[nodiscard]] inline std::string canonical_digest(const CheckpointResumeRequest &value) {
  return detail::sha256_hex(canonical_bytes(value));
}

} // namespace luca::serialization

namespace luca {
namespace checkpoint_resume_detail {

[[nodiscard]] inline std::string
canonical_record_sequence_digest(std::span<const LifecycleRecord> records) {
  auto output = serialization::detail::top_level_bytes();
  serialization::detail::append_map(output, 2);
  serialization::detail::append_key(output, "records");
  serialization::detail::append_array(output, static_cast<std::uint64_t>(records.size()));
  for (const auto &record : records)
    serialization::detail::append_lifecycle_record(output, record);
  serialization::detail::append_key(output, "schema_version");
  serialization::detail::append_text(output, "luca.lifecycle-record-sequence.v1");
  return serialization::detail::sha256_hex(output);
}

[[nodiscard]] inline bool partition_contains(const AccountSetPartition &partition,
                                             const AccountId &account) {
  return std::ranges::find(partition.keys(), account) != partition.keys().end();
}

[[nodiscard]] inline std::expected<void, CheckpointResumeError> verify_partition_membership(
    const AccountSetPartition &partition, std::span<const LifecycleRecord> prefix,
    const PortfolioState &checkpoint_state, std::span<const LifecycleRecord> suffix) {
  const auto verify_account =
      [&](const AccountId &account,
          std::string_view value_kind) -> std::expected<void, CheckpointResumeError> {
    if (!partition_contains(partition, account)) {
      return failure(CheckpointResumeDiagnosticCategory::incompatible_partition,
                     std::string{value_kind} + " account is outside the checkpoint partition");
    }
    return {};
  };

  for (const auto &record : prefix) {
    if (auto valid = verify_account(record.account(), "prefix record"); !valid)
      return valid;
  }
  for (const auto &position : checkpoint_state.positions()) {
    if (auto valid = verify_account(position.key().account(), "position"); !valid)
      return valid;
  }
  for (const auto &cash : checkpoint_state.settled_cash()) {
    if (auto valid = verify_account(cash.key().account(), "settled cash"); !valid)
      return valid;
  }
  for (const auto &obligation : checkpoint_state.open_settlement_obligations()) {
    if (auto valid = verify_account(obligation.key().account(), "settlement obligation"); !valid)
      return valid;
  }
  for (const auto &record : suffix) {
    if (auto valid = verify_account(record.account(), "suffix record"); !valid)
      return valid;
  }
  return {};
}

[[nodiscard]] inline bool exact_opposite(std::int64_t candidate, std::int64_t target) noexcept {
  return target != std::numeric_limits<std::int64_t>::min() && candidate == -target;
}

[[nodiscard]] inline std::expected<void, CheckpointResumeError>
verify_event_relationship(const LifecycleRecord &record, const LifecycleRecord &target) {
  if (target.action() == LifecycleAction::cancel || target.action() == LifecycleAction::reverse) {
    return failure(CheckpointResumeDiagnosticCategory::incompatible_event_relationship,
                   "cancellations and reversals are terminal lifecycle records");
  }
  if (record.account() != target.account()) {
    return failure(CheckpointResumeDiagnosticCategory::incompatible_account,
                   "lifecycle relationship crosses account identity");
  }

  if (record.action() == LifecycleAction::correct || record.action() == LifecycleAction::cancel) {
    if (record.economic_event_id() != target.economic_event_id()) {
      return failure(CheckpointResumeDiagnosticCategory::incompatible_event_relationship,
                     "correction or cancellation changes economic-event identity");
    }
  } else if (record.action() != LifecycleAction::reverse) {
    return failure(CheckpointResumeDiagnosticCategory::incompatible_event_relationship,
                   "an originating record must not have a causal target");
  } else if (record.economic_event_id() == target.economic_event_id()) {
    return failure(CheckpointResumeDiagnosticCategory::incompatible_event_relationship,
                   "reversal must start a distinct economic-event identity");
  }

  if (record.action() == LifecycleAction::cancel) {
    if (record.cancellation_at() && target.effective_at() && *record.cancellation_at() < *target.effective_at())
      return failure(CheckpointResumeDiagnosticCategory::incompatible_event_relationship, "void precedes target");
    return {};
  }
  if (record.event() == nullptr || target.event() == nullptr ||
      record.event()->index() != target.event()->index()) {
    return failure(CheckpointResumeDiagnosticCategory::incompatible_event_relationship,
                   "lifecycle relationship changes event type");
  }

  if (const auto *cash = std::get_if<CashMovement>(record.event())) {
    const auto &target_cash = std::get<CashMovement>(*target.event());
    if (cash->amount().currency() != target_cash.amount().currency()) {
      return failure(CheckpointResumeDiagnosticCategory::incompatible_event_relationship,
                     "cash lifecycle relationship changes currency");
    }
    if (record.action() == LifecycleAction::reverse &&
        !exact_opposite(cash->amount().scaled_value(), target_cash.amount().scaled_value())) {
      return failure(CheckpointResumeDiagnosticCategory::incompatible_event_relationship,
                     "cash reversal is not the exact offset");
    }
  } else {
    const auto &trade = std::get<EquityTrade>(*record.event());
    const auto &target_trade = std::get<EquityTrade>(*target.event());
    if (trade.instrument() != target_trade.instrument() ||
        trade.quote_currency() != target_trade.quote_currency()) {
      return failure(CheckpointResumeDiagnosticCategory::incompatible_event_relationship,
                     "equity lifecycle relationship changes its natural key");
    }
    if (record.action() == LifecycleAction::reverse &&
        (trade.charges().has_value() || target_trade.charges().has_value() ||
          trade.price() != target_trade.price() ||
         !exact_opposite(trade.quantity().scaled_value(),
                         target_trade.quantity().scaled_value()))) {
      return failure(CheckpointResumeDiagnosticCategory::incompatible_event_relationship,
                     "equity reversal does not exactly offset quantity and price");
    }
  }

  if (record.action() == LifecycleAction::reverse &&
      record.effective_at() < target.effective_at()) {
    return failure(CheckpointResumeDiagnosticCategory::incompatible_event_relationship,
                   "reversal economically predates its target");
  }
  return {};
}

[[nodiscard]] inline std::expected<void, CheckpointResumeError>
verify_suffix_relationships(std::span<const LifecycleRecord> prefix,
                            std::span<const LifecycleRecord> suffix) {
  std::unordered_set<std::string_view> prefix_ids;
  prefix_ids.reserve(prefix.size());
  for (const auto &record : prefix)
    prefix_ids.emplace(record.record_id().value());

  std::unordered_map<std::string_view, const LifecycleRecord *> records;
  records.reserve(prefix.size() + suffix.size());
  std::unordered_set<std::string_view> economic_origins;
  economic_origins.reserve(prefix.size() + suffix.size());
  std::unordered_set<std::string_view> successors;
  successors.reserve(prefix.size() + suffix.size());
  for (const auto &record : prefix) {
    records.emplace(record.record_id().value(), &record);
    if (record.action() == LifecycleAction::originate ||
        record.action() == LifecycleAction::reverse)
      economic_origins.emplace(record.economic_event_id().value());
    if (record.causal_record_id())
      successors.emplace(record.causal_record_id()->value());
  }

  auto prior_recorded_at = prefix.back().recorded_at();
  for (const auto &record : suffix) {
    if (record.recorded_at() < prior_recorded_at) {
      return failure(CheckpointResumeDiagnosticCategory::incompatible_event_relationship,
                     "recorded time decreases across the checkpoint suffix");
    }
    prior_recorded_at = record.recorded_at();
    if (records.contains(record.record_id().value())) {
      return failure(CheckpointResumeDiagnosticCategory::incompatible_event_relationship,
                     "suffix repeats a lifecycle record identity");
    }
    if ((record.action() == LifecycleAction::originate ||
         record.action() == LifecycleAction::reverse) &&
        !economic_origins.emplace(record.economic_event_id().value()).second) {
      return failure(CheckpointResumeDiagnosticCategory::incompatible_event_relationship,
                     "suffix repeats an economic-event origin identity");
    }

    if (!record.causal_record_id()) {
      if (record.action() != LifecycleAction::originate) {
        return failure(CheckpointResumeDiagnosticCategory::incompatible_event_relationship,
                       "non-originating suffix record has no causal target");
      }
    } else {
      if (prefix_ids.contains(record.causal_record_id()->value())) {
        return failure(CheckpointResumeDiagnosticCategory::late_lifecycle_knowledge,
                       "suffix lifecycle record targets checkpoint-prefix knowledge");
      }
      const auto target = records.find(record.causal_record_id()->value());
      if (target == records.end()) {
        return failure(CheckpointResumeDiagnosticCategory::incompatible_event_relationship,
                       "suffix causal target is not an earlier accepted record");
      }
      if (!successors.emplace(record.causal_record_id()->value()).second) {
        return failure(CheckpointResumeDiagnosticCategory::conflicting_lifecycle_successor,
                       "causal target has more than one lifecycle successor");
      }
      if (auto valid = verify_event_relationship(record, *target->second); !valid)
        return valid;
    }
    records.emplace(record.record_id().value(), &record);
  }
  return {};
}

[[nodiscard]] inline std::expected<void, CheckpointResumeError>
verify_manifest_lineage(const CheckpointManifest &manifest,
                        std::span<const LifecycleRecord> prefix) {
  const auto lifecycle_ids = manifest.lineage().lifecycle_record_ids();
  if (lifecycle_ids.size() != prefix.size()) {
    return failure(CheckpointResumeDiagnosticCategory::incompatible_prefix,
                   "manifest lifecycle lineage does not span the accepted prefix");
  }
  for (std::size_t index = 0; index < prefix.size(); ++index) {
    if (lifecycle_ids[index] != prefix[index].record_id()) {
      return failure(CheckpointResumeDiagnosticCategory::incompatible_prefix,
                     "manifest lifecycle lineage differs from the accepted prefix");
    }
  }

  std::vector<SourceRecordId> expected_sources;
  std::unordered_set<std::string_view> seen_sources;
  for (const auto &record : prefix) {
    for (const auto &source : record.provenance().source_records()) {
      if (seen_sources.emplace(source.value()).second)
        expected_sources.push_back(source);
    }
  }
  if (!std::ranges::equal(expected_sources, manifest.lineage().source_record_ids())) {
    return failure(CheckpointResumeDiagnosticCategory::incompatible_prefix,
                   "manifest source lineage differs from the accepted prefix");
  }

  std::unordered_map<std::string_view, const LifecycleRecord *> selected_heads;
  for (const auto &record : prefix) {
    if (record.recorded_at() <= manifest.evaluation_context().recorded_through() &&
        (!record.cancellation_at() || *record.cancellation_at() <= manifest.evaluation_context().economic_as_of()))
      selected_heads[record.economic_event_id().value()] = &record;
  }
  std::vector<const LifecycleRecord *> active;
  active.reserve(selected_heads.size());
  for (const auto &[economic_id, record] : selected_heads) {
    static_cast<void>(economic_id);
    if (record->action() != LifecycleAction::cancel && record->event() != nullptr &&
        *record->effective_at() <= manifest.evaluation_context().economic_as_of())
      active.push_back(record);
  }
  std::ranges::sort(active, [](const LifecycleRecord *left, const LifecycleRecord *right) {
    return std::pair{*left->effective_at(), left->acceptance_sequence().value()} <
           std::pair{*right->effective_at(), right->acceptance_sequence().value()};
  });

  const auto active_ids = manifest.lineage().active_record_ids();
  if (active.empty() || active.size() != active_ids.size()) {
    return failure(CheckpointResumeDiagnosticCategory::incompatible_prefix,
                   "manifest active lineage differs from the context-selected prefix");
  }
  for (std::size_t index = 0; index < active.size(); ++index) {
    if (active[index]->record_id() != active_ids[index]) {
      return failure(CheckpointResumeDiagnosticCategory::incompatible_prefix,
                     "manifest active lineage is not the resolved prefix lineage");
    }
  }

  const auto &watermark = manifest.resolved_event_watermark();
  const auto *last = active.back();
  if (watermark.record_id() != last->record_id() ||
      watermark.acceptance_sequence() != last->acceptance_sequence().value() ||
      watermark.effective_at() != *last->effective_at()) {
    return failure(CheckpointResumeDiagnosticCategory::incompatible_prefix,
                   "manifest resolved-event watermark differs from the accepted prefix");
  }
  return {};
}

} // namespace checkpoint_resume_detail

[[nodiscard]] inline std::expected<void, CheckpointResumeError>
check_checkpoint_resume_compatibility(const CheckpointResumeRequest &request,
                                      const CheckpointManifest &manifest,
                                      const PortfolioState &checkpoint_state,
                                      std::span<const LifecycleRecord> accepted_prefix,
                                      std::span<const LifecycleRecord> proposed_suffix) {
  using checkpoint_resume_detail::failure;

  if (accepted_prefix.empty()) {
    return failure(CheckpointResumeDiagnosticCategory::prefix_continuity,
                   "accepted checkpoint prefix must not be empty");
  }
  if (accepted_prefix.size() != manifest.event_prefix().record_count() ||
      accepted_prefix.back().record_id() != manifest.event_prefix().last_record_id()) {
    return failure(CheckpointResumeDiagnosticCategory::incompatible_prefix,
                   "accepted prefix does not match manifest prefix metadata");
  }
  for (std::size_t index = 0; index < accepted_prefix.size(); ++index) {
    const auto expected = manifest.event_prefix().first_sequence() + index;
    if (accepted_prefix[index].acceptance_sequence().value() != expected) {
      return failure(CheckpointResumeDiagnosticCategory::prefix_continuity,
                     "accepted checkpoint prefix is not contiguous");
    }
  }

  try {
    if (checkpoint_resume_detail::canonical_record_sequence_digest(accepted_prefix) !=
        manifest.event_prefix().canonical_input_digest().value()) {
      return failure(CheckpointResumeDiagnosticCategory::digest_mismatch,
                     "accepted-prefix digest does not match the checkpoint manifest");
    }
    if (serialization::canonical_digest(checkpoint_state) !=
        manifest.canonical_state_digest().value()) {
      return failure(CheckpointResumeDiagnosticCategory::digest_mismatch,
                     "portfolio-state digest does not match the checkpoint manifest");
    }
    if (serialization::canonical_digest(manifest) != request.checkpoint_manifest_digest().value()) {
      return failure(CheckpointResumeDiagnosticCategory::digest_mismatch,
                     "resume request does not identify the supplied checkpoint manifest");
    }
  } catch (const std::exception &error) {
    return failure(CheckpointResumeDiagnosticCategory::digest_mismatch,
                   std::string{"checkpoint digest verification failed: "} + error.what());
  }

  if (request.projection() != manifest.projection()) {
    return failure(CheckpointResumeDiagnosticCategory::incompatible_projection,
                   "projection identity or version changed");
  }
  if (request.engine_version() != manifest.engine_version()) {
    return failure(CheckpointResumeDiagnosticCategory::incompatible_engine,
                   "engine version changed");
  }
  if (request.policy() != manifest.policy()) {
    return failure(CheckpointResumeDiagnosticCategory::incompatible_policy,
                   "policy identity or version changed");
  }
  if (request.partition() != manifest.partition()) {
    return failure(CheckpointResumeDiagnosticCategory::incompatible_partition,
                   "partition definition changed");
  }
  if (request.evaluation_context() != manifest.evaluation_context()) {
    return failure(CheckpointResumeDiagnosticCategory::incompatible_context,
                   "evaluation context or explicit context input changed");
  }
  if (request.checkpoint_event_prefix() != manifest.event_prefix()) {
    return failure(CheckpointResumeDiagnosticCategory::incompatible_prefix,
                   "resume request identifies a different event prefix");
  }
  if (request.checkpoint_state_digest() != manifest.canonical_state_digest()) {
    return failure(CheckpointResumeDiagnosticCategory::digest_mismatch,
                   "resume request identifies a different checkpoint-state digest");
  }

  if (auto valid = checkpoint_resume_detail::verify_manifest_lineage(manifest, accepted_prefix);
      !valid)
    return valid;
  if (auto valid = checkpoint_resume_detail::verify_partition_membership(
          manifest.partition(), accepted_prefix, checkpoint_state, proposed_suffix);
      !valid)
    return valid;

  if (proposed_suffix.empty()) {
    return failure(CheckpointResumeDiagnosticCategory::prefix_continuity,
                   "checkpoint-resume suffix must not be empty");
  }
  auto expected_sequence = manifest.event_prefix().last_sequence();
  for (const auto &record : proposed_suffix) {
    if (expected_sequence == std::numeric_limits<std::uint64_t>::max()) {
      return failure(CheckpointResumeDiagnosticCategory::prefix_continuity,
                     "checkpoint prefix has no representable successor sequence");
    }
    ++expected_sequence;
    if (record.acceptance_sequence().value() != expected_sequence) {
      return failure(CheckpointResumeDiagnosticCategory::prefix_continuity,
                     "suffix acceptance sequences are not contiguous with the checkpoint prefix");
    }
  }

  if (auto valid =
          checkpoint_resume_detail::verify_suffix_relationships(accepted_prefix, proposed_suffix);
      !valid)
    return valid;

  const auto watermark_key = std::pair{manifest.resolved_event_watermark().effective_at(),
                                       manifest.resolved_event_watermark().acceptance_sequence()};
  for (const auto &record : proposed_suffix) {
    if (record.event() != nullptr &&
        std::pair{*record.effective_at(), record.acceptance_sequence().value()} <= watermark_key) {
      return failure(CheckpointResumeDiagnosticCategory::late_lifecycle_knowledge,
                     "suffix payload sorts at or before resolved checkpoint state");
    }
  }
  return {};
}

} // namespace luca
