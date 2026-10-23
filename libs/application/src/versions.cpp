#include "internal.hpp"
#include "luca/serialization/canonical.hpp"
#include <set>
#include <sstream>
namespace luca::application::detail {
bool submission(Operation op) {
  return op == Operation::submit_import || op == Operation::submit_settlement ||
         op == Operation::submit_refund || op == Operation::edit_transaction ||
         op == Operation::delete_transaction;
}
Json transaction_evidence(const BookBinding &b, const Json &a) {
  auto input = take(b.store.transaction_input(a.at("record_id")));
  const auto &locator = input.at("locator");
  const auto size = locator.at("byte_length").get<std::uint64_t>();
  require(size <= 65536, "PROJECTION_LIMIT_EXCEEDED");
  std::ostringstream bytes;
  take(b.store.payload(input.at("import_id"), bytes, locator.at("byte_offset"), size));
  const auto text = bytes.str();
  require(text.size() == size && serialization::detail::sha256_hex(std::as_bytes(
                                     std::span{text})) == locator.at("sha256").get<std::string>(),
          "SOURCE_EVIDENCE_CORRUPT");
  for (const auto *key : {"byte_offset", "byte_length", "line"})
    input["locator"][key] = std::to_string(input["locator"][key].get<std::uint64_t>());
  input["payload_utf8"] = text;
  input["authority"] = "source_evidence_only";
  // Raw bytes are retained for lossless evidence, including any source envelope.
  return input;
}
Json submit_version(const BookBinding &b, const Json &a, bool deletion) {
  const auto input = take(b.store.transaction_input(a.at("original_record_id")));
  const auto &old = input.at("transaction");
  const auto source = input.at("source_metadata").at("source_id").get<std::string>();
  require(std::find(b.submit_sources.begin(), b.submit_sources.end(), source) !=
              b.submit_sources.end(),
          "SOURCE_NOT_AUTHORIZED");
  require(std::any_of(
              b.accounts.begin(), b.accounts.end(),
              [&](const auto &v) { return v.account == old.at("account_id").get<std::string>(); }),
          "ACCOUNT_NOT_AUTHORIZED");
  const auto seed =
      Json::array({"luca.transaction-version/1", a.at("book_id"), source, a.at("idempotency_key"),
                   a.at("original_record_id"), deletion ? "delete" : "edit"})
          .dump();
  const auto id = "version-" + serialization::detail::sha256_hex(std::as_bytes(std::span{seed}));
  Json cancel = {{"record_id", id + "-void"},
                 {"economic_event_id", old.at("economic_event_id")},
                 {"account_id", old.at("account_id")},
                 {"action", "cancel"},
                 {"causal_record_id", old.at("record_id")},
                 {"void_at", a.at("void_at")},
                 {"atomic_group", id}};
  Json proof = {{"operation", deletion ? "delete_transaction" : "edit_transaction"},
                {"actor", b.transaction_actor},
                {"reason", a.at("reason")},
                {"original_record_id", a.at("original_record_id")},
                {"original_source_record_id", input.at("source_record_id")},
                {"command", a}};
  std::string body = Json{{"transaction", cancel}, {"source", proof}}.dump() + "\n";
  if (!deletion) {
    const auto text = a.at("replacement_event_utf8").get<std::string>();
    Json event;
    bool duplicate = false;
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int depth, Json::parse_event_t phase, Json &value) {
      require(depth <= 128, "INVALID_EVENT_JSON");
      if (phase == Json::parse_event_t::object_start)
        keys.emplace_back();
      else if (phase == Json::parse_event_t::object_end)
        keys.pop_back();
      else if (phase == Json::parse_event_t::key &&
               !keys.back().insert(value.get<std::string>()).second)
        duplicate = true;
      return true;
    };
    // Preserve malformed or ambiguous event text as invalid native input. Never
    // silently choose one duplicate financial field while constructing a pair.
    try {
      event = Json::parse(text, callback);
    } catch (const Json::exception &) {
      event = text;
    } catch (const ingestion::Failure &) {
      event = text;
    }
    if (duplicate)
      event = text;
    Json replacement = {{"record_id", id + "-replacement"},
                        {"economic_event_id", old.at("economic_event_id")},
                        {"account_id", old.at("account_id")},
                        {"action", "correct"},
                        {"causal_record_id", id + "-void"},
                        {"atomic_group", id},
                        {"event", event}};
    body += Json{{"transaction", replacement}, {"source", proof}}.dump() + "\n";
  }
  require(body.size() <= 65536, "LIMIT_EXCEEDED");
  const auto metadata = Json{{"source_id", source},
                             {"format", "luca.transactions.envelope.jsonl/1"},
                             {"validation_context_id", a.at("validation_context_id")},
                             {"atomicity", "whole_import"}};
  std::istringstream payload(body);
  auto accepted = b.store.receive(metadata, a.at("idempotency_key"), payload, body.size());
  if (!accepted && accepted.error().code == "STORAGE_UNAVAILABLE")
    throw ingestion::Failure{"OUTCOME_UNKNOWN", true};
  auto receipt = take(std::move(accepted));
  for (auto key : {"status_url", "records_url", "diagnostics_url"})
    receipt.erase(key);
  receipt["durable_acceptance"] = true;
  receipt["completion"] = "pending_or_in_progress";
  return receipt;
}
} // namespace luca::application::detail
