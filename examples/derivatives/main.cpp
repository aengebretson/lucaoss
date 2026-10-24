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
}
