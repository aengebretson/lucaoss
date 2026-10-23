#include "luca/transaction_versions.hpp"
#include "luca/period.hpp"
#include "luca/serialization/canonical_decode.hpp"
#include <cassert>
using namespace luca;
template<class T> auto take(T r){assert(r);return *r;}
auto at(int s){return Timestamp{std::chrono::seconds{1700000000+s}};}
auto proof(std::string id){return take(Provenance::create({SourceRecordId{id}},"test","1"));}
auto cash(std::string id,int amount){return EconomicEvent{CashMovement::create(take(EventHeader::create(EventId{id},AccountId{"a"},at(0),proof(id))),take(Money::parse(std::to_string(amount),take(Currency::from_code("USD")))))};}
int main(){
  LifecycleLedger ledger;take(ledger.accept(LifecycleRecordDraft::originate(EconomicEventId{"e"},at(1),cash("v1",100))));
  auto voided=LifecycleRecordDraft::cancel(EventId{"void"},EconomicEventId{"e"},EventId{"v1"},AccountId{"a"},at(3),proof("void"),at(2));
  auto replacement=LifecycleRecordDraft::correct(EconomicEventId{"e"},EventId{"void"},at(3),cash("v2",150));
  auto bad=LifecycleRecordDraft::correct(EconomicEventId{"different"},EventId{"void"},at(3),cash("bad",150));
  std::vector<LifecycleRecordDraft> invalid{voided,bad};assert(!ledger.accept_batch(invalid));assert(ledger.size()==1);
  std::vector<LifecycleRecordDraft> pair{voided,replacement};take(ledger.accept_batch(pair));assert(ledger.size()==3);
  assert(ledger.resolve(at(5),at(2)-std::chrono::nanoseconds{1}).active_events()[0].record().record_id().value()=="v1");
  assert(ledger.resolve(at(5),at(2)).active_events()[0].record().record_id().value()=="v2");
  assert(ledger.resolve(at(3)-std::chrono::nanoseconds{1},at(5)).active_events()[0].record().record_id().value()=="v1");
  auto versions=transaction_versions(ledger,at(5));assert(versions[0].void_version_id=="void" && versions[0].replacement_version_id=="v2");
  assert(versions[0].void_effective_at==at(2) && versions[0].void_recorded_at==at(3));
  assert(!versions[2].void_effective_at && !versions[2].void_recorded_at && versions[2].activation_effective_at==at(2));
  auto bytes=serialization::canonical_bytes(ledger);auto replay=take(serialization::decode_lifecycle_ledger(bytes));
  assert(serialization::canonical_bytes(replay)==bytes);
  LifecycleLedger separate;take(separate.accept(LifecycleRecordDraft::originate(EconomicEventId{"e"},at(1),cash("v1",100))));
  take(separate.accept(voided));assert(!separate.accept(replacement));assert(separate.size()==2);
  assert(changes_closed_period(ledger.records()[1],&ledger.records()[0],at(2)));
  assert(changes_closed_period(ledger.records()[2],&ledger.records()[1],at(2)));
  assert(!changes_closed_period(ledger.records()[2],&ledger.records()[1],at(1)));
}
