#pragma once

#include "luca/event.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace luca {

struct EconomicEventIdTag;
using EconomicEventId = Identifier<EconomicEventIdTag>;

enum class LifecycleAction { originate, correct, cancel, reverse };

// A lifecycle draft deliberately has no acceptance sequence. The sequence is
// assigned only after the complete acceptance operation validates successfully.
class LifecycleRecordDraft {
 public:
  [[nodiscard]] static LifecycleRecordDraft originate(
      EconomicEventId economic_event_id, Timestamp recorded_at, EconomicEvent event) {
    const auto record_id = header(event).id();
    const auto account = header(event).account();
    const auto provenance = header(event).provenance();
    return LifecycleRecordDraft(record_id, std::move(economic_event_id), account,
                                LifecycleAction::originate, recorded_at, provenance,
                                std::move(event), std::nullopt);
  }

  [[nodiscard]] static LifecycleRecordDraft correct(
      EconomicEventId economic_event_id, EventId supersedes_record_id,
      Timestamp recorded_at, EconomicEvent event) {
    const auto record_id = header(event).id();
    const auto account = header(event).account();
    const auto provenance = header(event).provenance();
    return LifecycleRecordDraft(record_id, std::move(economic_event_id), account,
                                LifecycleAction::correct, recorded_at, provenance,
                                std::move(event), std::move(supersedes_record_id));
  }

  [[nodiscard]] static LifecycleRecordDraft cancel(
      EventId record_id, EconomicEventId economic_event_id,
      EventId supersedes_record_id, AccountId account, Timestamp recorded_at,
      Provenance provenance, std::optional<Timestamp> cancellation_at = {}) {
    auto result = LifecycleRecordDraft(
        std::move(record_id), std::move(economic_event_id), std::move(account),
        LifecycleAction::cancel, recorded_at, std::move(provenance), std::nullopt,
        std::move(supersedes_record_id));
    result.cancellation_at_ = cancellation_at;
    return result;
  }

  [[nodiscard]] static LifecycleRecordDraft reverse(
      EconomicEventId economic_event_id, EventId reverses_record_id,
      Timestamp recorded_at, EconomicEvent event) {
    const auto record_id = header(event).id();
    const auto account = header(event).account();
    const auto provenance = header(event).provenance();
    return LifecycleRecordDraft(record_id, std::move(economic_event_id), account,
                                LifecycleAction::reverse, recorded_at, provenance,
                                std::move(event), std::move(reverses_record_id));
  }

  [[nodiscard]] const EventId& record_id() const noexcept { return record_id_; }
  [[nodiscard]] const EconomicEventId& economic_event_id() const noexcept {
    return economic_event_id_;
  }
  [[nodiscard]] const AccountId& account() const noexcept { return account_; }
  [[nodiscard]] LifecycleAction action() const noexcept { return action_; }
  [[nodiscard]] Timestamp recorded_at() const noexcept { return recorded_at_; }
  [[nodiscard]] const Provenance& provenance() const noexcept { return provenance_; }
  [[nodiscard]] const EconomicEvent* event() const noexcept {
    return event_ ? &*event_ : nullptr;
  }
  [[nodiscard]] std::optional<Timestamp> cancellation_at() const noexcept { return cancellation_at_; }
  [[nodiscard]] const std::optional<EventId>& causal_record_id() const noexcept {
    return causal_record_id_;
  }

 private:
  LifecycleRecordDraft(EventId record_id, EconomicEventId economic_event_id,
                       AccountId account, LifecycleAction action,
                       Timestamp recorded_at, Provenance provenance,
                       std::optional<EconomicEvent> event,
                       std::optional<EventId> causal_record_id)
      : record_id_(std::move(record_id)),
        economic_event_id_(std::move(economic_event_id)),
        account_(std::move(account)),
        action_(action),
        recorded_at_(recorded_at),
        provenance_(std::move(provenance)),
        event_(std::move(event)),
        causal_record_id_(std::move(causal_record_id)) {}

  EventId record_id_;
  EconomicEventId economic_event_id_;
  AccountId account_;
  LifecycleAction action_;
  Timestamp recorded_at_;
  Provenance provenance_;
  std::optional<EconomicEvent> event_;
  std::optional<EventId> causal_record_id_;
  std::optional<Timestamp> cancellation_at_;
};

class LifecycleSequence {
 public:
  static constexpr std::uint64_t first_value = 1;
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  auto operator<=>(const LifecycleSequence&) const = default;

 private:
  friend class LifecycleLedger;
  explicit constexpr LifecycleSequence(std::uint64_t value) noexcept : value_(value) {}
  std::uint64_t value_;
};

class LifecycleRecord {
 public:
  [[nodiscard]] const EventId& record_id() const noexcept { return record_id_; }
  [[nodiscard]] const EconomicEventId& economic_event_id() const noexcept {
    return economic_event_id_;
  }
  [[nodiscard]] const AccountId& account() const noexcept { return account_; }
  [[nodiscard]] LifecycleAction action() const noexcept { return action_; }
  [[nodiscard]] Timestamp recorded_at() const noexcept { return recorded_at_; }
  [[nodiscard]] LifecycleSequence acceptance_sequence() const noexcept {
    return acceptance_sequence_;
  }
  [[nodiscard]] const Provenance& provenance() const noexcept { return provenance_; }
  [[nodiscard]] const EconomicEvent* event() const noexcept {
    return event_ ? &*event_ : nullptr;
  }
  [[nodiscard]] std::optional<Timestamp> effective_at() const noexcept {
    if (!event_) return std::nullopt;
    return header(*event_).effective_at();
  }
  [[nodiscard]] std::optional<Timestamp> cancellation_at() const noexcept { return cancellation_at_; }
  [[nodiscard]] const std::optional<EventId>& causal_record_id() const noexcept {
    return causal_record_id_;
  }
  [[nodiscard]] const EventId* supersedes_record_id() const noexcept {
    return action_ == LifecycleAction::correct || action_ == LifecycleAction::cancel
               ? &*causal_record_id_
               : nullptr;
  }
  [[nodiscard]] const EventId* reverses_record_id() const noexcept {
    return action_ == LifecycleAction::reverse ? &*causal_record_id_ : nullptr;
  }
  bool operator==(const LifecycleRecord&) const = default;

 private:
  friend class LifecycleLedger;
  LifecycleRecord(LifecycleSequence sequence, const LifecycleRecordDraft& draft)
      : record_id_(draft.record_id()),
        economic_event_id_(draft.economic_event_id()),
        account_(draft.account()),
        action_(draft.action()),
        recorded_at_(draft.recorded_at()),
        acceptance_sequence_(sequence),
        provenance_(draft.provenance()),
        event_(draft.event() ? std::optional<EconomicEvent>{*draft.event()} : std::nullopt),
        causal_record_id_(draft.causal_record_id()), cancellation_at_(draft.cancellation_at()) {}

  EventId record_id_;
  EconomicEventId economic_event_id_;
  AccountId account_;
  LifecycleAction action_;
  Timestamp recorded_at_;
  LifecycleSequence acceptance_sequence_;
  Provenance provenance_;
  std::optional<EconomicEvent> event_;
  std::optional<EventId> causal_record_id_;
  std::optional<Timestamp> cancellation_at_;
};

enum class LifecycleDiagnosticCategory {
  invalid_record,
  duplicate_identity,
  deterministic_ordering,
  sequence_overflow,
  causal_reference_missing,
  causal_self_reference,
  causal_cycle,
  causal_reference_unavailable,
  incompatible_account,
  incompatible_event_relationship,
  conflicting_lifecycle_successor,
};

[[nodiscard]] constexpr std::string_view category_name(
    LifecycleDiagnosticCategory category) noexcept {
  switch (category) {
    case LifecycleDiagnosticCategory::invalid_record:
      return "invalid_record";
    case LifecycleDiagnosticCategory::duplicate_identity:
      return "duplicate_identity";
    case LifecycleDiagnosticCategory::deterministic_ordering:
      return "deterministic_ordering";
    case LifecycleDiagnosticCategory::sequence_overflow:
      return "sequence_overflow";
    case LifecycleDiagnosticCategory::causal_reference_missing:
      return "causal_reference_missing";
    case LifecycleDiagnosticCategory::causal_self_reference:
      return "causal_self_reference";
    case LifecycleDiagnosticCategory::causal_cycle:
      return "causal_cycle";
    case LifecycleDiagnosticCategory::causal_reference_unavailable:
      return "causal_reference_unavailable";
    case LifecycleDiagnosticCategory::incompatible_account:
      return "incompatible_account";
    case LifecycleDiagnosticCategory::incompatible_event_relationship:
      return "incompatible_event_relationship";
    case LifecycleDiagnosticCategory::conflicting_lifecycle_successor:
      return "conflicting_lifecycle_successor";
  }
  return "invalid_record";
}

class LifecycleError {
 public:
  [[nodiscard]] LifecycleDiagnosticCategory category() const noexcept {
    return category_;
  }
  [[nodiscard]] std::string_view category_name() const noexcept {
    return luca::category_name(category_);
  }
  [[nodiscard]] const EventId& record_id() const noexcept { return record_id_; }
  [[nodiscard]] std::optional<Timestamp> cancellation_at() const noexcept { return cancellation_at_; }
  [[nodiscard]] const std::optional<EventId>& causal_record_id() const noexcept {
    return causal_record_id_;
  }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }

 private:
  friend class LifecycleLedger;
  LifecycleError(LifecycleDiagnosticCategory category, EventId record_id,
                 std::optional<EventId> causal_record_id, std::string message)
      : category_(category),
        record_id_(std::move(record_id)),
        causal_record_id_(std::move(causal_record_id)),
        message_(std::move(message)) {}

  LifecycleDiagnosticCategory category_;
  EventId record_id_;
  std::optional<EventId> causal_record_id_;
  std::optional<Timestamp> cancellation_at_;
  std::string message_;
};

using LifecycleRecordReference = std::reference_wrapper<const LifecycleRecord>;

class ResolvedLifecycleChain {
 public:
  [[nodiscard]] const EconomicEventId& economic_event_id() const noexcept {
    return head().economic_event_id();
  }
  [[nodiscard]] std::span<const LifecycleRecordReference> lineage() const noexcept {
    return lineage_;
  }
  [[nodiscard]] const LifecycleRecord& head() const noexcept {
    return lineage_.back().get();
  }
  // Active here means active at the knowledge cutoff. An active chain whose
  // payload is after the economic cutoff is absent from active_events().
  [[nodiscard]] bool active() const noexcept {
    return head().action() != LifecycleAction::cancel;
  }

 private:
  friend class LifecycleLedger;
  explicit ResolvedLifecycleChain(std::vector<LifecycleRecordReference> lineage)
      : lineage_(std::move(lineage)) {}
  std::vector<LifecycleRecordReference> lineage_;
};

class ResolvedLifecycleEvent {
 public:
  [[nodiscard]] const LifecycleRecord& record() const noexcept { return record_.get(); }
  [[nodiscard]] const EconomicEvent& event() const noexcept { return *record().event(); }
  [[nodiscard]] std::span<const LifecycleRecordReference> lineage() const noexcept {
    return lineage_;
  }
  [[nodiscard]] const LifecycleRecord* reversed_record() const noexcept {
    return reversed_record_;
  }

 private:
  friend class LifecycleLedger;
  ResolvedLifecycleEvent(LifecycleRecordReference record,
                         std::vector<LifecycleRecordReference> lineage,
                         const LifecycleRecord* reversed_record)
      : record_(record),
        lineage_(std::move(lineage)),
        reversed_record_(reversed_record) {}

  LifecycleRecordReference record_;
  std::vector<LifecycleRecordReference> lineage_;
  const LifecycleRecord* reversed_record_;
};

class LifecycleResolution {
 public:
  [[nodiscard]] std::span<const ResolvedLifecycleChain> chains() const noexcept {
    return chains_;
  }
  [[nodiscard]] std::span<const ResolvedLifecycleEvent> active_events() const noexcept {
    return active_events_;
  }

 private:
  friend class LifecycleLedger;
  LifecycleResolution(std::vector<ResolvedLifecycleChain> chains,
                      std::vector<ResolvedLifecycleEvent> active_events)
      : chains_(std::move(chains)), active_events_(std::move(active_events)) {}

  std::vector<ResolvedLifecycleChain> chains_;
  std::vector<ResolvedLifecycleEvent> active_events_;
};

// Owned, validated suffix bound to one in-memory prefix identity. An exact ledger
// copy shares that prefix identity; any successful append invalidates old plans.
class PreparedLifecycleAppend {
 public:
  [[nodiscard]] std::span<const LifecycleRecord> records() const noexcept { return records_; }
 private:
  friend class LifecycleLedger;
  PreparedLifecycleAppend(std::vector<LifecycleRecord> records,
                          std::shared_ptr<const unsigned char> prefix)
      : records_(std::move(records)), prefix_(std::move(prefix)) {}
  std::vector<LifecycleRecord> records_;
  std::shared_ptr<const unsigned char> prefix_;
};

// An append-only, in-memory lifecycle ledger. Acceptance and resolution are not
// concurrently mutable. As with Ledger, references and spans can be invalidated
// by later acceptance that reallocates storage.
class LifecycleLedger {
 public:
  using RecordReference = LifecycleRecordReference;

  [[nodiscard]] bool empty() const noexcept { return records_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
  [[nodiscard]] std::span<const LifecycleRecord> records() const noexcept {
    return records_;
  }

  [[nodiscard]] std::expected<RecordReference, LifecycleError> accept(
      const LifecycleRecordDraft& draft) {
    const auto accepted = accept_batch(std::span<const LifecycleRecordDraft>{&draft, 1});
    if (!accepted) return std::unexpected(accepted.error());
    return std::cref(accepted->front());
  }

  // Batch acceptance exists so forward references and cycles receive their
  // stable causal diagnostics. The entire batch is rejected without mutation
  // when any record is invalid.
  [[nodiscard]] std::expected<std::span<const LifecycleRecord>, LifecycleError>
  accept_batch(std::span<const LifecycleRecordDraft> drafts) {
    auto prepared = prepare_batch(drafts);
    if (!prepared) return std::unexpected(prepared.error());
    return publish(std::move(*prepared));
  }

  // No mutation or I/O: the host can durably commit these owned canonical records
  // before publishing. Validation touches the suffix and indexed causal targets.
  [[nodiscard]] std::expected<PreparedLifecycleAppend, LifecycleError>
  prepare_batch(std::span<const LifecycleRecordDraft> drafts) const {
    return prepare_impl(drafts, false);
  }

  // Restore an already-accepted canonical record in exact sequence order.
  // This is replay, not authorization for a new economic mutation. Hosts must
  // restore complete durable admission groups before exposing a projection.
  [[nodiscard]] std::expected<RecordReference, LifecycleError> restore_record(
      const LifecycleRecordDraft& draft, std::uint64_t accepted_sequence) {
    if (accepted_sequence != next_sequence_)
      return std::unexpected(error(LifecycleDiagnosticCategory::deterministic_ordering,
          draft, "restored sequence is not the next accepted sequence"));
    auto prepared=prepare_impl(std::span{&draft, 1}, true);
    if(!prepared)return std::unexpected(prepared.error());
    auto restored=publish(std::move(*prepared));
    if(!restored)return std::unexpected(restored.error());
    return std::cref(restored->front());
  }

 private:
  [[nodiscard]] std::expected<PreparedLifecycleAppend, LifecycleError>
  prepare_impl(std::span<const LifecycleRecordDraft> drafts, bool restoring) const {
    if (drafts.empty()) return PreparedLifecycleAppend{{}, prefix_};

    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto available = maximum - next_sequence_ + 1;
    if (sequence_exhausted_ || drafts.size() > available) {
      return std::unexpected(error(
          LifecycleDiagnosticCategory::sequence_overflow, drafts.front(),
          "ledger-local lifecycle acceptance sequence is exhausted"));
    }

    std::vector<LifecycleRecord> pending;
    pending.reserve(drafts.size());
    for (std::size_t index = 0; index < drafts.size(); ++index) {
      pending.push_back(LifecycleRecord{
          LifecycleSequence{next_sequence_ + static_cast<std::uint64_t>(index)},
          drafts[index]});
    }

    std::unordered_map<std::string, const LifecycleRecord*> pending_records;
    pending_records.reserve(pending.size());

    std::unordered_map<std::string, const LifecycleRecord*> pending_origins;
    pending_origins.reserve(pending.size());
    for (const auto& record : pending) {
      if (record.record_id().value().empty() ||
          record.economic_event_id().value().empty() || record.account().value().empty()) {
        return std::unexpected(error(
            LifecycleDiagnosticCategory::invalid_record, record,
            "record, economic-event, and account identities must be non-empty"));
      }
      if (record_index_.contains(record.record_id().value()) ||
          pending_records.contains(record.record_id().value())) {
        return std::unexpected(error(LifecycleDiagnosticCategory::duplicate_identity,
                                     record, "record identity is already accepted"));
      }
      pending_records.emplace(record.record_id().value(), &record);

      if (record.action() == LifecycleAction::originate ||
          record.action() == LifecycleAction::reverse) {
        if (economic_origin_index_.contains(record.economic_event_id().value()) ||
            pending_origins.contains(record.economic_event_id().value())) {
          return std::unexpected(error(
              LifecycleDiagnosticCategory::duplicate_identity, record,
              "economic-event identity already has an origin"));
        }
        pending_origins.emplace(record.economic_event_id().value(), &record);
      }
    }

    auto prior_recorded_at =
        records_.empty() ? std::optional<Timestamp>{}
                         : std::optional<Timestamp>{records_.back().recorded_at()};
    for (const auto& record : pending) {
      if (prior_recorded_at && record.recorded_at() < *prior_recorded_at) {
        return std::unexpected(error(
            LifecycleDiagnosticCategory::deterministic_ordering, record,
            "recorded time must not decrease with acceptance sequence"));
      }
      prior_recorded_at = record.recorded_at();
    }

    for (const auto& record : pending) {
      if (!record.causal_record_id()) continue;
      if (*record.causal_record_id() == record.record_id()) {
        return std::unexpected(error(LifecycleDiagnosticCategory::causal_self_reference,
                                     record, "lifecycle record targets itself"));
      }
      if (!pending_records.contains(record.causal_record_id()->value()) &&
          !record_index_.contains(record.causal_record_id()->value())) {
        return std::unexpected(error(
            LifecycleDiagnosticCategory::causal_reference_missing, record,
            "causal target does not identify an accepted or batched record"));
      }
    }

    if (const auto cycle = find_cycle(pending_records)) {
      return std::unexpected(LifecycleError{
          LifecycleDiagnosticCategory::causal_cycle, (*cycle)->record_id(),
          (*cycle)->causal_record_id(), "causal references form a cycle"});
    }

    std::unordered_map<std::string, std::size_t> successors;
    for (const auto& record : pending) {
      if (!record.causal_record_id()) continue;
      const auto candidate = pending_records.find(record.causal_record_id()->value());
      const auto& target = candidate != pending_records.end() ? *candidate->second
          : *find(*record.causal_record_id());
      const auto target_key =
          std::pair{target.recorded_at(), target.acceptance_sequence()};
      const auto record_key =
          std::pair{record.recorded_at(), record.acceptance_sequence()};
      if (target_key >= record_key) {
        return std::unexpected(error(
            LifecycleDiagnosticCategory::causal_reference_unavailable, record,
            "causal target is not earlier in recorded and acceptance order"));
      }
      if (record.account() != target.account()) {
        return std::unexpected(error(LifecycleDiagnosticCategory::incompatible_account,
                                     record,
                                     "lifecycle relationship crosses accounts"));
      }
      if (successor_index_.contains(target.record_id().value()) ||
          successors.contains(target.record_id().value())) {
        return std::unexpected(error(
            LifecycleDiagnosticCategory::conflicting_lifecycle_successor, record,
            "causal target already has a lifecycle successor"));
      }
      if (const auto relationship_error = validate_relationship(record, target))
        return std::unexpected(*relationship_error);
      if (target.action() == LifecycleAction::cancel && record.action() == LifecycleAction::correct) {
        if(!restoring && !pending_records.contains(target.record_id().value()))
          return std::unexpected(error(LifecycleDiagnosticCategory::incompatible_event_relationship,
              record, "replacement must atomically include its dated void"));
        const auto* original = target.causal_record_id() ? find(*target.causal_record_id()) : nullptr;
        if (!original && target.causal_record_id()) {
          const auto found = pending_records.find(target.causal_record_id()->value());
          if (found != pending_records.end()) original = found->second;
        }
        if (!original || !original->event() || !record.event() ||
            original->event()->index() != record.event()->index())
          return std::unexpected(error(LifecycleDiagnosticCategory::incompatible_event_relationship,
              record, "replacement must retain the original event type"));
        if (const auto* a = std::get_if<EquityTrade>(record.event())) {
          const auto& b = std::get<EquityTrade>(*original->event());
          if (a->instrument() != b.instrument() || a->quote_currency() != b.quote_currency())
            return std::unexpected(error(LifecycleDiagnosticCategory::incompatible_event_relationship,
                record, "replacement changed the trade natural key"));
        } else if (std::get<CashMovement>(*record.event()).amount().currency() !=
                   std::get<CashMovement>(*original->event()).amount().currency())
          return std::unexpected(error(LifecycleDiagnosticCategory::incompatible_event_relationship,
              record, "replacement changed currency"));
      }
      successors.emplace(target.record_id().value(), records_.size());
    }

    return PreparedLifecycleAppend{std::move(pending), prefix_};
  }

 public:
  [[nodiscard]] std::expected<std::span<const LifecycleRecord>, LifecycleError>
  publish(PreparedLifecycleAppend prepared) {
    if (prepared.records_.empty()) return std::span<const LifecycleRecord>{};
    if (prepared.prefix_ != prefix_)
      return std::unexpected(error(LifecycleDiagnosticCategory::deterministic_ordering,
          prepared.records_.front(), "prepared append belongs to a different ledger revision"));
    auto& pending = prepared.records_;
    const auto next_prefix = std::make_shared<const unsigned char>(0);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto first = records_.size();
    const auto needed = records_.size() + pending.size();
    if (needed > records_.capacity())
      records_.reserve(std::max(needed, records_.capacity() <= records_.max_size() / 2
          ? records_.capacity() * 2 : needed));
    const auto grow = [](auto& index, std::size_t additional) {
      const auto target = index.size() + additional;
      if (target > index.bucket_count())
        index.reserve(std::max(target, index.bucket_count() <= index.max_size() / 2
            ? index.bucket_count() * 2 : target));
    };
    const auto origins = std::count_if(pending.begin(), pending.end(), [](const auto& record) {
      return record.action() == LifecycleAction::originate || record.action() == LifecycleAction::reverse;
    });
    const auto successors = std::count_if(pending.begin(), pending.end(), [](const auto& record) {
      return record.causal_record_id().has_value();
    });
    grow(record_index_, pending.size());
    grow(economic_origin_index_, static_cast<std::size_t>(origins));
    grow(successor_index_, static_cast<std::size_t>(successors));
    for (auto& record : pending) {
      const auto index = records_.size();
      records_.push_back(std::move(record));
      const auto& accepted = records_.back();
      record_index_.emplace(accepted.record_id().value(), index);
      if (accepted.action() == LifecycleAction::originate ||
          accepted.action() == LifecycleAction::reverse)
        economic_origin_index_.emplace(accepted.economic_event_id().value(), index);
      if (accepted.causal_record_id())
        successor_index_.emplace(accepted.causal_record_id()->value(), index);
    }

    const auto final_sequence = records_.back().acceptance_sequence().value();
    if (final_sequence == maximum)
      sequence_exhausted_ = true;
    else
      next_sequence_ = final_sequence + 1;
    prefix_ = next_prefix;
    return std::span<const LifecycleRecord>{records_}.subspan(first, pending.size());
  }

  [[nodiscard]] const LifecycleRecord* find(const EventId& record_id) const noexcept {
    const auto found = record_index_.find(record_id.value());
    return found == record_index_.end() ? nullptr : &records_[found->second];
  }

  // Resolution applies the inclusive knowledge cutoff before the inclusive
  // economic cutoff. Active payloads are ordered by effective time, then the
  // acceptance sequence of the selected payload record.
  [[nodiscard]] LifecycleResolution resolve(Timestamp recorded_through,
                                            Timestamp economic_as_of) const {
    struct WorkingChain {
      std::vector<LifecycleRecordReference> lineage;
    };

    std::vector<WorkingChain> working;
    std::unordered_map<std::string, std::size_t> chain_by_economic_id;
    std::set<std::string> gated;
    for (const auto& record : records_) {
      if (record.recorded_at() > recorded_through) continue;
      if ((record.cancellation_at() && *record.cancellation_at() > economic_as_of) ||
          (record.causal_record_id() && gated.contains(record.causal_record_id()->value()))) {
        gated.insert(record.record_id().value());
        continue;
      }
      const auto& id = record.economic_event_id().value();
      auto found = chain_by_economic_id.find(id);
      if (found == chain_by_economic_id.end()) {
        const auto index = working.size();
        working.push_back(WorkingChain{{std::cref(record)}});
        chain_by_economic_id.emplace(id, index);
      } else {
        working[found->second].lineage.emplace_back(std::cref(record));
      }
    }

    std::vector<ResolvedLifecycleChain> chains;
    std::vector<ResolvedLifecycleEvent> active_events;
    chains.reserve(working.size());
    active_events.reserve(working.size());
    for (auto& chain : working) {
      const auto& head = chain.lineage.back().get();
      const auto effective_at = head.effective_at();
      if (head.action() != LifecycleAction::cancel && effective_at &&
          *effective_at <= economic_as_of) {
        const LifecycleRecord* reversed_record = nullptr;
        if (head.reverses_record_id()) reversed_record = find(*head.reverses_record_id());
        active_events.push_back(
            ResolvedLifecycleEvent{std::cref(head), chain.lineage, reversed_record});
      }
      chains.push_back(ResolvedLifecycleChain{std::move(chain.lineage)});
    }
    std::sort(active_events.begin(), active_events.end(),
              [](const ResolvedLifecycleEvent& lhs,
                 const ResolvedLifecycleEvent& rhs) {
                const auto left_time = lhs.record().effective_at();
                const auto right_time = rhs.record().effective_at();
                if (left_time != right_time) return left_time < right_time;
                return lhs.record().acceptance_sequence() <
                       rhs.record().acceptance_sequence();
              });
    return LifecycleResolution{std::move(chains), std::move(active_events)};
  }

 private:
  [[nodiscard]] static LifecycleError error(
      LifecycleDiagnosticCategory category, const LifecycleRecordDraft& draft,
      std::string message) {
    return LifecycleError{category, draft.record_id(), draft.causal_record_id(),
                          std::move(message)};
  }

  [[nodiscard]] static LifecycleError error(
      LifecycleDiagnosticCategory category, const LifecycleRecord& record,
      std::string message) {
    return LifecycleError{category, record.record_id(), record.causal_record_id(),
                          std::move(message)};
  }

  [[nodiscard]] static std::optional<const LifecycleRecord*> find_cycle(
      const std::unordered_map<std::string, const LifecycleRecord*>& records) {
    std::unordered_map<std::string, unsigned char> state;
    state.reserve(records.size());
    std::vector<const LifecycleRecord*> ordered;
    ordered.reserve(records.size());
    for (const auto& [id, record] : records) {
      static_cast<void>(id);
      ordered.push_back(record);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto* lhs, const auto* rhs) {
      return lhs->acceptance_sequence() < rhs->acceptance_sequence();
    });
    for (const auto* record : ordered) {
      std::vector<const LifecycleRecord*> path;
      auto* current = record;
      while (current && state[current->record_id().value()] == 0) {
        state[current->record_id().value()] = 1;
        path.push_back(current);
        if (!current->causal_record_id()) { current = nullptr; break; }
        const auto target = records.find(current->causal_record_id()->value());
        current = target == records.end() ? nullptr : target->second;
      }
      if (current && state[current->record_id().value()] == 1) return current;
      for (const auto* item : path) state[item->record_id().value()] = 2;
    }
    return std::nullopt;
  }

  [[nodiscard]] static bool exact_opposite(std::int64_t candidate,
                                           std::int64_t target) noexcept {
    if (target == std::numeric_limits<std::int64_t>::min()) return false;
    return candidate == -target;
  }

  [[nodiscard]] static std::optional<LifecycleError> validate_relationship(
      const LifecycleRecord& record, const LifecycleRecord& target) {
    const bool replacement = record.action() == LifecycleAction::correct &&
        target.action() == LifecycleAction::cancel && target.cancellation_at() &&
        record.recorded_at() == target.recorded_at() &&
        record.acceptance_sequence().value() == target.acceptance_sequence().value() + 1 &&
        record.effective_at() && *record.effective_at() <= *target.cancellation_at();
    if ((target.action() == LifecycleAction::cancel && !replacement) ||
        target.action() == LifecycleAction::reverse) {
      return error(LifecycleDiagnosticCategory::incompatible_event_relationship,
                   record, "cancellations and reversals are terminal");
    }

    if (record.action() == LifecycleAction::correct ||
        record.action() == LifecycleAction::cancel) {
      if (record.economic_event_id() != target.economic_event_id()) {
        return error(LifecycleDiagnosticCategory::incompatible_event_relationship,
                     record,
                     "correction or cancellation changed economic-event identity");
      }
    } else if (record.economic_event_id() == target.economic_event_id()) {
      return error(LifecycleDiagnosticCategory::incompatible_event_relationship,
                   record, "reversal must start a new economic-event identity");
    }

    if (record.action() == LifecycleAction::cancel) {
      if (record.cancellation_at() && target.effective_at() &&
          *record.cancellation_at() < *target.effective_at())
        return error(LifecycleDiagnosticCategory::incompatible_event_relationship,
                     record, "void cannot precede the target economic date");
      return std::nullopt;
    }
    // The dated void and replacement share one economic identity. Payload
    // validation against the original version occurs in prepare_batch below.
    if (replacement) return std::nullopt;
    if (!record.event() || !target.event() ||
        record.event()->index() != target.event()->index()) {
      return error(LifecycleDiagnosticCategory::incompatible_event_relationship,
                   record, "lifecycle event types differ");
    }

    if (const auto* cash = std::get_if<CashMovement>(record.event())) {
      const auto& target_cash = std::get<CashMovement>(*target.event());
      if (cash->amount().currency() != target_cash.amount().currency()) {
        return error(LifecycleDiagnosticCategory::incompatible_event_relationship,
                     record, "cash relationship changes currency");
      }
      if (record.action() == LifecycleAction::reverse &&
          !exact_opposite(cash->amount().scaled_value(),
                          target_cash.amount().scaled_value())) {
        return error(LifecycleDiagnosticCategory::incompatible_event_relationship,
                     record, "cash reversal is not the exact offset");
      }
    } else {
      const auto& trade = std::get<EquityTrade>(*record.event());
      const auto& target_trade = std::get<EquityTrade>(*target.event());
      if (trade.instrument() != target_trade.instrument() ||
          trade.quote_currency() != target_trade.quote_currency()) {
        return error(LifecycleDiagnosticCategory::incompatible_event_relationship,
                     record, "equity relationship changes its natural key");
      }
      if (record.action() == LifecycleAction::reverse &&
          (trade.charges().has_value() || target_trade.charges().has_value() ||
          trade.price() != target_trade.price() ||
           !exact_opposite(trade.quantity().scaled_value(),
                           target_trade.quantity().scaled_value()))) {
        return error(LifecycleDiagnosticCategory::incompatible_event_relationship,
                     record, "equity reversal does not exactly offset quantity and price");
      }
    }

    if (record.action() == LifecycleAction::reverse &&
        record.effective_at() < target.effective_at()) {
      return error(LifecycleDiagnosticCategory::incompatible_event_relationship,
                   record, "reversal economically predates its target");
    }
    return std::nullopt;
  }

  std::vector<LifecycleRecord> records_;
  std::unordered_map<std::string, std::size_t> record_index_;
  std::unordered_map<std::string, std::size_t> economic_origin_index_;
  std::unordered_map<std::string, std::size_t> successor_index_;
  std::uint64_t next_sequence_ = LifecycleSequence::first_value;
  bool sequence_exhausted_ = false;
  std::shared_ptr<const unsigned char> prefix_ = std::make_shared<const unsigned char>(0);
};

}  // namespace luca
