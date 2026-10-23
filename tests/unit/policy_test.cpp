#include "luca/accounting/lots.hpp"
#include "luca/ingestion/policy.hpp"
#include <cstdio>
#include <source_location>
#include <unistd.h>
#include <sqlite3.h>
using namespace luca;
using namespace luca::ingestion;
void check(bool ok,std::source_location at=std::source_location::current()) {
  if(!ok){std::fprintf(stderr,"failed %u\n",at.line());std::abort();}
}
template<class T>T take(std::expected<T,Failure> r,std::source_location at=std::source_location::current()) {
  if(!r){std::fprintf(stderr,"%s at %u\n",r.error().code.c_str(),at.line());std::abort();}return *r;
}
void error(const auto& r,const char* code){check(!r);check(r.error().code==code);}
int main() {
  char tmp[]="/tmp/luca-policy-XXXXXX";check(mkdtemp(tmp)!=nullptr);
  Json config={{"tenant_id","t"},{"book_id","b"},{"source_ids",Json::array({"s"})},{"contexts",{{"v1",{{"adapter","luca.transactions.jsonl/1"},{"normalization","source-book-identity/1"},{"rules","cash-equity/1"},{"engine","luca/0.1"},{"reference_snapshot","ref"}}}}}};
  auto store=take(SqliteStore::open(tmp,config));
  Json target={{"legal_entity","firm"},{"custodian_account","custody"},{"account","account-a"}};
  Json entity={{"id","entity-1"},{"scope","legal_entity"},{"subject","firm"},{"legal_entity","firm"},{"effective_from","2026-01-01T00:00:00Z"},{"lot_selection","LIFO"},{"recognition","unspecified"}};
  error(store.resolve_policy(target,"2026-01-02T00:00:00Z"),"ENTITY_POLICY_REQUIRED");
  auto e=take(store.put_policy(entity,0,"admin"));
  check(take(store.put_policy(entity,0,"admin"))==e);
  auto changed=entity;changed["recognition"]="cash";
  error(store.put_policy(changed,0,"admin"),"POLICY_IDEMPOTENCY_CONFLICT");
  changed["id"]="unsupported";changed["lot_selection"]="FIFO";
  error(store.put_policy(changed,1,"admin"),"UNSUPPORTED_LOT_SELECTION");
  auto initial=take(store.resolve_policy(target,"2026-02-01T00:00:00Z"));
  auto custody=entity;custody["id"]="custody-1";custody["scope"]="custodian_account";custody["subject"]="custody";
  custody["recognition"]="accrual";custody["lot_selection"]=nullptr;
  error(store.put_policy(custody,0,"admin"),"POLICY_REVIEW_STALE");
  take(store.put_policy(custody,1,"admin"));
  auto account=custody;account["id"]="account-1";account["scope"]="account";account["subject"]="account-a";account["recognition"]="cash";
  take(store.put_policy(account,2,"admin"));
  auto p=take(store.resolve_policy(target,"2026-02-01T00:00:00Z"));
  check(p["lot_selection"]=="LIFO" && p["recognition"]=="cash" && p["lot_source"]=="entity-1" && p["recognition_source"]=="account-1");
  check(take(store.resolve_policy(target,"2026-02-01T00:00:00Z",1))==initial);
  auto other=target;other["account"]="account-b";
  check(take(store.resolve_policy(other,"2026-02-01T00:00:00Z"))["recognition"]=="accrual");
  other["legal_entity"]="other-firm";
  error(store.resolve_policy(other,"2026-02-01T00:00:00Z"),"ENTITY_POLICY_REQUIRED");
  account["id"]="account-2";account["recognition"]=nullptr;account["effective_from"]="2026-03-01T00:00:00Z";
  take(store.put_policy(account,3,"admin"));
  check(take(store.resolve_policy(target,"2026-03-02T00:00:00Z"))["recognition"]=="accrual");
  check(take(store.resolve_policy(target,"2026-02-01T00:00:00Z"))["recognition"]=="cash");
  auto typed=take(resolved_policy(p));LifecycleLedger ledger;
  auto report=accounting::project_policy_lots(ledger.resolve(Timestamp::max(),Timestamp::max()),std::span{&typed,1});
  check(bool(report) && report->resolved_policies.size()==1 && report->trades.empty());
  typed.lot_source="forged";
  check(!accounting::project_policy_lots(ledger.resolve(Timestamp::max(),Timestamp::max()),std::span{&typed,1}));
  Json evidence={{"projection","economic-lifo/1"},{"accepted_through",0},{"mapping_version","mapping-1"},{"policy",p},{"result_digest",std::string(64,'a')}};
  auto retained=take(store.retain_policy_result(evidence));check(take(store.retain_policy_result(evidence))==retained);
  check(take(store.policy_result(retained["id"]))==retained);
  auto impossible=evidence;impossible["accepted_through"]=1;
  error(store.retain_policy_result(impossible),"RESULT_SEQUENCE_UNAVAILABLE");
  auto omitted=take(resolved_policy(p));omitted.revisions.resize(1);
  auto incomplete=accounting::resolve_policy(omitted.target,omitted.revisions,omitted.effective_at,omitted.through_sequence);
  check(bool(incomplete));auto forged=evidence;forged["policy"]=policy_json(*incomplete);
  error(store.retain_policy_result(forged),"POLICY_EVIDENCE_MISMATCH");
  error(store.close_period("close","2026-04-01T00:00:00Z",0,"admin",true,3),"POLICY_REVIEW_STALE");
  auto close=take(store.close_period("close","2026-04-01T00:00:00Z",0,"admin",true,4));check(close["policy_through"]==4);
  account["id"]="backdated";
  error(store.put_policy(account,4,"admin"),"POLICY_PERIOD_CLOSED");
  account["id"]="future";account["effective_from"]="2026-05-01T00:00:00Z";take(store.put_policy(account,4,"admin"));
  check(take(store.close_period("close","2026-04-01T00:00:00Z",0,"admin",false,4))==close);
  auto reopened=take(SqliteStore::open(tmp,config));
  check(take(reopened.resolve_policy(target,"2026-02-01T00:00:00Z",3))==p);
  check(take(reopened.retain_policy_result(evidence))==retained);
  error(reopened.resolve_policy(target,"2026-02-01T00:00:00Z",999),"POLICY_SEQUENCE_UNAVAILABLE");
  auto knowledge=take(reopened.policy_history()).at("knowledge");check(!knowledge[0]["recorded_at"].is_null());
  // A legacy schema5 store has no recorded policy timestamps. Upgrade preserves
  // unknown knowledge rather than inventing dates from policy effective time.
  sqlite3* legacy=nullptr;check(sqlite3_open((reopened.path()/"intake.sqlite3").c_str(),&legacy)==SQLITE_OK);
  auto altered=sqlite3_exec(legacy,"ALTER TABLE policy_revisions DROP COLUMN recorded_ns; PRAGMA user_version=5",nullptr,nullptr,nullptr);
  if(altered!=SQLITE_OK)std::fprintf(stderr,"migration fixture: %s\n",sqlite3_errmsg(legacy));check(altered==SQLITE_OK);sqlite3_close(legacy);
  auto migrated=take(SqliteStore::open(tmp,config));check(take(migrated.policy_history())["knowledge"][0]["recorded_at"].is_null());
  check(take(migrated.resolve_policy(target,"2026-02-01T00:00:00Z",3))==p);
  std::filesystem::remove_all(tmp);std::puts("policy tests passed");
}
