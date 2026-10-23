#pragma once

#include "luca/accounting/journal.hpp"
#include "luca/lifecycle.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace luca {

enum class TradeDateProjectionDiagnosticCategory {
  invalid_context,
  unsupported_event,
  unsupported_currency,
  invalid_reversal_treatment,
  arithmetic_overflow,
  journal_invariant,
};

[[nodiscard]] constexpr std::string_view
category_name(TradeDateProjectionDiagnosticCategory category) noexcept {
  switch (category) {
  case TradeDateProjectionDiagnosticCategory::invalid_context:
    return "invalid_context";
  case TradeDateProjectionDiagnosticCategory::unsupported_event:
    return "unsupported_event";
  case TradeDateProjectionDiagnosticCategory::unsupported_currency:
    return "unsupported_currency";
  case TradeDateProjectionDiagnosticCategory::invalid_reversal_treatment:
    return "invalid_reversal_treatment";
  case TradeDateProjectionDiagnosticCategory::arithmetic_overflow:
    return "arithmetic_overflow";
  case TradeDateProjectionDiagnosticCategory::journal_invariant:
    return "journal_invariant";
  }
  return "invalid_context";
}

class TradeDateProjectionError {
public:
  TradeDateProjectionError(TradeDateProjectionDiagnosticCategory category,
                           std::optional<EventId> record_id, std::string message)
      : category_(category), record_id_(std::move(record_id)), message_(std::move(message)) {}

  [[nodiscard]] TradeDateProjectionDiagnosticCategory category() const noexcept {
    return category_;
  }
  [[nodiscard]] std::string_view category_name() const noexcept {
    return luca::category_name(category_);
  }
  [[nodiscard]] const std::optional<EventId> &record_id() const noexcept { return record_id_; }
  [[nodiscard]] const std::string &message() const noexcept { return message_; }

private:
  TradeDateProjectionDiagnosticCategory category_;
  std::optional<EventId> record_id_;
  std::string message_;
};

struct TradeDateProjectionContext {
  Timestamp recorded_through;
  Timestamp economic_as_of;
  std::chrono::year_month_day settlement_as_of_date;
  bool operator==(const TradeDateProjectionContext &) const = default;
};

class TradeDateProjectionResult {
public:
  static constexpr std::string_view engine_version_value = "fixture-accounting-engine-1";
  static constexpr std::string_view projection_version_value = "fixture-journal-projection-1";
  static constexpr std::string_view lifecycle_contract_version_value = "luca.event-lifecycle.v1";

  [[nodiscard]] constexpr std::string_view engine_version() const noexcept {
    return engine_version_value;
  }
  [[nodiscard]] constexpr std::string_view projection_version() const noexcept {
    return projection_version_value;
  }
  [[nodiscard]] constexpr std::string_view lifecycle_contract_version() const noexcept {
    return lifecycle_contract_version_value;
  }
  [[nodiscard]] const AccountingPolicyIdentity &policy() const noexcept { return policy_; }
  [[nodiscard]] const TradeDateProjectionContext &evaluation_context() const noexcept {
    return context_;
  }
  [[nodiscard]] std::span<const EventId> active_record_ids() const noexcept {
    return active_record_ids_;
  }
  [[nodiscard]] std::span<const EventId> lifecycle_record_ids() const noexcept {
    return lifecycle_record_ids_;
  }
  [[nodiscard]] std::span<const EconomicEventId> economic_event_ids() const noexcept {
    return economic_event_ids_;
  }
  [[nodiscard]] std::span<const SourceRecordId> source_record_ids() const noexcept {
    return source_record_ids_;
  }
  [[nodiscard]] std::span<const JournalEntry> entries() const noexcept { return entries_; }
  bool operator==(const TradeDateProjectionResult &) const = default;

private:
  friend std::expected<TradeDateProjectionResult, TradeDateProjectionError>
  project_trade_date_journals(const LifecycleResolution &, TradeDateProjectionContext);

  TradeDateProjectionResult(AccountingPolicyIdentity policy, TradeDateProjectionContext context,
                            std::vector<EventId> active_record_ids,
                            std::vector<EventId> lifecycle_record_ids,
                            std::vector<EconomicEventId> economic_event_ids,
                            std::vector<SourceRecordId> source_record_ids,
                            std::vector<JournalEntry> entries)
      : policy_(std::move(policy)), context_(context),
        active_record_ids_(std::move(active_record_ids)),
        lifecycle_record_ids_(std::move(lifecycle_record_ids)),
        economic_event_ids_(std::move(economic_event_ids)),
        source_record_ids_(std::move(source_record_ids)), entries_(std::move(entries)) {}

  AccountingPolicyIdentity policy_;
  TradeDateProjectionContext context_;
  std::vector<EventId> active_record_ids_;
  std::vector<EventId> lifecycle_record_ids_;
  std::vector<EconomicEventId> economic_event_ids_;
  std::vector<SourceRecordId> source_record_ids_;
  std::vector<JournalEntry> entries_;
};

namespace trade_date_projection_detail {

inline constexpr std::string_view policy_id = "fixture.trade-date.v1";
inline constexpr std::string_view policy_version = "1";
inline constexpr std::string_view recognition_rule = "trade_date_then_settlement";

template <class IdentifierType>
void append_once(std::vector<IdentifierType> &identifiers, const IdentifierType &candidate) {
  if (std::ranges::find(identifiers, candidate) == identifiers.end())
    identifiers.push_back(candidate);
}

[[nodiscard]] inline TradeDateProjectionError
error(TradeDateProjectionDiagnosticCategory category, std::string message,
      std::optional<EventId> record_id = std::nullopt) {
  return TradeDateProjectionError{category, std::move(record_id), std::move(message)};
}

[[nodiscard]] inline TradeDateProjectionError journal_error(const JournalError &cause,
                                                            const EventId &record_id) {
  const auto category = cause.category() == JournalDiagnosticCategory::arithmetic_overflow
                            ? TradeDateProjectionDiagnosticCategory::arithmetic_overflow
                            : TradeDateProjectionDiagnosticCategory::journal_invariant;
  return error(category,
               "journal construction failed [" + std::string{cause.category_name()} +
                   "]: " + cause.message(),
               record_id);
}

[[nodiscard]] inline std::expected<JournalDate, TradeDateProjectionError>
journal_date(Timestamp timestamp, const EventId &record_id) {
  const auto result = JournalDate::create(
      std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(timestamp)});
  if (!result)
    return std::unexpected(error(TradeDateProjectionDiagnosticCategory::invalid_context,
                                 "event effective time does not produce a valid UTC journal date",
                                 record_id));
  return *result;
}

[[nodiscard]] inline std::expected<JournalLineage, TradeDateProjectionError>
lineage(const ResolvedLifecycleEvent &resolved) {
  std::vector<EventId> record_ids;
  std::vector<SourceRecordId> source_record_ids;
  record_ids.reserve(resolved.lineage().size());
  for (const auto &reference : resolved.lineage()) {
    const auto &record = reference.get();
    record_ids.push_back(record.record_id());
    for (const auto &source_record_id : record.provenance().source_records())
      source_record_ids.push_back(source_record_id);
  }

  std::optional<EventId> reverses_record_id;
  if (resolved.reversed_record())
    reverses_record_id = resolved.reversed_record()->record_id();
  auto result =
      JournalLineage::create(std::move(record_ids), {resolved.record().economic_event_id()},
                             std::move(source_record_ids), std::move(reverses_record_id));
  if (!result)
    return std::unexpected(journal_error(result.error(), resolved.record().record_id()));
  return *result;
}

[[nodiscard]] inline std::expected<JournalSettlementContext, TradeDateProjectionError>
immediate_context(const EventId &record_id) {
  auto result = JournalSettlementContext::create(std::nullopt, std::nullopt, "immediate");
  if (!result)
    return std::unexpected(journal_error(result.error(), record_id));
  return *result;
}

[[nodiscard]] inline std::expected<JournalSettlementContext, TradeDateProjectionError>
equity_context(JournalDate trade_date, JournalDate settlement_date, const EventId &record_id) {
  if (settlement_date < trade_date) {
    return std::unexpected(error(TradeDateProjectionDiagnosticCategory::invalid_context,
                                 "equity settlement date must not precede its trade date",
                                 record_id));
  }
  auto result =
      JournalSettlementContext::create(trade_date, settlement_date, std::string{recognition_rule});
  if (!result)
    return std::unexpected(journal_error(result.error(), record_id));
  return *result;
}

[[nodiscard]] inline std::expected<JournalEntry, TradeDateProjectionError>
entry(const LifecycleRecord &record, const AccountingPolicyIdentity &policy,
      JournalEventType event_type, JournalDate recognized_on, RecognitionPhase phase,
      std::uint32_t phase_ordinal, JournalSettlementContext settlement_context,
      const JournalLineage &entry_lineage, Money amount, std::string_view debit_account,
      std::string_view credit_account, std::string_view phase_name) {
  const auto fixture_entry_stem = [&record] {
    const auto &record_id = record.record_id().value();
    if (record_id == "opening-cash-record")
      return std::string{"opening-cash"};
    if (record_id == "trade-record-v1")
      return std::string{"trade-v1"};
    if (record_id == "trade-record-v2")
      return std::string{"trade-v2"};
    if (record_id == "reversal-record-v1")
      return std::string{"reversal"};
    return record_id;
  }();
  const auto entry_id_value = "td." + fixture_entry_stem + "." + std::string{phase_name};
  const JournalEntryId entry_id{entry_id_value};
  auto debit = JournalLine::create(JournalLineId{entry_id_value + ".debit"}, entry_id,
                                   AccountId{std::string{debit_account}}, JournalSide::debit,
                                   amount, entry_lineage);
  if (!debit)
    return std::unexpected(journal_error(debit.error(), record.record_id()));
  auto credit = JournalLine::create(JournalLineId{entry_id_value + ".credit"}, entry_id,
                                    AccountId{std::string{credit_account}}, JournalSide::credit,
                                    amount, entry_lineage);
  if (!credit)
    return std::unexpected(journal_error(credit.error(), record.record_id()));

  auto result = JournalEntry::create(
      entry_id, policy, event_type, record.record_id(), header(*record.event()).effective_at(),
      record.recorded_at(), recognized_on, phase, phase_ordinal, std::move(settlement_context),
      entry_lineage, std::vector<JournalLine>{std::move(*debit), std::move(*credit)});
  if (!result)
    return std::unexpected(journal_error(result.error(), record.record_id()));
  return *result;
}

[[nodiscard]] inline std::expected<Money, TradeDateProjectionError>
positive_trade_value(const EquityTrade &trade, const EventId &record_id) {
  if (trade.charges().has_value() || trade.price().scaled_value() <= 0)
    return std::unexpected(error(TradeDateProjectionDiagnosticCategory::unsupported_event,
                                 "fixture trade-date policy requires a positive equity price",
                                 record_id));
  const auto amount =
      value(trade.quantity(), trade.price(), trade.quote_currency(), RoundingMode::half_even);
  if (!amount)
    return std::unexpected(error(TradeDateProjectionDiagnosticCategory::arithmetic_overflow,
                                 "equity valuation exceeds exact Money range", record_id));
  if (amount->scaled_value() <= 0)
    return std::unexpected(error(TradeDateProjectionDiagnosticCategory::unsupported_event,
                                 "equity valuation must round to a positive Money amount",
                                 record_id));
  return *amount;
}

[[nodiscard]] inline std::expected<Money, TradeDateProjectionError>
reversal_trade_value(const ResolvedLifecycleEvent &resolved, const EquityTrade &reversal) {
  const auto &record = resolved.record();
  const auto *target_record = resolved.reversed_record();
  if (record.action() != LifecycleAction::reverse || !target_record || !target_record->event()) {
    return std::unexpected(error(
        TradeDateProjectionDiagnosticCategory::invalid_reversal_treatment,
        "negative equity quantity must be an exact lifecycle reversal with an available target",
        record.record_id()));
  }
  const auto *target = std::get_if<EquityTrade>(target_record->event());
  const auto exact_quantity =
      target && target->quantity().scaled_value() != std::numeric_limits<std::int64_t>::min() &&
      reversal.quantity().scaled_value() == -target->quantity().scaled_value();
  if (!target || target->quantity().scaled_value() <= 0 || !exact_quantity ||
      reversal.price() != target->price() || reversal.instrument() != target->instrument() ||
      reversal.quote_currency() != target->quote_currency() ||
      reversal.header().account() != target->header().account()) {
    return std::unexpected(
        error(TradeDateProjectionDiagnosticCategory::invalid_reversal_treatment,
              "equity reversal must exactly offset a positive purchase in the same natural key",
              record.record_id()));
  }
  return positive_trade_value(*target, record.record_id());
}

struct PendingEntry {
  JournalEntry entry;
  LifecycleSequence acceptance_sequence;
};

[[nodiscard]] inline std::optional<TradeDateProjectionError>
append_entry(std::vector<PendingEntry> &pending, JournalEntry entry,
             LifecycleSequence acceptance_sequence) {
  if (std::ranges::any_of(pending, [&entry](const PendingEntry &candidate) {
        return candidate.entry.journal_entry_id() == entry.journal_entry_id();
      })) {
    return error(TradeDateProjectionDiagnosticCategory::journal_invariant,
                 "projected journal entry identity is not unique under fixture.trade-date.v1",
                 entry.active_record_id());
  }
  pending.push_back(PendingEntry{std::move(entry), acceptance_sequence});
  return std::nullopt;
}

} // namespace trade_date_projection_detail

// Projects the ordered active set selected by LifecycleLedger::resolve under
// the one concrete fixture.trade-date.v1 policy. The supplied cutoffs are
// recorded in the result and checked against every selected lifecycle record;
// the function performs no lifecycle selection and no I/O.
[[nodiscard]] inline std::expected<TradeDateProjectionResult, TradeDateProjectionError>
project_trade_date_journals(const LifecycleResolution &resolution,
                            TradeDateProjectionContext context) {
  using namespace trade_date_projection_detail;

  if (!context.settlement_as_of_date.ok()) {
    return std::unexpected(error(TradeDateProjectionDiagnosticCategory::invalid_context,
                                 "settlement cutoff must be a valid date"));
  }

  auto policy = AccountingPolicyIdentity::create(AccountingPolicyId{std::string{policy_id}},
                                                 std::string{policy_version});
  if (!policy)
    return std::unexpected(journal_error(policy.error(), EventId{std::string{policy_id}}));

  std::vector<EventId> active_record_ids;
  std::vector<EventId> lifecycle_record_ids;
  std::vector<EconomicEventId> economic_event_ids;
  std::vector<SourceRecordId> source_record_ids;
  std::vector<PendingEntry> pending;
  active_record_ids.reserve(resolution.active_events().size());
  economic_event_ids.reserve(resolution.active_events().size());

  std::optional<std::pair<Timestamp, LifecycleSequence>> previous_order;
  for (const auto &resolved : resolution.active_events()) {
    const auto &record = resolved.record();
    const auto &event = resolved.event();
    const auto effective_at = header(event).effective_at();
    const auto current_order = std::pair{effective_at, record.acceptance_sequence()};
    if (previous_order && *previous_order >= current_order) {
      return std::unexpected(
          error(TradeDateProjectionDiagnosticCategory::invalid_context,
                "resolved active events must retain effective-time and acceptance-sequence order",
                record.record_id()));
    }
    previous_order = current_order;
    if (record.recorded_at() > context.recorded_through || effective_at > context.economic_as_of) {
      return std::unexpected(error(TradeDateProjectionDiagnosticCategory::invalid_context,
                                   "selected active event falls after a supplied evaluation cutoff",
                                   record.record_id()));
    }
    if (record.event() == nullptr || header(*record.event()).id() != record.record_id() ||
        resolved.lineage().empty() ||
        resolved.lineage().back().get().record_id() != record.record_id()) {
      return std::unexpected(error(TradeDateProjectionDiagnosticCategory::invalid_context,
                                   "resolved active event is inconsistent with its lifecycle head",
                                   record.record_id()));
    }
    for (const auto &reference : resolved.lineage()) {
      const auto &lineage_record = reference.get();
      if (lineage_record.recorded_at() > context.recorded_through ||
          lineage_record.economic_event_id() != record.economic_event_id()) {
        return std::unexpected(
            error(TradeDateProjectionDiagnosticCategory::invalid_context,
                  "selected lifecycle lineage is inconsistent with the supplied knowledge cutoff",
                  record.record_id()));
      }
      append_once(lifecycle_record_ids, lineage_record.record_id());
      for (const auto &source_record_id : lineage_record.provenance().source_records())
        append_once(source_record_ids, source_record_id);
    }
    active_record_ids.push_back(record.record_id());
    append_once(economic_event_ids, record.economic_event_id());

    auto entry_lineage = lineage(resolved);
    if (!entry_lineage)
      return std::unexpected(entry_lineage.error());
    auto trade_date = journal_date(effective_at, record.record_id());
    if (!trade_date)
      return std::unexpected(trade_date.error());

    if (const auto *cash = std::get_if<CashMovement>(&event)) {
      if (cash->amount().currency().code() != "USD") {
        return std::unexpected(error(TradeDateProjectionDiagnosticCategory::unsupported_currency,
                                     "fixture trade-date policy supports only USD",
                                     record.record_id()));
      }
      if (record.action() == LifecycleAction::reverse) {
        return std::unexpected(
            error(TradeDateProjectionDiagnosticCategory::invalid_reversal_treatment,
                  "fixture trade-date policy does not post cash lifecycle reversals",
                  record.record_id()));
      }
      if (cash->amount().scaled_value() <= 0) {
        return std::unexpected(
            error(TradeDateProjectionDiagnosticCategory::unsupported_event,
                  "fixture trade-date policy supports positive cash contributions, not withdrawals",
                  record.record_id()));
      }
      auto settlement_context = immediate_context(record.record_id());
      if (!settlement_context)
        return std::unexpected(settlement_context.error());
      auto projected =
          entry(record, *policy, JournalEventType::cash_movement, *trade_date,
                RecognitionPhase::immediate, 0, std::move(*settlement_context), *entry_lineage,
                cash->amount(), "asset.cash", "equity.contributed-capital", "immediate");
      if (!projected)
        return std::unexpected(projected.error());
      if (auto append_error =
              append_entry(pending, std::move(*projected), record.acceptance_sequence()))
        return std::unexpected(std::move(*append_error));
      continue;
    }

    const auto &trade = std::get<EquityTrade>(event);
    if (trade.quote_currency().code() != "USD") {
      return std::unexpected(error(TradeDateProjectionDiagnosticCategory::unsupported_currency,
                                   "fixture trade-date policy supports only USD",
                                   record.record_id()));
    }

    const auto reversal = trade.quantity().scaled_value() < 0;
    if (!reversal && record.action() == LifecycleAction::reverse) {
      return std::unexpected(
          error(TradeDateProjectionDiagnosticCategory::invalid_reversal_treatment,
                "equity lifecycle reversal must have a negative exact-offset quantity",
                record.record_id()));
    }
    if (reversal && record.action() != LifecycleAction::reverse) {
      return std::unexpected(
          error(TradeDateProjectionDiagnosticCategory::unsupported_event,
                "ordinary equity sells are unsupported by the fixture trade-date policy",
                record.record_id()));
    }

    auto amount = reversal ? reversal_trade_value(resolved, trade)
                           : positive_trade_value(trade, record.record_id());
    if (!amount)
      return std::unexpected(amount.error());

    auto settlement_date = JournalDate::create(trade.settlement_date().value());
    if (!settlement_date)
      return std::unexpected(journal_error(settlement_date.error(), record.record_id()));
    auto settlement_context = equity_context(*trade_date, *settlement_date, record.record_id());
    if (!settlement_context)
      return std::unexpected(settlement_context.error());

    const auto debit_trade = reversal ? "asset.trade-receivable" : "asset.equity-securities";
    const auto credit_trade = reversal ? "asset.equity-securities" : "liability.trade-payable";
    auto trade_entry = entry(record, *policy, JournalEventType::equity_trade, *trade_date,
                             RecognitionPhase::trade_date, 0, *settlement_context, *entry_lineage,
                             *amount, debit_trade, credit_trade, "trade");
    if (!trade_entry)
      return std::unexpected(trade_entry.error());
    if (auto append_error =
            append_entry(pending, std::move(*trade_entry), record.acceptance_sequence()))
      return std::unexpected(std::move(*append_error));

    if (trade.settlement_date().value() <= context.settlement_as_of_date) {
      const auto debit_settlement = reversal ? "asset.cash" : "liability.trade-payable";
      const auto credit_settlement = reversal ? "asset.trade-receivable" : "asset.cash";
      auto settlement_entry =
          entry(record, *policy, JournalEventType::equity_trade, *settlement_date,
                RecognitionPhase::settlement_date, 1, std::move(*settlement_context),
                *entry_lineage, *amount, debit_settlement, credit_settlement, "settlement");
      if (!settlement_entry)
        return std::unexpected(settlement_entry.error());
      if (auto append_error =
              append_entry(pending, std::move(*settlement_entry), record.acceptance_sequence()))
        return std::unexpected(std::move(*append_error));
    }
  }

  std::sort(pending.begin(), pending.end(), [](const PendingEntry &lhs, const PendingEntry &rhs) {
    const auto &left = lhs.entry;
    const auto &right = rhs.entry;
    if (left.recognized_on() != right.recognized_on())
      return left.recognized_on() < right.recognized_on();
    if (lhs.acceptance_sequence != rhs.acceptance_sequence)
      return lhs.acceptance_sequence < rhs.acceptance_sequence;
    if (left.phase_ordinal() != right.phase_ordinal())
      return left.phase_ordinal() < right.phase_ordinal();
    return left.journal_entry_id() < right.journal_entry_id();
  });

  std::vector<JournalEntry> entries;
  entries.reserve(pending.size());
  for (auto &pending_entry : pending)
    entries.push_back(std::move(pending_entry.entry));

  return TradeDateProjectionResult{std::move(*policy),
                                   context,
                                   std::move(active_record_ids),
                                   std::move(lifecycle_record_ids),
                                   std::move(economic_event_ids),
                                   std::move(source_record_ids),
                                   std::move(entries)};
}

} // namespace luca
