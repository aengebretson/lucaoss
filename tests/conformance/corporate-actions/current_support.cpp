// Current adapter behavior only; future fixture arithmetic is checked separately.
#include "luca/adapters/transaction.hpp"
#include "luca/serialization/canonical_decode.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
using namespace luca;
void require(bool value) { if (!value) std::abort(); }
int main() {
  adapters::TransactionFields f;
  f.record_id="synthetic:record"; f.economic_id="synthetic:economic";
  f.action="originate"; f.account="synthetic:account";
  f.evidence_id="synthetic:source"; f.recorded_ns="1792497600000000000";
  f.effective_at="2026-10-20T12:00:00Z"; f.currency="USD";
  f.normalization="fixture:current-support/1";
  f.type="equity_trade"; f.instrument="synthetic:security";
  f.quantity="125"; f.price="8"; f.settlement="2026-10-21";
  auto trade=adapters::transaction(f); require(trade.has_value());
  LifecycleLedger ledger; require(ledger.accept(*trade).has_value());
  const auto prefix=serialization::canonical_bytes(ledger);
  const auto &event=std::get<EquityTrade>(*ledger.records()[0].event());
  const auto usd=Currency::from_code("USD"); require(usd.has_value());
  const auto principal=Money::parse("-1000",*usd); require(principal.has_value());
  const auto cash=trade_cash(event); require(cash.has_value() && *cash==*principal);
  f.record_id="synthetic:unsupported"; f.economic_id="synthetic:unsupported-economic";
  for(const auto *kind:std::array{"cash_dividend","stock_dividend","forward_split",
      "reverse_split","identity_change","spinoff","stock_merger","cash_merger",
      "mixed_merger","full_call","final_liquidation","partial_liquidation"}) {
    f.type=kind;
    const auto rejected=adapters::transaction(f);
    require(!rejected && rejected.error().code=="UNSUPPORTED_EVENT_TYPE");
    require(rejected.error().field=="/event/type");
    require(serialization::canonical_bytes(ledger)==prefix);
  }
  f.type="cash_movement";f.amount="40";
  auto cash_movement=adapters::transaction(f);require(cash_movement.has_value());
  // A generic cash movement can be admitted, but has no dividend entitlement,
  // eligibility or income classification. It is not proof of dividend support.
  require(ledger.accept(*cash_movement).has_value());
  require(ledger.size()==2);
  std::cout<<"Supported trade/cash adapters verified; 12 corporate-action kinds rejected\n";
}
