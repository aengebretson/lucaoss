// Same public strict-accept workload compiles against the base and candidate.
// Fixture construction/full-output hash are excluded from append timing.
#include "luca/lifecycle.hpp"
#include "luca/serialization/canonical.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>

using namespace luca;
void check(bool condition) { if(!condition) std::abort(); }
LifecycleRecordDraft draft(std::size_t index) {
  const auto id=std::to_string(index);
  const auto provenance=Provenance::create({SourceRecordId{"source-"+id}},"append-benchmark","1");
  const auto header=EventHeader::create(EventId{"record-"+id},AccountId{"account"},Timestamp{},*provenance);
  const auto currency=Currency::from_code("USD");
  return LifecycleRecordDraft::originate(EconomicEventId{"economic-"+id},Timestamp{},
      CashMovement::create(*header,Money::from_scaled(1000000,*currency)));
}
int main() {
  bool first=true;
  std::cout<<"{\"schema\":\"luca.strict-append-benchmark/1\",\"samples\":[";
  for(const std::size_t prefix:{1000,10000,100000}) {
    std::vector<LifecycleRecordDraft> initial, suffix;
    for(std::size_t i=0;i<prefix;++i) initial.push_back(draft(i));
    for(std::size_t i=prefix;i<prefix+100;++i) suffix.push_back(draft(i));
    for(int repetition=0;repetition<3;++repetition) {
      LifecycleLedger ledger; check(ledger.accept_batch(initial).has_value());
      const auto start=std::chrono::steady_clock::now();
      for(const auto& row:suffix) check(ledger.accept(row).has_value());
      const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
      check(ledger.size()==prefix+100);
      if(!first) std::cout<<','; first=false;
      std::cout<<"{\"prefix_records\":"<<prefix<<",\"appended_records\":100,\"repetition\":"<<repetition
               <<",\"append_seconds\":"<<seconds<<",\"canonical_sha256\":\""<<serialization::canonical_digest(ledger)<<"\"}";
    }
  }
  std::cout<<"]}\n";
}
