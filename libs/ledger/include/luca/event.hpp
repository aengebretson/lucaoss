#pragma once

#include "luca/core.hpp"
#include "luca/time.hpp"
#include "luca/trade_charges.hpp"

#include <expected>
#include <utility>
#include <variant>

namespace luca {

class EventHeader {
 public:
  [[nodiscard]] static std::expected<EventHeader, ValueError> create(
      EventId id, AccountId account, Timestamp effective_at, Provenance provenance) {
    if (id.value().empty() || account.value().empty())
      return std::unexpected(ValueError::empty_identifier);
    return EventHeader(std::move(id), std::move(account), effective_at,
                       std::move(provenance));
  }

  [[nodiscard]] const EventId& id() const noexcept { return id_; }
  [[nodiscard]] const AccountId& account() const noexcept { return account_; }
  [[nodiscard]] Timestamp effective_at() const noexcept { return effective_at_; }
  [[nodiscard]] const Provenance& provenance() const noexcept { return provenance_; }
  bool operator==(const EventHeader&) const = default;

 private:
  EventHeader(EventId id, AccountId account, Timestamp effective_at,
              Provenance provenance)
      : id_(std::move(id)), account_(std::move(account)), effective_at_(effective_at),
        provenance_(std::move(provenance)) {}

  EventId id_;
  AccountId account_;
  Timestamp effective_at_;
  Provenance provenance_;
};

class CashMovement {
 public:
  [[nodiscard]] static CashMovement create(EventHeader header, Money amount) {
    return CashMovement(std::move(header), amount);
  }

  [[nodiscard]] const EventHeader& header() const noexcept { return header_; }
  [[nodiscard]] Money amount() const noexcept { return amount_; }
  bool operator==(const CashMovement&) const = default;

 private:
  CashMovement(EventHeader header, Money amount)
      : header_(std::move(header)), amount_(amount) {}
  EventHeader header_;
  Money amount_;
};

class EquityTrade {
 public:
  [[nodiscard]] static std::expected<EquityTrade, ValueError> create(
      EventHeader header, InstrumentId instrument, Quantity quantity, Price price,
      Currency quote_currency, SettlementDate settlement_date,
      std::optional<TradeCharges> charges = std::nullopt) {
    if (instrument.value().empty())
      return std::unexpected(ValueError::empty_identifier);
    if (quantity.scaled_value() == 0)
      return std::unexpected(ValueError::zero_quantity);
    if (charges) {
      auto valid = validate_charges(*charges, quote_currency);
      if (!valid) return std::unexpected(valid.error());
    }
    return EquityTrade(std::move(header), std::move(instrument), quantity, price,
                       quote_currency, settlement_date, std::move(charges));
  }

  [[nodiscard]] const EventHeader& header() const noexcept { return header_; }
  [[nodiscard]] const InstrumentId& instrument() const noexcept { return instrument_; }
  [[nodiscard]] Quantity quantity() const noexcept { return quantity_; }
  [[nodiscard]] Price price() const noexcept { return price_; }
  [[nodiscard]] Currency quote_currency() const noexcept { return quote_currency_; }
  [[nodiscard]] SettlementDate settlement_date() const noexcept { return settlement_date_; }
  [[nodiscard]] const std::optional<TradeCharges>& charges() const noexcept { return charges_; }
  bool operator==(const EquityTrade&) const = default;

 private:
  EquityTrade(EventHeader header, InstrumentId instrument, Quantity quantity,
              Price price, Currency quote_currency, SettlementDate settlement_date, std::optional<TradeCharges> charges)
      : header_(std::move(header)), instrument_(std::move(instrument)),
        quantity_(quantity), price_(price), quote_currency_(quote_currency),
        settlement_date_(settlement_date), charges_(std::move(charges)) {}

  EventHeader header_;
  InstrumentId instrument_;
  Quantity quantity_;
  Price price_;
  Currency quote_currency_;
  SettlementDate settlement_date_;
  std::optional<TradeCharges> charges_;
};

// Signed cash flow: buys pay principal plus charges; sells receive principal
// less charges. Legacy unreported charges retain the original gross behavior.
[[nodiscard]] inline std::expected<Money, ValueError> trade_cash(const EquityTrade& trade) {
  auto gross = value(trade.quantity(), trade.price(), trade.quote_currency());
  if (!gross) return std::unexpected(gross.error());
  auto cash = Money::from_scaled(0, trade.quote_currency()).subtract(*gross);
  if (!cash) return std::unexpected(cash.error());
  if (trade.charges()) for (const auto& charge : *trade.charges()) {
    cash = cash->subtract(charge.amount);
    if (!cash) return std::unexpected(cash.error());
  }
  return cash;
}

using EconomicEvent = std::variant<CashMovement, EquityTrade>;

[[nodiscard]] inline const EventHeader& header(const EconomicEvent& event) noexcept {
  return std::visit([](const auto& value) -> const EventHeader& {
    return value.header();
  }, event);
}

}  // namespace luca
