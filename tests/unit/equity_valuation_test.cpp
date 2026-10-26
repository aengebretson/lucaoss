#include "luca/accounting/equity_valuation.hpp"
#include <fstream>
#include <iostream>
#include <source_location>
#include <sstream>
using namespace luca;
using namespace luca::accounting;
namespace ev = luca::accounting::equity_valuation;
void check(bool ok, std::source_location at=std::source_location::current()) {
  if(!ok){std::cerr << at.line() << " failed\n"; std::abort();}
}
template<class T> auto take(T v, std::source_location at=std::source_location::current()) {
  if(!v) {
    if constexpr(requires {v.error().code;})std::cerr<<v.error().code<<"\n";
    check(false,at);
  }
  return *v;
}
const auto usd=take(Currency::from_code("USD"));
const auto eur=take(Currency::from_code("EUR"));
Money money(std::string_view s) {return take(Money::parse(s,usd));}
Quantity quantity(std::string_view s) {return take(Quantity::parse(s));}
Price price(std::string_view s) {return take(Price::parse(s));}
Timestamp at(int n) {return Timestamp{std::chrono::seconds{1700000000+n}};}
Provenance proof(std::string id) {return take(Provenance::create({SourceRecordId{id}},"fixture","1"));}
void add(LifecycleLedger& ledger,std::string id,std::string q,std::string p,int n,
         std::optional<TradeCharges> charges=TradeCharges{}) {
  auto h=take(EventHeader::create(EventId{id},AccountId{"account"},at(n),proof("source-"+id)));
  auto t=take(EquityTrade::create(h,InstrumentId{"AAPL"},quantity(q),price(p),usd,
      take(SettlementDate::create(std::chrono::year{2026}/10/27)),charges));
  take(ledger.accept(LifecycleRecordDraft::originate(EconomicEventId{id},at(100),EconomicEvent{t})));
}
ev::Inputs inputs(const LifecycleLedger& ledger) {
  const std::vector<LotAccount> accounts{{"account","book","entity","custodian","LIFO/1"}};
  auto lots=take(project_lifo_lots(ledger.resolve(at(100),at(100)),accounts));
  return {{{"book","entity","custodian","AAPL","USD"},lots,"retained-prefix-1",at(100),at(100)},
          {},{"demo","1",{ev::MarkType::market},ev::PriceBasis::midpoint,std::chrono::seconds{10}},
          {at(100),at(100)}};
}
ev::Inputs book() {LifecycleLedger ledger; add(ledger,"buy","100","200",1); return inputs(ledger);}
ev::Observation mark(std::string id,std::string p,int effective=100,int known=100) {
  return {id,"1",InstrumentId{"AAPL"},usd,price(p),ev::MarkType::market,ev::PriceBasis::midpoint,
          at(effective),at(known),proof("quote-"+id)};
}
void error(const ev::Inputs& in,std::string code) {
  auto r=ev::project(in); check(!r); if(r.error().code!=code)std::cerr<<r.error().code<<" != "<<code<<"\n";
  check(r.error().code==code);
}
void fixtures() {
  std::ifstream file(LUCA_EQUITY_VALUATION_FIXTURE); check(file.good());
  std::string line, current; LifecycleLedger ledger; int sequence=0, rows=0;
  while(std::getline(file,line)) {
    if(line.empty() || line[0]=='#')continue;
    std::istringstream row(line);
    std::string scenario,id,q,p,c,m,pos,cost,r,v,u,total;
    check(bool(row>>scenario>>id>>q>>p>>c>>m>>pos>>cost>>r>>v>>u>>total));
    if(scenario!=current){ledger=LifecycleLedger{}; sequence=0; current=scenario;}
    TradeCharges charges{{"commission","commission","broker","","reported",money(c)}};
    add(ledger,id,q,p,++sequence,charges);
    auto in=inputs(ledger); in.observations.push_back(mark("current",m));
    auto out=take(ev::project(in));
    check(out.position==quantity(pos)); check(out.cost_carrying==money(cost));
    check(out.economic_realized==money(r)); check(out.market_value==money(v));
    check(out.unrealized==money(u)); check(out.total_economic==money(total));
    check(out.charges_complete && out.supported_complete); check(out==take(ev::project(out.inputs)));
    check(out.input_digest.size()==64); check(out.report_id.starts_with("equity-valuation:")); ++rows;
  }
  check(rows==12);
}
void selection_and_knowledge() {
  auto in=book(); auto out=take(ev::project(in));
  check(!out.market_value && !out.unrealized && !out.total_economic && !out.supported_complete);
  check(out.diagnostics==std::vector<ev::Diagnostic>{{"MARK_REQUIRED","AAPL"}});
  in.observations={mark("age-boundary","202",90)};
  out=take(ev::project(in)); check(out.unrealized==money("200"));
  in.observations[0].effective_at-=std::chrono::nanoseconds{1};
  out=take(ev::project(in)); check(!out.unrealized && out.diagnostics[0].code=="STALE_MARK");
  in.observations={mark("effective-future","300",101),mark("known-future","400",100,101)};
  out=take(ev::project(in)); check(!out.selected_mark && !out.market_value);
  in.observations.push_back(mark("earlier-known","201",99,99));
  out=take(ev::project(in)); check(out.selected_mark->id=="earlier-known" && out.unrealized==money("100"));
  in.context.known_through=at(99); error(in,"LOT_SNAPSHOT_CONTEXT_MISMATCH");
  in=book(); auto model=mark("model","203"); model.type=ev::MarkType::model; model.basis=ev::PriceBasis::model;
  in.observations={model}; check(!take(ev::project(in)).market_value);
  in.marks.preference.push_back(ev::MarkType::model); check(take(ev::project(in)).unrealized==money("300"));
  in.observations.push_back(mark("market","202",99,99));
  check(take(ev::project(in)).selected_mark->id=="market");
  auto wrong_basis=mark("last","500"); wrong_basis.basis=ev::PriceBasis::last; in.observations.push_back(wrong_basis);
  check(take(ev::project(in)).selected_mark->id=="market");
  in.observations.push_back(mark("tied","204",99,99)); error(in,"AMBIGUOUS_MARK");
  in.observations.push_back(mark("newer","205")); check(take(ev::project(in)).selected_mark->id=="newer");
  std::reverse(in.observations.begin(),in.observations.end()); check(take(ev::project(in)).selected_mark->id=="newer");
}
void validation_and_identity() {
  auto in=book(); in.observations={mark("m","202")}; auto baseline=take(ev::project(in));
  auto changed=in; changed.snapshot.evidence_id="another-source-prefix";
  check(baseline.report_id!=take(ev::project(changed)).report_id);
  changed=in; changed.observations[0].provenance=proof("different-source");
  check(baseline.report_id!=take(ev::project(changed)).report_id);
  changed=in; changed.observations.push_back(mark("future","300",101,101));
  check(baseline.unrealized==take(ev::project(changed)).unrealized);
  check(baseline.report_id!=take(ev::project(changed)).report_id);
  changed=in; changed.marks.version="2"; check(baseline.report_id!=take(ev::project(changed)).report_id);
  changed=in; changed.snapshot.evidence_id=""; error(changed,"LOT_SNAPSHOT_IDENTITY_REQUIRED");
  changed=in; changed.snapshot.as_of=at(101); error(changed,"LOT_SNAPSHOT_CONTEXT_MISMATCH");
  changed=in; changed.observations[0].currency=eur; error(changed,"OBSERVATION_POOL_MISMATCH");
  changed=in; changed.observations[0].instrument=InstrumentId{"MSFT"}; error(changed,"OBSERVATION_POOL_MISMATCH");
  changed=in; changed.observations[0].price=price("-1"); error(changed,"NEGATIVE_EQUITY_MARK");
  changed=in; changed.observations.push_back(changed.observations[0]); error(changed,"INVALID_OBSERVATION_ID");
  changed=in; changed.marks.max_age=std::chrono::nanoseconds{-1}; error(changed,"MARK_POLICY_REQUIRED");
  changed=in; changed.marks.preference.push_back(ev::MarkType::market); error(changed,"INVALID_MARK_PREFERENCE");
  changed=in; changed.marks.market_basis=ev::PriceBasis::model; error(changed,"INVALID_MARK_BASIS");
  changed=in; changed.observations[0].type=ev::MarkType::model; error(changed,"INVALID_OBSERVATION_TYPE");
  changed=in; changed.snapshot.lots.open_lots[0].pool.instrument="other"; error(changed,"LOT_POOL_MISMATCH");
  changed=in; changed.snapshot.lots.open_lots[0].remaining.principal=Money::from_scaled(0,eur); error(changed,"INVALID_OPEN_LOT");
  changed=in; changed.snapshot.lots.open_lots[0].opened_at=at(101); error(changed,"INVALID_OPEN_LOT");
  changed=in; auto second=changed.snapshot.lots.open_lots[0]; second.opening_record="opposite";
  second.short_position=true; changed.snapshot.lots.open_lots.push_back(second); error(changed,"MIXED_LONG_SHORT_POOL");
}
void completeness_and_overflow() {
  LifecycleLedger ledger; add(ledger,"unknown","100","200",1,std::nullopt);
  auto in=inputs(ledger); in.observations={mark("m","202")}; auto out=take(ev::project(in));
  check(out.unrealized==money("200") && !out.charges_complete && !out.supported_complete);
  check(out.diagnostics==std::vector<ev::Diagnostic>{{"CHARGES_INCOMPLETE","AAPL"}});
  LifecycleLedger empty; in=inputs(empty); out=take(ev::project(in));
  check(out.market_value==money("0") && out.unrealized==money("0") && out.total_economic==money("0") && out.supported_complete);
  LifecycleLedger tiny; add(tiny,"tiny","0.00000001","0.00000001",1); in=inputs(tiny);
  auto half=mark("half","50"); in.observations={half}; check(take(ev::project(in)).market_value==money("0"));
  in.observations[0].price=price("150"); check(take(ev::project(in)).market_value==money("0.000002"));
  LifecycleLedger tiny_short; add(tiny_short,"tiny-short","-0.00000001","0.00000001",1);
  auto short_in=inputs(tiny_short); short_in.observations={mark("negative-half","150")};
  check(take(ev::project(short_in)).market_value==money("-0.000002"));
  auto ancient=book(); ancient.snapshot.as_of=ancient.context.as_of=Timestamp::max();
  ancient.observations={mark("ancient","202")}; ancient.observations[0].effective_at=Timestamp::min();
  ancient.marks.max_age=std::chrono::nanoseconds::max();
  check(take(ev::project(ancient)).diagnostics[0].code=="STALE_MARK");
  LifecycleLedger huge; add(huge,"huge","92233720368.54775807","0.00000001",1); in=inputs(huge);
  in.observations={mark("overflow","92233720368.54775807")}; error(in,"AMOUNT_OVERFLOW");
  in.observations.clear(); auto extra=in.snapshot.lots.open_lots[0]; extra.opening_record="extra";
  in.snapshot.lots.open_lots.push_back(extra); error(in,"QUANTITY_OVERFLOW");
  auto normal=book(); auto a=normal.snapshot.lots.open_lots[0]; a.remaining.principal=Money::from_scaled(INT64_MAX,usd);
  auto b=a; b.opening_record="more"; normal.snapshot.lots.open_lots={a,b}; error(normal,"AMOUNT_OVERFLOW");
}
int main(){fixtures(); selection_and_knowledge(); validation_and_identity(); completeness_and_overflow();}
