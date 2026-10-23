#pragma once
#include "luca/accounting/actual_settlement.hpp"
#include "luca/accounting/balances.hpp"
#include "luca/accounting/lots.hpp"

namespace luca::accounting {
enum class ChargeTreatment { capitalize_opening };
enum class CashRecognition { both_legs };
// Hosts must choose these rules explicitly; neither is inferred from LIFO.
struct RecognitionRules {
  ChargeTreatment charges;
  CashRecognition cash;
  bool operator==(const RecognitionRules &) const = default;
};
struct AccountingContext {
  Timestamp recorded_through, economic_as_of;
  std::uint64_t accepted_through, settlement_through;
  std::string mapping_version;
  bool operator==(const AccountingContext &) const = default;
};
struct JournalTrace {
  std::string entry_id, ledger_account, policy_digest;
  std::vector<std::string> relief_records, settlement_ids;
  bool operator==(const JournalTrace &) const = default;
};
struct TradeObligation {
  std::string record_id, economic_event, account;
  Money contractual_cash, actual_cash, outstanding_cash;
  bool operator==(const TradeObligation &) const = default;
};
struct AccountingBreak {
  std::string code, record_id;
  Money amount;
  std::vector<std::string> settlement_ids;
  bool operator==(const AccountingBreak &) const = default;
};
struct AccountingReport {
  AccountingContext context;
  RecognitionRules rules;
  LotReport lots;
  std::vector<JournalEntry> journals;
  std::vector<JournalTrace> trace;
  JournalBalanceResult trial_balance;
  std::vector<TradeObligation> obligations;
  std::vector<AccountingBreak> breaks;
  std::vector<SettlementEvent> settlements;
  bool operator==(const AccountingReport &) const = default;
};
struct AccountingError {
  std::string code, record_id;
};
namespace trade_accounting_detail {
struct ArithmeticFailure {};
inline Money add(Money a, Money b) {
  auto v = a.add(b);
  if (!v)
    throw ArithmeticFailure{};
  return *v;
}
inline Money sub(Money a, Money b) {
  auto v = a.subtract(b);
  if (!v)
    throw ArithmeticFailure{};
  return *v;
}
inline Money neg(Money a) { return sub(Money::from_scaled(0, a.currency()), a); }
inline Money carrying(const LotAmounts &a, bool short_position) {
  auto total = a.principal;
  for (const auto &c : a.charges)
    total = short_position ? sub(total, c.amount) : add(total, c.amount);
  return total;
}
inline std::string digest(std::string_view value) {
  return serialization::detail::sha256_hex(std::as_bytes(std::span{value}));
}
inline void frame(std::string &out, std::string_view value) {
  out += std::to_string(value.size()) + ":" + std::string(value);
}
inline std::int64_t magnitude(std::int64_t n) {
  if (n == INT64_MIN)
    throw ArithmeticFailure{};
  return n < 0 ? -n : n;
}
inline Money fraction(Money a, std::int64_t n, std::int64_t d) {
  auto v = detail::divide_rounded(detail::Wide(a.scaled_value()) * n, d, RoundingMode::half_even);
  if (v < INT64_MIN || v > INT64_MAX)
    throw ArithmeticFailure{};
  return Money::from_scaled(static_cast<std::int64_t>(v), a.currency());
}
} // namespace trade_accounting_detail

// Pure ordered map/fold. Full custody/instrument/currency lot pools must be
// present. Independent complete books/pools can be scheduled in parallel; never
// split a dependent pool by date or merge partial lot state as an additive sum.
// This returns a regenerated journal projection, not an append-only posting log.
inline std::expected<AccountingReport, AccountingError>
project_trade_accounting(const LifecycleLedger &ledger, std::span<const ResolvedPolicy> policies,
                         std::span<const SettlementEvent> settlements, AccountingContext context,
                         RecognitionRules rules) {
  using namespace trade_accounting_detail;
  auto fail = [](std::string code, std::string record = "") {
    return std::unexpected(AccountingError{std::move(code), std::move(record)});
  };
  if (context.mapping_version.empty() || context.accepted_through != ledger.records().size() ||
      policies.empty() || rules.charges != ChargeTreatment::capitalize_opening ||
      rules.cash != CashRecognition::both_legs)
    return fail("INVALID_ACCOUNTING_CONTEXT");
  try {
    auto resolution = ledger.resolve(context.recorded_through, context.economic_as_of);

    std::map<std::string, const ResolvedPolicy *> bindings;
    std::string policy_encoding = "luca.equity-journals/1";
    for (const auto &p : policies) {
      if (p.target.book != policies.front().target.book)
        return fail("CROSS_BOOK_ACCOUNTING_POLICY");
      if (p.recognition == RecognitionBasis::unspecified)
        return fail("RECOGNITION_POLICY_REQUIRED", p.target.account);
      if (p.effective_at != context.economic_as_of ||
          !bindings.emplace(p.target.account, &p).second)
        return fail("INVALID_ACCOUNTING_POLICY");
      frame(policy_encoding, p.digest);
      frame(policy_encoding, p.target.account);
    }
    std::vector<ResolvedLifecycleEvent> partition;
    for (const auto &r : resolution.active_events())
      if (bindings.contains(r.record().account().value()))
        partition.push_back(r);
    auto lots = project_policy_lots(partition, policies);
    if (!lots)
      return fail(lots.error().code, lots.error().record_id);
    frame(policy_encoding, std::to_string(static_cast<int>(rules.charges)));
    frame(policy_encoding, std::to_string(static_cast<int>(rules.cash)));
    auto identity = AccountingPolicyIdentity::create(AccountingPolicyId{"luca.equity-journals"},
                                                     digest(policy_encoding));
    if (!identity)
      return fail("INVALID_JOURNAL_POLICY");
    std::map<std::string, const SettlementEvent *> settlement_index;
    for (const auto &e : settlements)
      if (!settlement_index.emplace(e.id, &e).second)
        return fail("INVALID_SETTLEMENT_ORDER", e.id);
    std::vector<JournalEntry> entries;
    std::vector<JournalTrace> traces;
    std::vector<AccountingBreak> breaks;
    std::vector<TradeObligation> obligations;
    std::map<std::string, const ResolvedLifecycleEvent *> active;
    std::map<std::string, const LotTrade *> trades;
    std::map<std::string, const ResolvedLifecycleEvent *> records;
    for (const auto &r : partition) {
      active[r.record().economic_event_id().value()] = &r;
      records[r.record().record_id().value()] = &r;
    }
    for (const auto &t : lots->trades)
      trades[t.record_id] = &t;
    std::map<std::string, std::vector<const LifecycleRecord *>> economic_history;
    for (const auto &r : ledger.records())
      if (r.recorded_at() <= context.recorded_through &&
          (!r.cancellation_at() || *r.cancellation_at() <= context.economic_as_of))
        economic_history[r.economic_event_id().value()].push_back(&r);
    std::map<std::string, std::vector<const LotRelief *>> reliefs_by_close;
    for (const auto &r : lots->reliefs)
      reliefs_by_close[r.closing_record].push_back(&r);
    auto lineage = [&](std::span<const std::string> economic_ids,
                       std::span<const SettlementEvent *const> cash_events) {
      std::vector<EventId> ids;
      std::vector<EconomicEventId> economics;
      std::vector<SourceRecordId> sources;
      for (const auto &id : economic_ids) {
        economics.push_back(EconomicEventId{id});
        for (const auto *r : economic_history[id]) {
          ids.push_back(r->record_id());
          for (const auto &s : r->provenance().source_records())
            sources.push_back(s);
        }
      }
      for (const auto *e : cash_events)
        for (const auto &s : e->sources)
          sources.push_back(s);
      return JournalLineage::create(std::move(ids), std::move(economics), std::move(sources));
    };
    auto emit = [&](std::string id, const LifecycleRecord &record, Timestamp at,
                    RecognitionPhase phase, const std::map<std::string, Money> &signed_lines,
                    const JournalLineage &proof, std::vector<std::string> relief,
                    std::vector<std::string> cash_ids) -> bool {
      std::vector<JournalLine> lines;
      for (const auto &[account, amount] : signed_lines)
        if (amount.scaled_value()) {
          auto positive = amount.scaled_value() < 0 ? neg(amount) : amount;
          auto line = JournalLine::create(
              JournalLineId{id + ":" + account}, JournalEntryId{id}, AccountId{account},
              amount.scaled_value() > 0 ? JournalSide::debit : JournalSide::credit, positive,
              proof);
          if (!line)
            return false;
          lines.push_back(*line);
        }
      if (lines.empty())
        return true;
      auto day = JournalDate::create(
          std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(at)});
      if (!day)
        return false;
      const auto *trade = record.event() ? std::get_if<EquityTrade>(record.event()) : nullptr;
      auto trade_day =
          trade ? JournalDate::create(std::chrono::year_month_day{
                      std::chrono::floor<std::chrono::days>(trade->header().effective_at())})
                : day;
      auto supplied = trade ? JournalDate::create(trade->settlement_date().value()) : day;
      if (!trade_day || !supplied)
        return false;
      auto settlement = JournalSettlementContext::create(
          trade ? std::optional{*trade_day} : std::nullopt,
          trade ? std::optional{*supplied} : std::nullopt, "actual-cash/1");
      if (!settlement)
        return false;
      auto known_at = record.recorded_at();
      for (const auto &id : proof.record_ids())
        if (const auto *r = ledger.find(id))
          known_at = std::max(known_at, r->recorded_at());
      for (const auto &id : cash_ids)
        known_at = std::max(known_at, settlement_index.at(id)->recorded_at);
      auto e = JournalEntry::create(
          JournalEntryId{id}, *identity,
          trade ? JournalEventType::equity_trade : JournalEventType::cash_movement,
          record.record_id(), at, known_at, *day, phase, 0, *settlement, proof, std::move(lines));
      if (!e)
        return false;
      entries.push_back(*e);
      const auto p = bindings.find(record.account().value());
      if (p == bindings.end())
        return false;
      traces.push_back({id, record.account().value(), p->second->digest, std::move(relief),
                        std::move(cash_ids)});
      return true;
    };
    auto posting = [](std::map<std::string, Money> &lines, std::string key, Money value) {
      auto [it, inserted] = lines.emplace(std::move(key), value);
      if (!inserted)
        it->second = add(it->second, value);
    };
    // Trade-date principal and obligations do not depend on recognition basis.
    // Opening charges remain in long cost/short net proceeds until lot relief.
    for (const auto &r : partition) {
      const auto &record = r.record();
      auto binding = bindings.find(record.account().value());
      if (binding == bindings.end())
        return fail("ACCOUNTING_ACCOUNT_REQUIRED", record.record_id().value());
      const std::array economics{record.economic_event_id().value()};
      auto proof = lineage(economics, {});
      if (!proof)
        return fail("JOURNAL_LINEAGE");
      std::map<std::string, Money> lines;
      std::vector<std::string> relieved;
      if (const auto *cash = std::get_if<CashMovement>(&r.event())) {
        if (cash->amount().scaled_value() <= 0)
          return fail("CASH_CONTRIBUTION_REQUIRED", record.record_id().value());
        posting(lines, "asset.cash", cash->amount());
        posting(lines, "equity.contributed-capital", neg(cash->amount()));
        if (!emit("cash:" + record.record_id().value(), record, cash->header().effective_at(),
                  RecognitionPhase::immediate, lines, *proof, {}, {}))
          return fail("JOURNAL_INVARIANT");
        continue;
      }
      const auto &t = *trades.at(record.record_id().value());
      const auto zero = Money::from_scaled(0, t.cash_flow.currency());
      posting(lines,
              t.cash_flow.scaled_value() >= 0 ? "asset.trade-receivable"
                                              : "liability.trade-payable",
              t.cash_flow);
      auto remaining = t.amounts;
      auto remaining_q = magnitude(t.signed_quantity.scaled_value());
      std::vector<std::string> evidence{record.economic_event_id().value()};
      for (const auto *relief_ptr : reliefs_by_close[t.record_id]) {
        const auto &relief = *relief_ptr;
        const auto *opening = records.at(relief.opening_record);
        evidence.push_back(opening->record().economic_event_id().value());
        relieved.push_back(relief.opening_record);
        const auto carrying_value = carrying(relief.opening, relief.short_position);
        posting(lines,
                relief.short_position ? "liability.short-equities" : "asset.equity-securities",
                relief.short_position ? carrying_value : neg(carrying_value));
        posting(lines,
                binding->second->recognition == RecognitionBasis::cash
                    ? "control.deferred-realized-pnl"
                    : "income.realized-pnl",
                neg(relief.net_pnl));
        remaining.principal = sub(remaining.principal, relief.closing.principal);
        for (std::size_t i = 0; i < remaining.charges.size(); ++i)
          remaining.charges[i].amount =
              sub(remaining.charges[i].amount, relief.closing.charges[i].amount);
        remaining_q -= relief.quantity.scaled_value();
      }
      if (remaining_q)
        posting(lines,
                t.signed_quantity.scaled_value() < 0 ? "liability.short-equities"
                                                     : "asset.equity-securities",
                t.signed_quantity.scaled_value() < 0 ? neg(carrying(remaining, true))
                                                     : carrying(remaining, false));
      proof = lineage(evidence, {});
      if (!proof)
        return fail("JOURNAL_LINEAGE");
      if (!emit("trade:" + t.record_id, record, header(r.event()).effective_at(),
                RecognitionPhase::trade_date, lines, *proof, relieved, {}))
        return fail("JOURNAL_INVARIANT", t.record_id);
    }
    std::vector<SettlementEvent> selected;
    std::set<std::string> settlement_ids;
    std::uint64_t previous = 0;
    std::vector<SettlementEvent> accepted_settlements;
    for (const auto &e : settlements)
      if (e.sequence <= context.settlement_through)
        accepted_settlements.push_back(e);
    auto settlement_check = validate_settlement_history(ledger, accepted_settlements);
    if (!settlement_check)
      return fail(settlement_check.error().code, settlement_check.error().id);
    for (const auto &e : settlements) {
      if (e.sequence != previous + 1 || !settlement_ids.insert(e.id).second || e.sources.empty())
        return fail("INVALID_SETTLEMENT_ORDER", e.id);
      previous = e.sequence;
      if (e.sequence > context.settlement_through)
        continue;
      if (bindings.contains(e.account) && e.recorded_at <= context.recorded_through &&
          e.effective_at <= context.economic_as_of)
        selected.push_back(e);
    }
    if (context.settlement_through > previous)
      return fail("SETTLEMENT_SEQUENCE_UNAVAILABLE");
    std::sort(selected.begin(), selected.end(), [](const auto &a, const auto &b) {
      return std::pair{a.effective_at, a.sequence} < std::pair{b.effective_at, b.sequence};
    });
    std::map<std::string, Money> paid;
    std::map<std::string, std::vector<std::string>> paid_ids;
    for (const auto &e : selected) {
      const auto *record = settlement_trade(ledger, e.trade_record);
      if (!record || record->account().value() != e.account ||
          record->economic_event_id().value() != e.economic_event || !record->event() ||
          !std::holds_alternative<EquityTrade>(*record->event()) ||
          std::get<EquityTrade>(*record->event()).quote_currency() != e.cash.currency())
        return fail("SETTLEMENT_BINDING_MISMATCH", e.id);
      const auto zero = Money::from_scaled(0, e.cash.currency());
      auto [p, inserted] = paid.emplace(e.economic_event, zero);
      p->second = add(p->second, e.cash);
      paid_ids[e.economic_event].push_back(e.id);
      std::map<std::string, Money> lines;
      posting(lines, "asset.cash", e.cash);
      const auto current = active.find(e.economic_event);
      auto control = "control.settlement-suspense";
      if (current != active.end()) {
        const auto *trade = std::get_if<EquityTrade>(&current->second->event());
        if (!trade)
          return fail("SETTLEMENT_BINDING_MISMATCH", e.id);
        auto cash = trade_cash(*trade);
        if (!cash)
          return fail("ACCOUNTING_AMOUNT_OVERFLOW");
        control = cash->scaled_value() >= 0 ? "asset.trade-receivable" : "liability.trade-payable";
      }
      posting(lines, control, neg(e.cash));
      const std::array economics{e.economic_event};
      const std::array<const SettlementEvent *, 1> events{&e};
      auto proof = lineage(economics, events);
      if (!proof)
        return fail("JOURNAL_LINEAGE");
      if (!emit("settlement:" + e.id, *record, e.effective_at, RecognitionPhase::actual_settlement,
                lines, *proof, {}, {e.id}))
        return fail("JOURNAL_INVARIANT", e.id);
    }
    for (const auto &t : lots->trades) {
      const auto *record = records.at(t.record_id);
      const auto eco = record->record().economic_event_id().value();
      auto it = paid.find(eco);
      const auto actual =
          it == paid.end() ? Money::from_scaled(0, t.cash_flow.currency()) : it->second;
      obligations.push_back({t.record_id, eco, record->record().account().value(), t.cash_flow,
                             actual, sub(t.cash_flow, actual)});
      if ((t.cash_flow.scaled_value() >= 0 &&
           (actual.scaled_value() < 0 || actual.scaled_value() > t.cash_flow.scaled_value())) ||
          (t.cash_flow.scaled_value() < 0 &&
           (actual.scaled_value() > 0 || actual.scaled_value() < t.cash_flow.scaled_value())))
        breaks.push_back({"SETTLEMENT_EXCESS_AFTER_CORRECTION", t.record_id,
                          sub(actual, t.cash_flow), paid_ids[eco]});
    }
    for (const auto &[eco, amount] : paid)
      if (!active.contains(eco) && amount.scaled_value())
        breaks.push_back({"SETTLEMENT_ON_VOIDED_TRADE", eco, amount, paid_ids[eco]});
    std::map<std::string, std::vector<const SettlementEvent *>> cash_by_economic;
    for (const auto &e : selected)
      cash_by_economic[e.economic_event].push_back(&e);
    // Cash P&L releases cumulatively, using the lesser settled proportion of
    // both trades in both trades. Cumulative rounding
    // avoids drift and exhausts the exact net P&L on full settlement. Refunds
    // reopen the deferred balance; no scheduled date participates.
    for (const auto &relief : lots->reliefs) {
      const auto *closing = records.at(relief.closing_record);
      const auto *opening = records.at(relief.opening_record);
      const auto *policy = bindings.at(closing->record().account().value());
      if (policy->recognition != RecognitionBasis::cash)
        continue;
      const auto close_at = header(closing->event()).effective_at();
      std::map<std::string, Money> cumulative;
      auto recognized = Money::from_scaled(0, relief.net_pnl.currency());
      const auto open_eco = opening->record().economic_event_id().value(),
                 close_eco = closing->record().economic_event_id().value();
      std::vector<const SettlementEvent *> evidence;
      auto release = [&](Timestamp at, std::string suffix) -> bool {
        std::int64_t numerator = 1, denominator = 1;
        for (const auto &pair : {std::pair{open_eco, relief.opening_record},
                                 std::pair{close_eco, relief.closing_record}}) {
          const auto signed_due = trades.at(pair.second)->cash_flow.scaled_value();
          const auto due = magnitude(signed_due);
          if (!due)
            continue;
          const auto found = cumulative.find(pair.first);
          const auto signed_actual = found == cumulative.end() ? 0 : found->second.scaled_value();
          auto actual =
              (signed_due > 0 && signed_actual > 0) || (signed_due < 0 && signed_actual < 0)
                  ? magnitude(signed_actual)
                  : 0;
          actual = std::min(actual, due);
          if (detail::Wide(actual) * denominator < detail::Wide(numerator) * due) {
            numerator = actual;
            denominator = due;
          }
        }
        auto target = fraction(relief.net_pnl, numerator, denominator);
        auto delta = sub(target, recognized);
        recognized = target;
        if (!delta.scaled_value())
          return true;
        std::map<std::string, Money> lines;
        posting(lines, "control.deferred-realized-pnl", delta);
        posting(lines, "income.realized-pnl", neg(delta));
        const std::array economics{open_eco, close_eco};
        auto proof = lineage(economics, evidence);
        if (!proof)
          return false;
        std::vector<std::string> ids;
        for (const auto *e : evidence)
          ids.push_back(e->id);
        std::string stem;
        frame(stem, relief.opening_record);
        frame(stem, relief.closing_record);
        frame(stem, suffix);
        return emit("recognition:" + digest(stem), closing->record(), at,
                    RecognitionPhase::actual_settlement, lines, *proof,
                    {relief.opening_record, relief.closing_record}, ids);
      };
      bool at_close = false;
      auto relevant = cash_by_economic[open_eco];
      const auto &closing_cash = cash_by_economic[close_eco];
      relevant.insert(relevant.end(), closing_cash.begin(), closing_cash.end());
      std::sort(relevant.begin(), relevant.end(), [](const auto *a, const auto *b) {
        return std::pair{a->effective_at, a->sequence} < std::pair{b->effective_at, b->sequence};
      });
      for (const auto *event : relevant) {
        const auto &e = *event;
        if (!at_close && e.effective_at > close_at) {
          if (!release(close_at, "trade"))
            return fail("JOURNAL_INVARIANT");
          at_close = true;
        }
        auto [it, inserted] =
            cumulative.emplace(e.economic_event, Money::from_scaled(0, e.cash.currency()));
        it->second = add(it->second, e.cash);
        evidence.push_back(&e);
        if (e.effective_at >= close_at) {
          at_close = true;
          if (!release(e.effective_at, e.id))
            return fail("JOURNAL_INVARIANT");
        }
      }
      if (!at_close && !release(close_at, "trade"))
        return fail("JOURNAL_INVARIANT");
    }
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
      return std::tuple{a.recognized_on(), a.effective_at(), a.journal_entry_id()} <
             std::tuple{b.recognized_on(), b.effective_at(), b.journal_entry_id()};
    });
    std::string revision;
    frame(revision, std::to_string(context.accepted_through));
    frame(revision, std::to_string(context.settlement_through));
    frame(revision, context.mapping_version);
    auto contract = JournalBalanceContract::create(
        {BookId{policies.front().target.book}, digest(revision), "luca/0.1", "equity-journals/1",
         *identity, context.recorded_through, context.economic_as_of,
         std::chrono::year_month_day{
             std::chrono::floor<std::chrono::days>(context.economic_as_of)}});
    if (!contract)
      return fail("INVALID_BALANCE_CONTEXT");
    auto balance = reduce_journal_balances(*contract, entries);
    if (!balance)
      return fail("TRIAL_BALANCE_INVARIANT");
    return AccountingReport{context,
                            rules,
                            *lots,
                            std::move(entries),
                            std::move(traces),
                            *balance,
                            std::move(obligations),
                            std::move(breaks),
                            std::move(selected)};
  } catch (const ArithmeticFailure &) {
    return fail("ACCOUNTING_AMOUNT_OVERFLOW");
  }
}
} // namespace luca::accounting
