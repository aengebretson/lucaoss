#include "internal.hpp"
#include "luca/ingestion/policy.hpp"
namespace luca::ingestion {
using namespace detail;
Result<Json> SqliteStore::put_policy(Json definition,std::uint64_t expected,std::string actor) const {
  return guarded([&] {
    token(actor);integer(expected,0,INT64_MAX);
    ensure(definition.is_object() && definition.size()==7,"INVALID_POLICY_REVISION");
    for(const auto* key:{"id","legal_entity","subject","scope","effective_from"})token(definition.at(key));
    ensure(definition.dump().size()<=4096,"LIMIT_EXCEEDED");
    Json request={{"definition",definition},{"expected_sequence",expected},{"actor",actor}};
    FileLock lock(impl_->directory/"locks/admission");
    auto db=impl_->db(true);
    auto old=db.one("SELECT body,request FROM policy_revisions WHERE id=?",{definition.at("id")});
    if(!old.is_null()) {
      ensure(parse(old["request"])==request,"POLICY_IDEMPOTENCY_CONFLICT");
      return parse(old["body"]);
    }
    auto sequence=db.one("SELECT coalesce(max(sequence),0) AS n FROM policy_revisions")["n"].get<std::uint64_t>();
    ensure(sequence==expected,"POLICY_REVIEW_STALE");
    ensure(sequence<10000,"POLICY_HISTORY_LIMIT");
    auto body=definition;body["sequence"]=sequence+1;body["actor"]=actor;
    auto revision=take(policy_revision(body));
    auto close=db.one("SELECT through_ns FROM period_closes ORDER BY through_ns DESC LIMIT 1");
    ensure(close.is_null() || revision.effective_from.time_since_epoch().count()>close["through_ns"].get<std::int64_t>(),"POLICY_PERIOD_CLOSED");
    db.exec("INSERT INTO policy_revisions VALUES (?,?,?,?,?)",{sequence+1,revision.id,revision.effective_from.time_since_epoch().count(),body.dump(),request.dump()});
    db.commit();return body;
  });
}
Result<Json> SqliteStore::policy_history() const {
  return guarded([&] {
    auto db=impl_->db();Json rows=Json::array();
    for(const auto& row:db.rows("SELECT body FROM policy_revisions ORDER BY sequence LIMIT 10001"))rows.push_back(parse(row["body"]));
    ensure(rows.size()<=10000,"POLICY_HISTORY_LIMIT");
    return Json{{"revisions",rows},{"through_sequence",rows.empty()?Json(0):rows.back()["sequence"]}};
  });
}
Result<Json> SqliteStore::resolve_policy(Json target,std::string at,std::optional<std::uint64_t> through) const {
  return guarded([&] {
    ensure(target.is_object() && target.size()==3,"INVALID_POLICY_TARGET");
    for(const auto* key:{"legal_entity","custodian_account","account"})token(target.at(key));
    auto date=adapters::parse_transaction_timestamp(at);ensure(bool(date),"INVALID_POLICY_DATE");
    auto history=take(policy_history());
    auto sequence=history["through_sequence"].get<std::uint64_t>();
    if(through){ensure(*through<=sequence,"POLICY_SEQUENCE_UNAVAILABLE");sequence=*through;}
    std::vector<accounting::PolicyRevision> revisions;
    for(const auto& row:history["revisions"])revisions.push_back(take(policy_revision(row)));
    auto resolved=accounting::resolve_policy({impl_->book,target["legal_entity"],target["custodian_account"],target["account"]},revisions,*date,sequence);
    ensure(bool(resolved),resolved?"":resolved.error().code);
    return policy_json(*resolved);
  });
}
Result<Json> SqliteStore::retain_policy_result(Json evidence) const {
  return guarded([&] {
    ensure(evidence.is_object() && evidence.size()==5,"INVALID_POLICY_RESULT");
    ensure(evidence.at("projection")=="economic-lifo/1","INVALID_POLICY_RESULT");
    integer(evidence.at("accepted_through"),0,INT64_MAX);
    token(evidence.at("mapping_version"));
    const auto& policy=evidence.at("policy");auto resolved=take(resolved_policy(policy));
    ensure(resolved.target.book==impl_->book,"INVALID_POLICY_TARGET");
    token(evidence.at("result_digest"),64);
    const auto result_digest=evidence.at("result_digest").get<std::string>();
    ensure(result_digest.size()==64 && result_digest.find_first_not_of("0123456789abcdef")==std::string::npos,"INVALID_RESULT_DIGEST");
    const auto body=evidence.dump(),id=sha(body);
    auto db=impl_->db(true);
    std::vector<accounting::PolicyRevision> history;
    ensure(evidence.at("accepted_through").get<std::uint64_t>()<=db.one("SELECT last_sequence FROM ledger_state WHERE id=1")["last_sequence"].get<std::uint64_t>(),"RESULT_SEQUENCE_UNAVAILABLE");
    for(const auto& row:db.rows("SELECT body FROM policy_revisions ORDER BY sequence LIMIT 10001"))
      history.push_back(take(policy_revision(parse(row["body"]))));
    ensure(history.size()<=10000,"POLICY_HISTORY_LIMIT");
    ensure(!history.empty() && resolved.through_sequence<=history.back().sequence,"POLICY_SEQUENCE_UNAVAILABLE");
    auto authoritative=accounting::resolve_policy(resolved.target,history,resolved.effective_at,resolved.through_sequence);
    ensure(authoritative && *authoritative==resolved,"POLICY_EVIDENCE_MISMATCH");
    for(const auto& revision:policy.at("revisions")) {
      const auto row=db.one("SELECT body FROM policy_revisions WHERE id=?",{revision.at("id")});
      ensure(!row.is_null(),"POLICY_REVISION_UNAVAILABLE");
      ensure(take(policy_revision(parse(row["body"])))==take(policy_revision(revision)),"POLICY_EVIDENCE_MISMATCH");
    }
    db.exec("INSERT OR IGNORE INTO policy_results VALUES (?,?)",{id,body});db.commit();
    return Json{{"id",id},{"evidence",evidence}};
  });
}
Result<Json> SqliteStore::policy_result(std::string id) const {
  return guarded([&] {
    token(id,64);auto db=impl_->db();
    auto row=db.one("SELECT body FROM policy_results WHERE id=?",{id});
    ensure(!row.is_null(),"POLICY_RESULT_NOT_FOUND");
    return Json{{"id",id},{"evidence",parse(row["body"])}};
  });
}
} // namespace luca::ingestion
