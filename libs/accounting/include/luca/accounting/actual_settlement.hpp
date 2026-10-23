#pragma once
#include "luca/lifecycle.hpp"
#include <map>
#include <set>

namespace luca::accounting {
// Actual signed cash evidence. Separate from contractual settlement dates and
// the trade lifecycle: correcting/voiding a trade never erases a bank movement.
struct SettlementEvent {
  std::string id, account, trade_record, economic_event;
  Timestamp effective_at, recorded_at;
  std::uint64_t sequence, ledger_through;
  Money cash;
  std::vector<SourceRecordId> sources;
  std::optional<std::string> reverses;
  bool operator==(const SettlementEvent &) const = default;
};
struct SettlementError {
  std::string code, id;
};
inline const LifecycleRecord *settlement_trade(const LifecycleLedger &ledger,
                                               const std::string &id) {
  return ledger.find(EventId{id});
}
// Validates an immutable accepted prefix against the exact ledger prefix known
// at each cash admission. Indexed lifecycle chains keep validation bounded by map lookups and
// the normally short correction chains; no current correction can
// retroactively invalidate cash that was valid when admitted.
inline std::expected<void, SettlementError>
validate_settlement_history(const LifecycleLedger &ledger,
                            std::span<const SettlementEvent> events) {
  std::map<std::string, std::vector<const LifecycleRecord *>> chains;
  for (const auto &r : ledger.records())
    chains[r.economic_event_id().value()].push_back(&r);
  std::map<std::string, const SettlementEvent *> ids;
  std::set<std::string> reversed;
  std::map<std::string, Money> totals;
  std::uint64_t sequence = 0, known = 0;
  Timestamp last_knowledge = Timestamp::min();
  std::vector<Timestamp> prefix_knowledge{Timestamp::min()};
  for (const auto &r : ledger.records())
    prefix_knowledge.push_back(std::max(prefix_knowledge.back(), r.recorded_at()));
  for (const auto &e : events) {
    auto fail = [&](std::string code) {
      return std::unexpected(SettlementError{std::move(code), e.id});
    };
    if (e.id.empty() || e.account.empty() || e.economic_event.empty() || e.sources.empty() ||
        !e.cash.scaled_value())
      return fail("INVALID_SETTLEMENT");
    if (e.sequence != ++sequence || e.ledger_through < known ||
        e.ledger_through > ledger.records().size() || !ids.emplace(e.id, &e).second)
      return fail("INVALID_SETTLEMENT_PREFIX");
    known = e.ledger_through;
    if (e.recorded_at < last_knowledge || e.recorded_at < prefix_knowledge[known])
      return fail("SETTLEMENT_KNOWLEDGE_MISMATCH");
    last_knowledge = e.recorded_at;
    for (const auto &source : e.sources)
      if (source.value().empty())
        return fail("INVALID_SETTLEMENT_SOURCE");
    if (e.effective_at > e.recorded_at)
      return fail("SETTLEMENT_IN_FUTURE");
    const auto *record = settlement_trade(ledger, e.trade_record);
    if (!record || record->acceptance_sequence().value() > known || !record->event() ||
        !std::holds_alternative<EquityTrade>(*record->event()))
      return fail("SETTLEMENT_TRADE_REQUIRED");
    const auto &trade = std::get<EquityTrade>(*record->event());
    if (record->account().value() != e.account ||
        record->economic_event_id().value() != e.economic_event ||
        trade.quote_currency() != e.cash.currency())
      return fail("SETTLEMENT_BINDING_MISMATCH");
    if (e.effective_at < trade.header().effective_at())
      return fail("SETTLEMENT_BEFORE_TRADE");
    if (e.recorded_at < record->recorded_at())
      return fail("SETTLEMENT_KNOWLEDGE_MISMATCH");
    auto [it, inserted] =
        totals.emplace(e.economic_event, Money::from_scaled(0, e.cash.currency()));
    auto next = it->second.add(e.cash);
    if (!next)
      return fail("SETTLEMENT_AMOUNT_OVERFLOW");
    if (e.reverses) {
      auto old = ids.find(*e.reverses);
      if (old == ids.end() || old->second == &e || old->second->reverses ||
          old->second->account != e.account || old->second->trade_record != e.trade_record ||
          old->second->cash.currency() != e.cash.currency() ||
          old->second->effective_at > e.effective_at ||
          old->second->cash.scaled_value() == INT64_MIN ||
          e.cash.scaled_value() != -old->second->cash.scaled_value())
        return fail("INVALID_SETTLEMENT_REVERSAL");
      if (!reversed.insert(*e.reverses).second)
        return fail("SETTLEMENT_ALREADY_REVERSED");
    } else {
      const LifecycleRecord *head = nullptr;
      for (const auto *r : chains[e.economic_event]) {
        if (r->acceptance_sequence().value() > known)
          break;
        if (r->cancellation_at() && *r->cancellation_at() > e.effective_at)
          continue;
        head = r;
      }
      if (!head || !head->event() || header(*head->event()).effective_at() > e.effective_at)
        return fail("SETTLEMENT_TRADE_INACTIVE");
      const auto *active = std::get_if<EquityTrade>(head->event());
      if (!active || active->quote_currency() != e.cash.currency())
        return fail("SETTLEMENT_BINDING_MISMATCH");
      auto due = trade_cash(*active);
      if (!due)
        return fail("SETTLEMENT_AMOUNT_OVERFLOW");
      if ((due->scaled_value() > 0 &&
           (e.cash.scaled_value() < 0 || next->scaled_value() > due->scaled_value() ||
            next->scaled_value() < 0)) ||
          (due->scaled_value() < 0 &&
           (e.cash.scaled_value() > 0 || next->scaled_value() < due->scaled_value() ||
            next->scaled_value() > 0)) ||
          !due->scaled_value())
        return fail("SETTLEMENT_EXCEEDS_OBLIGATION");
    }
    it->second = *next;
  }
  return {};
}
inline std::expected<void, SettlementError>
validate_settlement(const LifecycleLedger &ledger, std::span<const SettlementEvent> prior,
                    const SettlementEvent &event) {
  if (event.ledger_through != ledger.records().size())
    return std::unexpected(SettlementError{"SETTLEMENT_LEDGER_CUTOFF_MISMATCH", event.id});
  std::vector<SettlementEvent> all(prior.begin(), prior.end());
  all.push_back(event);
  return validate_settlement_history(ledger, all);
}
} // namespace luca::accounting
