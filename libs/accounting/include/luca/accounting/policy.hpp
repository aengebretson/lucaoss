#pragma once
#include "luca/serialization/canonical.hpp"
#include <optional>
#include <set>

namespace luca::accounting {
enum class PolicyScope { legal_entity, custodian_account, account };
enum class LotSelection { lifo };
// Recognition configuration is independent of economic lot relief. These values
// do not enable a journal implementation; consumers must check their capabilities.
enum class RecognitionBasis { unspecified, cash, accrual };
struct PolicyRevision {
  std::string id, legal_entity, subject, actor;
  PolicyScope scope;
  Timestamp effective_from;
  std::uint64_t sequence;
  std::optional<LotSelection> lot_selection;
  std::optional<RecognitionBasis> recognition;
  bool operator==(const PolicyRevision&) const = default;
};
struct PolicyTarget {
  std::string book, legal_entity, custodian_account, account;
  bool operator==(const PolicyTarget&) const = default;
};
struct ResolvedPolicy {
  PolicyTarget target;
  LotSelection lot_selection = LotSelection::lifo;
  RecognitionBasis recognition = RecognitionBasis::unspecified;
  std::string lot_source, recognition_source;
  std::vector<PolicyRevision> revisions;
  Timestamp effective_at;
  std::uint64_t through_sequence;
  std::string digest;
  bool operator==(const ResolvedPolicy&) const = default;
};
struct PolicyError { std::string code; };
inline bool valid_policy_revision(const PolicyRevision& r) {
  if(r.id.empty() || r.legal_entity.empty() || r.subject.empty() || r.actor.empty() || !r.sequence)
    return false;
  if(r.scope!=PolicyScope::legal_entity && r.scope!=PolicyScope::custodian_account && r.scope!=PolicyScope::account) return false;
  if(r.lot_selection && *r.lot_selection!=LotSelection::lifo) return false;
  if(r.recognition && *r.recognition!=RecognitionBasis::unspecified && *r.recognition!=RecognitionBasis::cash && *r.recognition!=RecognitionBasis::accrual) return false;
  return r.scope!=PolicyScope::legal_entity || (r.subject==r.legal_entity && r.lot_selection && r.recognition);
}
// All I/O and snapshot selection are explicit. A revision replaces that scope's
// override: an absent setting means inherit, never retain an older override.
inline std::expected<ResolvedPolicy,PolicyError> resolve_policy(
    const PolicyTarget& target, std::span<const PolicyRevision> revisions,
    Timestamp effective_at, std::uint64_t through_sequence) {
  if(target.book.empty() || target.legal_entity.empty() || target.custodian_account.empty() || target.account.empty())
    return std::unexpected(PolicyError{"INVALID_POLICY_TARGET"});
  std::array<const PolicyRevision*,3> chosen{};
  std::set<std::string> ids;
  std::set<std::uint64_t> sequences;
  for(const auto& r:revisions) {
    if(!valid_policy_revision(r) || !ids.insert(r.id).second || !sequences.insert(r.sequence).second)
      return std::unexpected(PolicyError{"INVALID_POLICY_REVISION"});
    if(r.sequence>through_sequence || r.effective_from>effective_at || r.legal_entity!=target.legal_entity)continue;
    const auto index=static_cast<std::size_t>(r.scope);
    const auto& subject=index==0?target.legal_entity:index==1?target.custodian_account:target.account;
    if(r.subject!=subject)continue;
    auto& old=chosen[index];
    if(!old || std::pair{r.effective_from,r.sequence}>std::pair{old->effective_from,old->sequence})old=&r;
  }
  if(!chosen[0])return std::unexpected(PolicyError{"ENTITY_POLICY_REQUIRED"});
  ResolvedPolicy result{target,LotSelection::lifo,RecognitionBasis::unspecified,"","",{},effective_at,through_sequence,""};
  for(const auto* r:chosen)if(r) {
    result.revisions.push_back(*r);
    if(r->lot_selection){result.lot_selection=*r->lot_selection;result.lot_source=r->id;}
    if(r->recognition){result.recognition=*r->recognition;result.recognition_source=r->id;}
  }
  // Length framing prevents ambiguous concatenations. Digest identifies resolved
  // configuration; evaluation cutoffs are retained separately in the evidence.
  std::string encoded;
  auto field=[&](const std::string& s){encoded+=std::to_string(s.size())+":"+s;};
  field("luca.resolved-policy/1");field(target.book);field(target.legal_entity);
  field(target.custodian_account);field(target.account);
  for(const auto& r:result.revisions) {
    field(r.id);field(std::to_string(r.sequence));field(r.actor);
    field(std::to_string(static_cast<int>(r.scope)));field(r.subject);
    field(std::to_string(r.effective_from.time_since_epoch().count()));
    field(r.lot_selection?"LIFO":"inherit");
    field(r.recognition?std::to_string(static_cast<int>(*r.recognition)):"inherit");
  }
  result.digest=serialization::detail::sha256_hex(std::as_bytes(std::span{encoded}));
  return result;
}
} // namespace luca::accounting
