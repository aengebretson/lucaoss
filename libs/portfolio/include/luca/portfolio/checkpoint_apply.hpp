#pragma once

#include "luca/portfolio/checkpoint_resume.hpp"
#include "luca/portfolio/lifecycle_projection.hpp"

#include <exception>
#include <expected>
#include <map>
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

namespace checkpoint_apply_detail {

[[nodiscard]] inline LifecycleRecordDraft draft_from(const LifecycleRecord &record) {
  switch (record.action()) {
  case LifecycleAction::originate:
    return LifecycleRecordDraft::originate(record.economic_event_id(), record.recorded_at(),
                                           *record.event());
  case LifecycleAction::correct:
    return LifecycleRecordDraft::correct(record.economic_event_id(), *record.supersedes_record_id(),
                                         record.recorded_at(), *record.event());
  case LifecycleAction::cancel:
    return LifecycleRecordDraft::cancel(record.record_id(), record.economic_event_id(),
                                        *record.supersedes_record_id(), record.account(),
                                        record.recorded_at(), record.provenance(), record.cancellation_at());
  case LifecycleAction::reverse:
    return LifecycleRecordDraft::reverse(record.economic_event_id(), *record.reverses_record_id(),
                                         record.recorded_at(), *record.event());
  }
  std::terminate();
}

[[nodiscard]] inline LifecycleProjectionResult project_event(const EconomicEvent &event,
                                                             LifecycleProjectionContext context) {
  Ledger ledger;
  if (!ledger.append(event))
    std::terminate();

  return LifecycleProjectionResult{
      .positions = project_positions(ledger.entries(), context.economic_as_of),
      .settled_cash =
          project_cash(ledger.entries(), CashProjectionContext{context.economic_as_of,
                                                               context.settlement_as_of_date}),
      .open_settlement_obligations = project_settlement_obligations(
          ledger.entries(),
          SettlementProjectionContext{context.economic_as_of, context.settlement_as_of_date}),
  };
}

[[nodiscard]] inline std::expected<PortfolioState, CheckpointApplyError>
merge_state(const PortfolioState &checkpoint_state, const LifecycleProjectionResult &suffix) {
  if (!suffix.positions)
    return std::unexpected(CheckpointApplyError{suffix.positions.error()});
  if (!suffix.settled_cash)
    return std::unexpected(CheckpointApplyError{suffix.settled_cash.error()});
  if (!suffix.open_settlement_obligations) {
    return std::unexpected(CheckpointApplyError{suffix.open_settlement_obligations.error()});
  }

  std::map<PositionKey, Quantity> positions;
  for (const auto &position : checkpoint_state.positions())
    positions.emplace(position.key(), position.quantity());
  for (const auto &position : *suffix.positions) {
    const auto found = positions.find(position.key());
    if (found == positions.end()) {
      positions.emplace(position.key(), position.quantity());
      continue;
    }
    const auto quantity = found->second.add(position.quantity());
    if (!quantity) {
      return std::unexpected(CheckpointApplyError{PositionProjectionError::quantity_overflow});
    }
    if (quantity->scaled_value() == 0)
      positions.erase(found);
    else
      found->second = *quantity;
  }

  std::map<CashKey, Money> settled_cash;
  for (const auto &balance : checkpoint_state.settled_cash())
    settled_cash.emplace(balance.key(), balance.amount());
  for (const auto &balance : *suffix.settled_cash) {
    const auto found = settled_cash.find(balance.key());
    if (found == settled_cash.end()) {
      settled_cash.emplace(balance.key(), balance.amount());
      continue;
    }
    const auto amount = found->second.add(balance.amount());
    if (!amount)
      return std::unexpected(CheckpointApplyError{CashProjectionError::amount_overflow});
    if (amount->scaled_value() == 0)
      settled_cash.erase(found);
    else
      found->second = *amount;
  }

  std::map<SettlementObligationKey, Money> obligations;
  for (const auto &obligation : checkpoint_state.open_settlement_obligations())
    obligations.emplace(obligation.key(), obligation.amount());
  for (const auto &obligation : *suffix.open_settlement_obligations) {
    const auto found = obligations.find(obligation.key());
    if (found == obligations.end()) {
      obligations.emplace(obligation.key(), obligation.amount());
      continue;
    }
    const auto amount = found->second.add(obligation.amount());
    if (!amount) {
      return std::unexpected(CheckpointApplyError{SettlementProjectionError::amount_overflow});
    }
    if (amount->scaled_value() == 0)
      obligations.erase(found);
    else
      found->second = *amount;
  }

  std::vector<Position> merged_positions;
  merged_positions.reserve(positions.size());
  for (const auto &[key, quantity] : positions)
    merged_positions.emplace_back(key, quantity);

  std::vector<CashBalance> merged_cash;
  merged_cash.reserve(settled_cash.size());
  for (const auto &[key, amount] : settled_cash)
    merged_cash.emplace_back(key.account(), amount);

  std::vector<SettlementObligation> merged_obligations;
  merged_obligations.reserve(obligations.size());
  for (const auto &[key, amount] : obligations) {
    merged_obligations.emplace_back(key.account(), key.settlement_date(), key.direction(), amount);
  }

  return PortfolioState{std::move(merged_positions), std::move(merged_cash),
                        std::move(merged_obligations)};
}

} // namespace checkpoint_apply_detail

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

  std::vector<LifecycleRecordDraft> drafts;
  drafts.reserve(proposed_suffix.size());
  for (const auto &record : proposed_suffix)
    drafts.push_back(checkpoint_apply_detail::draft_from(record));

  LifecycleLedger suffix_ledger;
  const auto accepted = suffix_ledger.accept_batch(drafts);
  if (!accepted)
    return std::unexpected(CheckpointApplyError{accepted.error()});

  const auto &context = manifest.evaluation_context();
  const auto resolution =
      suffix_ledger.resolve(context.recorded_through(), context.economic_as_of());
  const auto projection_context =
      LifecycleProjectionContext{context.economic_as_of(), context.settlement_as_of_date().value()};

  auto state = checkpoint_state;
  for (const auto &resolved : resolution.active_events()) {
    const auto projected =
        checkpoint_apply_detail::project_event(resolved.event(), projection_context);
    auto merged = checkpoint_apply_detail::merge_state(state, projected);
    if (!merged)
      return std::unexpected(std::move(merged.error()));
    state = std::move(*merged);
  }
  return state;
}

} // namespace luca
