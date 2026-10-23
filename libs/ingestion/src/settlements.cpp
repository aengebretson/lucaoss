#include "internal.hpp"
#include "luca/ingestion/settlement.hpp"
namespace luca::ingestion {
using namespace detail;
Result<Json> SqliteStore::receive_settlement(std::string key, Json request,
                                             std::string actor) const {
  return guarded([&] {
    token(key);
    token(actor);
    ensure(request.is_object() && request.dump().size() <= 65536, "INVALID_SETTLEMENT_REQUEST");
    Json submitted = {{"request", request}, {"actor", actor}};
    auto db = impl_->db(true);
    auto old = db.one("SELECT request,body FROM settlement_requests WHERE id=?", {key});
    if (!old.is_null()) {
      ensure(parse(old["request"]) == submitted, "SETTLEMENT_IDEMPOTENCY_CONFLICT");
      return Json{{"id", key}, {"state", "received"}};
    }
    ensure(db.one("SELECT count(*) AS n FROM settlement_requests")["n"].get<std::size_t>() < 100000,
           "SETTLEMENT_REQUEST_LIMIT");
    db.exec("INSERT INTO settlement_requests VALUES (?,?,'pending',?)",
            {key, submitted.dump(), Json{{"id", key}, {"state", "pending"}}.dump()});
    db.commit();
    return Json{{"id", key}, {"state", "received"}};
  });
}
Result<Json> SqliteStore::settlement_status(std::string key) const {
  return guarded([&] {
    auto db = impl_->db();
    auto r = db.one("SELECT body FROM settlement_requests WHERE id=?", {key});
    ensure(!r.is_null(), "SETTLEMENT_NOT_FOUND");
    return parse(r["body"]);
  });
}
Result<Json> SqliteStore::settlement_events(std::uint64_t after, std::size_t limit) const {
  return guarded([&] {
    integer(after, 0, INT64_MAX);
    integer(limit, 1, 1000);
    auto db = impl_->db();
    Json result = Json::array();
    for (const auto &r :
         db.rows("SELECT body FROM settlement_events WHERE sequence>? ORDER BY sequence LIMIT ?",
                 {after, limit}))
      result.push_back(parse(r["body"]));
    return result;
  });
}
Result<std::optional<WorkResult>> SqliteStore::process_settlement() const {
  return guarded([&]() -> std::optional<WorkResult> {
    // Shared admission fence establishes a serializable boundary with trade
    // changes, policy edits and close. Complete books can execute independently.
    FileLock lock(impl_->directory / "locks/admission");
    auto db = impl_->db(true);
    auto row =
        db.one("SELECT * FROM settlement_requests WHERE state='pending' ORDER BY rowid LIMIT 1");
    if (row.is_null())
      return std::nullopt;
    const auto id = row["id"].get<std::string>();
    try {
      const auto submitted = parse(row["request"]);
      const auto &request = submitted.at("request");
      const std::set<std::string> fields = {"account",  "trade_record", "effective_at", "cash",
                                            "currency", "reverses",     "source"};
      ensure(request.size() == fields.size(), "INVALID_SETTLEMENT_REQUEST");
      for (const auto &[key, value] : request.items())
        ensure(fields.contains(key), "INVALID_SETTLEMENT_REQUEST");
      for (const auto *key : {"account", "trade_record", "effective_at", "cash", "currency"})
        token(request.at(key));
      ensure(request.at("source").is_object() && !request.at("source").empty(),
             "SETTLEMENT_SOURCE_REQUIRED");
      ensure(request.at("reverses").is_null() || request.at("reverses").is_string(),
             "INVALID_SETTLEMENT_REVERSAL");
      LifecycleLedger ledger;
      auto records = db.rows("SELECT canonical FROM ledger_records ORDER BY sequence LIMIT 100001");
      ensure(records.size() <= 100000, "SETTLEMENT_LEDGER_LIMIT");
      for (const auto &record : records) {
        const auto &bytes = record["canonical"].get_binary();
        auto decoded = serialization::decode_lifecycle_record(std::as_bytes(std::span{bytes}));
        ensure(bool(decoded) && bool(ledger.accept(decoded->draft)), "SETTLEMENT_LEDGER_INVALID");
      }
      const auto *trade = accounting::settlement_trade(ledger, request.at("trade_record"));
      ensure(trade != nullptr, "SETTLEMENT_TRADE_REQUIRED");
      std::vector<accounting::SettlementEvent> prior;
      for (const auto &r : db.rows("SELECT body FROM settlement_events ORDER BY sequence"))
        prior.push_back(take(settlement_event(parse(r["body"]))));
      Json event = request;
      event.erase("source");
      event["source"] = request["source"];
      event["id"] = id;
      event["sequence"] = prior.size() + 1;
      event["ledger_through"] = ledger.records().size();
      event["economic_event"] = trade->economic_event_id().value();
      event["source_record"] = "settlement-source-" + framed({id, row["request"]});
      const auto prior_knowledge =
          prior.empty() ? std::int64_t{0} : prior.back().recorded_at.time_since_epoch().count();
      const auto ledger_knowledge =
          db.one("SELECT recorded_ns FROM ledger_state WHERE id=1")["recorded_ns"]
              .get<std::int64_t>();
      event["recorded_at"] = timestamp(std::max({impl_->now(), prior_knowledge, ledger_knowledge}));
      event["actor"] = submitted.at("actor");
      auto typed = take(settlement_event(event));
      auto validated = accounting::validate_settlement(ledger, prior, typed);
      ensure(bool(validated), validated ? "" : validated.error().code);
      auto closed = db.one("SELECT through_ns FROM period_closes ORDER BY through_ns DESC LIMIT 1");
      ensure(closed.is_null() || typed.effective_at.time_since_epoch().count() >
                                     closed["through_ns"].get<std::int64_t>(),
             "PERIOD_CLOSED");
      db.exec("INSERT INTO settlement_events VALUES (?,?,?)",
              {event["sequence"], id, event.dump()});
      db.exec("UPDATE settlement_requests SET state='committed',body=? WHERE id=?",
              {Json{{"id", id}, {"state", "committed"}, {"event", event}}.dump(), id});
      impl_->fault("settlement_before_commit");
      db.commit();
      impl_->fault("settlement_after_commit");
      return WorkResult{id, "settlement", "committed", ""};
    } catch (const Failure &e) {
      if (e.retryable)
        throw;
      db.exec("UPDATE settlement_requests SET state='invalid',body=? WHERE id=?",
              {Json{{"id", id},
                    {"state", "invalid"},
                    {"code", e.code},
                    {"request", parse(row["request"])}}
                   .dump(),
               id});
      db.commit();
      return WorkResult{id, "settlement", "invalid", e.code};
    } catch (const Json::exception &) {
      db.exec("UPDATE settlement_requests SET state='invalid',body=? WHERE id=?",
              {Json{{"id", id},
                    {"state", "invalid"},
                    {"code", "INVALID_SETTLEMENT_REQUEST"},
                    {"request", parse(row["request"])}}
                   .dump(),
               id});
      db.commit();
      return WorkResult{id, "settlement", "invalid", "INVALID_SETTLEMENT_REQUEST"};
    }
  });
}
} // namespace luca::ingestion
