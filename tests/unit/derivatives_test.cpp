#include "../../examples/derivatives/fixture.hpp"
#include <future>
#include <fstream>
#include <sstream>
using namespace derivative_fixture;

void economics() {
  const auto c=contract();auto terms=c.terms();
  terms.point_value.reset();check(!Contract::create(terms));
  terms=c.terms();terms.point_value=Factor{1,1};check(!Contract::create(terms));
  terms=c.terms();terms.point_value=Factor{200,2};check(take(Contract::create(terms))==c);
  terms=c.terms();terms.revision="definition-2";check(take(Contract::create(terms)).digest()!=c.digest());
  terms=c.terms();terms.exercise_style=ExerciseStyle::none;check(!Contract::create(terms));
  terms=c.terms();terms.boundaries.payment=day(1);check(!Contract::create(terms));
  check(take(quoted_money(c,quantity("2"),price("10")))==money("2000"));
  check(take(price_change(c,quantity("2"),price("10"),price("12.5")))==money("500"));
  check(take(price_change(c,quantity("-2"),price("10"),price("12.5")))==money("-500"));
  check(take(notional_exposure(c,quantity("2"),price("6000")))==money("1200000"));
  check(take(quoted_money(quantity("3"),price("1"),{1,3},usd))==money("1"));
  check(take(quoted_money(quantity("1"),price("0.00000001"),{50,1},usd))==money("0"));
  check(take(quoted_money(quantity("1"),price("0.00000003"),{50,1},usd))==money("0.000002"));
  check(take(quoted_money(quantity("-1"),price("0.00000003"),{50,1},usd))==money("-0.000002"));
  check(!quoted_money(Quantity::from_scaled(INT64_MAX),Price::from_scaled(INT64_MAX),{INT64_MAX,1},usd));
  check(valid_trade_price(c,price("0")));check(!valid_trade_price(c,price("-0.05")));
  check(valid_trade_price(c,price("2.95")));check(!valid_trade_price(c,price("3.05")));
  auto in=book();observation(in,"m1","10",1);observation(in,"m2","12.5",2);
  check(take(select_mark(c,in.observations,in.marks,day(3),day(1)))->id=="m1");
  observation(in,"tie","13",2);check(!select_mark(c,in.observations,in.marks,day(3),day(3)));
  observation(in,"newer","14",3);check(take(select_mark(c,in.observations,in.marks,day(3),day(3)))->id=="newer");
  std::reverse(in.observations.begin(),in.observations.end());
  check(take(select_mark(c,in.observations,in.marks,day(3),day(3)))->id=="newer");
  in.observations.clear();in.marks.preference={MarkType::official_fixing};check(!project(in,{day(0),day(8),day(30),"UTC"}));
}
void option_pnl() {
  auto in=book();trade(in,"buy","2","10",1);trade(in,"sell","-2","12.5",2);
  auto r=report(in);check(r.realized_activity==money("500"));check(r.total_pnl_activity==money("500"));
  check(r.closing.position==quantity("0"));check(r.closing.cash==money("0"));
  check(r.closing.obligations.size()==2);check(r.closing.obligations[0].outstanding==money("-2000"));
  check(!r.supported_complete);check(r==report(in)); // unmarked interior exposure retained
  pay(in,"bank-buy","buy","-2000",3);pay(in,"bank-sell","sell","2500",4);
  r=report(in);check(r.closing.cash==money("500"));check(r.closing.realized==money("500"));
  check(r.closing.obligations[0].outstanding==money("0"));
  check(r.closing_trial_balance.controls()[0].debit_balances==money("500"));
  auto short_book=book();trade(short_book,"short","-2","12.5",1);trade(short_book,"cover","2","10",2);
  check(report(short_book).realized_activity==money("500"));
  auto partial=book();trade(partial,"buy","2","10",1,"2");trade(partial,"sell","-1","12.5",2,"1");
  observation(partial,"mark","12.5",2);r=report(partial);
  check(r.closing.realized==money("248"));check(r.closing.unrealized==money("249"));
  check(r.total_pnl_activity==money("497"));check(r.closing.cost_carrying==money("1001"));
  check(r.closing.value==money("1250"));check(!r.closing.notional);
  trade(partial,"sell2","-1","12.5",3,"1");r=report(partial);check(r.closing.realized==money("496"));
  // Charges allocation conserves each micro-unit; last close exhausts remainder.
  auto fraction=book();trade(fraction,"buy","3","10",1,"0.000001");trade(fraction,"c1","-1","10",2);
  trade(fraction,"c2","-1","10",3);trade(fraction,"c3","-1","10",4);
  check(report(fraction).closing.realized==money("-0.000001"));
  auto lifo=book();trade(lifo,"a","1","10",1);trade(lifo,"b","1","11",2);trade(lifo,"c","-1","12.5",3);
  observation(lifo,"mark","12.5",3);r=report(lifo);check(r.closing.reliefs[0].opening_record=="b");
  check(r.closing.realized==money("150"));check(r.closing.unrealized==money("250"));
  auto unknown=book();add(unknown,"unknown",Trade{quantity("1"),price("10"),{}},1);
  r=report(unknown);check(!r.supported_complete);check(!r.closing.value);
  auto zero=book();trade(zero,"zero","1","0",1);trade(zero,"close","-1","0",2);check(report(zero).closing.realized==money("0"));
}
void option_lifecycle() {
  auto in=book();trade(in,"buy","2","10",1);observation(in,"official","6000",10,MarkType::official_fixing);
  add(in,"expire",OptionTerminal{quantity("2"),"official","1"},10);
  auto r=report(in,0,12);check(r.closing.realized==money("-2000"));check(r.closing.position==quantity("0"));
  check(r.closing.obligations.back().contractual==money("0"));
  auto exercise=book();trade(exercise,"buy","2","10",1);observation(exercise,"fix","6020",10,MarkType::official_fixing);
  add(exercise,"ex1",OptionTerminal{quantity("1"),"fix","1"},10);
  add(exercise,"ex2",OptionTerminal{quantity("1"),"fix","1"},10);
  r=report(exercise,0,12);check(r.closing.realized==money("2000"));
  pay(exercise,"premium","buy","-2000",2);pay(exercise,"exercise1","ex1","2000",11);
  pay(exercise,"exercise2","ex2","2000",11);r=report(exercise,0,12);check(r.closing.cash==money("2000"));
  auto excessive=exercise;add(excessive,"ex3",OptionTerminal{quantity("1"),"fix","1"},10,11);
  check(!project(excessive,{day(0),day(12),day(30),"UTC"}));
  auto missing=exercise;missing.observations.clear();auto error=project(missing,{day(0),day(12),day(30),"UTC"});
  check(!error && error.error().code=="OFFICIAL_FIXING_REQUIRED");
  auto indicative=exercise;indicative.observations[0].type=MarkType::model;check(!project(indicative,{day(0),day(12),day(30),"UTC"}));
  auto short_option=book();trade(short_option,"s","-1","10",1);observation(short_option,"f","6020",10,MarkType::official_fixing);
  add(short_option,"assignment",OptionTerminal{quantity("-1"),"f","1"},10);
  r=report(short_option,0,12);check(r.closing.realized==money("-1000"));check(r.closing.obligations.back().contractual==money("-2000"));
  auto put=book();auto terms=put.contract.terms();terms.right=OptionRight::put;put.contract=take(Contract::create(terms));
  trade(put,"buy","1","10",1);observation(put,"f","5980",10,MarkType::official_fixing);
  add(put,"ex",OptionTerminal{quantity("1"),"f","1"},10);check(report(put,0,12).closing.realized==money("1000"));
  auto open=book();trade(open,"buy","1","10",1);observation(open,"m","0",10);
  r=report(open,0,12);check(!r.supported_complete);check(r.closing.position==quantity("1"));
  check(r.closing.value==money("0"));check(r.closing.unrealized==money("-1000"));
}
void history() {
  auto dependent=book();trade(dependent,"older","1","10",1);trade(dependent,"newer","1","11",2);
  trade(dependent,"close","-1","12.5",3);
  add(dependent,"edit-newer",Trade{quantity("1"),price("9"),TradeCharges{}},2,5,Action::correct,"newer","newer");
  observation(dependent,"mark","12.5",6);
  auto corrected=report(dependent);
  check(corrected.closing.realized==money("350"));check(corrected.closing.unrealized==money("250"));
  check(corrected.closing.reliefs[0].opening_record=="edit-newer");
  check(corrected.closing.lots[0].lot.opening_record=="older");

  auto in=book();trade(in,"buy","2","10",1);trade(in,"sell","-2","12.5",2);
  add(in,"edit",Trade{quantity("-2"),price("13"),TradeCharges{}},2,5,Action::correct,"sell","sell");
  check(report(in,0,4).closing.realized==money("500"));
  check(report(in,4,8).realized_activity==money("100"));
  check(report(in,0,8,3).closing.realized==money("500"));
  check(report(in,0,8).closing.realized==money("600"));
  pay(in,"premium","buy","-2000",3);pay(in,"sale","sell","2500",4);
  in.events.push_back({"void","sell",in.contract.digest(),4,day(6),day(6),Action::void_event,"edit",{},{},proof("void")});
  observation(in,"m","10",6);auto r=report(in);check(r.closing.cash==money("500"));
  check(r.closing.position==quantity("2"));check(!r.supported_complete);
  pay(in,"refund","sell","-2500",7,CashKind::obligation,"sale");r=report(in);check(r.closing.cash==money("-2000"));
  add(in,"replacement",Trade{quantity("-2"),price("13"),TradeCharges{}},2,7,Action::originate,"new-sale",{},"void");
  check(report(in,0,7).closing.position==quantity("2"));check(report(in,0,8).closing.position==quantity("0"));
  auto deferred=book();trade(deferred,"buy","1","10",1);
  add(deferred,"edit",Trade{quantity("1"),price("11"),TradeCharges{}},1,5,Action::correct,"buy","buy");
  deferred.events.push_back({"void","buy",deferred.contract.digest(),3,day(3),day(6),Action::void_event,"edit",{},{},proof("void")});
  check(report(deferred,0,4).closing.position==quantity("0"));check(report(deferred,0,8).closing.position==quantity("0"));
  auto closed_book=book();trade(closed_book,"buy","2","10",1);trade(closed_book,"sell","-2","12.5",2);
  const auto close=retain_close(report(closed_book,0,4,4));
  add(closed_book,"edit",Trade{quantity("-2"),price("13"),TradeCharges{}},2,5,Action::correct,"sell","sell");
  check(report(closed_book,4,8,30,close).realized_activity==money("100"));
  auto moved=closed_book;moved.events.back().effective_at=day(3);
  check(report(moved,4,8,30,close).realized_activity==money("100"));
  auto changed=closed_book;changed.events[0].provenance=proof("changed");
  check(!project(changed,{day(4),day(8),day(30),"UTC"},close));
  auto markbook=book();trade(markbook,"buy","1","10",1);observation(markbook,"m","11",2);
  const auto markclose=retain_close(report(markbook,0,4,4));observation(markbook,"late","11",3,MarkType::market,5);
  auto e=project(markbook,{day(4),day(8),day(30),"UTC"},markclose);
  check(!e && e.error().code=="CLOSED_HISTORY_EXCEPTION" && !e.error().expected.empty() && !e.error().actual.empty()); // even same money, changed mark evidence
  check(!project(markbook,{day(0),day(8),day(30),"UTC"},markclose));
  auto mismatch=book();trade(mismatch,"b","1","10",1);mismatch.events[0].contract_digest="different";
  check(!project(mismatch,{day(0),day(8),day(30),"UTC"}));
  auto future=book();trade(future,"buy","1","10",1);
  add(future,"later",Trade{quantity("1"),price("11"),TradeCharges{}},7,5,Action::correct,"buy","buy");
  check(report(future,0,4).closing.position==quantity("1"));check(report(future,0,6).closing.position==quantity("0"));
  check(report(future,0,8).closing.position==quantity("1"));
}
void period_and_replay() {
  auto in=book();trade(in,"buy","2","10",1);observation(in,"m1","11",2);
  observation(in,"m2","12.5",4);trade(in,"sell","-2","12.5",5);pay(in,"purchase","buy","-2000",3);
  pay(in,"sale","sell","2500",6);
  auto all=report(in,0,8),a=report(in,0,4),b=report(in,4,8);
  check(take(a.total_pnl_activity->add(*b.total_pnl_activity))==*all.total_pnl_activity);
  check(a.effects.size()+b.effects.size()==all.effects.size());
  std::vector<std::string> ids;for(const auto& e:all.effects)ids.push_back(e.id);
  std::vector<std::string> parts;for(const auto& e:a.effects)parts.push_back(e.id);for(const auto& e:b.effects)parts.push_back(e.id);
  check(ids==parts);check(all==report(all.inputs,0,8));
  auto future_inputs=in;trade(future_inputs,"future","1","10",9);
  auto extended=report(future_inputs,0,8);check(all.effects==extended.effects);
  check(all.report_id!=extended.report_id); // manifests pin all input prefixes
  const auto& j=all.closing_journals.front();
  auto construct=[&](RecognitionPhase phase,JournalSettlementContext settlement) {
    return JournalEntry::create(j.journal_entry_id(),j.policy(),JournalEventType::derivative_projection,
        j.active_record_id(),j.effective_at(),j.recorded_at(),j.recognized_on(),phase,0,
        settlement,j.lineage(),std::vector<JournalLine>{j.lines().begin(),j.lines().end()});
  };
  for(auto phase:{RecognitionPhase::immediate,RecognitionPhase::trade_date,
      RecognitionPhase::settlement_date,RecognitionPhase::actual_settlement})
    check(!construct(phase,j.settlement_context()));
  auto fake_dates=take(JournalSettlementContext::create(j.recognized_on(),j.recognized_on(),"invalid"));
  check(!construct(RecognitionPhase::period_adjustment,fake_dates));
  std::map<std::string,Money> totals=a.opening.balances;
  for(const auto& e:all.effects)for(const auto& l:e.journal.lines()) {
    auto [it,inserted]=totals.emplace(l.account_id().value(),money("0"));(void)inserted;
    it->second=take(l.side()==JournalSide::debit?it->second.add(l.amount()):it->second.subtract(l.amount()));
  }
  for(const auto& [account,m]:all.closing.balances)check(totals.at(account)==m);
  auto other=book(Profile::spxw_pm_european_cash,"b");trade(other,"s","-1","10",1);trade(other,"c","1","11",2);
  auto first=std::async(std::launch::async,[&]{return report(in);});
  auto second=std::async(std::launch::async,[&]{return report(other);});
  check(first.get()==report(in));check(second.get()==report(other));
  auto collateral=book();pay(collateral,"initial-margin","","-1000",0,CashKind::collateral);
  check(report(collateral).closing.collateral==money("1000"));
  auto gap=book();trade(gap,"buy","1","10",1);observation(gap,"first","10",1);observation(gap,"last","11",4);
  gap.marks.max_age=std::chrono::days{1};check(!report(gap,2,5).supported_complete);
  gap.rules.recognize_unrealized=true;check(!project(gap,{day(2),day(5),day(30),"UTC"}));
  auto valued=book();valued.rules.recognize_unrealized=true;trade(valued,"buy","1","10",1);
  observation(valued,"first","10",1);observation(valued,"last","11",2);
  check(report(valued).closing.recognized_unrealized==money("100"));
  auto late=valued;late.rules.recognize_unrealized=false;observation(late,"lastest","12",6);
  auto anchor=retain_close(report(late,0,8,8));observation(late,"interior-late","11.5",3,MarkType::market,9);
  auto late_result=project(late,{day(8),day(9),day(10),"UTC"},anchor);
  check(!late_result && late_result.error().code=="CLOSED_HISTORY_EXCEPTION");
  check(!project(in,{day(0),day(8),day(30),"America/Chicago"}));
  check(!project(in,{day(0),day(8),day(30),"UTC"},{},Limits{100,1,100}));
  auto cash=book();auto revisions=cash.policy.revisions;revisions[0].recognition=RecognitionBasis::cash;
  cash.policy=take(resolve_policy(cash.policy.target,revisions,cash.policy.effective_at,1));
  check(!project(cash,{day(0),day(8),day(30),"UTC"}));
}
void v1_compatibility() {
  auto in=book();
  in.rules={ChargeTreatment::capitalize_opening,false,"derivative-rules/1"}; // PR34 aggregate API
  trade(in,"buy-two","2","10",1);trade(in,"sell-two","-2","12.5",2);
  pay(in,"bank-buy","buy-two","-2000",3);pay(in,"bank-sell","sell-two","2500",4);
  auto r=report(in);
  // Captured from independently installed PR34 consumer at b96490f.
  check(r.report_id=="ccd9d438ee84fa3f56d4c5a5040f24636aa902d34253e29d1f6998a87e214a2c");
  std::string fixture=LUCA_DERIVATIVE_FIXTURE;
  std::ifstream frozen(fixture.substr(0,fixture.find_last_of('/'))+"/spx-v1-evidence.txt");check(bool(frozen));
  std::vector<std::string> actual{r.input_digest,r.history_digest,r.closing.digest};
  for(const auto& e:r.effects)actual.push_back(e.id);
  std::string expected;for(const auto& value:actual){check(bool(frozen>>expected));check(expected==value);}
  check(!(frozen>>expected));
  auto anchor=retain_close(r);check(report(in,8,9,30,anchor).closing.digest==r.closing.digest);
}
void futures() {
  auto in=book(Profile::es_linear_cash);const auto& c=in.contract;
  auto missing_policy=in;missing_policy.rules.futures.reset();
  auto policy_error=project(missing_policy,{day(0),day(8),day(30),"UTC"});
  check(!policy_error && policy_error.error().code=="EXPLICIT_ES_POLICY_REQUIRED");
  auto incompatible=book();incompatible.rules.futures=in.rules.futures;
  check(!project(incompatible,{day(0),day(8),day(30),"UTC"}));
  check(impl::journal_policy_digest(in)!=impl::journal_policy_digest(book()));
  check(take(quoted_money(c,quantity("1"),price("0.25")))==money("12.5"));
  check(take(price_change(c,quantity("2"),price("6000"),price("6001")))==money("100"));
  check(take(impl::futures_change(c,quantity("1"),price("0.0001"),price("0.0003")))==money("0.01"));
  trade(in,"buy","2","6000",1,"2");
  observation(in,"settle1","6001",2,MarkType::official_settlement);
  add(in,"reset1",DailySettlement{"settle1","1",false,day(2)},2);
  observation(in,"mark","6001.25",3);auto r=report(in,0,4);
  check(r.closing.realized==money("98"));check(r.closing.unrealized==money("25"));
  check(r.total_pnl_activity==money("123"));check(r.closing.cash==money("0"));
  check(r.closing.obligations[0].contractual==money("-2"));check(r.closing.obligations[1].contractual==money("100"));
  check(r.closing.notional==money("600125"));check(r.closing.value==money("25"));
  pay(in,"vm","reset1","100",3);pay(in,"fees","buy","-2",3);
  pay(in,"margin","","-1000",3,CashKind::collateral);r=report(in,0,4);
  check(r.closing.cash==money("-902"));check(r.closing.collateral==money("1000"));
  check(r.closing.realized==money("98"));check(r.closing.obligations[1].outstanding==money("0"));
  observation(in,"settle2","6002",4,MarkType::official_settlement);
  add(in,"reset2",DailySettlement{"settle2","1",false,day(4)},4);
  r=report(in,0,5);check(r.closing.realized==money("198"));
  // preference chooses market before settlement; age is explicit, so mark6001.25
  // remains selected and residual valuation is -75 after reset6002.
  check(r.closing.unrealized==money("-75"));check(r.total_pnl_activity==money("123"));
  in.marks.preference={MarkType::official_settlement,MarkType::market};r=report(in,0,5);
  check(r.closing.unrealized==money("0"));check(r.total_pnl_activity==money("198"));
  trade(in,"close","-1","6003",5,"1");r=report(in,0,6);
  check(r.closing.realized==money("247"));check(r.closing.position==quantity("1"));
  auto amended=book(Profile::es_linear_cash);trade(amended,"opening","2","6000",1);
  observation(amended,"official","6001",2,MarkType::official_settlement);
  add(amended,"daily",DailySettlement{"official","1",false,day(2)},2);
  observation(amended,"valuation","6001.25",3);pay(amended,"actual-vm","daily","100",3);
  add(amended,"correct-opening",Trade{quantity("2"),price("5999"),TradeCharges{}},1,5,
      Action::correct,"opening","opening");
  auto replayed=report(amended,0,6);check(replayed.closing.realized==money("200"));
  check(replayed.closing.unrealized==money("25"));check(replayed.closing.cash==money("100"));
  check(replayed.closing.obligations[1].outstanding==money("100"));
  check(report(amended,4,6).realized_activity==money("100"));
  auto short_book=book(Profile::es_linear_cash);trade(short_book,"short","-2","6000",1);
  trade(short_book,"cover","1","5999.75",2);observation(short_book,"m","5999.5",2);
  r=report(short_book);check(r.closing.realized==money("12.5"));check(r.closing.unrealized==money("25"));
  auto negative=book(Profile::es_linear_cash);trade(negative,"buy","1","-1",1);trade(negative,"sell","-1","0",2);
  check(report(negative).closing.realized==money("50"));
  auto final=book(Profile::es_linear_cash);trade(final,"buy","1","6000",1);
  observation(final,"fix","6010",10,MarkType::official_settlement);add(final,"final",DailySettlement{"fix","1",true,day(11)},10);
  r=report(final,0,12);check(r.closing.position==quantity("0"));check(r.closing.realized==money("500"));
  auto invalid=final;invalid.observations[0].type=MarkType::model;check(!project(invalid,{day(0),day(12),day(30),"UTC"}));
  auto missing=book(Profile::es_linear_cash);trade(missing,"buy","1","6000",1);
  add(missing,"reset",DailySettlement{"absent","1",false,day(2)},2);check(!project(missing,{day(0),day(8),day(30),"UTC"}));
}
void fixture_rows() {
  std::ifstream file(LUCA_DERIVATIVE_FIXTURE);check(bool(file));std::string line;int count=0;
  while(std::getline(file,line)) {
    if(line.empty() || line[0]=='#')continue;
    std::istringstream row(line);std::string profile,q,entry,exit,oc,cc,gross,net;
    check(bool(row>>profile>>q>>entry>>exit>>oc>>cc>>gross>>net));
    auto in=book(profile=="ES"?Profile::es_linear_cash:profile=="SPX"?Profile::spx_am_european_cash:Profile::spxw_pm_european_cash);
    trade(in,"open",q,entry,1,oc);auto signedq=quantity(q).scaled_value();
    const auto closeq=Quantity::from_scaled(-signedq);
    add(in,"close",Trade{closeq,price(exit),TradeCharges{{"charge","commission","broker","","reported",money(cc)}}},2);
    auto r=report(in);check(r.closing.realized==money(net));
    check(take(price_change(in.contract,quantity(q),price(entry),price(exit)))==money(gross));++count;
  }
  check(count==7);
}
int main(){fixture_rows();economics();option_pnl();option_lifecycle();history();period_and_replay();v1_compatibility();futures();std::cout<<"derivative conformance passed\n";}
