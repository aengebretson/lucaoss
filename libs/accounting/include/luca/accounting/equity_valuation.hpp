#pragma once
#include "luca/accounting/lots.hpp"
#include <algorithm>

namespace luca::accounting::equity_valuation {
inline constexpr std::string_view version = "equity-position-valuation/1";
struct Diagnostic {
  std::string code, id;
  bool operator==(const Diagnostic&) const = default;
};
template<class T> using Result = std::expected<T, Diagnostic>;
enum class MarkType { market, model };
enum class PriceBasis { bid, ask, midpoint, last, model };
struct Observation {
  std::string id, revision;
  InstrumentId instrument;
  Currency currency;
  Price price;
  MarkType type;
  PriceBasis basis;
  Timestamp effective_at, known_at;
  Provenance provenance;
  bool operator==(const Observation&) const = default;
};
struct MarkPolicy {
  std::string id, version;
  std::vector<MarkType> preference;
  PriceBasis market_basis;
  std::chrono::nanoseconds max_age;
  bool operator==(const MarkPolicy&) const = default;
};
// The host first resolves the complete dependent history at these exact cutoffs,
// projects lots, then selects one COMPLETE pool. evidence_id identifies the
// retained source/policy prefixes, including zero-effect versions. This API does
// not infer missing history, reselect lots, or verify the host's source manifest.
struct LotSnapshot {
  LotPool pool;
  LotReport lots;
  std::string evidence_id;
  Timestamp as_of, known_through;
  bool operator==(const LotSnapshot&) const = default;
};
struct Context {
  Timestamp as_of, known_through;
  bool operator==(const Context&) const = default;
};
struct Inputs {
  LotSnapshot snapshot;
  std::vector<Observation> observations;
  MarkPolicy marks;
  Context context;
  bool operator==(const Inputs&) const = default;
};
struct Report {
  Inputs inputs;
  Quantity position;
  Money cost_carrying, economic_realized;
  std::optional<Money> market_value, unrealized, total_economic;
  std::optional<Observation> selected_mark;
  bool charges_complete, supported_complete;
  std::vector<Diagnostic> diagnostics;
  std::string input_digest, report_id;
  bool operator==(const Report&) const = default;
};
namespace impl {
inline void field(std::string& s, std::string_view v) {
  s += std::to_string(v.size()) + ":" + std::string(v);
}
template<class T> inline void number(std::string& s, T v) { field(s, std::to_string(v)); }
inline void time(std::string& s, Timestamp t) { number(s, t.time_since_epoch().count()); }
inline void money(std::string& s, Money m) { number(s,m.scaled_value()); field(s,m.currency().code()); }
inline void pool(std::string& s, const LotPool& p) {
  for(const auto* v:{&p.book,&p.legal_entity,&p.custodian_account,&p.instrument,&p.currency})field(s,*v);
}
inline void amounts(std::string& s, const LotAmounts& a) {
  money(s,a.principal); number(s,a.charges.size());
  for(const auto& c:a.charges) {
    for(const auto* v:{&c.id,&c.category,&c.payee,&c.schedule_version,&c.state})field(s,*v);
    money(s,c.amount);
  }
}
inline void policy(std::string& s, const ResolvedPolicy& p) {
  for(const auto* v:{&p.target.book,&p.target.legal_entity,&p.target.custodian_account,&p.target.account,
                     &p.lot_source,&p.recognition_source,&p.digest})field(s,*v);
  number(s,static_cast<int>(p.lot_selection)); number(s,static_cast<int>(p.recognition));
  time(s,p.effective_at); number(s,p.through_sequence); number(s,p.revisions.size());
  for(const auto& r:p.revisions) {
    for(const auto* v:{&r.id,&r.legal_entity,&r.subject,&r.actor})field(s,*v);
    number(s,static_cast<int>(r.scope)); time(s,r.effective_from); number(s,r.sequence);
    number(s,r.lot_selection.has_value()); if(r.lot_selection)number(s,static_cast<int>(*r.lot_selection));
    number(s,r.recognition.has_value()); if(r.recognition)number(s,static_cast<int>(*r.recognition));
  }
}
inline std::string encoding(const Inputs& in) {
  std::string s; field(s,version);
  const auto& snap=in.snapshot; pool(s,snap.pool); field(s,snap.evidence_id);
  time(s,snap.as_of); time(s,snap.known_through); number(s,snap.lots.charges_complete);
  number(s,snap.lots.open_lots.size());
  for(const auto& l:snap.lots.open_lots) {
    pool(s,l.pool); field(s,l.opening_record); field(s,l.policy_version); time(s,l.opened_at);
    number(s,l.opening_sequence); number(s,l.short_position); number(s,l.charges_known);
    number(s,l.remaining_quantity.scaled_value()); amounts(s,l.remaining);
  }
  number(s,snap.lots.reliefs.size());
  for(const auto& r:snap.lots.reliefs) {
    pool(s,r.pool); field(s,r.opening_record); field(s,r.closing_record); field(s,r.policy_version);
    number(s,r.short_position); number(s,r.charges_known); number(s,r.quantity.scaled_value());
    amounts(s,r.opening); amounts(s,r.closing); money(s,r.gross_pnl); money(s,r.net_pnl);
  }
  number(s,snap.lots.trades.size());
  for(const auto& t:snap.lots.trades) {
    pool(s,t.pool); field(s,t.record_id); field(s,t.policy_version); number(s,t.signed_quantity.scaled_value());
    amounts(s,t.amounts); money(s,t.commissions); money(s,t.fees); money(s,t.cash_flow); number(s,t.charges_known);
  }
  number(s,snap.lots.resolved_policies.size()); for(const auto& p:snap.lots.resolved_policies)policy(s,p);
  field(s,in.marks.id); field(s,in.marks.version); number(s,in.marks.max_age.count());
  number(s,static_cast<int>(in.marks.market_basis));
  number(s,in.marks.preference.size()); for(auto t:in.marks.preference)number(s,static_cast<int>(t));
  time(s,in.context.as_of); time(s,in.context.known_through); number(s,in.observations.size());
  for(const auto& o:in.observations) {
    field(s,o.id); field(s,o.revision); field(s,o.instrument.value()); field(s,o.currency.code());
    number(s,o.price.scaled_value()); number(s,static_cast<int>(o.type)); number(s,static_cast<int>(o.basis));
    time(s,o.effective_at); time(s,o.known_at); number(s,o.provenance.source_records().size());
    for(const auto& id:o.provenance.source_records())field(s,id.value());
    field(s,o.provenance.transformation_name()); field(s,o.provenance.transformation_version());
    number(s,o.provenance.transformation_metadata().has_value());
    if(o.provenance.transformation_metadata())field(s,*o.provenance.transformation_metadata());
  }
  return s;
}
inline std::string hash(std::string_view s) {
  return serialization::detail::sha256_hex(std::as_bytes(std::span{s}));
}
inline bool valid_amounts(const LotAmounts& a, Currency c) {
  return a.principal.currency()==c && a.principal.scaled_value()>=0 && bool(validate_charges(a.charges,c));
}
inline bool reported(const LotAmounts& a) {
  return std::all_of(a.charges.begin(),a.charges.end(),[](const auto& c){return c.state=="reported";});
}
} // namespace impl

// A valuation snapshot, not a journal/admission/period operation. Authoritative
// multiplication rounds once at the complete pool's signed quantity, half-even
// to Money's six places. Carrying and realized reuse the already allocated lots.
[[nodiscard]] inline Result<Report> project(const Inputs& in) {
  const auto& snap=in.snapshot; const auto& p=snap.pool; const auto& lots=snap.lots;
  auto fail=[](std::string code, std::string id="") {return std::unexpected(Diagnostic{std::move(code),std::move(id)});};
  if(p.book.empty() || p.legal_entity.empty() || p.custodian_account.empty() || p.instrument.empty() ||
     snap.evidence_id.empty())return fail("LOT_SNAPSHOT_IDENTITY_REQUIRED");
  auto c=Currency::from_code(p.currency); if(!c)return fail("INVALID_POOL_CURRENCY");
  if(snap.as_of!=in.context.as_of || snap.known_through!=in.context.known_through)
    return fail("LOT_SNAPSHOT_CONTEXT_MISMATCH");
  std::set<MarkType> preference;
  if(in.marks.id.empty() || in.marks.version.empty() || in.marks.preference.empty() || in.marks.max_age.count()<0)
    return fail("MARK_POLICY_REQUIRED");
  if(in.marks.market_basis!=PriceBasis::bid && in.marks.market_basis!=PriceBasis::ask &&
     in.marks.market_basis!=PriceBasis::midpoint && in.marks.market_basis!=PriceBasis::last)
    return fail("INVALID_MARK_BASIS");
  for(auto t:in.marks.preference)
    if((t!=MarkType::market && t!=MarkType::model) || !preference.insert(t).second)
      return fail("INVALID_MARK_PREFERENCE");
  auto zero=Money::from_scaled(0,*c);
  Report out{in,Quantity::from_scaled(0),zero,zero,{},{},{},{},lots.charges_complete,true,{},{},{}};
  std::set<std::string> openings, trades;
  std::set<std::pair<std::string,std::string>> reliefs;
  std::optional<bool> short_pool;
  for(const auto& l:lots.open_lots) {
    if(l.pool!=p)return fail("LOT_POOL_MISMATCH",l.opening_record);
    if(l.opening_record.empty() || l.policy_version.empty() || !l.opening_sequence ||
       !openings.insert(l.opening_record).second || l.remaining_quantity.scaled_value()<=0 ||
       l.opened_at>snap.as_of || !impl::valid_amounts(l.remaining,*c))return fail("INVALID_OPEN_LOT",l.opening_record);
    if(short_pool && *short_pool!=l.short_position)return fail("MIXED_LONG_SHORT_POOL",l.opening_record);
    short_pool=l.short_position;
    auto q=Quantity::from_scaled(l.remaining_quantity.scaled_value()*(l.short_position?-1:1));
    auto sum=out.position.add(q); if(!sum)return fail("QUANTITY_OVERFLOW",l.opening_record); out.position=*sum;
    auto carrying=lot_carrying_amount(l); if(!carrying)return fail("AMOUNT_OVERFLOW",l.opening_record);
    auto next=l.short_position?out.cost_carrying.subtract(*carrying):out.cost_carrying.add(*carrying);
    if(!next)return fail("AMOUNT_OVERFLOW",l.opening_record); out.cost_carrying=*next;
    out.charges_complete=out.charges_complete && l.charges_known && impl::reported(l.remaining);
  }
  for(const auto& r:lots.reliefs) {
    if(r.pool!=p)return fail("LOT_POOL_MISMATCH",r.closing_record);
    if(r.opening_record.empty() || r.closing_record.empty() || r.policy_version.empty() ||
       !reliefs.emplace(r.opening_record,r.closing_record).second || r.quantity.scaled_value()<=0 ||
       !impl::valid_amounts(r.opening,*c) || !impl::valid_amounts(r.closing,*c) ||
       r.net_pnl.currency()!=*c || r.gross_pnl.currency()!=*c)return fail("INVALID_LOT_RELIEF",r.closing_record);
    auto gross=r.short_position?r.opening.principal.subtract(r.closing.principal):r.closing.principal.subtract(r.opening.principal);
    if(!gross)return fail("AMOUNT_OVERFLOW",r.closing_record);
    auto net=lot_detail::net_pnl(*gross,r.opening,r.closing); if(!net)return fail("AMOUNT_OVERFLOW",r.closing_record);
    if(*gross!=r.gross_pnl || *net!=r.net_pnl)return fail("LOT_RELIEF_PNL_MISMATCH",r.closing_record);
    auto sum=out.economic_realized.add(r.net_pnl); if(!sum)return fail("AMOUNT_OVERFLOW",r.closing_record);
    out.economic_realized=*sum;
    out.charges_complete=out.charges_complete && r.charges_known && impl::reported(r.opening) && impl::reported(r.closing);
  }
  for(const auto& t:lots.trades) {
    if(t.pool!=p)return fail("LOT_POOL_MISMATCH",t.record_id);
    if(t.record_id.empty() || t.policy_version.empty() || !trades.insert(t.record_id).second ||
       t.signed_quantity.scaled_value()==0 || !impl::valid_amounts(t.amounts,*c) ||
       t.commissions.currency()!=*c || t.fees.currency()!=*c || t.cash_flow.currency()!=*c)
      return fail("INVALID_LOT_TRADE",t.record_id);
    out.charges_complete=out.charges_complete && t.charges_known && impl::reported(t.amounts);
  }
  std::set<std::pair<std::string,std::string>> observations;
  bool stale=false;
  for(const auto& o:in.observations) {
    if(o.id.empty() || o.revision.empty() || !observations.emplace(o.id,o.revision).second)
      return fail("INVALID_OBSERVATION_ID",o.id);
    if(o.instrument.value()!=p.instrument || o.currency!=*c)return fail("OBSERVATION_POOL_MISMATCH",o.id);
    if(o.price.scaled_value()<0)return fail("NEGATIVE_EQUITY_MARK",o.id);
    if((o.type!=MarkType::market && o.type!=MarkType::model) ||
       (o.type==MarkType::model && o.basis!=PriceBasis::model) ||
       (o.type==MarkType::market && o.basis!=PriceBasis::bid && o.basis!=PriceBasis::ask &&
        o.basis!=PriceBasis::midpoint && o.basis!=PriceBasis::last))return fail("INVALID_OBSERVATION_TYPE",o.id);
    if(o.effective_at<=in.context.as_of && o.known_at<=in.context.known_through && preference.contains(o.type) &&
       (o.type==MarkType::model || o.basis==in.marks.market_basis) &&
       detail::Wide(in.context.as_of.time_since_epoch().count())-o.effective_at.time_since_epoch().count()>in.marks.max_age.count())stale=true;
  }
  for(auto type:in.marks.preference) {
    const Observation* selected=nullptr; bool ambiguous=false;
    for(const auto& o:in.observations) {
      if(o.type!=type || o.effective_at>in.context.as_of || o.known_at>in.context.known_through ||
         (o.type==MarkType::market && o.basis!=in.marks.market_basis) ||
         detail::Wide(in.context.as_of.time_since_epoch().count())-o.effective_at.time_since_epoch().count()>in.marks.max_age.count())continue;
      const auto key=std::pair{o.effective_at,o.known_at};
      if(!selected || key>std::pair{selected->effective_at,selected->known_at}) {selected=&o; ambiguous=false;}
      else if(key==std::pair{selected->effective_at,selected->known_at} && o!=*selected)ambiguous=true;
    }
    if(ambiguous)return fail("AMBIGUOUS_MARK",selected->id);
    if(selected){out.selected_mark=*selected; break;}
  }
  if(out.position.scaled_value()==0) {
    out.market_value=zero; out.unrealized=zero; out.total_economic=out.economic_realized;
  } else if(out.selected_mark) {
    auto v=value(out.position,out.selected_mark->price,*c); if(!v)return fail("AMOUNT_OVERFLOW",out.selected_mark->id);
    auto u=v->subtract(out.cost_carrying); if(!u)return fail("AMOUNT_OVERFLOW",out.selected_mark->id);
    auto total=out.economic_realized.add(*u); if(!total)return fail("AMOUNT_OVERFLOW",out.selected_mark->id);
    out.market_value=*v; out.unrealized=*u; out.total_economic=*total;
  } else out.diagnostics.push_back({stale?"STALE_MARK":"MARK_REQUIRED",p.instrument});
  if(!out.charges_complete)out.diagnostics.push_back({"CHARGES_INCOMPLETE",p.instrument});
  out.supported_complete=out.diagnostics.empty();
  out.input_digest=impl::hash(impl::encoding(in));
  std::string identity; impl::field(identity,version); impl::field(identity,out.input_digest);
  out.report_id="equity-valuation:"+impl::hash(identity);
  return out;
}
} // namespace luca::accounting::equity_valuation
