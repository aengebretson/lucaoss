#pragma once
#include "luca/event.hpp"
#include <span>
namespace luca::accounting {
struct TransactionAmounts { Money principal, commissions, fees, total; };
inline std::expected<TransactionAmounts, ValueError> transaction_amounts(const EquityTrade& t) {
  if(t.quantity().scaled_value()==INT64_MIN)return std::unexpected(ValueError::overflow);
  auto principal=value(Quantity::from_scaled(t.quantity().scaled_value()<0?-t.quantity().scaled_value():t.quantity().scaled_value()),t.price(),t.quote_currency());
  auto commissions=charge_total(t.charges().value_or(TradeCharges{}),t.quote_currency(),true);
  auto fees=charge_total(t.charges().value_or(TradeCharges{}),t.quote_currency(),false);
  auto total=trade_cash(t);
  if(!principal)return std::unexpected(principal.error());
  if(!commissions)return std::unexpected(commissions.error());
  if(!fees)return std::unexpected(fees.error());
  if(!total)return std::unexpected(total.error());
  return TransactionAmounts{*principal,*commissions,*fees,*total};
}
inline std::expected<TransactionAmounts, ValueError> sum_transaction_amounts(
    Currency currency, std::span<const TransactionAmounts> values) {
  auto zero=Money::from_scaled(0,currency); TransactionAmounts out{zero,zero,zero,zero};
  for(const auto& v:values) {
    auto p=out.principal.add(v.principal), c=out.commissions.add(v.commissions),
         f=out.fees.add(v.fees), t=out.total.add(v.total);
    if(!p)return std::unexpected(p.error());if(!c)return std::unexpected(c.error());
    if(!f)return std::unexpected(f.error());if(!t)return std::unexpected(t.error());
    out={*p,*c,*f,*t};
  } return out;
}
}
