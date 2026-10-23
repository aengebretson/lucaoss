#pragma once
#include "luca/accounting/derivatives/projection.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace derivative_fixture {
using namespace luca;
using namespace luca::accounting;
using namespace luca::accounting::derivatives;
inline void check(bool value,std::source_location at=std::source_location::current()) {
  if(!value){std::cerr<<"check failed "<<at.file_name()<<':'<<at.line()<<'\n';std::abort();}
}
template<class T> auto take(T result,std::source_location at=std::source_location::current()) {
  if(!result){if constexpr(requires{result.error().code;})std::cerr<<result.error().code<<'\n';check(false,at);}
  return *result;
}
inline auto usd=take(Currency::from_code("USD"));
inline Money money(std::string_view s){return take(Money::parse(s,usd));}
inline Price price(std::string_view s){return take(Price::parse(s));}
inline Quantity quantity(std::string_view s){return take(Quantity::parse(s));}
inline Timestamp day(int n){return Timestamp{std::chrono::sys_days{std::chrono::year{2026}/1/1}}+std::chrono::days{n};}
inline Provenance proof(std::string id){return take(Provenance::create({SourceRecordId{"source:"+id}},"synthetic-hand-calculation","1"));}
inline Contract contract(Profile profile=Profile::spxw_pm_european_cash,std::string instrument="synthetic-option") {
  bool opt=profile!=Profile::es_linear_cash;
  return take(Contract::create({InstrumentId{instrument},InstrumentId{"SP500"},"definition-1",profile,
      opt?Family::index_option:Family::index_future,QuantityUnit::contracts,QuoteUnit::index_points,
      {opt?100:50,1},Factor{opt?100:50,1},usd,
      opt?std::vector<TickBand>{{price("0"),price("0.05")},{price("3"),price("0.1")}}:
          std::vector<TickBand>{{price("0"),price("0.25")}},
      opt?std::optional{price("6000")}:std::nullopt,opt?std::optional{OptionRight::call}:std::nullopt,
      opt?ExerciseStyle::european:ExerciseStyle::none,SettlementMethod::cash,
      opt?"spx-european-cash/1":"es-linear-settlement/1",
      {day(0),day(9),day(10),day(10),day(10),day(11)},day(-2),day(-2),proof("definition-1")}));
}
inline Inputs book(Profile profile=Profile::spxw_pm_european_cash,std::string account="a") {
  PolicyTarget target{"book-"+account,"entity","custody",account};
  std::vector<PolicyRevision> revisions{{"p","entity","entity","operator",PolicyScope::legal_entity,
      day(-10),1,LotSelection::lifo,RecognitionBasis::accrual}};
  auto policy=take(resolve_policy(target,revisions,day(-1),1));
  return {contract(profile),policy,day(-10),{"market-first","1",{MarkType::market,MarkType::official_settlement},std::chrono::days{30}},
      {ChargeTreatment::capitalize_opening,false,
       "derivative-rules/1"},{},{},{}};
}
inline void add(Inputs& in,std::string id,Payload payload,int effective,int known=-1,Action action=Action::originate,
    std::string economic="",std::optional<std::string> predecessor={},std::optional<std::string> replaces={}) {
  in.events.push_back({id,economic.empty()?id:economic,in.contract.digest(),in.events.size()+1,day(effective),
      day(known<0?effective:known),action,predecessor,replaces,payload,proof(id)});
}
inline void trade(Inputs& in,std::string id,std::string q,std::string p,int at,std::string charge="0") {
  add(in,id,Trade{quantity(q),price(p),TradeCharges{{"charge","commission","broker","","reported",money(charge)}}},at);
}
inline void observation(Inputs& in,std::string id,std::string p,int effective,MarkType type=MarkType::market,int known=-1) {
  in.observations.push_back({id,"1",in.contract.digest(),in.contract.terms().instrument,type,price(p),usd,
      day(effective),day(known<0?effective:known),proof(id)});
}
inline void pay(Inputs& in,std::string id,std::string economic,std::string amount,int at,CashKind kind=CashKind::obligation,
    std::optional<std::string> refunds={},int known=-1) {
  in.payments.push_back({id,economic,in.payments.size()+1,kind,money(amount),day(at),day(known<0?at:known),refunds,proof(id)});
}
inline Report report(const Inputs& in,int from=0,int until=8,int known=30,std::optional<ClosedAnchor> closed={}) {
  return take(project(in,{day(from),day(until),day(known),"UTC"},closed));
}
}
