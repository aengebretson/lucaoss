#include "internal.hpp"
namespace luca::ingestion {
using namespace detail;
void SqliteStore::Impl::enqueue(Db &db, std::string kind, std::string subject,
                                std::string import_id) const {
  db.exec("INSERT INTO work(id,kind,subject_id,state,import_id,ready_ns) VALUES "
          "(?,?,?,'queued',?,?) ON CONFLICT(kind,subject_id) DO UPDATE SET "
          "state='queued',owner=NULL,generation=work.generation+1,lease_until=0,ready_ns=excluded."
          "ready_ns,attempts=0,last_error=NULL",
          {uuid(), kind, subject, import_id, now()});
}
std::int64_t SqliteStore::Impl::event(Db &db, const std::string &id, std::string type,
                                      Json subjects) const {
  db.exec("UPDATE progress SET revision=revision+1 WHERE import_id=?", {id});
  auto revision = db.one("SELECT revision FROM progress WHERE import_id=?", {id})["revision"]
                      .get<std::int64_t>();
  if (subjects.size() > 32)
    subjects.erase(subjects.begin() + 32, subjects.end());
  Json body = {{"event_id", uuid()},
               {"import_id", id},
               {"revision", std::to_string(revision)},
               {"type", type},
               {"occurred_at", timestamp(now())},
               {"subject_ids", subjects},
               {"links", Json::object()}};
  db.exec("INSERT INTO events VALUES (?,?,?)", {id, revision, body.dump()});
  return revision;
}
void SqliteStore::Impl::history(Db &db, const Json &ids, std::int64_t revision) const {
  for (const auto &id : ids) {
    auto row = db.one("SELECT * FROM staged_records WHERE id=?", {id});
    Json state = {{"validation", row["validation"]}, {"commitment", row["commitment"]},
                  {"group_id", row["group_id"]},     {"run_id", row["run_id"]},
                  {"commit_id", row["commit_id"]},   {"ledger_revision_examined", nullptr}};
    auto run = row["run_id"].is_null()
                   ? Json()
                   : db.one("SELECT body FROM validation_runs WHERE id=?", {row["run_id"]});
    if (!run.is_null()) {
      auto body = parse(run["body"]);
      state["context_id"] = body["context_id"];
      state["ledger_revision_examined"] = body.value("ledger_revision_examined", Json());
    } else {
      auto group =
          row["group_id"].is_null()
              ? Json()
              : db.one("SELECT context_id FROM admission_groups WHERE id=?", {row["group_id"]});
      state["context_id"] =
          group.is_null() ? parse(import(db, row["import_id"])["metadata"])["validation_context_id"]
                          : group["context_id"];
    }
    db.exec("INSERT INTO record_history VALUES (?,?,?)", {id, revision, state.dump()});
  }
}
void SqliteStore::Impl::counts(Db &db, const std::string &id, const Json &vd,
                               const Json &cd) const {
  auto row = db.one("SELECT vc,cc FROM progress WHERE import_id=?", {id});
  auto vc = parse(row["vc"]), cc = parse(row["cc"]);
  for (auto pair : {std::pair{&vc, &vd}, std::pair{&cc, &cd}})
    for (const auto &[key, value] : pair.second->items()) {
      auto next = pair.first->value(key, std::int64_t{0}) + value.get<std::int64_t>();
      ensure(next >= 0, "STATE_CORRUPT");
      (*pair.first)[key] = next;
    }
  db.exec("UPDATE progress SET vc=?,cc=? WHERE import_id=?", {vc.dump(), cc.dump(), id});
}
void SqliteStore::Impl::state(Db &db, const Json &row, Json &vd, Json &cd, Json validation,
                              Json commitment, Json run, Json commit) const {
  auto delta = [](Json &counters, const Json &from, const Json &to) {
    const auto a = text(from), b = text(to);
    counters[a] = counters.value(a, std::int64_t{0}) - 1;
    counters[b] = counters.value(b, std::int64_t{0}) + 1;
  };
  if (!validation.is_null())
    delta(vd, row["validation"], validation);
  else
    validation = row["validation"];
  if (!commitment.is_null())
    delta(cd, row["commitment"], commitment);
  else
    commitment = row["commitment"];
  db.exec("UPDATE staged_records SET validation=?,commitment=?,run_id=?,commit_id=? WHERE id=?",
          {validation, commitment, run.is_null() ? row["run_id"] : run,
           commit.is_null() ? row["commit_id"] : commit, row["id"]});
}
bool SqliteStore::Impl::pipeline(Db &db, const std::string &id) const {
  auto transfer = db.one("SELECT t.id,t.queue_reserved FROM transfers t JOIN imports i ON "
                         "i.transfer_id=t.id WHERE i.id=?",
                         {id});
  if (transfer["queue_reserved"] == 1)
    return true;
  if (db.one("SELECT slots FROM capacity WHERE id=1")["slots"] >= limits["max_pending_imports"])
    return false;
  db.exec("UPDATE transfers SET queue_reserved=1 WHERE id=?", {transfer["id"]});
  db.exec("UPDATE capacity SET slots=slots+1 WHERE id=1");
  return true;
}
void SqliteStore::Impl::release(Db &db, const std::string &id) const {
  if (!db.one("SELECT 1 FROM work WHERE import_id=? AND state IN ('queued','running','retry_wait') "
              "LIMIT 1",
              {id})
           .is_null())
    return;
  auto row = db.one("SELECT t.id,t.queue_reserved FROM transfers t JOIN imports i ON "
                    "i.transfer_id=t.id WHERE i.id=?",
                    {id});
  if (row["queue_reserved"] == 1) {
    db.exec("UPDATE transfers SET queue_reserved=0 WHERE id=?", {row["id"]});
    db.exec("UPDATE capacity SET slots=slots-1 WHERE id=1");
  }
}
void SqliteStore::Impl::wake() const {
  auto db = this->db(true);
  auto rows = db.rows(
      "SELECT DISTINCT g.* FROM admission_groups g JOIN dependencies d ON d.group_id=g.id WHERE "
      "g.validation='blocked' AND g.commitment='not_committed' AND ((d.kind='record' AND "
      "EXISTS(SELECT 1 FROM ledger_records l WHERE l.record_id=d.key)) OR (d.kind='snapshot' AND "
      "EXISTS(SELECT 1 FROM reference_snapshots r WHERE r.id=d.key))) LIMIT 100");
  for (const auto &row : rows)
    if (pipeline(db, row["import_id"])) {
      db.exec("DELETE FROM dependencies WHERE group_id=?", {row["id"]});
      enqueue(db, "validate_group", row["id"], row["import_id"]);
      event(db, row["import_id"], "validation.queued", Json::array({row["id"]}));
    }
  db.commit();
}
void SqliteStore::Impl::diagnostic(Db &db, std::string id, Json error, Json record, Json group,
                                   Json run, std::string classification) const {
  const auto diagnostic_id =
      run.is_null() ? sha(Json::array({id, record.is_null() ? Json() : record["id"], error["code"],
                                       error.value("field_path", std::string())})
                              .dump())
                    : uuid();
  Json body = {{"diagnostic_id", diagnostic_id},
               {"scope", !record.is_null()  ? "record"
                         : !group.is_null() ? "group"
                                            : "import"},
               {"import_id", id},
               {"record_id", record.is_null() ? Json() : record["id"]},
               {"group_id", group},
               {"run_id", run},
               {"code", error["code"]},
               {"severity", "error"},
               {"message", error.value("message", std::string("Validation failed."))},
               {"field_path", error.value("field_path", std::string())},
               {"locator", record.is_null() ? Json() : parse(record["body"])["locator"]},
               {"related_ids", error.value("related_ids", Json::array())},
               {"classification", classification},
               {"retryable", classification == "dependency"},
               {"details", error.value("details", Json::object())}};
  db.exec("INSERT OR IGNORE INTO diagnostics(id,import_id,group_id,record_id,run_id,code,body) "
          "VALUES (?,?,?,?,?,?,?)",
          {diagnostic_id, id, group, body["record_id"], run, error["code"], body.dump()});
}
Json SqliteStore::Impl::status(Db &db, const std::string &id) const {
  auto row = import(db, id), progress = db.one("SELECT * FROM progress WHERE import_id=?", {id});
  auto validation = parse(progress["vc"]), commitment = parse(progress["cc"]);
  Json vc = Json::object(), cc = Json::object();
  for (const auto *key : {"pending", "running", "valid", "invalid", "blocked"})
    vc[key] = validation.value(key, 0);
  for (const auto *key : {"not_committed", "queued", "committed", "conflict"})
    cc[key] = commitment.value(key, 0);
  std::set<std::string> states;
  for (const auto &work : db.rows("SELECT DISTINCT state FROM work WHERE import_id=?", {id}))
    states.insert(work["state"]);
  std::string health = "idle";
  for (const auto *next : {"attention_required", "running", "queued", "retry_wait"})
    if (states.contains(next)) {
      health = next;
      break;
    }
  return {{"import_id", id},
          {"revision", std::to_string(progress["revision"].get<std::int64_t>())},
          {"receipt", parse(row["receipt"])},
          {"metadata", parse(row["metadata"])},
          {"validation_context", parse(row["context"])},
          {"enumeration", {{"state", progress["enumeration"]}, {"records", progress["total"]}}},
          {"validation_counts", vc},
          {"commitment_record_counts", cc},
          {"work_health", health},
          {"import_error_count", progress["errors"]},
          {"all_records_committed", progress["enumeration"] == "complete" &&
                                        progress["errors"] == 0 &&
                                        cc["committed"] == progress["total"]},
          {"event_cursor", std::to_string(progress["revision"].get<std::int64_t>())}};
}
Json SqliteStore::Impl::record_public(Db &db, const Json &row, Json state) const {
  auto data = parse(row["body"]);
  if (state.is_null())
    state = row;
  auto run = state["run_id"].is_null()
                 ? Json()
                 : db.one("SELECT body FROM validation_runs WHERE id=?", {state["run_id"]});
  Json context = run.is_null() ? Json::object() : parse(run["body"]);
  if (run.is_null()) {
    auto group =
        state["group_id"].is_null()
            ? Json()
            : db.one("SELECT context_id FROM admission_groups WHERE id=?", {state["group_id"]});
    context["context_id"] =
        group.is_null() ? parse(import(db, row["import_id"])["metadata"])["validation_context_id"]
                        : group["context_id"];
  }
  return {{"record_id", row["id"]},
          {"ordinal", row["ordinal"]},
          {"source_record_id", data["evidence_id"]},
          {"locator", data["locator"]},
          {"canonical_record_id", data["canonical_id"]},
          {"economic_event_id", data["economic_id"]},
          {"group_id", state["group_id"]},
          {"validation",
           {{"state", state["validation"]},
            {"run_id", state["run_id"]},
            {"context_id", state.value("context_id", context.value("context_id", Json()))},
            {"ledger_revision_examined",
             state.value("ledger_revision_examined",
                         context.value("ledger_revision_examined", Json()))}}},
          {"commitment", {{"state", state["commitment"]}, {"commit_id", state["commit_id"]}}}};
}
Result<Json> SqliteStore::status(std::string id) const {
  return guarded([&] {
    auto db = impl_->db();
    return impl_->status(db, id);
  });
}
Result<Json> SqliteStore::record(std::string id, std::string record_id, std::string run) const {
  return guarded([&] {
    auto db = impl_->db();
    impl_->import(db, id);
    auto row = db.one("SELECT * FROM staged_records WHERE import_id=? AND id=?", {id, record_id});
    ensure(!row.is_null(), "NOT_FOUND");
    auto result = impl_->record_public(db, row);
    if (!run.empty()) {
      auto old =
          db.one("SELECT body FROM outcomes WHERE run_id=? AND record_id=?", {run, record_id});
      ensure(!old.is_null(), "NOT_FOUND");
      result["historical_validation"] = parse(old["body"]);
    }
    return result;
  });
}
Result<Json> SqliteStore::group(std::string id, std::string group_id) const {
  return guarded([&] {
    auto db = impl_->db();
    auto row = db.one("SELECT * FROM admission_groups WHERE import_id=? AND id=?", {id, group_id});
    ensure(!row.is_null(), "NOT_FOUND");
    return row;
  });
}
Result<Json> SqliteStore::register_reference(std::string id, Json definition) const {
  return guarded([&] {
    auto &s = *impl_;
    token(id);
    ensure(definition.is_object() && definition.size() == 2 &&
               definition.at("accounts").is_array() && definition.at("instruments").is_array(),
           "INVALID_ENVELOPE");
    ensure(definition["accounts"].size() + definition["instruments"].size() <= 200000,
           "LIMIT_EXCEEDED");
    for (const auto *key : {"accounts", "instruments"}) {
      std::set<std::string> values;
      for (const auto &value : definition[key])
        values.insert(token(value));
      definition[key] = values;
    }
    const auto body = definition.dump(), digest = sha(body);
    {
      auto db = s.db(true);
      auto old = db.one("SELECT digest FROM reference_snapshots WHERE id=?", {id});
      ensure(old.is_null() || old["digest"] == digest, "REFERENCE_CONFLICT");
      db.exec("INSERT OR IGNORE INTO reference_snapshots VALUES (?,?,?)", {id, digest, body});
      db.commit();
    }
    s.wake();
    return Json{{"snapshot_id", id}, {"digest", digest}};
  });
}
Result<Json> SqliteStore::register_context(std::string id, Json definition) const {
  return guarded([&] {
    auto &s = *impl_;
    token(id);
    ensure(definition.is_object() && definition.size() == 5, "INVALID_CONFIGURATION");
    for (const auto *key : {"adapter", "normalization", "rules", "engine", "reference_snapshot"})
      token(definition.at(key));
    auto db = s.db(true);
    auto config = parse(db.one("SELECT value FROM config WHERE id=1")["value"]);
    ensure(!config["contexts"].contains(id) || config["contexts"][id] == definition,
           "CONTEXT_CONFLICT");
    config["contexts"][id] = definition;
    db.exec("UPDATE config SET value=? WHERE id=1", {config.dump()});
    db.exec("INSERT OR IGNORE INTO context_history VALUES (?,?,?)",
            {id, definition.dump(), s.now()});
    db.commit();
    return Json{{"context_id", id}};
  });
}
Result<Json> SqliteStore::revalidate(std::string id, std::string key, Json request) const {
  return guarded([&] {
    auto &s = *impl_;
    token(key, 128);
    auto context_id = token(request.at("validation_context_id"));
    auto selection = request.at("groups");
    ensure(selection.is_array() && !selection.empty() && selection.size() <= 1000,
           "LIMIT_EXCEEDED");
    std::set<std::string> ids;
    for (auto &row : selection) {
      ensure(row.is_object() && row.size() == 2 && row.contains("group_id") &&
                 row.contains("expected_revision"),
             "INVALID_SELECTION");
      auto group = token(row["group_id"]);
      ensure(ids.insert(group).second, "INVALID_SELECTION");
      if (row["expected_revision"].is_number_integer())
        row["expected_revision"] = std::to_string(row["expected_revision"].get<std::int64_t>());
      token(row["expected_revision"]);
    }
    std::sort(selection.begin(), selection.end(),
              [](const Json &a, const Json &b) { return a["group_id"] < b["group_id"]; });
    request["groups"] = selection;
    auto identity = sha(id + std::string(1, '\0') + key);
    auto db = s.db(true);
    s.import(db, id);
    auto prior =
        db.one("SELECT body FROM revalidation_requests WHERE id=?", {"revalidate-" + identity});
    if (!prior.is_null()) {
      auto old = parse(prior["body"]);
      ensure(old["request"] == request.dump(), "IDEMPOTENCY_CONFLICT");
      return old["receipt"];
    }
    auto context = s.context(db, context_id);
    Json groups = Json::array();
    for (auto &item : selection) {
      auto group = db.one("SELECT * FROM admission_groups WHERE id=? AND import_id=?",
                          {item["group_id"], id});
      ensure(!group.is_null() &&
                 std::to_string(group["revision"].get<std::int64_t>()) ==
                     text(item["expected_revision"]) &&
                 group["commitment"] != "committed",
             "STALE_SELECTION");
      auto old = s.context(db, group["context_id"]);
      ensure(old["adapter"] == context["adapter"] &&
                 old["normalization"] == context["normalization"],
             "INCOMPATIBLE_CONTEXT");
      groups.push_back(group);
    }
    ensure(s.pipeline(db, id), "CAPACITY_UNAVAILABLE", true);
    for (const auto &group : groups) {
      db.exec("UPDATE admission_groups SET "
              "context_id=?,revision=revision+1,validation='pending',commitment='not_committed',"
              "run_id=NULL WHERE id=?",
              {context_id, group["id"]});
      Json vd = Json::object(), cd = Json::object();
      for (const auto &record :
           db.rows("SELECT * FROM staged_records WHERE group_id=?", {group["id"]}))
        s.state(db, record, vd, cd, "pending", "not_committed");
      db.exec("UPDATE staged_records SET run_id=NULL WHERE group_id=?", {group["id"]});
      s.counts(db, id, vd, cd);
      db.exec("DELETE FROM dependencies WHERE group_id=?", {group["id"]});
      db.exec("UPDATE work SET state='superseded',generation=generation+1,owner=NULL WHERE "
              "subject_id=? AND kind IN ('validate_group','admit_group')",
              {group["id"]});
      s.enqueue(db, "validate_group", group["id"], id);
    }
    Json subjects = Json::array();
    for (const auto &group : groups)
      subjects.push_back(group["id"]);
    auto revision = s.event(db, id, "validation.queued", subjects);
    for (const auto &group : groups) {
      Json records = Json::array();
      for (auto &row : db.rows("SELECT id FROM staged_records WHERE group_id=?", {group["id"]}))
        records.push_back(row["id"]);
      s.history(db, records, revision);
    }
    Json receipt = {{"revalidation_id", identity},
                    {"import_id", id},
                    {"status_url", "/v1/books/" + quote(s.book) + "/imports/" + id}};
    db.exec("INSERT INTO revalidation_requests VALUES (?,?,?)",
            {"revalidate-" + identity,
             Json{{"request", request.dump()}, {"receipt", receipt}}.dump(), s.now()});
    db.commit();
    return receipt;
  });
}
Result<Json> SqliteStore::page(std::string collection, std::string id, Json filters,
                               std::string cursor, std::size_t limit) const {
  return guarded([&] {
    auto &s = *impl_;
    integer(limit, 1, s.limits["max_page_size"]);
    ensure(filters.is_object(), "INVALID_FILTER");
    const std::map<std::string, std::set<std::string>> permitted = {
        {"records", {"validation_state", "commitment_state", "group_id", "commit_id"}},
        {"diagnostics", {"record_id", "group_id", "run_id", "code"}},
        {"validation_runs", {"group_id"}},
        {"commits", {"group_id"}}};
    ensure(permitted.contains(collection), "INVALID_FILTER");
    for (const auto &[key, value] : filters.items()) {
      ensure(permitted.at(collection).contains(key), "INVALID_FILTER");
      token(value);
    }
    auto db = s.db();
    s.import(db, id);
    const bool records = collection == "records";
    auto maximum =
        records
            ? db.one("SELECT revision AS n FROM progress WHERE import_id=?", {id})["n"]
                  .get<std::int64_t>()
            : db.one("SELECT coalesce(max(seq),0) AS n FROM " + collection + " WHERE import_id=?",
                     {id})["n"]
                  .get<std::int64_t>();
    std::int64_t snapshot = maximum, after = 0;
    if (!cursor.empty()) {
      Json value;
      try {
        value = Json::parse(unhex(cursor));
      } catch (const Json::exception &) {
        fail("INVALID_CURSOR");
      }
      ensure(value.is_array() && value.size() == 8 && value[0] == s.tenant && value[1] == s.book &&
                 value[2] == collection && value[3] == id && value[4] == filters,
             "INVALID_CURSOR");
      snapshot = integer(value[5], 0, maximum);
      after = integer(value[6], 0, INT64_MAX);
      ensure(integer(value[7], 0, INT64_MAX) > s.now(), "CURSOR_EXPIRED");
    }
    std::string sql;
    std::vector<Json> values = {id, after, snapshot};
    if (records)
      sql = "SELECT r.*,h.body AS history FROM staged_records r JOIN record_history h ON "
            "h.record_id=r.id WHERE r.import_id=? AND r.ordinal>? AND h.revision=(SELECT "
            "max(x.revision) FROM record_history x WHERE x.record_id=r.id AND x.revision<=?)";
    else
      sql = "SELECT seq,body FROM " + collection + " WHERE import_id=? AND seq>? AND seq<=?";
    for (const auto &[key, value] : filters.items()) {
      std::string field = key == "validation_state"   ? "validation"
                          : key == "commitment_state" ? "commitment"
                                                      : key;
      sql += records ? " AND json_extract(h.body,'$." + field + "')=?" : " AND " + field + "=?";
      values.push_back(value);
    }
    sql += records ? " ORDER BY r.ordinal LIMIT ?" : " ORDER BY seq LIMIT ?";
    values.push_back(limit + 1);
    auto rows = db.query(sql, values);
    Json items = Json::array();
    for (std::size_t i = 0; i < std::min(limit, rows.size()); ++i)
      items.push_back(records ? s.record_public(db, rows[i], parse(rows[i]["history"]))
                              : parse(rows[i]["body"]));
    Json next = nullptr;
    if (rows.size() > limit)
      next = hex(
          Json::array({s.tenant, s.book, collection, id, filters, snapshot,
                       rows[limit - 1][records ? "ordinal" : "seq"], s.now() + 900LL * 1000000000})
              .dump());
    return Json{
        {"items", items}, {"snapshot_revision", std::to_string(snapshot)}, {"next_cursor", next}};
  });
}
Result<Json> SqliteStore::events(std::string id, std::uint64_t after, std::size_t limit) const {
  return guarded([&] {
    auto &s = *impl_;
    integer(limit, 1, s.limits["max_page_size"]);
    auto db = s.db();
    s.import(db, id);
    auto high = db.one("SELECT revision FROM progress WHERE import_id=?", {id})["revision"];
    ensure(after <= high.get<std::uint64_t>(), "INVALID_CURSOR");
    Json items = Json::array();
    auto rows =
        db.rows("SELECT * FROM events WHERE import_id=? AND revision>? ORDER BY revision LIMIT ?",
                {id, after, limit});
    for (auto &row : rows)
      items.push_back(parse(row["body"]));
    return Json{{"items", items},
                {"next_after", rows.empty()
                                   ? std::to_string(after)
                                   : std::to_string(rows.back()["revision"].get<std::int64_t>())},
                {"high_watermark", std::to_string(high.get<std::int64_t>())}};
  });
}
Result<Json> SqliteStore::capabilities() const {
  return guarded([&] {
    auto &s = *impl_;
    auto db = s.db();
    auto cfg = parse(db.one("SELECT value FROM config WHERE id=1")["value"]),
         processing = db.one("SELECT body FROM processing_config WHERE id=1");
    return Json{{"implementation", "native-staged-ingestion/2"},
                {"tenant_id", s.tenant},
                {"book_id", s.book},
                {"limits", s.limits},
                {"contexts", cfg["contexts"]},
                {"durability_profile", "local-fsync-" + s.mode + "-v1"},
                {"sqlite_version", sqlite3_libversion()},
                {"usage", db.one("SELECT bytes,slots FROM capacity WHERE id=1")},
                {"retention", "no automatic receipt/payload purge"},
                {"validation_available", true},
                {"admission_available", true},
                {"python_required", false},
                {"http_available", false},
                {"processing_limits", processing.is_null() ? Json() : parse(processing["body"])}};
  });
}
Result<Json> SqliteStore::book_state() const {
  return guarded([&] {
    auto db = impl_->db();
    return db.one("SELECT revision,last_sequence,recorded_ns FROM ledger_state WHERE id=1");
  });
}
Result<Json> SqliteStore::ledger_records(std::uint64_t after, std::size_t limit) const {
  return guarded([&] {
    integer(limit, 1, impl_->limits["max_page_size"]);
    integer(after, 0, INT64_MAX);
    auto db = impl_->db();
    auto rows = db.rows("SELECT * FROM ledger_records WHERE sequence>? ORDER BY sequence LIMIT ?",
                        {after, limit});
    for (auto &row : rows) {
      const auto &bytes = row["canonical"].get_binary();
      row["canonical"] = hex(std::as_bytes(std::span{bytes}));
    }
    return rows;
  });
}
Result<Json> SqliteStore::work_attempts(std::string id, std::uint64_t after,
                                        std::size_t limit) const {
  return guarded([&] {
    integer(limit, 1, impl_->limits["max_page_size"]);
    integer(after, 0, INT64_MAX);
    auto db = impl_->db();
    impl_->import(db, id);
    auto rows = db.rows("SELECT a.seq,a.body FROM attempts a JOIN work w ON w.id=a.work_id WHERE "
                        "w.import_id=? AND a.seq>? ORDER BY a.seq LIMIT ?",
                        {id, after, limit});
    Json items = Json::array();
    for (auto &row : rows)
      items.push_back(parse(row["body"]));
    return Json{{"items", items}, {"next_after", rows.empty() ? Json(after) : rows.back()["seq"]}};
  });
}
} // namespace luca::ingestion
