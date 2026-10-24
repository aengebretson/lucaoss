#pragma once
#include "luca/accounting/derivatives/contracts.hpp"
#include "luca/accounting/balances.hpp"
#include "luca/accounting/trade_accounting.hpp"
#include <variant>

namespace luca::accounting::derivatives {
inline constexpr std::string_view projection_version="derivative-accounting/1";
inline constexpr std::string_view chart_version="derivative-chart/1";
struct Trade {
  Quantity signed_quantity;
  Price price;
  std::optional<TradeCharges> charges;
  bool operator==(const Trade&) const = default;
};
// Signed quantity is the position being extinguished (long positive). Each lot
// fragment is consumed once; no synthesized offsetting equity/derivative trade.
struct OptionTerminal {
  Quantity signed_quantity;
  std::string fixing_id, fixing_revision;
  bool operator==(const OptionTerminal&) const = default;
};
using Payload=std::variant<Trade,OptionTerminal>;
enum class Action { originate, correct, void_event };
struct Revision {
  std::string id, economic_id, contract_digest;
  std::uint64_t sequence;
  Timestamp effective_at, known_at;
  Action action;
  std::optional<std::string> predecessor, replaces_void;
  std::optional<Payload> payload;
  Provenance provenance;
  bool operator==(const Revision&) const = default;
};
enum class CashKind { obligation, collateral };
struct Payment {
  std::string id, economic_id;
  std::uint64_t sequence;
  CashKind kind;
  Money cash; // actual bank-account change; collateral deposit is negative
  Timestamp effective_at, known_at;
  std::optional<std::string> refunds;
  Provenance provenance;
  bool operator==(const Payment&) const = default;
};
struct Rules {
  ChargeTreatment option_charges;
  bool recognize_unrealized;
  std::string version;
  bool operator==(const Rules&) const = default;
};
struct Inputs {
  Contract contract;
  ResolvedPolicy policy; // one complete custody/account/instrument/revision pool
  Timestamp policy_known_at;
  MarkPolicy marks;
  Rules rules;
  std::vector<Revision> events;
  std::vector<Observation> observations;
  std::vector<Payment> payments;
  bool operator==(const Inputs&) const = default;
};
struct Context {
  Timestamp from, until, known_through;
  std::string timezone;
  bool operator==(const Context&) const = default;
};
struct Limits { std::size_t rows=10000, boundaries=4096, work=2000000; };
struct Obligation {
  std::string economic_id, record_id;
  Money contractual, actual, outstanding;
  Timestamp due_at;
  bool operator==(const Obligation&) const = default;
};
struct BasisLot {
  OpenLot lot;
  Price basis;
  std::vector<std::string> resets;
  bool operator==(const BasisLot&) const = default;
};
struct Snapshot {
  Timestamp at;
  Quantity position;
  Money cost_carrying, realized, recognized_realized, cash, collateral;
  std::optional<Money> value, unrealized, recognized_unrealized, notional;
  std::optional<Observation> selected_mark;
  std::vector<BasisLot> lots;
  std::vector<LotRelief> reliefs;
  std::vector<Obligation> obligations;
  std::vector<Diagnostic> diagnostics;
  // Debit-positive chart amounts. Journals validate this projection separately.
  std::map<std::string,Money> balances;
  std::vector<std::string> event_evidence, observation_evidence, payment_evidence;
  std::string digest;
  bool complete=true;
  bool operator==(const Snapshot&) const = default;
};
struct Effect {
  Timestamp at;
  std::string id;
  JournalEntry journal;
  bool operator==(const Effect&) const = default;
};
struct Report {
  Inputs inputs; // FULL owned immutable evidence, including unselected revisions
  Context context;
  Snapshot opening, closing;
  Money realized_activity, recognized_realized_activity;
  std::optional<Money> total_pnl_activity;
  std::vector<Effect> effects;
  std::vector<JournalEntry> closing_journals;
  JournalBalanceResult closing_trial_balance;
  std::string input_digest, history_digest, report_id;
  bool supported_complete;
  bool operator==(const Report&) const = default;
};
struct ClosedAnchor {
  Inputs inputs;
  Timestamp from, until, known_through;
  std::string closing_digest, history_digest, report_id;
  bool operator==(const ClosedAnchor&) const = default;
};
namespace impl {
inline Result<Money> plus(Money a,Money b) {
  auto r=a.add(b);if(!r)return std::unexpected(Diagnostic{"AMOUNT_OVERFLOW",""});return *r;
}
inline Result<Money> minus(Money a,Money b) {
  auto r=a.subtract(b);if(!r)return std::unexpected(Diagnostic{"AMOUNT_OVERFLOW",""});return *r;
}
inline Result<Money> negate(Money a) { return minus(Money::from_scaled(0,a.currency()),a); }
inline Result<void> accumulate(Money& a,Money b) {
  auto r=plus(a,b);if(!r)return std::unexpected(r.error());a=*r;return {};
}
inline void observation(std::string& s,const Observation& o) {
  field(s,o.id);field(s,o.revision);field(s,o.contract_digest);field(s,o.instrument.value());
  number(s,static_cast<int>(o.type));number(s,o.price.scaled_value());field(s,o.currency.code());
  time(s,o.effective_at);time(s,o.known_at);provenance(s,o.provenance);
}
inline void revision(std::string& s,const Revision& e) {
  field(s,e.id);field(s,e.economic_id);field(s,e.contract_digest);number(s,static_cast<std::int64_t>(e.sequence));
  time(s,e.effective_at);time(s,e.known_at);number(s,static_cast<int>(e.action));
  number(s,e.predecessor.has_value());if(e.predecessor)field(s,*e.predecessor);
  number(s,e.replaces_void.has_value());if(e.replaces_void)field(s,*e.replaces_void);
  number(s,e.payload.has_value());
  if(e.payload)std::visit([&](const auto& p){
    using T=std::decay_t<decltype(p)>;
    if constexpr(std::is_same_v<T,Trade>) {
      field(s,"trade");number(s,p.signed_quantity.scaled_value());number(s,p.price.scaled_value());
      number(s,p.charges.has_value());
      if(p.charges){number(s,static_cast<std::int64_t>(p.charges->size()));for(const auto& c:*p.charges){
        field(s,c.id);field(s,c.category);field(s,c.payee);field(s,c.schedule_version);field(s,c.state);
        number(s,c.amount.scaled_value());field(s,c.amount.currency().code());}}
    } else if constexpr(std::is_same_v<T,OptionTerminal>) {
      field(s,"option-terminal");number(s,p.signed_quantity.scaled_value());field(s,p.fixing_id);field(s,p.fixing_revision);
    }
  },*e.payload);
  provenance(s,e.provenance);
}
inline void payment(std::string& s,const Payment& p) {
  field(s,p.id);field(s,p.economic_id);number(s,static_cast<std::int64_t>(p.sequence));
  number(s,static_cast<int>(p.kind));number(s,p.cash.scaled_value());field(s,p.cash.currency().code());
  time(s,p.effective_at);time(s,p.known_at);number(s,p.refunds.has_value());if(p.refunds)field(s,*p.refunds);
  provenance(s,p.provenance);
}
inline std::string input_encoding(const Inputs& in) {
  std::string s;field(s,projection_version);field(s,chart_version);field(s,in.contract.digest());
  // Never trust a caller-supplied policy digest as a substitute for its contents.
  time(s,in.policy_known_at);
  const auto& p=in.policy;field(s,p.digest);field(s,p.target.book);field(s,p.target.legal_entity);
  field(s,p.target.custodian_account);field(s,p.target.account);number(s,static_cast<int>(p.recognition));
  number(s,static_cast<int>(p.lot_selection));field(s,p.lot_source);field(s,p.recognition_source);
  time(s,p.effective_at);number(s,static_cast<std::int64_t>(p.through_sequence));
  number(s,static_cast<std::int64_t>(p.revisions.size()));
  for(const auto& r:p.revisions){field(s,r.id);field(s,r.legal_entity);field(s,r.subject);field(s,r.actor);
    number(s,static_cast<int>(r.scope));time(s,r.effective_from);number(s,static_cast<std::int64_t>(r.sequence));
    number(s,r.lot_selection.has_value());if(r.lot_selection)number(s,static_cast<int>(*r.lot_selection));
    number(s,r.recognition.has_value());if(r.recognition)number(s,static_cast<int>(*r.recognition));}
  field(s,in.marks.id);field(s,in.marks.version);number(s,in.marks.max_age.count());
  number(s,static_cast<std::int64_t>(in.marks.preference.size()));
  for(auto t:in.marks.preference)number(s,static_cast<int>(t));
  field(s,in.rules.version);number(s,static_cast<int>(in.rules.option_charges));
  number(s,in.rules.recognize_unrealized);
  number(s,static_cast<std::int64_t>(in.events.size()));for(const auto& e:in.events)revision(s,e);
  number(s,static_cast<std::int64_t>(in.observations.size()));for(const auto& o:in.observations)observation(s,o);
  number(s,static_cast<std::int64_t>(in.payments.size()));for(const auto& c:in.payments)payment(s,c);
  return s;
}
struct Active {const Revision* event;Timestamp recognized_at;};
inline Result<std::vector<Active>> resolve(const Inputs& in,Timestamp at,Timestamp known) {
  std::map<std::string,const Revision*> records,latest,voids,selected_by_eco,selected_voids;
  std::set<std::string> replaced;
  std::map<std::string,Timestamp> gates;
  std::vector<Active> out;
  std::uint64_t seq=0;std::optional<Timestamp> previous;
  for(const auto& e:in.events) {
    auto fail=[&](std::string c){return std::unexpected(Diagnostic{std::move(c),e.id});};
    if(e.id.empty() || e.economic_id.empty() || e.sequence!=++seq ||
       (previous && e.known_at<*previous) || !records.emplace(e.id,&e).second ||
       e.contract_digest!=in.contract.digest() || e.known_at<in.contract.terms().known_at)
      return fail("INVALID_DERIVATIVE_REVISION");
    previous=e.known_at;
    if(e.action!=Action::originate && e.action!=Action::correct && e.action!=Action::void_event)
      return fail("INVALID_DERIVATIVE_ACTION");
    if((e.action==Action::void_event)==e.payload.has_value())return fail("INVALID_REVISION_PAYLOAD");
    if(e.action==Action::originate) {
      if(e.predecessor || latest.contains(e.economic_id))return fail("DUPLICATE_ECONOMIC_ID");
      auto gate=Timestamp::min();
      if(e.replaces_void) {
        auto old=records.find(*e.replaces_void);
        if(old==records.end() || old->second->action!=Action::void_event || old->second->known_at>e.known_at)
          return fail("LINKED_VOID_REQUIRED");
        if(!replaced.insert(*e.replaces_void).second)return fail("DUPLICATE_LINKED_REPLACEMENT");
        gate=std::max({gate,e.known_at,old->second->effective_at,gates.at(old->second->economic_id)});
      }
      gates.emplace(e.economic_id,gate);
    } else {
      if(!e.predecessor || e.replaces_void || !latest.contains(e.economic_id) ||
          latest.at(e.economic_id)->id!=*e.predecessor || voids.contains(e.economic_id))
        return fail("REVISION_PREDECESSOR_REQUIRED");
      if(e.action==Action::void_event)voids[e.economic_id]=&e;
    }
    latest[e.economic_id]=&e;
    if(e.known_at<=known) {
      if(e.action==Action::void_event)selected_voids[e.economic_id]=&e;
      else if(e.action!=Action::correct || e.known_at<=at)selected_by_eco[e.economic_id]=&e;
    }
  }
  // All structural validation precedes independent knowledge selection.
  for(const auto& [eco,selected]:selected_by_eco) {
    const auto it=selected_voids.find(eco);
    const auto* cancel=it==selected_voids.end()?nullptr:it->second;
    if(cancel && std::max(cancel->effective_at,gates.at(eco))<=at)continue;
    auto gate=gates.at(eco);
    if(selected->action==Action::correct)gate=std::max(gate,selected->known_at);
    // A correction removes the predecessor at recording even if its new payload
    // takes effect later. A dated void before that recording cannot resurrect it.
    auto recognition=std::max(gate,selected->effective_at);
    if(recognition<=at)out.push_back({selected,recognition});
  }
  std::sort(out.begin(),out.end(),[](const Active& a,const Active& b){
    return std::pair{a.event->effective_at,a.event->sequence}<std::pair{b.event->effective_at,b.event->sequence};});
  return out;
}
inline Result<Snapshot> snapshot(const Inputs& in,Timestamp at,Timestamp known) {
  const auto& c=in.contract;const auto currency=c.terms().currency;
  const auto zero=Money::from_scaled(0,currency);const bool opt=true;
  Snapshot s{.at=at,.position=Quantity::from_scaled(0),.cost_carrying=zero,.realized=zero,
      .recognized_realized=zero,.cash=zero,.collateral=zero};
  auto fail=[](std::string code,std::string id=""){return std::unexpected(Diagnostic{std::move(code),std::move(id)});};
  auto diag=[&](std::string code,std::string id=""){s.diagnostics.push_back({std::move(code),std::move(id)});s.complete=false;};
  auto active=resolve(in,at,known);if(!active)return std::unexpected(active.error());
  auto mark=select_mark(c,in.observations,in.marks,at,known);if(!mark)return std::unexpected(mark.error());
  if(c.terms().known_at>known)return fail("CONTRACT_NOT_KNOWN");
  std::map<std::string,std::size_t> obligations;
  std::map<std::pair<std::string,std::string>,const Observation*> observations;
  for(const auto& o:in.observations)observations.emplace(std::pair{o.id,o.revision},&o);
  std::vector<const Observation*> selected_observations;
  auto add_due=[&](const Revision& e,Money due,Timestamp payment_at)->Result<void>{
    if(!obligations.emplace(e.economic_id,s.obligations.size()).second)return fail("DUPLICATE_OBLIGATION",e.id);
    s.obligations.push_back({e.economic_id,e.id,due,zero,due,payment_at});return {};
  };
  std::int64_t net_position=0;
  LotPool pool{in.policy.target.book,in.policy.target.legal_entity,in.policy.target.custodian_account,
               c.terms().instrument.value(),std::string(currency.code())};
  for(const auto& a:*active) {
    const auto& e=*a.event;
    if(!e.payload)return fail("INVALID_REVISION_PAYLOAD",e.id);
    const auto* trade=std::get_if<Trade>(&*e.payload);
    const auto* terminal=std::get_if<OptionTerminal>(&*e.payload);
    if(terminal && !opt)return fail("UNSUPPORTED_FUTURES_OPTION_TERMINAL",e.id);
    auto signed_q=trade?trade->signed_quantity.scaled_value():terminal->signed_quantity.scaled_value();
    if(signed_q==INT64_MIN || signed_q==0 || signed_q%100000000)return fail("INTEGER_CONTRACTS_REQUIRED",e.id);
    auto quantity=signed_q<0?-signed_q:signed_q;
    bool sell=signed_q<0;
    Price price=trade?trade->price:Price::from_scaled(0);
    TradeCharges charges;
    bool known_charges=true;
    if(trade) {
      if(e.effective_at<c.terms().boundaries.trading_from || e.effective_at>c.terms().boundaries.last_trade ||
          !valid_trade_price(c,price))return fail("INVALID_CONTRACT_TRADE",e.id);
      known_charges=trade->charges.has_value();charges=trade->charges.value_or(TradeCharges{});
    } else {
      if(e.effective_at!=c.terms().boundaries.expiration)return fail("EUROPEAN_EXPIRATION_REQUIRED",e.id);
      auto it=observations.find(std::pair{terminal->fixing_id,terminal->fixing_revision});
      if(it==observations.end())return fail("OFFICIAL_FIXING_REQUIRED",e.id);
      const auto& o=*it->second;
      if(o.type!=MarkType::official_fixing || o.effective_at!=c.terms().boundaries.fixing ||
          o.known_at>known || o.known_at>e.known_at || o.price.scaled_value()%1000000)
        return fail("INVALID_OFFICIAL_FIXING",e.id);
      {std::string key;field(key,o.id);field(key,o.revision);s.observation_evidence.push_back(key);}
      selected_observations.push_back(&o);
      auto intrinsic=*c.terms().right==OptionRight::call ? o.price.subtract(*c.terms().strike) : c.terms().strike->subtract(o.price);
      if(!intrinsic)return fail("AMOUNT_OVERFLOW",e.id);
      price=Price::from_scaled(std::max<std::int64_t>(0,intrinsic->scaled_value()));
      // Consume the specified side, never open an opposite position on expiry.
      if(s.lots.empty() || s.lots.back().lot.short_position!=sell)return fail("TERMINAL_POSITION_MISMATCH",e.id);
      auto available=net_position<0?-net_position:net_position;
      if(quantity>available)return fail("TERMINAL_OVERCONSUMPTION",e.id);
      sell=!sell; // closing direction only, terminal retains original position sign
    }
    if(!validate_charges(charges,currency))return fail("INVALID_DERIVATIVE_CHARGE",e.id);
    auto charge_total=zero;std::set<std::string> charge_ids;
    for(const auto& charge:charges) {
      if(charge.id.empty() || !charge_ids.insert(charge.id).second || charge.amount.currency()!=currency ||
         charge.amount.scaled_value()<0)return fail("INVALID_DERIVATIVE_CHARGE",e.id);
      known_charges=known_charges && charge.state=="reported";
      if(auto r=accumulate(charge_total,charge.amount);!r)return std::unexpected(r.error());
    }
    if(!known_charges)diag("CHARGES_INCOMPLETE",e.id);
    auto principal=quoted_money(c,Quantity::from_scaled(quantity),price);
    if(!principal)return std::unexpected(principal.error());
    LotAmounts remaining{*principal,charges};auto due=zero;
    if(opt){auto signed_principal=sell?Result<Money>{*principal}:negate(*principal);
      if(!signed_principal)return std::unexpected(signed_principal.error());
      auto r=minus(*signed_principal,charge_total);if(!r)return std::unexpected(r.error());due=*r;}
    while(quantity>0 && !s.lots.empty() && s.lots.back().lot.short_position!=sell) {
      auto& basis=s.lots.back();auto& lot=basis.lot;
      auto take=std::min(quantity,lot.remaining_quantity.scaled_value());
      auto opening=lot_detail::split(lot.remaining,take,lot.remaining_quantity.scaled_value());
      auto closing=lot_detail::split(remaining,take,quantity);
      if(!opening || !closing)return fail("LOT_ALLOCATION_ERROR",e.id);
      auto gross=lot.short_position?minus(opening->principal,closing->principal):minus(closing->principal,opening->principal);
      if(!gross)return std::unexpected(gross.error());
      auto net=*gross;
      if(opt){auto r=lot_detail::net_pnl(net,*opening,*closing);if(!r)return fail("AMOUNT_OVERFLOW",e.id);net=*r;}
      if(auto r=accumulate(s.realized,net);!r)return std::unexpected(r.error());
      s.reliefs.push_back({pool,lot.opening_record,e.id,in.policy.digest,lot.short_position,
          lot.charges_known && known_charges,Quantity::from_scaled(take),*opening,*closing,*gross,net});
      lot.remaining_quantity=Quantity::from_scaled(lot.remaining_quantity.scaled_value()-take);
      quantity-=take;if(lot.remaining_quantity.scaled_value()==0)s.lots.pop_back();
    }
    if(quantity) {
      if(terminal)return fail("TERMINAL_OVERCONSUMPTION",e.id);
      s.lots.push_back({OpenLot{pool,e.id,in.policy.digest,e.effective_at,e.sequence,sell,known_charges,
          Quantity::from_scaled(quantity),remaining},price,{}});
    }
    auto next_position=detail::narrow(detail::Wide(net_position)+
        (terminal?-detail::Wide(signed_q):detail::Wide(signed_q)));
    if(!next_position || *next_position==INT64_MIN)return fail("AMOUNT_OVERFLOW",e.id);
    net_position=*next_position;
    if(auto r=add_due(e,due,terminal?c.terms().boundaries.payment:e.effective_at);!r)return std::unexpected(r.error());
  }
  // Actual cash is immutable and independently selected, even on voided causes.
  std::map<std::string,const Payment*> payment_ids;
  std::map<std::string,Timestamp> known_economic;
  for(const auto& e:in.events)if(e.action!=Action::void_event) {
    auto [it,inserted]=known_economic.emplace(e.economic_id,e.known_at);
    if(!inserted)it->second=std::min(it->second,e.known_at);
  }
  std::map<std::string,Money> refunded;
  std::uint64_t seq=0;std::optional<Timestamp> previous;
  auto suspense=zero;
  for(const auto& p:in.payments) {
    if(p.id.empty() || p.sequence!=++seq || (previous && p.known_at<*previous) ||
        !payment_ids.emplace(p.id,&p).second || p.cash.currency()!=currency ||
        p.cash.scaled_value()==INT64_MIN || p.cash.scaled_value()==0 ||
        (p.kind!=CashKind::obligation && p.kind!=CashKind::collateral))return fail("INVALID_ACTUAL_CASH",p.id);
    previous=p.known_at;
    if(p.kind==CashKind::obligation) {
      auto found=known_economic.find(p.economic_id);
      if(found==known_economic.end() || found->second>p.known_at)return fail("PAYMENT_ECONOMIC_ID_REQUIRED",p.id);
    } else if(!p.economic_id.empty() || p.refunds)return fail("INVALID_COLLATERAL_LINK",p.id);
    if(p.refunds) {
      auto it=payment_ids.find(*p.refunds);
      if(it==payment_ids.end() || it->second==&p || it->second->refunds ||
          it->second->kind!=p.kind || it->second->economic_id!=p.economic_id ||
          it->second->effective_at>p.effective_at ||
          (it->second->cash.scaled_value()>0)==(p.cash.scaled_value()>0))return fail("INVALID_REFUND",p.id);
      auto [sum,added]=refunded.emplace(*p.refunds,zero);(void)added;
      if(auto r=accumulate(sum->second,p.cash);!r)return std::unexpected(r.error());
      auto total=plus(sum->second,it->second->cash);if(!total)return std::unexpected(total.error());
      if((it->second->cash.scaled_value()>0 && total->scaled_value()<0) ||
          (it->second->cash.scaled_value()<0 && total->scaled_value()>0))return fail("REFUND_EXCEEDS_ACTUAL",p.id);
    }
    if(p.effective_at>at || p.known_at>known)continue;
    s.payment_evidence.push_back(p.id);
    if(auto r=accumulate(s.cash,p.cash);!r)return std::unexpected(r.error());
    if(p.kind==CashKind::collateral) {
      auto neg=negate(p.cash);if(!neg)return std::unexpected(neg.error());
      if(auto r=accumulate(s.collateral,*neg);!r)return std::unexpected(r.error());
    } else if(auto it=obligations.find(p.economic_id);it!=obligations.end()) {
      if(auto r=accumulate(s.obligations[it->second].actual,p.cash);!r)return std::unexpected(r.error());
    } else {if(auto r=accumulate(suspense,p.cash);!r)return std::unexpected(r.error());diag("CASH_ON_INACTIVE_CAUSE",p.id);}
  }
  auto due_total=zero;
  for(auto& ob:s.obligations) {
    auto r=minus(ob.contractual,ob.actual);if(!r)return std::unexpected(r.error());ob.outstanding=*r;
    if(auto r2=accumulate(due_total,*r);!r2)return std::unexpected(r2.error());
    const auto due=ob.contractual.scaled_value(),actual=ob.actual.scaled_value();
    if((due>=0 && (actual<0 || actual>due)) || (due<0 && (actual>0 || actual<due)))diag("SETTLEMENT_MISMATCH",ob.economic_id);
  }
  std::int64_t position=0;
  for(const auto& lot:s.lots) {
    auto sum=detail::narrow(detail::Wide(position)+lot.lot.remaining_quantity.scaled_value()*(lot.lot.short_position?-1:1));
    if(!sum)return fail("AMOUNT_OVERFLOW");position=*sum;
    if(opt){auto cost=lot_carrying_amount(lot.lot);if(!cost)return fail("AMOUNT_OVERFLOW");
      auto signed_cost=lot.lot.short_position?negate(*cost):Result<Money>{*cost};
      if(!signed_cost)return std::unexpected(signed_cost.error());
      if(auto r=accumulate(s.cost_carrying,*signed_cost);!r)return std::unexpected(r.error());}
  }
  s.position=Quantity::from_scaled(position);s.recognized_realized=s.realized;
  if(position==0){s.value=zero;s.unrealized=zero;s.notional=zero;}
  else if(!*mark)diag("MARK_REQUIRED",c.terms().instrument.value());
  else {
    s.selected_mark=**mark;
    {std::string key;field(key,(**mark).id);field(key,(**mark).revision);s.observation_evidence.push_back(key);}
    selected_observations.push_back(&*s.selected_mark);
    const auto price=(**mark).price;
    if(opt){auto v=quoted_money(c,s.position,price);if(!v)return std::unexpected(v.error());s.value=*v;
      auto u=minus(*v,s.cost_carrying);if(!u)return std::unexpected(u.error());s.unrealized=*u;
      // Premium observation does not contain the underlying level.
      s.diagnostics.push_back({"OPTION_UNDERLYING_NOTIONAL_REQUIRES_SEPARATE_INPUT",""});
    }
  }
  if(position && at>=c.terms().boundaries.expiration)diag("TERMINAL_LIFECYCLE_INCOMPLETE");
  if(in.rules.recognize_unrealized) {
    if(!s.unrealized)return fail("VALUATION_REQUIRED_FOR_RECOGNITION",c.terms().instrument.value());
    s.recognized_unrealized=s.unrealized;
  } else s.recognized_unrealized=zero;
  if(s.collateral.scaled_value()<0)diag("COLLATERAL_WITHDRAWAL_EXCEEDS_DEPOSIT");
  auto neg_realized=negate(s.recognized_realized), neg_suspense=negate(suspense);
  if(!neg_realized || !neg_suspense)return fail("AMOUNT_OVERFLOW");
  s.balances={{"asset.cash",s.cash},{"asset.margin-collateral",s.collateral},
      {"control.derivative-obligations",due_total},{"control.settlement-suspense",*neg_suspense},
      {"asset.derivative-carrying",s.cost_carrying},{"income.derivative-realized",*neg_realized}};
  if(in.rules.recognize_unrealized && s.unrealized) {
    if(auto r=accumulate(s.balances.at("asset.derivative-carrying"),*s.unrealized);!r)return std::unexpected(r.error());
    auto neg=negate(*s.unrealized);if(!neg)return std::unexpected(neg.error());
    s.balances.emplace("income.derivative-unrealized",*neg);
  }
  // State identity binds relevant revision/mark/cash CONTENT, including zero effects.
  std::string encoded;field(encoded,c.digest());field(encoded,in.policy.digest);
  for(const auto& [account,m]:s.balances){field(encoded,account);number(encoded,m.scaled_value());}
  number(encoded,position);number(encoded,s.value.has_value());if(s.value)number(encoded,s.value->scaled_value());
  std::map<std::string,const Revision*> records;
  std::map<std::string,Timestamp> activation;
  for(const auto& e:in.events) {
    records.emplace(e.id,&e);
    if(e.action==Action::originate) {
      auto gate=Timestamp::min();
      if(e.replaces_void) {
        const auto* parent=records.at(*e.replaces_void);
        gate=std::max({e.known_at,parent->effective_at,activation.at(parent->economic_id)});
      }
      activation[e.economic_id]=gate;
    }
    auto gate=std::max(activation.at(e.economic_id),e.action==Action::correct?e.known_at:e.effective_at);
    if(e.known_at<=known && gate<=at){revision(encoded,e);s.event_evidence.push_back(e.id);}
  }
  for(const auto* selected:selected_observations)observation(encoded,*selected);
  for(const auto& p:in.payments)if(p.effective_at<=at && p.known_at<=known)payment(encoded,p);
  for(const auto& d:s.diagnostics){field(encoded,d.code);field(encoded,d.id);}
  s.digest=hash(encoded);
  return s;
}
inline Result<std::optional<JournalEntry>> journal(const Inputs& in,
    const std::map<std::string,Money>& balances,Timestamp at,Timestamp /*known*/,std::string id,
    const Snapshot& before,const Snapshot& after) {
  std::vector<EventId> ids;std::vector<EconomicEventId> economic;std::vector<SourceRecordId> sources;
  // Separate evidence streams are never looked up as lifecycle EventIds.
  // Full typed evidence is owned in Report.inputs; this lineage is the union.
  std::set<std::string> event_ids(before.event_evidence.begin(),before.event_evidence.end());
  event_ids.insert(after.event_evidence.begin(),after.event_evidence.end());
  std::set<std::string> observation_ids(before.observation_evidence.begin(),before.observation_evidence.end());
  observation_ids.insert(after.observation_evidence.begin(),after.observation_evidence.end());
  std::set<std::string> payment_ids(before.payment_evidence.begin(),before.payment_evidence.end());
  payment_ids.insert(after.payment_evidence.begin(),after.payment_evidence.end());
  auto journal_known=std::max(in.contract.terms().known_at,in.policy_known_at);
  for(const auto& e:in.events)if(event_ids.contains(e.id)) {
    journal_known=std::max(journal_known,e.known_at);
    ids.push_back(EventId{"derivative-event:"+e.id});economic.push_back(EconomicEventId{e.economic_id});
    for(const auto& src:e.provenance.source_records())sources.push_back(src);
  }
  for(const auto& o:in.observations) {
    std::string key;field(key,o.id);field(key,o.revision);
    if(observation_ids.contains(key)) {
      journal_known=std::max(journal_known,o.known_at);
      for(const auto& src:o.provenance.source_records())sources.push_back(src);
    }
  }
  for(const auto& p:in.payments)if(payment_ids.contains(p.id)) {
    journal_known=std::max(journal_known,p.known_at);
    ids.push_back(EventId{"derivative-payment:"+p.id});
    economic.push_back(EconomicEventId{p.economic_id.empty()?"collateral:"+p.id:p.economic_id});
    for(const auto& src:p.provenance.source_records())sources.push_back(src);
  }
  for(const auto& src:in.contract.terms().provenance.source_records())sources.push_back(src);
  bool any=false;for(const auto& [account,m]:balances){(void)account;any=any||m.scaled_value()!=0;}
  if(!any)return std::optional<JournalEntry>{};
  if(ids.empty())return std::unexpected(Diagnostic{"JOURNAL_EVENT_LINEAGE_REQUIRED",""});
  auto lineage=JournalLineage::create(ids,economic,sources);
  auto policy=AccountingPolicyIdentity::create(AccountingPolicyId{std::string(projection_version)},
      hash(in.policy.digest+":"+in.rules.version+":"+std::to_string(in.rules.recognize_unrealized)));
  auto date=JournalDate::create(std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(at)});
  auto settlement=JournalSettlementContext::create({},{},std::string(chart_version));
  if(!lineage || !policy || !date || !settlement)return std::unexpected(Diagnostic{"JOURNAL_CONTEXT",""});
  std::vector<JournalLine> lines;
  for(const auto& [account,m]:balances)if(m.scaled_value()) {
    auto amount=m.scaled_value()<0?negate(m):Result<Money>{m};if(!amount)return std::unexpected(amount.error());
    auto line=JournalLine::create(JournalLineId{id+":"+account},JournalEntryId{id},AccountId{account},
        m.scaled_value()>0?JournalSide::debit:JournalSide::credit,*amount,*lineage);
    if(!line)return std::unexpected(Diagnostic{"JOURNAL_LINE_INVARIANT",id});lines.push_back(*line);
  }
  auto entry=JournalEntry::create(JournalEntryId{id},*policy,JournalEventType::derivative_projection,
      ids.front(),at,journal_known,*date,RecognitionPhase::period_adjustment,0,*settlement,*lineage,std::move(lines));
  if(!entry)return std::unexpected(Diagnostic{"JOURNAL_BALANCE_INVARIANT",id});
  return std::optional{*entry};
}
inline Result<std::map<std::string,Money>> difference(const Snapshot& before,const Snapshot& after) {
  auto balances=after.balances;
  for(const auto& [account,m]:before.balances) {
    auto [it,inserted]=balances.emplace(account,Money::from_scaled(0,m.currency()));(void)inserted;
    auto d=minus(it->second,m);if(!d)return std::unexpected(d.error());it->second=*d;
  }
  return balances;
}
template<class T> bool prefix(const std::vector<T>& before,const std::vector<T>& after) {
  return before.size()<=after.size() && std::equal(before.begin(),before.end(),after.begin());
}
} // namespace impl

// Full replay of ONE COMPLETE account/custody/instrument/contract-revision pool.
// Inputs are not canonical admission; adapters must retain this new versioned
// envelope independently until a coordinated canonical/storage migration exists.
inline Result<Report> project(const Inputs& in,Context context,
    const std::optional<ClosedAnchor>& closed=std::nullopt,Limits limits={}) {
  auto fail=[](std::string code,std::string id=""){return std::unexpected(Diagnostic{std::move(code),std::move(id)});};
  if(context.timezone!="UTC")return fail("UNSUPPORTED_REPORTING_TIMEZONE");
  if(context.from==Timestamp::min() || context.from>=context.until)return fail("INVALID_PERIOD");
  if(in.events.size()>limits.rows || in.observations.size()>limits.rows || in.payments.size()>limits.rows)
    return fail("DERIVATIVE_REPLAY_LIMIT_EXCEEDED");
  if(!impl::option(in.contract.terms().profile))return fail("UNSUPPORTED_FUTURES_ACCOUNTING");
  if(in.rules.version!="derivative-rules/1" || in.rules.option_charges!=ChargeTreatment::capitalize_opening)
    return fail("UNSUPPORTED_DERIVATIVE_RULES");
  if(in.policy.recognition!=RecognitionBasis::accrual)return fail("DERIVATIVE_ACCRUAL_POLICY_REQUIRED");
  if(in.policy_known_at>context.known_through)return fail("POLICY_NOT_KNOWN");
  auto resolved=resolve_policy(in.policy.target,in.policy.revisions,in.policy.effective_at,in.policy.through_sequence);
  if(!resolved || *resolved!=in.policy)return fail("INVALID_RESOLVED_POLICY");
  for(const auto& r:in.policy.revisions)if(r.effective_from>context.from)
    return fail("INTRAPERIOD_POLICY_CHANGE_UNSUPPORTED",r.id);
  // Atomic equal-time causes; mark/fixing events are independent explicit inputs.
  std::set<Timestamp> boundaries;
  auto add=[&](Timestamp at){if(at>=context.from && at<context.until)boundaries.insert(at);};
  for(const auto& e:in.events)if(e.known_at<=context.known_through) {
    add(e.effective_at);
    if(e.action==Action::correct || e.replaces_void)add(e.known_at);
  }
  for(const auto& o:in.observations)if(o.known_at<=context.known_through) {
    add(o.effective_at);
    // Age eligibility changes at max_age + 1ns, even with no new observation.
    auto expires=detail::Wide(o.effective_at.time_since_epoch().count())+in.marks.max_age.count()+1;
    if(expires<=INT64_MAX && expires>=INT64_MIN)add(Timestamp{std::chrono::nanoseconds{static_cast<std::int64_t>(expires)}});
  }
  for(const auto& p:in.payments)if(p.known_at<=context.known_through)add(p.effective_at);
  add(in.contract.terms().boundaries.expiration);
  if(boundaries.size()>limits.boundaries ||
      detail::Wide(boundaries.size()+3)*(in.events.size()+in.observations.size()+in.payments.size()+1)>limits.work)
    return fail("DERIVATIVE_REPLAY_LIMIT_EXCEEDED");
  // Definition revisions cannot silently splice one open lot pool into another.
  // An explicit migration is required; there is no symbol or multiplier fallback.
  auto prefix=in;prefix.events.clear();prefix.observations.clear();prefix.payments.clear();
  const auto static_digest=impl::hash(impl::input_encoding(prefix));
  if(closed) {
    if(context.from<closed->until)return fail("CLOSED_PERIOD_RETAINED_REPORT_REQUIRED",closed->report_id);
    auto old=closed->inputs;old.events.clear();old.observations.clear();old.payments.clear();
    if(impl::hash(impl::input_encoding(old))!=static_digest ||
        !impl::prefix(closed->inputs.events,in.events) || !impl::prefix(closed->inputs.observations,in.observations) ||
        !impl::prefix(closed->inputs.payments,in.payments))
      return std::unexpected(Diagnostic{"CLOSED_INPUT_EVIDENCE_CHANGED",closed->report_id,
          impl::hash(impl::input_encoding(closed->inputs)),impl::hash(impl::input_encoding(in))});
    if(context.known_through<closed->known_through)return fail("CLOSED_KNOWLEDGE_REGRESSION",closed->report_id);
    auto check=project(in,{closed->from,closed->until,context.known_through,"UTC"},{},limits);
    if(!check)return std::unexpected(check.error());
    if(check->closing.digest!=closed->closing_digest || check->history_digest!=closed->history_digest)
      return std::unexpected(Diagnostic{"CLOSED_HISTORY_EXCEPTION",closed->report_id,closed->history_digest,check->history_digest});
  }
  const auto input_digest=impl::hash(impl::input_encoding(in));
  auto opening=impl::snapshot(in,context.from-std::chrono::nanoseconds{1},context.known_through);
  if(!opening)return std::unexpected(opening.error());
  auto closing=impl::snapshot(in,context.until-std::chrono::nanoseconds{1},context.known_through);
  if(!closing)return std::unexpected(closing.error());
  std::vector<Effect> effects;auto before=*opening;
  bool complete=opening->complete && closing->complete;
  std::string history;impl::field(history,opening->digest);
  for(auto at:boundaries) {
    auto after=impl::snapshot(in,at,context.known_through);if(!after)return std::unexpected(after.error());
    complete=complete && after->complete;
    if(before.digest!=after->digest){impl::time(history,at);impl::field(history,after->digest);}
    auto delta=impl::difference(before,*after);if(!delta)return std::unexpected(delta.error());
    std::string encoded;impl::field(encoded,static_digest);impl::time(encoded,at);
    impl::field(encoded,before.digest);impl::field(encoded,after->digest);
    auto id="derivative-effect:"+impl::hash(encoded);
    auto entry=impl::journal(in,*delta,at,context.known_through,id,before,*after);if(!entry)return std::unexpected(entry.error());
    if(*entry)effects.push_back({at,id,**entry});
    before=std::move(*after);
  }
  std::string identity;impl::field(identity,input_digest);impl::time(identity,context.from);
  impl::time(identity,context.until);impl::time(identity,context.known_through);impl::field(identity,context.timezone);
  auto report_id=impl::hash(identity);
  std::vector<JournalEntry> closing_journals;
  auto entry=impl::journal(in,closing->balances,closing->at,context.known_through,"derivative-snapshot:"+report_id,*closing,*closing);
  if(!entry)return std::unexpected(entry.error());if(*entry)closing_journals.push_back(**entry);
  auto policy=AccountingPolicyIdentity::create(AccountingPolicyId{std::string(projection_version)},
      impl::hash(in.policy.digest+":"+in.rules.version+":"+std::to_string(in.rules.recognize_unrealized)));
  if(!policy)return fail("JOURNAL_POLICY_INVARIANT");
  auto balance_context=JournalBalanceContract::create({BookId{in.policy.target.book},input_digest,
      "luca/0.1",std::string(projection_version),*policy,context.known_through,closing->at,
      std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(closing->at)}});
  if(!balance_context)return fail("JOURNAL_BALANCE_CONTEXT");
  auto balance=reduce_journal_balances(*balance_context,closing_journals);
  if(!balance)return fail("TRIAL_BALANCE_INVARIANT");
  auto activity=impl::minus(closing->realized,opening->realized);
  auto recognized=impl::minus(closing->recognized_realized,opening->recognized_realized);
  if(!activity || !recognized)return fail("AMOUNT_OVERFLOW");
  std::optional<Money> total;
  if(opening->unrealized && closing->unrealized) {
    auto movement=impl::minus(*closing->unrealized,*opening->unrealized);
    if(!movement)return std::unexpected(movement.error());
    auto sum=impl::plus(*activity,*movement);if(!sum)return std::unexpected(sum.error());total=*sum;
  }
  impl::field(history,closing->digest);
  return Report{in,context,*opening,*closing,*activity,*recognized,total,std::move(effects),
      std::move(closing_journals),*balance,input_digest,impl::hash(history),report_id,complete};
}
inline ClosedAnchor retain_close(const Report& r) {
  return {r.inputs,r.context.from,r.context.until,r.context.known_through,r.closing.digest,r.history_digest,r.report_id};
}
} // namespace luca::accounting::derivatives
