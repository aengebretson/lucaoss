#pragma once
#include "internal.hpp"
#include <queue>
namespace luca::ingestion::detail {
inline Json issue(std::string code, std::string field = "",
                  std::string message = "Input does not satisfy the transaction schema.") {
  return {{"code", code}, {"field_path", field}, {"message", message}};
}
inline bool supported(const Json &context) {
  return context["adapter"] == "luca.transactions.jsonl/1" &&
         context["normalization"] == "source-book-identity/1" &&
         context["rules"] == "cash-equity/1" && context["engine"] == "luca/0.1";
}
inline adapters::TransactionFields fields(const Json &p, std::int64_t recorded = 0) {
  return {p[0],  p[1],  p[2],  p[3],  p[4],  p[5], std::to_string(recorded), p[7], p[8], p[9],
          p[10], p[11], p[12], p[13], p[14], p[15]};
}
inline std::string pointer(std::string key) {
  std::string out;
  for (char c : key) {
    if (c == '~')
      out += "~0";
    else if (c == '/')
      out += "~1";
    else
      out += c;
  }
  return out;
}
// Source evidence time is retained metadata, not a financial nanosecond value.
// Preserve the original wire contract's full calendar range without narrowing.
inline bool source_timestamp(std::string_view value) {
  using namespace adapters::transaction_detail;
  if (value.size() < 20 || value.size() > 30 || value[10] != 'T' || value[13] != ':' ||
      value[16] != ':' || value.back() != 'Z' || value.substr(0, 4) == "0000")
    return false;
  auto day = date(value.substr(0, 10), "/source_event_at");
  auto hours = digits(value.substr(11, 2), ""), minutes = digits(value.substr(14, 2), ""),
       seconds = digits(value.substr(17, 2), "");
  if (!day || !hours || !minutes || !seconds || *hours > 23 || *minutes > 59 || *seconds > 59)
    return false;
  if (value.size() == 20)
    return true;
  return value.size() >= 22 && value[19] == '.' &&
         digits(value.substr(20, value.size() - 21), "").has_value();
}
inline Json parse_record(const SqliteStore::Impl &store, const std::string &import_id,
                         const Json &meta, const Json &context, std::uint64_t ordinal,
                         std::string_view raw, Json locator) {
  const auto evidence = "src-" + framed({import_id, std::to_string(ordinal)});
  Json result = {{"evidence_id", evidence}, {"locator", locator},      {"raw", nullptr},
                 {"packet", nullptr},       {"issues", Json::array()}, {"canonical_id", nullptr},
                 {"economic_id", nullptr},  {"raw_id", nullptr},       {"raw_causal", nullptr},
                 {"atomic_group", nullptr}, {"ordinal", ordinal},      {"context", context}};
  std::vector<std::set<std::string>> keys;
  bool duplicate = false;
  auto callback = [&](int depth, Json::parse_event_t event, Json &value) {
    ensure(depth <= 128, "JSON_DEPTH_EXCEEDED");
    if (event == Json::parse_event_t::object_start)
      keys.emplace_back();
    else if (event == Json::parse_event_t::object_end)
      keys.pop_back();
    else if (event == Json::parse_event_t::key &&
             !keys.back().insert(value.get<std::string>()).second)
      duplicate = true;
    return true;
  };
  Json data;
  try {
    data = Json::parse(raw, callback);
  } catch (const Failure &) {
    result["issues"].push_back(issue("PARSE_ERROR"));
    return result;
  } catch (const Json::exception &error) {
    const auto validation = serialization::detail::unicode_nfc::validate(raw);
    const bool utf8 =
        validation == serialization::detail::unicode_nfc::ValidationResult::invalid_utf8 ||
        std::string_view(error.what()).find("surrogate") != std::string_view::npos;
    result["issues"].push_back(issue(utf8 ? "INVALID_UTF8" : "PARSE_ERROR"));
    return result;
  }
  if (duplicate) {
    result["issues"].push_back(issue("DUPLICATE_FIELD"));
    return result;
  }
  if (!data.is_object()) {
    result["issues"].push_back(issue("RECORD_TYPE"));
    return result;
  }
  result["raw"] = data;
  const std::set<std::string> allowed = {
      "record_id",          "economic_event_id", "action",       "account_id", "causal_record_id",
      "external_record_id", "source_event_at",   "atomic_group", "event"};
  for (const auto &[key, value] : data.items())
    if (!allowed.contains(key))
      result["issues"].push_back(
          issue(key == "book_id" ? "CROSS_BOOK_DEPENDENCY" : "FIELD_UNKNOWN", "/" + pointer(key)));
  auto field = [&](const Json &object, std::string key, std::string path,
                   bool required = true) -> std::string {
    if (!object.contains(key) && !required)
      return "";
    try {
      return token(object.value(key, Json()));
    } catch (const Failure &) {
      result["issues"].push_back(
          issue(object.contains(key) ? "FIELD_TYPE" : "FIELD_REQUIRED", path));
      return "";
    }
  };
  const auto record = field(data, "record_id", "/record_id"),
             economic = field(data, "economic_event_id", "/economic_event_id"),
             action = field(data, "action", "/action"),
             account = field(data, "account_id", "/account_id");
  const auto causal = field(data, "causal_record_id", "/causal_record_id", action != "originate"),
             group = field(data, "atomic_group", "/atomic_group", false);
  for (const auto *optional : {"external_record_id", "source_event_at"})
    if (data.contains(optional))
      field(data, optional, "/" + std::string(optional));
  if (data.contains("source_event_at") &&
      (!data["source_event_at"].is_string() ||
       !source_timestamp(data["source_event_at"].get<std::string>())))
    result["issues"].push_back(issue("INVALID_TIMESTAMP", "/source_event_at"));
  result["raw_id"] = record.empty() ? Json() : Json(record);
  result["raw_causal"] = causal.empty() ? Json() : Json(causal);
  result["atomic_group"] = group.empty() ? Json() : Json(group);
  if (data.contains("atomic_group") && group.empty())
    result["membership_unknown"] = true;
  auto identity = [&](const std::string &kind, const std::string &label) {
    return label.empty() ? std::string()
                         : kind + "-" +
                               framed({"source-book-identity/1", store.tenant, store.book,
                                       meta["source_id"], kind, label});
  };
  result["canonical_id"] = record.empty() ? Json() : Json(identity("event", record));
  result["economic_id"] = economic.empty() ? Json() : Json(identity("economic", economic));
  auto event = data.value("event", Json::object());
  if (action == "cancel") {
    if (data.contains("event"))
      result["issues"].push_back(issue("FIELD_UNKNOWN", "/event"));
    event = Json::object();
  } else if (!event.is_object() || event.empty()) {
    result["issues"].push_back(issue("FIELD_REQUIRED", "/event"));
    event = Json::object();
  }
  const auto event_type = event.value("type", Json());
  const std::set<std::string> event_keys =
      event_type == "cash_movement"
          ? std::set<std::string>{"type", "effective_at", "currency", "amount"}
          : std::set<std::string>{"type",  "effective_at",   "instrument_id",  "quantity",
                                  "price", "quote_currency", "settlement_date"};
  for (const auto &[key, value] : event.items())
    if (!event_keys.contains(key))
      result["issues"].push_back(issue("FIELD_UNKNOWN", "/event/" + pointer(key)));
  Json values = Json::object();
  for (const auto &key : event_keys)
    values[key] = field(event, key, "/event/" + key, action != "cancel");
  if (action != "cancel" && event_type != "cash_movement" && event_type != "equity_trade")
    result["issues"].push_back(issue("UNSUPPORTED_EVENT_TYPE", "/event/type"));
  for (const auto *key : {"amount", "quantity", "price"})
    if (event.contains(key) && !event[key].is_string()) {
      const auto path = "/event/" + std::string(key);
      auto &errors = result["issues"];
      errors.erase(std::remove_if(errors.begin(), errors.end(),
                                  [&](const Json &error) { return error["field_path"] == path; }),
                   errors.end());
      errors.push_back(issue("INVALID_DECIMAL", path));
    }
  if (result["issues"].empty()) {
    result["packet"] = Json::array(
        {result["canonical_id"], result["economic_id"], action, account, identity("event", causal),
         evidence, "0", event_type.is_string() ? event_type : Json(""),
         values.value("effective_at", Json("")),
         values.value("currency", values.value("quote_currency", Json(""))),
         values.value("amount", Json("")), values.value("instrument_id", Json("")),
         values.value("quantity", Json("")), values.value("price", Json("")),
         values.value("settlement_date", Json("")), context["normalization"]});
    if (supported(context)) {
      auto value = adapters::transaction(fields(result["packet"]));
      if (!value)
        result["issues"].push_back(
            issue(value.error().code, value.error().field, value.error().message));
    }
  }
  std::sort(result["issues"].begin(), result["issues"].end(), [](const Json &a, const Json &b) {
    return std::pair{a["field_path"], a["code"]} < std::pair{b["field_path"], b["code"]};
  });
  return result;
}
inline std::vector<Json> groups_for(const Json &records, bool whole) {
  std::vector<std::size_t> parent(records.size());
  std::iota(parent.begin(), parent.end(), 0);
  auto root = [&](std::size_t i) {
    while (parent[i] != i) {
      parent[i] = parent[parent[i]];
      i = parent[i];
    }
    return i;
  };
  auto join = [&](std::size_t a, std::size_t b) { parent[root(a)] = root(b); };
  std::map<std::string, std::size_t> ids, labels, origins;
  for (std::size_t i = 0; i < records.size(); ++i) {
    const auto &row = records[i];
    for (auto pair : {std::pair{&ids, "raw_id"}, std::pair{&labels, "atomic_group"}})
      if (!row[pair.second].is_null()) {
        const auto key = text(row[pair.second]);
        auto [it, inserted] = pair.first->emplace(key, i);
        if (!inserted)
          join(i, it->second);
      }
    const auto action = row["raw"].is_object() ? row["raw"].value("action", Json()) : Json();
    if ((action == "originate" || action == "reverse") && !row["economic_id"].is_null()) {
      auto [it, inserted] = origins.emplace(text(row["economic_id"]), i);
      if (!inserted)
        join(i, it->second);
    }
    if (whole && i)
      join(i, 0);
  }
  for (std::size_t i = 0; i < records.size(); ++i)
    if (!records[i]["raw_causal"].is_null()) {
      auto found = ids.find(text(records[i]["raw_causal"]));
      if (found != ids.end())
        join(i, found->second);
    }
  std::map<std::size_t, std::size_t> group_index;
  std::vector<Json> groups;
  for (std::size_t i = 0; i < records.size(); ++i) {
    auto [it, inserted] = group_index.emplace(root(i), groups.size());
    if (inserted)
      groups.push_back(Json::array());
    groups[it->second].push_back(Json{{"id", records[i]["id"]}});
  }
  return groups;
}
inline Json causal_order(const Json &records) {
  std::map<std::string, std::size_t> ids;
  for (std::size_t i = 0; i < records.size(); ++i)
    if (!records[i]["raw_id"].is_null())
      ids[text(records[i]["raw_id"])] = i;
  std::vector<unsigned> degrees(records.size());
  std::vector<std::vector<std::size_t>> children(records.size());
  for (std::size_t i = 0; i < records.size(); ++i)
    if (!records[i]["raw_causal"].is_null()) {
      auto target = ids.find(text(records[i]["raw_causal"]));
      if (target != ids.end()) {
        ++degrees[i];
        children[target->second].push_back(i);
      }
    }
  std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> ready;
  for (std::size_t i = 0; i < records.size(); ++i)
    if (!degrees[i])
      ready.push(i);
  Json ordered = Json::array();
  while (!ready.empty()) {
    auto index = ready.top();
    ready.pop();
    ordered.push_back(records[index]);
    for (auto child : children[index])
      if (--degrees[child] == 0)
        ready.push(child);
  }
  return ordered.size() == records.size() ? ordered : records;
}
inline std::vector<LifecycleRecordDraft> drafts(const Json &records, std::int64_t now) {
  std::vector<LifecycleRecordDraft> result;
  result.reserve(records.size());
  for (const auto &row : records) {
    auto value = adapters::transaction(fields(row["packet"], now));
    ensure(value.has_value(), "INVALID_PREPARED_CANDIDATE");
    result.push_back(std::move(*value));
  }
  return result;
}
inline Json core_error(const LifecycleError &error) {
  std::string code = "ADMISSION_CONFLICT", classification = "conflict";
  const auto category = error.category_name();
  if (category == "causal_reference_missing") {
    code = "MISSING_PREDECESSOR";
    classification = "dependency";
  } else if (category == "causal_cycle" || category == "causal_self_reference") {
    code = "CAUSAL_CYCLE";
    classification = "invalid";
  } else if (category == "duplicate_identity")
    code = "DUPLICATE_IDENTITY";
  else if (category == "conflicting_lifecycle_successor")
    code = "CONFLICTING_SUCCESSOR";
  else if (category == "incompatible_account") {
    code = "INCOMPATIBLE_ACCOUNT";
    classification = "invalid";
  } else if (category == "incompatible_event_relationship") {
    code = "INVALID_LIFECYCLE_RELATIONSHIP";
    classification = "invalid";
  }
  auto result = issue(code, "", std::string(error.message()));
  result["classification"] = classification;
  result["canonical_id"] = error.record_id().value();
  result["related_ids"] = Json::array();
  if (error.causal_record_id())
    result["related_ids"].push_back(error.causal_record_id()->value());
  return result;
}
} // namespace luca::ingestion::detail
