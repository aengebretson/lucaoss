#pragma once
#include "luca/ingestion/sqlite.hpp"
#include "luca/accounting/policy.hpp"
#include "luca/adapters/transaction.hpp"
namespace luca::ingestion {
inline Result<accounting::PolicyRevision> policy_revision(const Json& j) {
  try {
    const auto scope=j.at("scope").get<std::string>();
    auto date=adapters::parse_transaction_timestamp(j.at("effective_from").get<std::string>());
    if(!date || (scope!="legal_entity" && scope!="custodian_account" && scope!="account"))
      return std::unexpected(Failure{"INVALID_POLICY_REVISION"});
    accounting::PolicyRevision r{j.at("id"),j.at("legal_entity"),j.at("subject"),j.at("actor"),
      scope=="legal_entity"?accounting::PolicyScope::legal_entity:scope=="custodian_account"?accounting::PolicyScope::custodian_account:accounting::PolicyScope::account,
      *date,j.at("sequence").get<std::uint64_t>(),{},{} };
    if(!j.at("lot_selection").is_null()) {
      if(j.at("lot_selection")!="LIFO")return std::unexpected(Failure{"UNSUPPORTED_LOT_SELECTION"});
      r.lot_selection=accounting::LotSelection::lifo;
    }
    if(!j.at("recognition").is_null()) {
      const auto value=j.at("recognition").get<std::string>();
      if(value!="unspecified" && value!="cash" && value!="accrual")return std::unexpected(Failure{"UNSUPPORTED_RECOGNITION"});
      r.recognition=value=="cash"?accounting::RecognitionBasis::cash:value=="accrual"?accounting::RecognitionBasis::accrual:accounting::RecognitionBasis::unspecified;
    }
    if(!accounting::valid_policy_revision(r))return std::unexpected(Failure{"INVALID_POLICY_REVISION"});
    return r;
  }catch(const Json::exception&){return std::unexpected(Failure{"INVALID_POLICY_REVISION"});}
}
inline Json policy_json(const accounting::ResolvedPolicy& p) {
  Json revisions=Json::array();
  for(const auto& r:p.revisions)revisions.push_back({
    {"id",r.id},{"legal_entity",r.legal_entity},{"subject",r.subject},{"actor",r.actor},
    {"scope",r.scope==accounting::PolicyScope::legal_entity?"legal_entity":r.scope==accounting::PolicyScope::custodian_account?"custodian_account":"account"},
    {"sequence",r.sequence},{"effective_from",serialization::detail::canonical_timestamp(r.effective_from)},
    {"lot_selection",r.lot_selection?Json("LIFO"):Json()},
    {"recognition",r.recognition?Json(*r.recognition==accounting::RecognitionBasis::cash?"cash":*r.recognition==accounting::RecognitionBasis::accrual?"accrual":"unspecified"):Json()}});
  return {{"schema","luca.resolved-policy/1"},{"book",p.target.book},{"legal_entity",p.target.legal_entity},
    {"custodian_account",p.target.custodian_account},{"account",p.target.account},{"lot_selection","LIFO"},
    {"recognition",p.recognition==accounting::RecognitionBasis::cash?"cash":p.recognition==accounting::RecognitionBasis::accrual?"accrual":"unspecified"},
    {"recognition_applied",false},{"lot_source",p.lot_source},{"recognition_source",p.recognition_source},
    {"revisions",revisions},{"effective_at",serialization::detail::canonical_timestamp(p.effective_at)},
    {"through_sequence",p.through_sequence},{"digest",p.digest}};
}
inline Result<accounting::ResolvedPolicy> resolved_policy(const Json& j) {
  try {
    std::vector<accounting::PolicyRevision> rows;
    for(const auto& row:j.at("revisions")) {
      auto r=policy_revision(row);if(!r)return std::unexpected(r.error());rows.push_back(*r);
    }
    auto at=adapters::parse_transaction_timestamp(j.at("effective_at").get<std::string>());
    if(!at)return std::unexpected(Failure{"INVALID_POLICY_DATE"});
    auto p=accounting::resolve_policy({j.at("book"),j.at("legal_entity"),j.at("custodian_account"),j.at("account")},rows,*at,j.at("through_sequence"));
    if(!p)return std::unexpected(Failure{p.error().code});
    if(policy_json(*p)!=j)return std::unexpected(Failure{"POLICY_EVIDENCE_MISMATCH"});
    return *p;
  }catch(const Json::exception&){return std::unexpected(Failure{"INVALID_POLICY_EVIDENCE"});}
}
} // namespace luca::ingestion
