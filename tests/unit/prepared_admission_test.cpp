#include "luca/adapters/transaction.hpp"
#include "luca/serialization/canonical_decode.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <source_location>

using namespace luca;
void check(bool condition, std::source_location where=std::source_location::current()) {
  if (!condition) { std::fprintf(stderr,"failed %s:%u\n",where.file_name(),where.line()); std::abort(); }
}
adapters::TransactionFields fields(std::string id="a") {
  return {id,"econ-"+id,"originate","account","","source-"+id,"1","cash_movement",
          "2026-10-20T12:00:00.123456789Z","USD","100.000000","","","","","source-book-identity/1"};
}
LifecycleRecordDraft draft(std::string id="a") {
  auto result=adapters::transaction(fields(id)); check(result.has_value()); return *result;
}
int main() {
  LifecycleLedger ledger;
  std::array batch{draft()};
  auto preparation=ledger.prepare_batch(batch); check(preparation.has_value()); check(ledger.empty());
  check(preparation->records().size()==1);
  const auto bytes=serialization::canonical_bytes(preparation->records()[0]);
  auto decoded=serialization::decode_lifecycle_record(bytes); check(decoded.has_value());
  check(decoded->acceptance_sequence==1);
  LifecycleLedger reconstructed; check(reconstructed.accept(decoded->draft).has_value());
  check(serialization::canonical_bytes(reconstructed.records()[0])==bytes);
  auto trailing=bytes; trailing.push_back(std::byte{0});
  check(!serialization::decode_lifecycle_record(trailing));
  // An owned preparation survives its caller's draft storage and can publish
  // against an identical value copy, but never against an unrelated prefix.
  LifecycleLedger copy=ledger, unrelated;
  check(!unrelated.publish(*preparation)); check(unrelated.empty());
  check(copy.publish(*preparation).has_value()); check(copy.size()==1); check(ledger.empty());
  check(ledger.publish(*preparation).has_value()); check(ledger.size()==1);
  check(!ledger.publish(*preparation)); check(ledger.size()==1);
  std::array b{draft("b")}, c{draft("c")};
  auto stale=ledger.prepare_batch(b); check(stale.has_value());
  check(ledger.accept_batch(c).has_value()); check(!ledger.publish(std::move(*stale))); check(ledger.size()==2);
  const auto prefix=serialization::canonical_bytes(ledger);
  std::array bad{draft("new"),draft("a")};
  check(!ledger.prepare_batch(bad)); check(serialization::canonical_bytes(ledger)==prefix);
  check(ledger.accept_batch({}).has_value()); check(serialization::canonical_bytes(ledger)==prefix);
  // A long causal chain exercises the iterative suffix-only cycle check.
  std::vector<LifecycleRecordDraft> chain;
  for(int i=0;i<10000;++i) {
    auto f=fields("chain-"+std::to_string(i)); f.economic_id="chain-economic";
    if(i) { f.action="correct"; f.causal_id="chain-"+std::to_string(i-1); }
    auto value=adapters::transaction(f); check(value.has_value()); chain.push_back(*value);
  }
  LifecycleLedger long_ledger;
  check(long_ledger.accept_batch(chain).has_value()); check(long_ledger.size()==10000);
  for(const auto& [value,code]:std::array<std::pair<std::string,std::string>,3>{{{"1.0000001","EXCESS_PRECISION"},{"nan","INVALID_DECIMAL"},{"9223372036854775808","VALUE_OVERFLOW"}}}) {
    auto f=fields(); f.amount=value; auto result=adapters::transaction(f);
    check(!result); check(result.error().code==code);
  }
  for(const auto& value:{"2026-02-30T00:00:00Z","2026-10-20T25:00:00Z","2026-10-20T00:00:60Z","2026-10-20T00:00:00.1234567890Z","9999-01-01T00:00:00Z"}) {
    auto f=fields(); f.effective_at=value; auto result=adapters::transaction(f);
    check(!result); check(result.error().code=="INVALID_TIMESTAMP");
  }
  auto f=fields(); f.account="e\xcc\x81"; check(!adapters::transaction(f));
  f=fields(); f.type="equity_trade"; f.instrument="ABC"; f.quantity="10"; f.price="1.250000"; f.settlement="2026-10-22";
  check(adapters::transaction(f).has_value());
  f.quantity="0"; auto zero=adapters::transaction(f); check(!zero); check(zero.error().code=="ZERO_QUANTITY");
}
