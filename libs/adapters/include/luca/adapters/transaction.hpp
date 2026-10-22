#pragma once

#include "luca/lifecycle.hpp"
#include "luca/serialization/unicode_nfc.hpp"
#include <charconv>
#include <chrono>
#include <expected>
#include <string>
#include <string_view>

namespace luca::adapters {

struct TransactionDiagnostic {
  std::string code;
  std::string field;
  std::string message;
};

// External representation only. Identity/reference mapping is explicit and must
// already have supplied the actual book-scoped identifiers and evidence identity.
struct ChargeFields {
  std::string id, category, payee, schedule_version, state, amount, currency;
};
struct TransactionFields {
  std::string record_id, economic_id, action, account, causal_id, evidence_id;
  std::string recorded_ns, type, effective_at, currency, amount;
  std::string instrument, quantity, price, settlement, normalization;
  std::optional<std::vector<ChargeFields>> charges;
};

namespace transaction_detail {
inline std::unexpected<TransactionDiagnostic> invalid(
    std::string code, std::string field, std::string message) {
  return std::unexpected(TransactionDiagnostic{std::move(code), std::move(field), std::move(message)});
}

inline std::expected<int, TransactionDiagnostic> digits(std::string_view text, std::string field) {
  int result = 0;
  const auto parsed = std::from_chars(text.data(), text.data()+text.size(), result);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data()+text.size())
    return invalid("INVALID_DATE", std::move(field), "Expected calendar digits");
  for (const char c : text) if (c < '0' || c > '9')
    return invalid("INVALID_DATE", std::move(field), "Expected calendar digits");
  return result;
}

inline std::expected<SettlementDate, TransactionDiagnostic> date(std::string_view text, std::string field) {
  if (text.size()!=10 || text[4]!='-' || text[7]!='-')
    return invalid("INVALID_DATE", field, "Expected YYYY-MM-DD");
  auto y=digits(text.substr(0,4),field), m=digits(text.substr(5,2),field), d=digits(text.substr(8,2),field);
  if (!y || !m || !d) return invalid("INVALID_DATE",field,"Invalid calendar date");
  auto result=SettlementDate::create(std::chrono::year{*y}/std::chrono::month{static_cast<unsigned>(*m)}/std::chrono::day{static_cast<unsigned>(*d)});
  if (!result) return invalid("INVALID_DATE",field,"Invalid calendar date");
  return *result;
}

inline std::expected<Timestamp, TransactionDiagnostic> timestamp(std::string_view text) {
  const std::string field="/event/effective_at";
  if (text.size()<20 || text[10]!='T' || text[13]!=':' || text[16]!=':' || text.back()!='Z')
    return invalid("INVALID_TIMESTAMP",field,"Expected UTC RFC3339 with Z and at most nanoseconds");
  auto day=date(text.substr(0,10),field);
  auto h=digits(text.substr(11,2),field), m=digits(text.substr(14,2),field), s=digits(text.substr(17,2),field);
  if (!day || !h || !m || !s || *h>23 || *m>59 || *s>59)
    return invalid("INVALID_TIMESTAMP",field,"Invalid UTC timestamp");
  std::int64_t nanos=0;
  if (text.size()!=20) {
    if (text[19]!='.' || text.size()<22 || text.size()>30)
      return invalid("INVALID_TIMESTAMP",field,"Invalid fractional seconds");
    auto fraction=digits(text.substr(20,text.size()-21),field);
    if (!fraction) return invalid("INVALID_TIMESTAMP",field,"Invalid fractional seconds");
    nanos=*fraction;
    for (std::size_t i=text.size()-21;i<9;++i) nanos*=10;
  }
  const auto days=std::chrono::sys_days{day->value()}.time_since_epoch().count();
  const auto wide=(detail::Wide(days)*86400+*h*3600+*m*60+*s)*1000000000+nanos;
  auto narrowed=detail::narrow(wide);
  if (!narrowed) return invalid("INVALID_TIMESTAMP",field,"Timestamp exceeds nanosecond range");
  return Timestamp{std::chrono::nanoseconds{*narrowed}};
}

template<class T> inline std::expected<T, TransactionDiagnostic> decimal(
    std::string_view text, std::string field) {
  auto value=T::parse(text);
  if (value) return *value;
  const auto code=value.error()==ValueError::precision_loss ? "EXCESS_PRECISION" :
      value.error()==ValueError::overflow ? "VALUE_OVERFLOW" : "INVALID_DECIMAL";
  return invalid(code,std::move(field),"Expected representable exact decimal");
}

} // namespace transaction_detail

// Public source-adapter parsers. Enterprise/CLI adapters must not depend on
// transaction_detail; financial value construction remains in the shared core.
[[nodiscard]] inline std::expected<SettlementDate, TransactionDiagnostic>
parse_transaction_date(std::string_view text, std::string field = "/event/settlement_date") {
  return transaction_detail::date(text, std::move(field));
}
[[nodiscard]] inline std::expected<Timestamp, TransactionDiagnostic>
parse_transaction_timestamp(std::string_view text) {
  return transaction_detail::timestamp(text);
}

inline std::expected<LifecycleRecordDraft, TransactionDiagnostic> transaction(const TransactionFields& f) {
  using namespace transaction_detail;
  for (const auto* text : {&f.record_id,&f.economic_id,&f.account,&f.causal_id,&f.evidence_id,&f.instrument,&f.normalization})
    if (text->find('\0')!=std::string::npos || serialization::detail::unicode_nfc::validate(*text)!=serialization::detail::unicode_nfc::ValidationResult::valid)
      return invalid("INVALID_IDENTIFIER","","Canonical text must be NUL-free UTF-8 NFC");
  if (f.record_id.empty() || f.economic_id.empty() || f.account.empty() || f.evidence_id.empty() || f.normalization.empty())
    return invalid("FIELD_REQUIRED","","Required identity/context is empty");
  std::int64_t recorded=0;
  const auto parsed=std::from_chars(f.recorded_ns.data(),f.recorded_ns.data()+f.recorded_ns.size(),recorded);
  if (parsed.ec!=std::errc{} || parsed.ptr!=f.recorded_ns.data()+f.recorded_ns.size())
    return invalid("INVALID_TIMESTAMP","/recorded_at","Invalid host recorded time");
  const Timestamp recorded_at{std::chrono::nanoseconds{recorded}};
  const bool origin=f.action=="originate";
  if (!origin && f.action!="correct" && f.action!="cancel" && f.action!="reverse")
    return invalid("UNSUPPORTED_ACTION","/action","Unsupported lifecycle action");
  if (origin ? !f.causal_id.empty() : f.causal_id.empty())
    return invalid("FIELD_REQUIRED","/causal_record_id","Causal reference does not match action");
  auto provenance=Provenance::create({SourceRecordId{f.evidence_id}},"luca.transactions.jsonl",f.normalization);
  if (!provenance) return invalid("FIELD_REQUIRED","","Invalid provenance");
  if (f.action=="cancel")
    return LifecycleRecordDraft::cancel(EventId{f.record_id},EconomicEventId{f.economic_id},EventId{f.causal_id},AccountId{f.account},recorded_at,*provenance);
  auto effective=timestamp(f.effective_at);
  if (!effective) return std::unexpected(effective.error());
  auto header=EventHeader::create(EventId{f.record_id},AccountId{f.account},*effective,*provenance);
  auto currency=Currency::from_code(f.currency);
  if (!currency) return invalid("INVALID_CURRENCY","/event/currency","Expected three uppercase currency letters");
  std::optional<EconomicEvent> event;
  if (f.type=="cash_movement") {
    auto amount=Money::parse(f.amount,*currency);
    if (!amount) return invalid(amount.error()==ValueError::overflow ? "VALUE_OVERFLOW" : amount.error()==ValueError::precision_loss ? "EXCESS_PRECISION" : "INVALID_DECIMAL","/event/amount","Expected representable exact amount");
    event=CashMovement::create(*header,*amount);
  } else if (f.type=="equity_trade") {
    auto quantity=decimal<Quantity>(f.quantity,"/event/quantity");
    auto price=decimal<Price>(f.price,"/event/price");
    auto settlement=date(f.settlement,"/event/settlement_date");
    if (!quantity) return std::unexpected(quantity.error());
    if (!price) return std::unexpected(price.error());
    if (!settlement) return std::unexpected(settlement.error());
    std::optional<TradeCharges> charges;
    if (f.charges) {
      charges.emplace();
      for (const auto& c : *f.charges) {
        auto denomination = Currency::from_code(c.currency);
        if (!denomination) return invalid("INVALID_CURRENCY", "/event/charges", "Invalid charge currency");
        auto amount = Money::parse(c.amount, *denomination);
        if (!amount) return invalid("INVALID_DECIMAL", "/event/charges", "Invalid charge amount");
        for (const auto* text : {&c.id, &c.payee, &c.schedule_version})
          if (serialization::detail::unicode_nfc::validate(*text) != serialization::detail::unicode_nfc::ValidationResult::valid)
            return invalid("INVALID_IDENTIFIER", "/event/charges", "Charge text must be NFC UTF-8");
        charges->push_back({c.id,c.category,c.payee,c.schedule_version,c.state,*amount});
      }
      if (!validate_charges(*charges, *currency))
        return invalid("INVALID_CHARGES", "/event/charges", "Expected unique nonnegative same-currency charges with category, payee and provenance");
    }
    auto trade=EquityTrade::create(*header,InstrumentId{f.instrument},*quantity,*price,*currency,*settlement,std::move(charges));
    if (!trade) return invalid(trade.error()==ValueError::zero_quantity ? "ZERO_QUANTITY" : "FIELD_REQUIRED","/event/quantity","Invalid equity trade");
    event=*trade;
  } else return invalid("UNSUPPORTED_EVENT_TYPE","/event/type","Unsupported event type");
  if (origin) return LifecycleRecordDraft::originate(EconomicEventId{f.economic_id},recorded_at,*event);
  if (f.action=="correct") return LifecycleRecordDraft::correct(EconomicEventId{f.economic_id},EventId{f.causal_id},recorded_at,*event);
  return LifecycleRecordDraft::reverse(EconomicEventId{f.economic_id},EventId{f.causal_id},recorded_at,*event);
}
} // namespace luca::adapters
