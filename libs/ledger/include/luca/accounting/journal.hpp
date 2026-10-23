#pragma once

#include "luca/lifecycle.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace luca {

struct JournalEntryIdTag;
struct JournalLineIdTag;
struct AccountingPolicyIdTag;
using JournalEntryId = Identifier<JournalEntryIdTag>;
using JournalLineId = Identifier<JournalLineIdTag>;
using AccountingPolicyId = Identifier<AccountingPolicyIdTag>;

enum class JournalSide { debit, credit };
enum class JournalEventType { cash_movement, equity_trade };
enum class RecognitionPhase { immediate, trade_date, settlement_date, actual_settlement };

// These names are stable machine-readable categories. More detailed messages
// are explanatory only and are not intended for programmatic matching.
enum class JournalDiagnosticCategory {
  schema_shape,
  duplicate_identity,
  policy_context_mismatch,
  unbalanced_entry,
  mixed_currency_entry,
  invalid_line_amount,
  lineage_missing,
  lineage_mismatch,
  arithmetic_overflow,
};

[[nodiscard]] constexpr std::string_view
category_name(JournalDiagnosticCategory category) noexcept {
  switch (category) {
  case JournalDiagnosticCategory::schema_shape:
    return "schema_shape";
  case JournalDiagnosticCategory::duplicate_identity:
    return "duplicate_identity";
  case JournalDiagnosticCategory::policy_context_mismatch:
    return "policy_context_mismatch";
  case JournalDiagnosticCategory::unbalanced_entry:
    return "unbalanced_entry";
  case JournalDiagnosticCategory::mixed_currency_entry:
    return "mixed_currency_entry";
  case JournalDiagnosticCategory::invalid_line_amount:
    return "invalid_line_amount";
  case JournalDiagnosticCategory::lineage_missing:
    return "lineage_missing";
  case JournalDiagnosticCategory::lineage_mismatch:
    return "lineage_mismatch";
  case JournalDiagnosticCategory::arithmetic_overflow:
    return "arithmetic_overflow";
  }
  return "schema_shape";
}

class JournalError {
public:
  JournalError(JournalDiagnosticCategory category, std::string message)
      : category_(category), message_(std::move(message)) {}

  [[nodiscard]] JournalDiagnosticCategory category() const noexcept { return category_; }
  [[nodiscard]] std::string_view category_name() const noexcept {
    return luca::category_name(category_);
  }
  [[nodiscard]] const std::string &message() const noexcept { return message_; }

private:
  JournalDiagnosticCategory category_;
  std::string message_;
};

class JournalDate {
public:
  [[nodiscard]] static std::expected<JournalDate, JournalError>
  create(std::chrono::year_month_day value) {
    if (!value.ok()) {
      return std::unexpected(
          JournalError{JournalDiagnosticCategory::schema_shape, "journal date must be valid"});
    }
    return JournalDate{value};
  }

  [[nodiscard]] constexpr std::chrono::year_month_day value() const noexcept { return value_; }
  auto operator<=>(const JournalDate &) const = default;

private:
  explicit constexpr JournalDate(std::chrono::year_month_day value) : value_(value) {}
  std::chrono::year_month_day value_;
};

class AccountingPolicyIdentity {
public:
  [[nodiscard]] static std::expected<AccountingPolicyIdentity, JournalError>
  create(AccountingPolicyId id, std::string version) {
    if (id.value().empty() || version.empty()) {
      return std::unexpected(
          JournalError{JournalDiagnosticCategory::schema_shape,
                       "accounting policy identity and version must be non-empty"});
    }
    return AccountingPolicyIdentity{std::move(id), std::move(version)};
  }

  [[nodiscard]] const AccountingPolicyId &id() const noexcept { return id_; }
  [[nodiscard]] const std::string &version() const noexcept { return version_; }
  bool operator==(const AccountingPolicyIdentity &) const = default;

private:
  AccountingPolicyIdentity(AccountingPolicyId id, std::string version)
      : id_(std::move(id)), version_(std::move(version)) {}

  AccountingPolicyId id_;
  std::string version_;
};

namespace journal_detail {

template <class IdentifierType>
[[nodiscard]] bool has_empty_identifier(const std::vector<IdentifierType> &identifiers) noexcept {
  return std::ranges::any_of(identifiers, [](const auto &id) { return id.value().empty(); });
}

template <class IdentifierType>
[[nodiscard]] bool contains_identifier(std::span<const IdentifierType> identifiers,
                                       const IdentifierType &candidate) noexcept {
  return std::ranges::find(identifiers, candidate) != identifiers.end();
}

[[nodiscard]] constexpr bool valid_side(JournalSide side) noexcept {
  return side == JournalSide::debit || side == JournalSide::credit;
}

[[nodiscard]] constexpr bool valid_event_type(JournalEventType event_type) noexcept {
  return event_type == JournalEventType::cash_movement ||
         event_type == JournalEventType::equity_trade;
}

[[nodiscard]] constexpr bool valid_phase(RecognitionPhase phase) noexcept {
  return phase == RecognitionPhase::immediate || phase == RecognitionPhase::trade_date ||
         phase == RecognitionPhase::settlement_date || phase == RecognitionPhase::actual_settlement;
}

} // namespace journal_detail

// Lineage owns its identifiers and retains their declared order. It only
// validates shape; lifecycle resolution and provenance interpretation happen
// before journal construction.
class JournalLineage {
public:
  [[nodiscard]] static std::expected<JournalLineage, JournalError>
  create(std::vector<EventId> record_ids, std::vector<EconomicEventId> economic_event_ids,
         std::vector<SourceRecordId> source_record_ids,
         std::optional<EventId> reverses_record_id = std::nullopt) {
    if (record_ids.empty() || economic_event_ids.empty() || source_record_ids.empty()) {
      return std::unexpected(
          JournalError{JournalDiagnosticCategory::lineage_missing,
                       "record, economic-event, and source-record lineage must be non-empty"});
    }
    if (journal_detail::has_empty_identifier(record_ids) ||
        journal_detail::has_empty_identifier(economic_event_ids) ||
        journal_detail::has_empty_identifier(source_record_ids) ||
        (reverses_record_id && reverses_record_id->value().empty())) {
      return std::unexpected(JournalError{JournalDiagnosticCategory::schema_shape,
                                          "lineage and reversal identities must be non-empty"});
    }
    return JournalLineage{std::move(record_ids), std::move(economic_event_ids),
                          std::move(source_record_ids), std::move(reverses_record_id)};
  }

  [[nodiscard]] std::span<const EventId> record_ids() const noexcept { return record_ids_; }
  [[nodiscard]] std::span<const EconomicEventId> economic_event_ids() const noexcept {
    return economic_event_ids_;
  }
  [[nodiscard]] std::span<const SourceRecordId> source_record_ids() const noexcept {
    return source_record_ids_;
  }
  [[nodiscard]] const std::optional<EventId> &reverses_record_id() const noexcept {
    return reverses_record_id_;
  }
  bool operator==(const JournalLineage &) const = default;

private:
  JournalLineage(std::vector<EventId> record_ids, std::vector<EconomicEventId> economic_event_ids,
                 std::vector<SourceRecordId> source_record_ids,
                 std::optional<EventId> reverses_record_id)
      : record_ids_(std::move(record_ids)), economic_event_ids_(std::move(economic_event_ids)),
        source_record_ids_(std::move(source_record_ids)),
        reverses_record_id_(std::move(reverses_record_id)) {}

  std::vector<EventId> record_ids_;
  std::vector<EconomicEventId> economic_event_ids_;
  std::vector<SourceRecordId> source_record_ids_;
  std::optional<EventId> reverses_record_id_;
};

class JournalSettlementContext {
public:
  [[nodiscard]] static std::expected<JournalSettlementContext, JournalError>
  create(std::optional<JournalDate> trade_date, std::optional<JournalDate> settlement_date,
         std::string recognition_rule) {
    if (recognition_rule.empty()) {
      return std::unexpected(JournalError{JournalDiagnosticCategory::schema_shape,
                                          "settlement recognition rule must be non-empty"});
    }
    if (trade_date.has_value() != settlement_date.has_value()) {
      return std::unexpected(
          JournalError{JournalDiagnosticCategory::policy_context_mismatch,
                       "trade and settlement dates must both be present or both be absent"});
    }
    return JournalSettlementContext{trade_date, settlement_date, std::move(recognition_rule)};
  }

  [[nodiscard]] const std::optional<JournalDate> &trade_date() const noexcept {
    return trade_date_;
  }
  [[nodiscard]] const std::optional<JournalDate> &settlement_date() const noexcept {
    return settlement_date_;
  }
  [[nodiscard]] const std::string &recognition_rule() const noexcept { return recognition_rule_; }
  bool operator==(const JournalSettlementContext &) const = default;

private:
  JournalSettlementContext(std::optional<JournalDate> trade_date,
                           std::optional<JournalDate> settlement_date, std::string recognition_rule)
      : trade_date_(trade_date), settlement_date_(settlement_date),
        recognition_rule_(std::move(recognition_rule)) {}

  std::optional<JournalDate> trade_date_;
  std::optional<JournalDate> settlement_date_;
  std::string recognition_rule_;
};

class JournalLine {
public:
  [[nodiscard]] static std::expected<JournalLine, JournalError>
  create(JournalLineId journal_line_id, JournalEntryId journal_entry_id, AccountId account_id,
         JournalSide side, Money amount, JournalLineage lineage) {
    if (journal_line_id.value().empty() || journal_entry_id.value().empty() ||
        account_id.value().empty() || !journal_detail::valid_side(side)) {
      return std::unexpected(
          JournalError{JournalDiagnosticCategory::schema_shape,
                       "line, parent entry, and account identities and side must be valid"});
    }
    if (amount.scaled_value() <= 0) {
      return std::unexpected(JournalError{JournalDiagnosticCategory::invalid_line_amount,
                                          "journal line amount must be strictly positive"});
    }
    return JournalLine{std::move(journal_line_id),
                       std::move(journal_entry_id),
                       std::move(account_id),
                       side,
                       amount,
                       std::move(lineage)};
  }

  [[nodiscard]] const JournalLineId &journal_line_id() const noexcept { return journal_line_id_; }
  [[nodiscard]] const JournalEntryId &journal_entry_id() const noexcept {
    return journal_entry_id_;
  }
  [[nodiscard]] const AccountId &account_id() const noexcept { return account_id_; }
  [[nodiscard]] JournalSide side() const noexcept { return side_; }
  [[nodiscard]] Money amount() const noexcept { return amount_; }
  [[nodiscard]] const JournalLineage &lineage() const noexcept { return lineage_; }
  bool operator==(const JournalLine &) const = default;

private:
  JournalLine(JournalLineId journal_line_id, JournalEntryId journal_entry_id, AccountId account_id,
              JournalSide side, Money amount, JournalLineage lineage)
      : journal_line_id_(std::move(journal_line_id)),
        journal_entry_id_(std::move(journal_entry_id)), account_id_(std::move(account_id)),
        side_(side), amount_(amount), lineage_(std::move(lineage)) {}

  JournalLineId journal_line_id_;
  JournalEntryId journal_entry_id_;
  AccountId account_id_;
  JournalSide side_;
  Money amount_;
  JournalLineage lineage_;
};

class JournalEntry {
public:
  [[nodiscard]] static std::expected<JournalEntry, JournalError>
  create(JournalEntryId journal_entry_id, AccountingPolicyIdentity policy,
         JournalEventType event_type, EventId active_record_id, Timestamp effective_at,
         Timestamp recorded_at, JournalDate recognized_on, RecognitionPhase recognition_phase,
         std::uint32_t phase_ordinal, JournalSettlementContext settlement_context,
         JournalLineage lineage, std::vector<JournalLine> lines) {
    if (journal_entry_id.value().empty() || active_record_id.value().empty() ||
        !journal_detail::valid_event_type(event_type) ||
        !journal_detail::valid_phase(recognition_phase)) {
      return std::unexpected(
          JournalError{JournalDiagnosticCategory::schema_shape,
                       "entry, active record, event type, and recognition phase must be valid"});
    }
    if (!journal_detail::contains_identifier(lineage.record_ids(), active_record_id)) {
      return std::unexpected(
          JournalError{JournalDiagnosticCategory::lineage_mismatch,
                       "active lifecycle record must occur in entry record lineage"});
    }
    if ((event_type == JournalEventType::cash_movement &&
         (settlement_context.trade_date() || settlement_context.settlement_date() ||
          recognition_phase != RecognitionPhase::immediate)) ||
        (event_type == JournalEventType::equity_trade &&
         (!settlement_context.trade_date() || !settlement_context.settlement_date() ||
          recognition_phase == RecognitionPhase::immediate))) {
      return std::unexpected(
          JournalError{JournalDiagnosticCategory::policy_context_mismatch,
                       "cash entries require immediate recognition without settlement dates; "
                       "equity entries require trade and settlement dates"});
    }
    if ((recognition_phase == RecognitionPhase::trade_date &&
         recognized_on != *settlement_context.trade_date()) ||
        (recognition_phase == RecognitionPhase::settlement_date &&
         recognized_on != *settlement_context.settlement_date())) {
      return std::unexpected(
          JournalError{JournalDiagnosticCategory::policy_context_mismatch,
                       "recognized date must equal the date selected by the recognition phase"});
    }
    if (recognition_phase == RecognitionPhase::actual_settlement &&
        (recognized_on.value() != std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(effective_at)} ||
         recognized_on < *settlement_context.trade_date()))
      return std::unexpected(JournalError{JournalDiagnosticCategory::policy_context_mismatch,
          "actual settlement recognition must use its actual effective date, not the scheduled date"});
    if (lines.size() < 2) {
      return std::unexpected(JournalError{JournalDiagnosticCategory::schema_shape,
                                          "journal entry must contain at least two lines"});
    }

    std::unordered_set<std::string> line_ids;
    line_ids.reserve(lines.size());
    const auto currency = lines.front().amount().currency();
    for (const auto &line : lines) {
      if (!line_ids.emplace(line.journal_line_id().value()).second) {
        return std::unexpected(
            JournalError{JournalDiagnosticCategory::duplicate_identity,
                         "journal line identities must be unique within an entry"});
      }
      if (line.journal_entry_id() != journal_entry_id) {
        return std::unexpected(
            JournalError{JournalDiagnosticCategory::lineage_mismatch,
                         "journal line parent identity does not match its entry"});
      }
      if (line.lineage() != lineage) {
        return std::unexpected(JournalError{JournalDiagnosticCategory::lineage_mismatch,
                                            "journal line lineage does not match its entry"});
      }
      if (line.amount().currency() != currency) {
        return std::unexpected(JournalError{JournalDiagnosticCategory::mixed_currency_entry,
                                            "all journal lines in an entry must use one currency"});
      }
    }

    auto debit_total = Money::from_scaled(0, currency);
    auto credit_total = Money::from_scaled(0, currency);
    for (const auto &line : lines) {
      auto &total = line.side() == JournalSide::debit ? debit_total : credit_total;
      const auto sum = total.add(line.amount());
      if (!sum) {
        return std::unexpected(JournalError{JournalDiagnosticCategory::arithmetic_overflow,
                                            "journal side total exceeds exact Money range"});
      }
      total = *sum;
    }
    if (debit_total != credit_total) {
      return std::unexpected(JournalError{JournalDiagnosticCategory::unbalanced_entry,
                                          "journal debit and credit totals must be equal"});
    }

    return JournalEntry{std::move(journal_entry_id),
                        std::move(policy),
                        event_type,
                        std::move(active_record_id),
                        effective_at,
                        recorded_at,
                        recognized_on,
                        recognition_phase,
                        phase_ordinal,
                        std::move(settlement_context),
                        std::move(lineage),
                        std::move(lines),
                        debit_total,
                        credit_total};
  }

  [[nodiscard]] const JournalEntryId &journal_entry_id() const noexcept {
    return journal_entry_id_;
  }
  [[nodiscard]] const AccountingPolicyIdentity &policy() const noexcept { return policy_; }
  [[nodiscard]] JournalEventType event_type() const noexcept { return event_type_; }
  [[nodiscard]] const EventId &active_record_id() const noexcept { return active_record_id_; }
  [[nodiscard]] Timestamp effective_at() const noexcept { return effective_at_; }
  [[nodiscard]] Timestamp recorded_at() const noexcept { return recorded_at_; }
  [[nodiscard]] JournalDate recognized_on() const noexcept { return recognized_on_; }
  [[nodiscard]] RecognitionPhase recognition_phase() const noexcept { return recognition_phase_; }
  [[nodiscard]] std::uint32_t phase_ordinal() const noexcept { return phase_ordinal_; }
  [[nodiscard]] const JournalSettlementContext &settlement_context() const noexcept {
    return settlement_context_;
  }
  [[nodiscard]] const JournalLineage &lineage() const noexcept { return lineage_; }
  [[nodiscard]] std::span<const JournalLine> lines() const noexcept { return lines_; }
  [[nodiscard]] Currency currency() const noexcept { return debit_total_.currency(); }
  [[nodiscard]] Money debit_total() const noexcept { return debit_total_; }
  [[nodiscard]] Money credit_total() const noexcept { return credit_total_; }
  bool operator==(const JournalEntry &) const = default;

private:
  JournalEntry(JournalEntryId journal_entry_id, AccountingPolicyIdentity policy,
               JournalEventType event_type, EventId active_record_id, Timestamp effective_at,
               Timestamp recorded_at, JournalDate recognized_on, RecognitionPhase recognition_phase,
               std::uint32_t phase_ordinal, JournalSettlementContext settlement_context,
               JournalLineage lineage, std::vector<JournalLine> lines, Money debit_total,
               Money credit_total)
      : journal_entry_id_(std::move(journal_entry_id)), policy_(std::move(policy)),
        event_type_(event_type), active_record_id_(std::move(active_record_id)),
        effective_at_(effective_at), recorded_at_(recorded_at), recognized_on_(recognized_on),
        recognition_phase_(recognition_phase), phase_ordinal_(phase_ordinal),
        settlement_context_(std::move(settlement_context)), lineage_(std::move(lineage)),
        lines_(std::move(lines)), debit_total_(debit_total), credit_total_(credit_total) {}

  JournalEntryId journal_entry_id_;
  AccountingPolicyIdentity policy_;
  JournalEventType event_type_;
  EventId active_record_id_;
  Timestamp effective_at_;
  Timestamp recorded_at_;
  JournalDate recognized_on_;
  RecognitionPhase recognition_phase_;
  std::uint32_t phase_ordinal_;
  JournalSettlementContext settlement_context_;
  JournalLineage lineage_;
  std::vector<JournalLine> lines_;
  Money debit_total_;
  Money credit_total_;
};

} // namespace luca
