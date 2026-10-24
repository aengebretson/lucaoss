#include "../../examples/derivatives/fixture.hpp"
#include <algorithm>
#include <chrono>
using namespace derivative_fixture;
int main() {
  for(int count:{250,1000,2000}) {
    auto in=book(Profile::es_linear_cash);
    for(int i=0;i<count;++i)trade(in,"buy"+std::to_string(i),"1","6000",1);
    observation(in,"opening","6000",1);
    for(int d=2;d<=4;++d) {
      auto id="official"+std::to_string(d);
      observation(in,id,std::to_string(5999+d),d,MarkType::official_settlement);
      add(in,"daily"+std::to_string(d),DailySettlement{id,"1",false,day(d)},d);
    }
    trade(in,"close","-"+std::to_string(count),"6004",5);
    std::vector<std::int64_t> samples;
    for(int repeat=0;repeat<7;++repeat) {
      auto begin=std::chrono::steady_clock::now();auto r=report(in,0,6);
      samples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count());
      check(r.closing.realized==Money::from_scaled(static_cast<std::int64_t>(count)*200000000,usd));
    }
    std::sort(samples.begin(),samples.end());
    std::cout<<"{\"open_lots\":"<<count<<",\"daily_resets\":3,\"runs\":7,\"median_us\":"<<samples[3]<<"}\n";
  }
}
