#pragma once
#include "luca/accounting/period_accounting.hpp"

namespace luca::accounting {
struct ActivityPnlKey {
  LotPool pool;
  std::string account; // attribution follows the closing trade, not the opening lot owner
  auto operator<=>(const ActivityPnlKey &) const = default;
};
struct ActivityPnlRow {
  ActivityPnlKey key;
  PeriodPnlAmounts opening, movement, closing;
  bool opening_charges_complete, closing_charges_complete;
  std::vector<EventId> records;
  std::vector<std::string> settlements;
  std::vector<JournalEntryId> entries;
  bool operator==(const ActivityPnlRow &) const = default;
};
struct ActivityPnlFilter {
  std::string account, instrument;
};
struct ActivityPnlSelection {
  std::string report_id;
  std::vector<ActivityPnlRow> rows;
  std::vector<ActivityPnl> totals; // exactly all selected rows, before application paging
  bool operator==(const ActivityPnlSelection &) const = default;
};
// Attribution and presentation selection happen after the complete custody-pool
// fold. This never filters authoritative inputs or recalculates partial lots.
inline std::expected<ActivityPnlSelection, PeriodError>
select_period_pnl(const PeriodAccountingReport &report, ActivityPnlFilter filter = {}) {
  using namespace period_accounting_detail;
  try {
    auto key_for = [&](const EventId &id) -> std::optional<ActivityPnlKey> {
      auto record = std::ranges::find(report.inputs.ledger, id, &LifecycleRecord::record_id);
      if (record == report.inputs.ledger.end() || !record->event())
        return {};
      const auto *trade = std::get_if<EquityTrade>(record->event());
      if (!trade)
        return {};
      auto target = std::ranges::find(report.inputs.targets, record->account().value(),
                                      &PolicyTarget::account);
      if (target == report.inputs.targets.end())
        return {};
      return ActivityPnlKey{{target->book, target->legal_entity, target->custodian_account,
                             trade->instrument().value(),
                             std::string(trade->quote_currency().code())},
                            target->account};
    };
    auto zero_for = [](const ActivityPnlKey &key) {
      auto currency = Currency::from_code(key.pool.currency);
      if (!currency)
        throw ArithmeticFailure{};
      auto zero = Money::from_scaled(0, *currency);
      return PeriodPnlAmounts{zero, zero, zero};
    };
    struct Detail {
      PeriodPnlAmounts amounts;
      bool complete = true;
      std::set<EventId> records;
      std::set<std::string> settlements;
    };
    auto fold = [&](const AccountingReport &endpoint) {
      std::map<ActivityPnlKey, Detail> out;
      auto value = [&](const ActivityPnlKey &key) -> Detail & {
        return out.try_emplace(key, Detail{zero_for(key), true, {}, {}}).first->second;
      };
      for (const auto &trade : endpoint.lots.trades)
        if (auto key = key_for(EventId{trade.record_id})) {
          auto &v = value(*key);
          v.complete &= trade.charges_known;
          v.records.insert(EventId{trade.record_id});
        }
      for (const auto &relief : endpoint.lots.reliefs)
        if (auto key = key_for(EventId{relief.closing_record})) {
          auto &v = value(*key);
          v.amounts.economic = add(v.amounts.economic, relief.net_pnl);
          v.complete &= relief.charges_known;
          v.records.insert(EventId{relief.opening_record});
          v.records.insert(EventId{relief.closing_record});
        }
      std::map<std::string, const JournalTrace *> trace;
      for (const auto &t : endpoint.trace)
        trace.emplace(t.entry_id, &t);
      for (const auto &entry : endpoint.journals)
        if (auto key = key_for(entry.active_record_id())) {
          auto &v = value(*key);
          for (const auto &line : entry.lines())
            if (line.account_id().value() == "income.realized-pnl")
              v.amounts.recognized =
                  add(v.amounts.recognized,
                      line.side() == JournalSide::credit ? line.amount() : neg(line.amount()));
          v.records.insert(entry.lineage().record_ids().begin(),
                           entry.lineage().record_ids().end());
          if (auto t = trace.find(entry.journal_entry_id().value()); t != trace.end())
            v.settlements.insert(t->second->settlement_ids.begin(),
                                 t->second->settlement_ids.end());
        }
      for (auto &[key, v] : out)
        v.amounts.deferred = sub(v.amounts.economic, v.amounts.recognized);
      return out;
    };
    auto a = fold(report.opening), b = fold(report.closing);
    std::set<ActivityPnlKey> keys;
    for (const auto *values : {&a, &b})
      for (const auto &[key, value] : *values)
        keys.insert(key);
    for (const auto &effect : report.effects)
      if (auto key = key_for(effect.journal.active_record_id()))
        keys.insert(*key);
    ActivityPnlSelection selection{report.report_id, {}, {}};
    std::map<Currency, ActivityPnl> totals;
    for (const auto &key : keys) {
      if ((!filter.account.empty() && filter.account != key.account) ||
          (!filter.instrument.empty() && filter.instrument != key.pool.instrument))
        continue;
      auto zero = zero_for(key);
      auto av = a.contains(key) ? a.at(key) : Detail{zero, true, {}, {}};
      auto bv = b.contains(key) ? b.at(key) : Detail{zero, true, {}, {}};
      ActivityPnlRow row{key,
                         av.amounts,
                         {sub(bv.amounts.economic, av.amounts.economic),
                          sub(bv.amounts.recognized, av.amounts.recognized),
                          sub(bv.amounts.deferred, av.amounts.deferred)},
                         bv.amounts,
                         av.complete,
                         bv.complete,
                         {},
                         {},
                         {}};
      auto records = av.records;
      records.insert(bv.records.begin(), bv.records.end());
      auto settlements = av.settlements;
      settlements.insert(bv.settlements.begin(), bv.settlements.end());
      auto recognized_movement = zero.recognized;
      for (const auto &effect : report.effects)
        if (key_for(effect.journal.active_record_id()) == std::optional{key}) {
          row.entries.push_back(effect.journal.journal_entry_id());
          records.insert(effect.journal.lineage().record_ids().begin(),
                         effect.journal.lineage().record_ids().end());
          settlements.insert(effect.settlement_ids.begin(), effect.settlement_ids.end());
          for (const auto &line : effect.journal.lines())
            if (line.account_id().value() == "income.realized-pnl")
              recognized_movement =
                  add(recognized_movement,
                      line.side() == JournalSide::credit ? line.amount() : neg(line.amount()));
        }
      if (recognized_movement != row.movement.recognized)
        return std::unexpected(PeriodError{"PERIOD_PNL_DETAIL_INVARIANT", key.account, "", ""});
      row.records.assign(records.begin(), records.end());
      row.settlements.assign(settlements.begin(), settlements.end());
      auto currency = row.movement.economic.currency();
      auto &total =
          totals.try_emplace(currency, ActivityPnl{currency, zero, zero, zero}).first->second;
      auto sum = [&](PeriodPnlAmounts &out, const PeriodPnlAmounts &in) {
        out.economic = add(out.economic, in.economic);
        out.recognized = add(out.recognized, in.recognized);
        out.deferred = add(out.deferred, in.deferred);
      };
      sum(total.opening, row.opening);
      sum(total.movement, row.movement);
      sum(total.closing, row.closing);
      selection.rows.push_back(std::move(row));
    }
    for (auto &[currency, total] : totals)
      selection.totals.push_back(std::move(total));
    return selection;
  } catch (const ArithmeticFailure &) {
    return std::unexpected(PeriodError{"PERIOD_PNL_AMOUNT_OVERFLOW", "", "", ""});
  }
}
} // namespace luca::accounting
