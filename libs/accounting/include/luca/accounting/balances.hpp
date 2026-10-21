#pragma once

#include "luca/accounting/journal.hpp"

#include <algorithm>
#include <chrono>
#include <expected>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luca {

struct BookIdTag;
struct ChartAccountIdTag;
using BookId = Identifier<BookIdTag>;
using ChartAccountId = Identifier<ChartAccountIdTag>;

enum class JournalBalanceError {
  invalid_contract,
  incompatible_contract,
  policy_mismatch,
  outside_cutoff,
  duplicate_entry,
  duplicate_line,
  amount_overflow,
};

[[nodiscard]] constexpr std::string_view category_name(JournalBalanceError error) noexcept {
  switch (error) {
  case JournalBalanceError::invalid_contract: return "invalid_contract";
  case JournalBalanceError::incompatible_contract: return "incompatible_contract";
  case JournalBalanceError::policy_mismatch: return "policy_mismatch";
  case JournalBalanceError::outside_cutoff: return "outside_cutoff";
  case JournalBalanceError::duplicate_entry: return "duplicate_entry";
  case JournalBalanceError::duplicate_line: return "duplicate_line";
  case JournalBalanceError::amount_overflow: return "amount_overflow";
  }
  return "invalid_contract";
}

// The caller assigns already-selected journals to one explicit book and input
// revision. This contract does not infer ownership or resolve economic history.
class JournalBalanceContract {
public:
  static constexpr std::string_view operation_id = "reduce.journals.exact";
  static constexpr std::string_view operation_version = "1";
  struct Definition {
    BookId book;
    std::string input_revision;
    std::string engine_version;
    std::string journal_projection_version;
    AccountingPolicyIdentity accounting_policy;
    Timestamp recorded_through;
    Timestamp economic_as_of;
    std::chrono::year_month_day settlement_as_of_date;
    bool operator==(const Definition &) const = default;
  };

  [[nodiscard]] static std::expected<JournalBalanceContract, JournalBalanceError>
  create(Definition definition) {
    const auto token = [](std::string_view text) {
      return !text.empty() && std::ranges::all_of(text, [](unsigned char c) {
        return c > 0x20 && c != 0x7f;
      });
    };
    if (!token(definition.book.value()) || !token(definition.input_revision) ||
        !token(definition.engine_version) || !token(definition.journal_projection_version) ||
        !token(definition.accounting_policy.id().value()) ||
        !token(definition.accounting_policy.version()) || !definition.settlement_as_of_date.ok())
      return std::unexpected(JournalBalanceError::invalid_contract);
    return JournalBalanceContract{std::move(definition)};
  }
  [[nodiscard]] const Definition &definition() const noexcept { return definition_; }
  bool operator==(const JournalBalanceContract &) const = default;

private:
  explicit JournalBalanceContract(Definition definition) : definition_(std::move(definition)) {}
  Definition definition_;
};

struct JournalBalanceKey {
  BookId book;
  ChartAccountId account;
  Currency currency;
  auto operator<=>(const JournalBalanceKey &) const = default;
};

struct JournalLineReference {
  JournalEntryId entry;
  JournalLineId line;
  auto operator<=>(const JournalLineReference &) const = default;
};

struct JournalAccountBalance {
  JournalBalanceKey key;
  Money debits;
  Money credits;
  Money net; // debit-positive; credits have negative net balances
  std::vector<JournalLineReference> lines;
  bool operator==(const JournalAccountBalance &) const = default;
};

struct JournalCurrencyControl {
  Currency currency;
  Money debits;  // gross posting turnover; equals credits
  Money credits;
  Money debit_balances; // sum of positive account net balances; equals credit_balances
  Money credit_balances;
  bool operator==(const JournalCurrencyControl &) const = default;
};

struct JournalEntryEvidence {
  JournalEntryId entry;
  JournalLineage lineage;
  bool operator==(const JournalEntryEvidence &) const = default;
};

namespace journal_balance_detail { class Builder; }

// Only validated reductions can construct a result. Returned spans are const;
// results own all evidence and remain usable after journals are destroyed.
class JournalBalanceResult {
public:
  [[nodiscard]] const JournalBalanceContract &contract() const noexcept { return contract_; }
  [[nodiscard]] std::span<const JournalAccountBalance> balances() const noexcept {
    return balances_;
  }
  [[nodiscard]] std::span<const JournalCurrencyControl> controls() const noexcept {
    return controls_;
  }
  [[nodiscard]] std::span<const JournalEntryEvidence> evidence() const noexcept { return evidence_; }
  bool operator==(const JournalBalanceResult &) const = default;

private:
  friend class journal_balance_detail::Builder;
  JournalBalanceResult(JournalBalanceContract contract,
                       std::vector<JournalAccountBalance> balances,
                       std::vector<JournalCurrencyControl> controls,
                       std::vector<JournalEntryEvidence> evidence)
      : contract_(std::move(contract)), balances_(std::move(balances)),
        controls_(std::move(controls)), evidence_(std::move(evidence)) {}
  JournalBalanceContract contract_;
  std::vector<JournalAccountBalance> balances_;
  std::vector<JournalCurrencyControl> controls_;
  std::vector<JournalEntryEvidence> evidence_;
};

namespace journal_balance_detail {

class Builder {
public:
  explicit Builder(JournalBalanceContract contract) : contract_(std::move(contract)) {}

  std::expected<void, JournalBalanceError> add(const JournalEntry &entry) {
    const auto &context = contract_.definition();
    if (entry.policy() != context.accounting_policy)
      return std::unexpected(JournalBalanceError::policy_mismatch);
    if (entry.recorded_at() > context.recorded_through ||
        entry.effective_at() > context.economic_as_of ||
        (entry.recognition_phase() == RecognitionPhase::settlement_date
             ? entry.recognized_on().value() > context.settlement_as_of_date
             : std::chrono::sys_days{entry.recognized_on().value()} >
                   std::chrono::floor<std::chrono::days>(context.economic_as_of)))
      return std::unexpected(JournalBalanceError::outside_cutoff);
    if (!evidence_.emplace(entry.journal_entry_id(),
                           JournalEntryEvidence{entry.journal_entry_id(), entry.lineage()}).second)
      return std::unexpected(JournalBalanceError::duplicate_entry);

    for (const auto &line : entry.lines()) {
      const auto zero = Money::from_scaled(0, line.amount().currency());
      JournalAccountBalance row{
          {context.book, ChartAccountId{line.account_id().value()}, line.amount().currency()},
          line.side() == JournalSide::debit ? line.amount() : zero,
          line.side() == JournalSide::credit ? line.amount() : zero, zero,
          {{entry.journal_entry_id(), line.journal_line_id()}}};
      if (auto result = add_row(row); !result) return result;
    }
    return {};
  }

  std::expected<void, JournalBalanceError> add(const JournalBalanceResult &partial) {
    if (contract_ != partial.contract())
      return std::unexpected(JournalBalanceError::incompatible_contract);
    for (const auto &item : partial.evidence())
      if (!evidence_.emplace(item.entry, item).second)
        return std::unexpected(JournalBalanceError::duplicate_entry);
    for (const auto &row : partial.balances())
      if (auto result = add_row(row); !result) return result;
    return {};
  }

  std::expected<JournalBalanceResult, JournalBalanceError> finish() {
    std::vector<JournalAccountBalance> balances;
    balances.reserve(rows_.size());
    for (auto &[key, row] : rows_) {
      const auto net = row.debits.subtract(row.credits);
      if (!net) return std::unexpected(JournalBalanceError::amount_overflow);
      row.net = *net;
      std::ranges::sort(row.lines);
      auto &control = controls_.at(key.currency);
      auto &side = row.net.scaled_value() >= 0 ? control.debit_balances : control.credit_balances;
      const auto magnitude = Money::from_scaled(
          row.net.scaled_value() >= 0 ? row.net.scaled_value() : -row.net.scaled_value(), key.currency);
      const auto sum = side.add(magnitude);
      if (!sum) return std::unexpected(JournalBalanceError::amount_overflow);
      side = *sum;
      balances.push_back(std::move(row)); // retain zero net balances and their evidence
    }
    std::vector<JournalCurrencyControl> controls;
    for (const auto &[currency, control] : controls_) {
      (void)currency;
      controls.push_back(control);
    }
    std::vector<JournalEntryEvidence> evidence;
    evidence.reserve(evidence_.size());
    for (auto &[id, item] : evidence_) {
      (void)id;
      evidence.push_back(std::move(item));
    }
    return JournalBalanceResult{std::move(contract_), std::move(balances),
                                std::move(controls), std::move(evidence)};
  }

private:
  std::expected<void, JournalBalanceError> add_row(const JournalAccountBalance &input) {
    for (const auto &line : input.lines)
      if (!line_ids_.insert(line.line).second)
        return std::unexpected(JournalBalanceError::duplicate_line);
    const auto zero = Money::from_scaled(0, input.key.currency);
    auto [row, inserted] = rows_.try_emplace(
        input.key, JournalAccountBalance{input.key, zero, zero, zero, {}});
    (void)inserted;
    const auto debit = row->second.debits.add(input.debits);
    const auto credit = row->second.credits.add(input.credits);
    if (!debit || !credit) return std::unexpected(JournalBalanceError::amount_overflow);
    row->second.debits = *debit;
    row->second.credits = *credit;
    row->second.lines.insert(row->second.lines.end(), input.lines.begin(), input.lines.end());

    auto [control, created] = controls_.try_emplace(
        input.key.currency, JournalCurrencyControl{input.key.currency, zero, zero, zero, zero});
    (void)created;
    const auto total_debit = control->second.debits.add(input.debits);
    const auto total_credit = control->second.credits.add(input.credits);
    if (!total_debit || !total_credit)
      return std::unexpected(JournalBalanceError::amount_overflow);
    control->second.debits = *total_debit;
    control->second.credits = *total_credit;
    return {};
  }

  JournalBalanceContract contract_;
  std::map<JournalBalanceKey, JournalAccountBalance> rows_;
  std::map<Currency, JournalCurrencyControl> controls_;
  std::map<JournalEntryId, JournalEntryEvidence> evidence_;
  std::set<JournalLineId> line_ids_;
};

} // namespace journal_balance_detail

// Partition whole journal entries, never individual lines. Nonnegative debit
// and credit turnover is checked separately; no rounding or FX occurs. Identity,
// associativity and commutativity hold for disjoint entries under one contract
// when each currency's gross debit/credit total fits int64. This monotone bound
// makes every grouping representable, unlike arbitrary signed-net reduction.
[[nodiscard]] inline std::expected<JournalBalanceResult, JournalBalanceError>
reduce_journal_balances(const JournalBalanceContract &contract,
                        std::span<const JournalEntry> entries) {
  journal_balance_detail::Builder builder{contract};
  for (const auto &entry : entries)
    if (auto result = builder.add(entry); !result) return std::unexpected(result.error());
  return builder.finish();
}

[[nodiscard]] inline std::expected<JournalBalanceResult, JournalBalanceError>
merge_journal_balances(const JournalBalanceContract &contract,
                       std::span<const JournalBalanceResult> partials) {
  journal_balance_detail::Builder builder{contract};
  for (const auto &partial : partials)
    if (auto result = builder.add(partial); !result) return std::unexpected(result.error());
  return builder.finish();
}

} // namespace luca
