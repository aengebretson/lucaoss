#include "fixture.hpp"
using namespace derivative_fixture;
int main() {
  auto in=book();
  trade(in,"buy-two","2","10",1);trade(in,"sell-two","-2","12.5",2);
  pay(in,"bank-buy","buy-two","-2000",3);pay(in,"bank-sell","sell-two","2500",4);
  const auto r=report(in);
  check(r.closing.realized==money("500"));check(r.closing.cash==money("500"));
  std::cout<<"profile=SPXW realized_scaled="<<r.closing.realized.scaled_value()
           <<" cash_scaled="<<r.closing.cash.scaled_value()<<" report="<<r.report_id<<'\n';
  auto es=book(Profile::es_linear_cash);
  trade(es,"buy","2","6000",1);observation(es,"settle","6001",2,MarkType::official_settlement);
  add(es,"daily",DailySettlement{"settle","1",false,day(2)},2);
  observation(es,"mark","6001.25",3);pay(es,"vm","daily","100",3);
  const auto f=report(es,0,4);
  check(f.closing.realized==money("100"));check(f.closing.unrealized==money("25"));
  std::cout<<"profile=ES realized_scaled="<<f.closing.realized.scaled_value()
           <<" remaining_scaled="<<f.closing.unrealized->scaled_value()
           <<" actual_cash_scaled="<<f.closing.cash.scaled_value()<<'\n';
}
