#pragma once
#include "luca/accounting/actual_settlement.hpp"
#include "luca/adapters/transaction.hpp"
#include "luca/ingestion/sqlite.hpp"
namespace luca::ingestion {
inline Result<accounting::SettlementEvent> settlement_event(const Json &row) {
  try {
    const auto at =
        adapters::parse_transaction_timestamp(row.at("effective_at").get<std::string>());
    const auto recorded =
        adapters::parse_transaction_timestamp(row.at("recorded_at").get<std::string>());
    const auto currency = Currency::from_code(row.at("currency").get<std::string>());
    if (!at || !recorded || !currency)
      return std::unexpected(Failure{"INVALID_SETTLEMENT_EVENT"});
    auto cash = Money::parse(row.at("cash").get<std::string>(), *currency);
    if (!cash)
      return std::unexpected(Failure{"INVALID_SETTLEMENT_EVENT"});
    return accounting::SettlementEvent{row.at("id"),
                                       row.at("account"),
                                       row.at("trade_record"),
                                       row.at("economic_event"),
                                       *at,
                                       *recorded,
                                       row.at("sequence"),
                                       row.at("ledger_through"),
                                       *cash,
                                       {SourceRecordId{row.at("source_record").get<std::string>()}},
                                       row.at("reverses").is_null()
                                           ? std::nullopt
                                           : std::optional<std::string>{row.at("reverses")}};
  } catch (const Json::exception &) {
    return std::unexpected(Failure{"INVALID_SETTLEMENT_EVENT"});
  }
}
} // namespace luca::ingestion
