#pragma once
#include "luca/accounting/lots.hpp"
#include <numeric>

namespace luca::accounting::derivatives {
inline constexpr std::string_view contract_version = "derivative-contract/1";
struct Diagnostic {
  std::string code, id, expected, actual;
  bool operator==(const Diagnostic&) const = default;
};
template<class T> using Result = std::expected<T, Diagnostic>;
struct Factor {
  std::int64_t numerator, denominator;
  bool operator==(const Factor&) const = default;
};
enum class Profile { spx_am_european_cash, spxw_pm_european_cash, es_linear_cash };
enum class Family { index_option, index_future };
enum class QuantityUnit { contracts };
enum class QuoteUnit { index_points };
enum class ExerciseStyle { european, none };
enum class SettlementMethod { cash };
enum class OptionRight { call, put };
struct Boundaries {
  Timestamp trading_from, last_trade, fixing, exercise, expiration, payment;
  bool operator==(const Boundaries&) const = default;
};
struct TickBand {
  Price from; // inclusive, ascending; trade-price grid only, never fixing grid
  Price tick;
  bool operator==(const TickBand&) const = default;
};
struct Terms {
  InstrumentId instrument, underlying;
  std::string revision;
  Profile profile;
  Family family;
  QuantityUnit quantity_unit;
  QuoteUnit quote_unit;
  // Descriptive index units/contract and money/point/contract are DISTINGUISHED.
  // Conversion uses point_value ONCE; contract_size is never multiplied again.
  Factor contract_size;
  std::optional<Factor> point_value;
  Currency currency;
  std::vector<TickBand> ticks;
  std::optional<Price> strike;
  std::optional<OptionRight> right;
  ExerciseStyle exercise_style;
  SettlementMethod settlement_method;
  std::string payoff_profile;
  Boundaries boundaries;
  Timestamp effective_from, known_at;
  Provenance provenance;
  bool operator==(const Terms&) const = default;
};
namespace impl {
inline void field(std::string& out, std::string_view s) {
  out += std::to_string(s.size()) + ":" + std::string(s);
}
inline void number(std::string& out, std::int64_t n) { field(out,std::to_string(n)); }
inline void time(std::string& out, Timestamp t) { number(out,t.time_since_epoch().count()); }
inline std::string hash(std::string_view s) {
  return serialization::detail::sha256_hex(std::as_bytes(std::span{s}));
}
inline void provenance(std::string& s, const Provenance& p) {
  number(s,static_cast<std::int64_t>(p.source_records().size()));
  for(const auto& id:p.source_records()) field(s,id.value());
  field(s,p.transformation_name()); field(s,p.transformation_version());
  number(s,p.transformation_metadata().has_value());
  if(p.transformation_metadata()) field(s,*p.transformation_metadata());
}
inline bool factor(Factor f) { return f.numerator>0 && f.denominator>0; }
inline bool equals(Factor f,std::int64_t n) {
  return factor(f) && detail::Wide(f.numerator)==detail::Wide(n)*f.denominator;
}
inline bool option(Profile p) { return p!=Profile::es_linear_cash; }
}
class Contract {
 public:
  static Result<Contract> create(Terms t) {
    auto fail=[&](std::string c){return std::unexpected(Diagnostic{std::move(c),t.instrument.value()});};
    if(t.instrument.value().empty() || t.underlying.value().empty() || t.revision.empty())
      return fail("CONTRACT_IDENTITY_REQUIRED");
    if(!t.point_value || !impl::factor(*t.point_value))return fail("MONETARY_FACTOR_REQUIRED");
    if(!impl::factor(t.contract_size))return fail("INVALID_CONTRACT_SIZE");
    const auto& b=t.boundaries;
    if(b.trading_from>b.last_trade || b.last_trade>b.fixing || b.fixing>b.expiration ||
       b.exercise!=b.expiration || b.payment<b.expiration || t.effective_from>b.trading_from)
      return fail("INVALID_CONTRACT_BOUNDARIES");
    if(t.quantity_unit!=QuantityUnit::contracts || t.quote_unit!=QuoteUnit::index_points ||
       t.settlement_method!=SettlementMethod::cash || t.currency.code()!="USD")
      return fail("UNSUPPORTED_CONTRACT_UNITS");
    if(t.profile!=Profile::spx_am_european_cash && t.profile!=Profile::spxw_pm_european_cash &&
       t.profile!=Profile::es_linear_cash)return fail("UNSUPPORTED_PROFILE");
    const bool opt=impl::option(t.profile);
    if(opt) {
      if(t.family!=Family::index_option || !t.strike || t.strike->scaled_value()<0 || !t.right ||
         (*t.right!=OptionRight::call && *t.right!=OptionRight::put) ||
         t.exercise_style!=ExerciseStyle::european || !impl::equals(*t.point_value,100) ||
         !impl::equals(t.contract_size,100) || t.payoff_profile!="spx-european-cash/1")
        return fail("INCOMPATIBLE_SPX_TERMS");
      if(t.ticks!=std::vector<TickBand>{{Price::from_scaled(0),Price::from_scaled(5000000)},
          {Price::from_scaled(300000000),Price::from_scaled(10000000)}})
        return fail("UNSUPPORTED_SPX_TICK_RULE");
    } else if(t.family!=Family::index_future || t.strike || t.right ||
        t.exercise_style!=ExerciseStyle::none || !impl::equals(*t.point_value,50) ||
        !impl::equals(t.contract_size,50) || t.payoff_profile!="es-linear-settlement/1" ||
        t.ticks!=std::vector<TickBand>{{Price::from_scaled(0),Price::from_scaled(25000000)}})
      return fail("INCOMPATIBLE_ES_TERMS");
    for(auto* f:{&t.contract_size,&*t.point_value}) {
      auto gcd=std::gcd(f->numerator,f->denominator); f->numerator/=gcd;f->denominator/=gcd;
    }
    std::string s;impl::field(s,contract_version);
    impl::field(s,t.instrument.value());impl::field(s,t.underlying.value());impl::field(s,t.revision);
    for(auto v:{static_cast<int>(t.profile),static_cast<int>(t.family),static_cast<int>(t.quantity_unit),
        static_cast<int>(t.quote_unit),static_cast<int>(t.exercise_style),static_cast<int>(t.settlement_method)})
      impl::number(s,v);
    for(auto f:{t.contract_size,*t.point_value}){impl::number(s,f.numerator);impl::number(s,f.denominator);}
    impl::field(s,t.currency.code()); impl::number(s,static_cast<std::int64_t>(t.ticks.size()));
    for(const auto& r:t.ticks){impl::number(s,r.from.scaled_value());impl::number(s,r.tick.scaled_value());}
    impl::number(s,t.strike.has_value());if(t.strike)impl::number(s,t.strike->scaled_value());
    impl::number(s,t.right.has_value());if(t.right)impl::number(s,static_cast<int>(*t.right));
    impl::field(s,t.payoff_profile);
    for(auto at:{b.trading_from,b.last_trade,b.fixing,b.exercise,b.expiration,b.payment,t.effective_from,t.known_at})
      impl::time(s,at);
    impl::provenance(s,t.provenance);
    auto digest=impl::hash(s);
    return Contract{std::move(t),std::move(digest)};
  }
  const Terms& terms() const { return terms_; }
  const std::string& digest() const { return digest_; }
  bool operator==(const Contract&) const = default;
 private:
  Contract(Terms t,std::string d):terms_(std::move(t)),digest_(std::move(d)){}
  Terms terms_;
  std::string digest_;
};
// q (contracts, scale 8) * quote (points, scale 8) * factor (USD/point/contract)
// -> Money (scale 6). Cancel exact factors BEFORE checked widened multiplication;
// perform ONE explicit half-even rounding to Money. No float or pre-scaling.
inline Result<Money> quoted_money(Quantity q, Price p, Factor f, Currency currency) {
  if(!impl::factor(f))return std::unexpected(Diagnostic{"MONETARY_FACTOR_REQUIRED",""});
  std::array<detail::Wide,3> n{q.scaled_value(),p.scaled_value(),f.numerator};
  std::array<std::int64_t,2> d{10000000000LL,f.denominator};
  for(auto& a:n)for(auto& b:d){
    auto rem=static_cast<std::int64_t>((a<0?-a:a)%b);
    auto g=std::gcd(rem,b);a/=g;b/=g;
  }
  detail::Wide numerator=1,denominator=1;
  for(auto a:n)if(__builtin_mul_overflow(numerator,a,&numerator))
    return std::unexpected(Diagnostic{"AMOUNT_OVERFLOW",""});
  for(auto b:d)denominator*=b;
  auto v=detail::narrow(detail::divide_rounded(numerator,denominator,RoundingMode::half_even));
  if(!v)return std::unexpected(Diagnostic{"AMOUNT_OVERFLOW",""});
  return Money::from_scaled(*v,currency);
}
inline Result<Money> quoted_money(const Contract& c, Quantity q, Price p) {
  return quoted_money(q,p,*c.terms().point_value,c.terms().currency);
}
inline Result<Money> price_change(const Contract& c, Quantity q, Price from, Price to) {
  auto change=to.subtract(from);
  if(!change)return std::unexpected(Diagnostic{"PRICE_CHANGE_OVERFLOW",c.terms().instrument.value()});
  return quoted_money(c,q,*change);
}
inline bool valid_trade_price(const Contract& c, Price p) {
  if(impl::option(c.terms().profile) && p.scaled_value()<0)return false;
  auto tick=c.terms().ticks.front().tick;
  for(const auto& b:c.terms().ticks)if(p>=b.from)tick=b.tick;
  return p.scaled_value()%tick.scaled_value()==0;
}
// Separate exposure measure: option premium P&L NEVER uses this underlying level.
inline Result<Money> notional_exposure(const Contract& c, Quantity signed_q, Price underlying_level) {
  if(underlying_level.scaled_value()<0)return std::unexpected(Diagnostic{"NEGATIVE_UNDERLYING_LEVEL",""});
  return quoted_money(c,signed_q,underlying_level);
}
enum class MarkType { market, model, official_settlement, official_fixing };
struct Observation {
  std::string id, revision, contract_digest;
  InstrumentId instrument;
  MarkType type;
  Price price;
  Currency currency;
  Timestamp effective_at, known_at;
  Provenance provenance;
  bool operator==(const Observation&) const = default;
};
struct MarkPolicy {
  std::string id, version;
  std::vector<MarkType> preference;
  std::chrono::nanoseconds max_age;
  bool operator==(const MarkPolicy&) const = default;
};
inline Result<std::optional<Observation>> select_mark(const Contract& c,
    std::span<const Observation> observations, const MarkPolicy& policy, Timestamp at, Timestamp known) {
  std::set<MarkType> types;
  if(policy.id.empty() || policy.version.empty() || policy.preference.empty() || policy.max_age.count()<0)
    return std::unexpected(Diagnostic{"MARK_POLICY_REQUIRED",""});
  for(auto t:policy.preference)if((t!=MarkType::market && t!=MarkType::model &&
      t!=MarkType::official_settlement) || !types.insert(t).second)
    return std::unexpected(Diagnostic{"INVALID_MARK_PREFERENCE",""});
  std::set<std::pair<std::string,std::string>> ids;
  for(const auto& o:observations) {
    if(o.id.empty() || o.revision.empty() || !ids.emplace(o.id,o.revision).second)
      return std::unexpected(Diagnostic{"INVALID_OBSERVATION_ID",o.id});
    if(o.contract_digest!=c.digest() || o.instrument!=c.terms().instrument || o.currency!=c.terms().currency)
      return std::unexpected(Diagnostic{"OBSERVATION_CONTRACT_MISMATCH",o.id});
    if(o.type!=MarkType::market && o.type!=MarkType::model && o.type!=MarkType::official_settlement &&
        o.type!=MarkType::official_fixing)return std::unexpected(Diagnostic{"INVALID_MARK_TYPE",o.id});
    if(impl::option(c.terms().profile) && o.price.scaled_value()<0)
      return std::unexpected(Diagnostic{"NEGATIVE_OPTION_MARK",o.id});
  }
  for(auto type:policy.preference) {
    const Observation* selected=nullptr;bool ambiguous=false;
    for(const auto& o:observations) {
      if(o.type!=type || o.effective_at>at || o.known_at>known ||
          detail::Wide(at.time_since_epoch().count())-o.effective_at.time_since_epoch().count()>policy.max_age.count())continue;
      const auto key=std::pair{o.effective_at,o.known_at};
      if(!selected || key>std::pair{selected->effective_at,selected->known_at}){selected=&o;ambiguous=false;}
      else if(key==std::pair{selected->effective_at,selected->known_at} && o!=*selected)ambiguous=true;
    }
    if(ambiguous)return std::unexpected(Diagnostic{"AMBIGUOUS_MARK",selected->id});
    if(selected)return std::optional{*selected};
  }
  return std::optional<Observation>{};
}
} // namespace luca::accounting::derivatives
