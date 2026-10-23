#pragma once
#include "luca/lifecycle.hpp"
#include "luca/accounting/policy.hpp"
#include <map>
#include <set>
#include <tuple>

namespace luca::accounting {
struct LotPool {
  std::string book, legal_entity, custodian_account, instrument, currency;
  auto operator<=>(const LotPool &) const = default;
};
struct LotAccount {
  std::string ledger_account, book, legal_entity, custodian_account, policy_version;
};
struct LotError {
  std::string code, record_id;
};
struct LotAmounts {
  Money principal;
  TradeCharges charges;
  bool operator==(const LotAmounts &) const = default;
};
struct OpenLot {
  LotPool pool;
  std::string opening_record, policy_version;
  Timestamp opened_at;
  std::uint64_t opening_sequence;
  bool short_position, charges_known;
  Quantity remaining_quantity;
  LotAmounts remaining;
  bool operator==(const OpenLot &) const = default;
};
// Economic carrying amount: long acquisition cost including allocated charges,
// or short opening proceeds after allocated charges. Not a tax-law determination.
inline std::expected<Money, ValueError> lot_carrying_amount(const OpenLot &lot) {
  auto amount = lot.remaining.principal;
  for (const auto &charge : lot.remaining.charges) {
    auto next = lot.short_position ? amount.subtract(charge.amount) : amount.add(charge.amount);
    if (!next)
      return std::unexpected(next.error());
    amount = *next;
  }
  return amount;
}
struct LotRelief {
  LotPool pool;
  std::string opening_record, closing_record, policy_version;
  bool short_position, charges_known;
  Quantity quantity;
  LotAmounts opening, closing;
  Money gross_pnl, net_pnl;
  bool operator==(const LotRelief &) const = default;
};
struct LotTrade {
  LotPool pool;
  std::string record_id, policy_version;
  Quantity signed_quantity;
  LotAmounts amounts;
  Money commissions, fees, cash_flow;
  bool charges_known;
  bool operator==(const LotTrade &) const = default;
};
struct LotReport {
  std::vector<OpenLot> open_lots;
  std::vector<LotRelief> reliefs;
  std::vector<LotTrade> trades;
  bool charges_complete = true;
  std::vector<ResolvedPolicy> resolved_policies;
  bool operator==(const LotReport &) const = default;
};
namespace lot_detail {
inline std::expected<LotAmounts, ValueError> split(LotAmounts &remaining, std::int64_t take,
                                                   std::int64_t quantity) {
  if (take <= 0 || take > quantity)
    return std::unexpected(ValueError::zero_quantity);
  auto part = [&](Money amount) {
    // Both operands are nonnegative int64; their product fits signed 128 bits.
    return Money::from_scaled(
        static_cast<std::int64_t>(detail::divide_rounded(detail::Wide(amount.scaled_value()) * take,
                                                         quantity, RoundingMode::half_even)),
        amount.currency());
  };
  LotAmounts out{part(remaining.principal), remaining.charges};
  remaining.principal = *remaining.principal.subtract(out.principal);
  for (std::size_t i = 0; i < out.charges.size(); ++i) {
    out.charges[i].amount = part(remaining.charges[i].amount);
    remaining.charges[i].amount = *remaining.charges[i].amount.subtract(out.charges[i].amount);
  }
  return out;
}
inline std::expected<Money, ValueError> net_pnl(Money gross, const LotAmounts &opening,
                                                const LotAmounts &closing) {
  for (const auto *amounts : {&opening, &closing})
    for (const auto &c : amounts->charges) {
      auto next = gross.subtract(c.amount);
      if (!next)
        return std::unexpected(next.error());
      gross = *next;
    }
  return gross;
}
} // namespace lot_detail

// Input is the shared lifecycle-resolved snapshot, ordered by effective time
// then acceptance sequence. Each complete pool can execute independently. Within
// a pool this is an ordered stateful fold, not a commutative reduction. Late
// corrections/cancellations require replay; numeric negation is not lot undo.
[[nodiscard]] inline std::expected<LotReport, LotError>
project_lifo_lots(std::span<const ResolvedLifecycleEvent> active,
                  std::span<const LotAccount> accounts) {
  std::map<std::string, LotAccount> bindings;
  std::map<LotPool, std::vector<OpenLot>> pools;
  std::map<LotPool, std::string> policies;
  for (const auto &account : accounts) {
    if (account.ledger_account.empty() || account.book.empty() || account.legal_entity.empty() ||
        account.custodian_account.empty() || account.policy_version.empty() ||
        !bindings.emplace(account.ledger_account, account).second)
      return std::unexpected(LotError{"INVALID_LOT_ACCOUNT", ""});
  }
  LotReport report;
  std::optional<std::pair<Timestamp, std::uint64_t>> previous;
  std::set<std::string> record_ids;
  for (const auto &resolved : active) {
    const auto *trade = std::get_if<EquityTrade>(&resolved.event());
    if (!trade)
      continue;
    const auto id = trade->header().id().value();
    auto order =
        std::pair{trade->header().effective_at(), resolved.record().acceptance_sequence().value()};
    if ((previous && order <= *previous) || !record_ids.insert(id).second)
      return std::unexpected(LotError{"INVALID_LOT_ORDER", id});
    previous = order;
    const auto fail = [&](std::string code) {
      return std::unexpected(LotError{std::move(code), id});
    };
    if (resolved.record().action() == LifecycleAction::reverse)
      return fail("LOT_REVERSAL_REQUIRES_CANCEL_OR_CORRECTION");
    auto account = bindings.find(trade->header().account().value());
    if (account == bindings.end())
      return fail("LOT_ACCOUNT_REQUIRED");
    const auto &a = account->second;
    LotPool pool{a.book, a.legal_entity, a.custodian_account, trade->instrument().value(),
                 std::string(trade->quote_currency().code())};
    auto [policy, inserted] = policies.emplace(pool, a.policy_version);
    if (!inserted && policy->second != a.policy_version)
      return fail("CONFLICTING_POOL_POLICY");
    auto quantity = trade->quantity().scaled_value();
    if (quantity == std::numeric_limits<std::int64_t>::min() || trade->price().scaled_value() <= 0)
      return fail("INVALID_LOT_TRADE");
    const bool sell = quantity < 0;
    if (sell)
      quantity = -quantity;
    auto principal =
        value(Quantity::from_scaled(quantity), trade->price(), trade->quote_currency());
    auto cash = trade_cash(*trade);
    if (!principal || !cash)
      return fail("LOT_AMOUNT_OVERFLOW");
    LotAmounts remaining{*principal, trade->charges().value_or(TradeCharges{})};
    auto commissions = charge_total(remaining.charges, trade->quote_currency(), true);
    auto fees = charge_total(remaining.charges, trade->quote_currency(), false);
    if (!commissions || !fees)
      return fail("LOT_AMOUNT_OVERFLOW");
    bool known = trade->charges().has_value();
    for (const auto &c : remaining.charges)
      known = known && c.state == "reported";
    report.charges_complete = report.charges_complete && known;
    report.trades.push_back({pool, id, a.policy_version, trade->quantity(), remaining, *commissions,
                             *fees, *cash, known});
    auto &stack = pools[pool];
    while (quantity > 0 && !stack.empty() && stack.back().short_position != sell) {
      auto &lot = stack.back();
      const auto take = std::min(quantity, lot.remaining_quantity.scaled_value());
      auto opening = lot_detail::split(lot.remaining, take, lot.remaining_quantity.scaled_value());
      auto closing = lot_detail::split(remaining, take, quantity);
      if (!opening || !closing)
        return fail("LOT_ALLOCATION_ERROR");
      auto gross = lot.short_position ? opening->principal.subtract(closing->principal)
                                      : closing->principal.subtract(opening->principal);
      if (!gross)
        return fail("LOT_AMOUNT_OVERFLOW");
      auto net = lot_detail::net_pnl(*gross, *opening, *closing);
      if (!net)
        return fail("LOT_AMOUNT_OVERFLOW");
      report.reliefs.push_back({pool, lot.opening_record, id, a.policy_version, lot.short_position,
                                lot.charges_known && known, Quantity::from_scaled(take), *opening,
                                *closing, *gross, *net});
      lot.remaining_quantity = Quantity::from_scaled(lot.remaining_quantity.scaled_value() - take);
      quantity -= take;
      if (lot.remaining_quantity.scaled_value() == 0)
        stack.pop_back();
    }
    if (quantity > 0)
      stack.push_back({pool, id, a.policy_version, trade->header().effective_at(),
                       resolved.record().acceptance_sequence().value(), sell, known,
                       Quantity::from_scaled(quantity), std::move(remaining)});
  }
  for (auto &[pool, lots] : pools)
    for (auto &lot : lots)
      report.open_lots.push_back(std::move(lot));
  return report;
}

inline std::expected<LotReport, LotError> project_lifo_lots(const LifecycleResolution &resolution,
                                                            std::span<const LotAccount> accounts) {
  return project_lifo_lots(resolution.active_events(), accounts);
}

// Shared application boundary: policies are immutable resolved values, not store
// handles. Retain full evidence even when a selected account has no trades.
inline std::expected<LotReport, LotError> project_policy_lots(
    const LifecycleResolution& resolution, std::span<const ResolvedPolicy> policies) {
  std::vector<LotAccount> accounts;
  for(const auto& policy:policies) {
    auto checked=resolve_policy(policy.target,policy.revisions,policy.effective_at,policy.through_sequence);
    if(!checked || *checked!=policy)return std::unexpected(LotError{"INVALID_RESOLVED_POLICY",""});
    const auto& t=policy.target;
    // Compatibility concerns the lot algorithm, not recognition settings or
    // account-specific evidence. Multiple ledger accounts can share one pool.
    accounts.push_back({t.account,t.book,t.legal_entity,t.custodian_account,"economic-lifo/1"});
  }
  auto report=project_lifo_lots(resolution,accounts);
  if(!report)return report;
  report->resolved_policies.assign(policies.begin(),policies.end());
  return report;
}

// Entity view preserves the underlying custodian lots. Long and short quantities
// are kept separately; there is no cross-custodian relief or tax consolidation.
struct FirmLotBalance {
  std::string book, legal_entity, instrument, currency;
  Quantity long_quantity, short_quantity;
  Money long_principal, short_principal, commissions, fees;
};
inline std::expected<std::vector<FirmLotBalance>, LotError>
firm_lot_balances(const LotReport &report) {
  using Key = std::tuple<std::string, std::string, std::string, std::string>;
  std::map<Key, FirmLotBalance> rows;
  for (const auto &lot : report.open_lots) {
    const auto &p = lot.pool;
    const auto zero = Money::from_scaled(0, lot.remaining.principal.currency());
    auto [it, inserted] = rows.try_emplace(
        Key{p.book, p.legal_entity, p.instrument, p.currency},
        FirmLotBalance{p.book, p.legal_entity, p.instrument, p.currency, Quantity::from_scaled(0),
                       Quantity::from_scaled(0), zero, zero, zero, zero});
    auto &row = it->second;
    auto &quantity = lot.short_position ? row.short_quantity : row.long_quantity;
    auto &principal = lot.short_position ? row.short_principal : row.long_principal;
    auto q = quantity.add(lot.remaining_quantity);
    auto v = principal.add(lot.remaining.principal);
    if (!q || !v)
      return std::unexpected(LotError{"FIRM_LOT_OVERFLOW", lot.opening_record});
    quantity = *q;
    principal = *v;
    for (const auto &charge : lot.remaining.charges) {
      auto &total = charge.category == "commission" ? row.commissions : row.fees;
      auto sum = total.add(charge.amount);
      if (!sum)
        return std::unexpected(LotError{"FIRM_LOT_OVERFLOW", lot.opening_record});
      total = *sum;
    }
  }
  std::vector<FirmLotBalance> out;
  for (const auto &[key, row] : rows)
    out.push_back(row);
  return out;
}
} // namespace luca::accounting
