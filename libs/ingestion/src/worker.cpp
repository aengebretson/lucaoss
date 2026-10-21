#include "transactions.hpp"
#include <condition_variable>
#include <deque>
#include <thread>
namespace luca::ingestion {
using namespace detail;
struct Worker::Impl {
  SqliteStore store;
  std::shared_ptr<SqliteStore::Impl> s;
  WorkerLimits limits;
  std::string owner = uuid();
  std::mutex mutex;
  LifecycleLedger ledger;
  std::int64_t sequence = 0, revision = 0, recorded_ns = 0;
  Impl(SqliteStore input, WorkerLimits settings)
      : store(std::move(input)), s(store.impl_), limits(settings) {
    ensure(limits.batch_records >= 1 && limits.batch_records <= 1000 &&
               limits.max_group_records >= 1 && limits.max_group_records <= 1000 &&
               limits.max_records >= 1 && limits.max_record_bytes >= 1 &&
               limits.max_ledger_records >= 1 && limits.lease_seconds >= 1 &&
               limits.max_attempts >= 1,
           "INVALID_WORKER_LIMITS");
    auto db = s->db(true);
    Json config = {{"max_records", limits.max_records},
                   {"max_record_bytes", limits.max_record_bytes},
                   {"max_group_records", limits.max_group_records},
                   {"max_ledger_records", limits.max_ledger_records}};
    auto old = db.one("SELECT body FROM processing_config WHERE id=1");
    ensure(old.is_null() || parse(old["body"]) == config, "INCOMPATIBLE_WORKER_LIMITS");
    db.exec("INSERT OR IGNORE INTO processing_config VALUES (1,?)", {config.dump()});
    db.commit();
  }
  void lease(Db &db, const Json &job) {
    auto row = db.one("SELECT * FROM work WHERE id=?", {job["id"]});
    ensure(!row.is_null() && row["state"] == "running" && row["owner"] == owner &&
               row["generation"] == job["generation"] &&
               row["lease_until"].get<std::int64_t>() > s->now(),
           "LEASE_LOST", true);
  }
  Json claim() {
    auto db = s->db(true);
    const auto now = s->now();
    ensure(now >= 0 &&
               now <= INT64_MAX - static_cast<std::int64_t>(limits.lease_seconds) * 1000000000,
           "CLOCK_RANGE");
    while (true) {
      auto row =
          db.one("SELECT * FROM work WHERE kind IN ('parse_import','validate_group','admit_group') "
                 "AND ((state IN ('queued','retry_wait') AND ready_ns<=?) OR (state='running' AND "
                 "lease_until<=?)) ORDER BY ready_ns,id LIMIT 1",
                 {now, now});
      if (row.is_null()) {
        db.commit();
        return nullptr;
      }
      if (row["attempts"] >= limits.max_attempts) {
        db.exec("UPDATE work SET state='attention_required',owner=NULL WHERE id=?", {row["id"]});
        s->event(db, row["import_id"], "work.attention_required", Json::array({row["id"]}));
        s->release(db, row["import_id"]);
        continue;
      }
      if (!s->pipeline(db, row["import_id"])) {
        db.commit();
        return nullptr;
      }
      db.exec(
          "UPDATE work SET "
          "state='running',owner=?,generation=generation+1,attempts=attempts+1,lease_until=? WHERE "
          "id=?",
          {owner, now + static_cast<std::int64_t>(limits.lease_seconds) * 1000000000, row["id"]});
      auto job = db.one("SELECT * FROM work WHERE id=?", {row["id"]});
      s->event(db, row["import_id"], "work.started", Json::array({row["id"]}));
      db.commit();
      return job;
    }
  }
  void complete(Db &db, const Json &job) {
    lease(db, job);
    db.exec("UPDATE work SET state='complete',owner=NULL,lease_until=0 WHERE id=?", {job["id"]});
    s->release(db, job["import_id"]);
  }
  void renew(const Json &job) {
    auto db = s->db(true);
    lease(db, job);
    db.exec("UPDATE work SET lease_until=? WHERE id=?",
            {s->now() + static_cast<std::int64_t>(limits.lease_seconds) * 1000000000, job["id"]});
    db.commit();
  }
  void failure(const Json &job, const Failure &error) {
    auto db = s->db(true);
    try {
      lease(db, job);
    } catch (const Failure &) {
      return;
    }
    const auto state = job["attempts"] >= limits.max_attempts || error.code == "LEDGER_CORRUPT" ||
                               error.code == "PAYLOAD_CORRUPT"
                           ? "attention_required"
                           : "retry_wait";
    Json body = {{"work_id", job["id"]},
                 {"generation", job["generation"]},
                 {"attempt", job["attempts"]},
                 {"code", error.code},
                 {"at", timestamp(s->now())}};
    db.exec("INSERT INTO attempts(work_id,body) VALUES (?,?)", {job["id"], body.dump()});
    const auto attempt = std::min<std::uint32_t>(6, job["attempts"].get<std::uint32_t>()),
               jitter =
                   static_cast<unsigned>(std::stoul(uuid().substr(0, 6), nullptr, 16) % 1000000000);
    db.exec("UPDATE work SET state=?,owner=NULL,lease_until=0,ready_ns=?,last_error=? WHERE id=?",
            {state, s->now() + std::min<std::int64_t>(60, 1LL << attempt) * 1000000000 + jitter,
             body.dump(), job["id"]});
    s->event(db, job["import_id"], std::string("work.") + state, Json::array({job["id"]}));
    s->release(db, job["import_id"]);
    db.commit();
  }
  Json sync(Db &db) {
    auto state = db.one("SELECT * FROM ledger_state WHERE id=1");
    ensure(sequence <= state["last_sequence"].get<std::int64_t>(), "LEDGER_CORRUPT");
    while (sequence < state["last_sequence"].get<std::int64_t>()) {
      auto rows = db.rows(
          "SELECT * FROM ledger_records WHERE sequence>? ORDER BY sequence LIMIT 1000", {sequence});
      ensure(!rows.empty(), "LEDGER_CORRUPT");
      for (const auto &row : rows) {
        const auto &binary = row["canonical"].get_binary();
        auto bytes = std::as_bytes(std::span{binary});
        ensure(row["sequence"] == sequence + 1 &&
                   serialization::detail::sha256_hex(bytes) == text(row["digest"]),
               "LEDGER_CORRUPT");
        auto decoded = serialization::decode_lifecycle_record(bytes);
        ensure(decoded && decoded->acceptance_sequence == static_cast<std::uint64_t>(sequence + 1),
               "LEDGER_CORRUPT");
        auto accepted = ledger.accept(decoded->draft);
        ensure(accepted.has_value(), "LEDGER_CORRUPT");
        ensure(serialization::canonical_bytes(accepted->get()) ==
                   serialization::CanonicalBytes(bytes.begin(), bytes.end()),
               "LEDGER_CORRUPT");
        ++sequence;
      }
    }
    ensure(sequence == state["last_sequence"], "LEDGER_CORRUPT");
    revision = state["revision"];
    recorded_ns = state["recorded_ns"];
    return state;
  }
  void publish_records(const Json &job, const Json &batch) {
    auto db = s->db(true);
    lease(db, job);
    const auto revision = s->event(db, job["import_id"], "import.enumerating");
    Json ids = Json::array(), vd = Json::object(), cd = Json::object();
    for (const auto &data : batch) {
      auto id = text(job["import_id"]) + ":" + std::to_string(data["ordinal"].get<std::uint64_t>());
      auto encoded = data.dump();
      auto old = db.one("SELECT body FROM staged_records WHERE id=?", {id});
      if (!old.is_null()) {
        ensure(old["body"] == encoded, "PAYLOAD_CORRUPT");
        continue;
      }
      const std::string state = data["issues"].empty() ? "pending" : "invalid";
      db.exec("INSERT INTO staged_records(id,import_id,ordinal,body,validation) VALUES (?,?,?,?,?)",
              {id, job["import_id"], data["ordinal"], encoded, state});
      auto row = db.one("SELECT * FROM staged_records WHERE id=?", {id});
      for (const auto &error : data["issues"])
        s->diagnostic(db, job["import_id"], error, row);
      ids.push_back(id);
      vd[state] = vd.value(state, 0) + 1;
      cd["not_committed"] = cd.value("not_committed", 0) + 1;
    }
    s->counts(db, job["import_id"], vd, cd);
    s->history(db, ids, revision);
    db.commit();
    s->fault("after_parse_batch");
    renew(job);
  }
  void parse_import(const Json &job) {
    Json receipt;
    {
      auto db = s->db();
      receipt = s->import(db, job["import_id"]);
    }
    auto metadata = parse(receipt["metadata"]), context = parse(receipt["context"]);
    std::ifstream input(s->payload_path(receipt["transfer_id"]), std::ios::binary);
    ensure(input.good(), "PAYLOAD_UNAVAILABLE");
    Hash digest;
    std::uint64_t offset = 0, ordinal = 0, line = 0;
    Json batch = Json::array(), failure = nullptr;
    std::string raw;
    raw.reserve(limits.max_record_bytes);
    while (input.peek() != std::char_traits<char>::eof()) {
      ++line;
      const auto begin = offset;
      raw.clear();
      Hash line_hash;
      bool blank = true, too_long = false;
      char c;
      while (input.get(c)) {
        ++offset;
        digest.update(std::string_view(&c, 1));
        line_hash.update(std::string_view(&c, 1));
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\v' && c != '\f')
          blank = false;
        if (raw.size() < limits.max_record_bytes)
          raw += c;
        else
          too_long = true;
        if (c == '\n')
          break;
      }
      if (blank && !too_long)
        continue;
      if (++ordinal > limits.max_records) {
        failure = issue("IMPORT_LIMIT_EXCEEDED");
        break;
      }
      Json locator = {{"byte_offset", begin},
                      {"byte_length", offset - begin},
                      {"line", line},
                      {"sha256", line_hash.finish()}};
      auto data = parse_record(*s, job["import_id"], metadata, context, ordinal,
                               too_long ? std::string_view() : std::string_view(raw), locator);
      if (too_long)
        data["issues"] = Json::array({issue("RECORD_LIMIT_EXCEEDED")});
      batch.push_back(std::move(data));
      if (batch.size() == limits.batch_records) {
        publish_records(job, batch);
        batch = Json::array();
      }
    }
    ensure(!input.bad(), "PAYLOAD_UNAVAILABLE");
    if (!batch.empty())
      publish_records(job, batch);
    if (failure.is_null())
      ensure(digest.finish() == text(parse(receipt["receipt"])["payload"]["digest"]),
             "PAYLOAD_CORRUPT");
    auto db = s->db(true);
    lease(db, job);
    auto rows = db.rows("SELECT * FROM staged_records WHERE import_id=? ORDER BY ordinal",
                        {job["import_id"]});
    Json records = Json::array(), ids = Json::array();
    bool declared = false, unknown = false, unparsed = false;
    for (const auto &row : rows) {
      auto data = parse(row["body"]);
      data["id"] = row["id"];
      ids.push_back(row["id"]);
      declared = declared || !data["atomic_group"].is_null();
      unknown = unknown || data.value("membership_unknown", false);
      unparsed = unparsed || data["raw"].is_null();
      records.push_back(std::move(data));
    }
    if (unknown || (declared && unparsed))
      failure = issue("GROUP_MEMBERSHIP_UNKNOWN");
    auto at = s->event(db, job["import_id"], "import.enumerated");
    if (!failure.is_null()) {
      s->diagnostic(db, job["import_id"], failure);
      db.exec("UPDATE progress SET enumeration='failed',errors=1 WHERE import_id=?",
              {job["import_id"]});
    } else {
      for (const auto &members : groups_for(records, metadata["atomicity"] == "whole_import")) {
        Json member_ids = Json::array();
        for (const auto &record : members)
          member_ids.push_back(record["id"]);
        auto hash = sha(member_ids.dump()), group_id = "group-" + hash;
        db.exec(
            "INSERT INTO admission_groups(id,import_id,context_id,member_hash,member_count) VALUES "
            "(?,?,?,?,?)",
            {group_id, job["import_id"], metadata["validation_context_id"], hash, members.size()});
        for (const auto &record : members)
          db.exec("UPDATE staged_records SET group_id=? WHERE id=?", {group_id, record["id"]});
        s->enqueue(db, "validate_group", group_id, job["import_id"]);
      }
      db.exec("UPDATE progress SET enumeration='complete',total=? WHERE import_id=?",
              {rows.size(), job["import_id"]});
    }
    s->history(db, ids, at);
    complete(db, job);
    s->fault("before_enumeration_commit");
    db.commit();
  }
  Json group_input(Db &db, const Json &job, Json &rows, Json &context) {
    auto group = db.one("SELECT * FROM admission_groups WHERE id=?", {job["subject_id"]});
    ensure(!group.is_null(), "STATE_CORRUPT");
    rows = db.rows("SELECT * FROM staged_records WHERE group_id=? ORDER BY ordinal", {group["id"]});
    context = s->context(db, group["context_id"]);
    return group;
  }
  void check_group(Db &db, const Json &job, const Json &group) {
    lease(db, job);
    auto current =
        db.one("SELECT revision,context_id FROM admission_groups WHERE id=?", {group["id"]});
    ensure(current["revision"] == group["revision"] && current["context_id"] == group["context_id"],
           "LEASE_LOST", true);
  }
  static Json input_records(const Json &rows) {
    Json records = Json::array();
    for (const auto &row : rows) {
      auto data = parse(row["body"]);
      data["id"] = row["id"];
      records.push_back(std::move(data));
    }
    return records;
  }
  void validate(const Json &job) {
    Json rows, context, group, ref;
    {
      auto db = s->db();
      group = group_input(db, job, rows, context);
      ref = db.one("SELECT * FROM reference_snapshots WHERE id=?", {context["reference_snapshot"]});
    }
    std::set<std::string> accounts, instruments;
    if (!ref.is_null()) {
      auto data = parse(ref["body"]);
      for (const auto &id : data["accounts"])
        accounts.insert(id);
      for (const auto &id : data["instruments"])
        instruments.insert(id);
    }
    Json errors = Json::object(), states = Json::object(), group_errors = Json::array(),
         dependencies = Json::array();
    for (const auto &row : rows) {
      auto data = parse(row["body"]);
      auto id = text(row["id"]);
      errors[id] = data["issues"];
      states[id] = errors[id].empty() ? "valid" : "invalid";
    }
    if (!supported(context)) {
      auto e = issue("UNSUPPORTED_CONTEXT");
      e["classification"] = "dependency";
      group_errors.push_back(e);
    } else if (ref.is_null()) {
      auto e = issue("REFERENCE_SNAPSHOT_UNAVAILABLE");
      e["classification"] = "dependency";
      group_errors.push_back(e);
      dependencies.push_back(Json::array({"snapshot", context["reference_snapshot"]}));
    } else
      for (const auto &row : rows) {
        auto data = parse(row["body"]);
        const auto id = text(row["id"]);
        if (data["packet"].is_null() || states[id] == "invalid")
          continue;
        const auto &raw = data["raw"];
        if (!accounts.contains(raw["account_id"])) {
          auto e = issue("UNKNOWN_ACCOUNT", "/account_id");
          e["classification"] = "dependency";
          errors[id].push_back(e);
          states[id] = "blocked";
        }
        if (raw.contains("event") && raw["event"].value("type", Json()) == "equity_trade" &&
            !instruments.contains(raw["event"]["instrument_id"])) {
          auto e = issue("UNKNOWN_INSTRUMENT", "/event/instrument_id");
          e["classification"] = "dependency";
          errors[id].push_back(e);
          states[id] = "blocked";
        }
      }
    if (rows.size() > limits.max_group_records) {
      auto e = issue("GROUP_LIMIT_EXCEEDED");
      e["classification"] = "invalid";
      group_errors.push_back(e);
    }
    Json examined = nullptr;
    bool valid = std::all_of(states.begin(), states.end(),
                             [](const Json &state) { return state == "valid"; });
    if (group_errors.empty() && valid) {
      {
        auto db = s->db();
        sync(db);
      }
      examined = revision;
      auto ordered = causal_order(input_records(rows));
      auto now = std::max(s->now(), recorded_ns);
      ensure(now >= 0, "CLOCK_RANGE");
      auto prepared = ledger.prepare_batch(drafts(ordered, now));
      if (!prepared) {
        auto error = core_error(prepared.error());
        auto target = rows[0];
        for (const auto &row : rows)
          if (parse(row["body"])["canonical_id"] == error["canonical_id"]) {
            target = row;
            break;
          }
        auto id = text(target["id"]);
        errors[id].push_back(error);
        states[id] = error["classification"] == "dependency" ? "blocked" : "invalid";
        if (error["classification"] == "dependency")
          dependencies.push_back(Json::array({"record", error["related_ids"][0]}));
      }
    }
    std::string outcome = "valid";
    for (const auto &e : group_errors)
      if (e["classification"] == "invalid")
        outcome = "invalid";
    for (const auto &state : states)
      if (state == "invalid")
        outcome = "invalid";
    if (outcome == "valid" && (!group_errors.empty() ||
                               std::any_of(states.begin(), states.end(),
                                           [](const Json &state) { return state == "blocked"; })))
      outcome = "blocked";
    const bool unavailable_context =
        std::any_of(group_errors.begin(), group_errors.end(),
                    [](const Json &error) { return error["classification"] == "dependency"; });
    if (unavailable_context)
      for (auto &state : states)
        if (state == "valid")
          state = "blocked";
    s->fault("before_validation_publish");
    auto db = s->db(true);
    check_group(db, job, group);
    const auto run = "run-" + uuid();
    Json body = {{"run_id", run},
                 {"kind", "validation"},
                 {"group_id", group["id"]},
                 {"context_id", group["context_id"]},
                 {"context", context},
                 {"reference_digest", ref.is_null() ? Json() : ref["digest"]},
                 {"engine_version", "luca/0.1"},
                 {"ledger_revision_examined", examined},
                 {"outcome", outcome},
                 {"finished_at", timestamp(s->now())}};
    db.exec("INSERT INTO validation_runs(id,import_id,group_id,body) VALUES (?,?,?,?)",
            {run, job["import_id"], group["id"], body.dump()});
    Json vd = Json::object(), cd = Json::object(), ids = Json::array();
    for (const auto &row : rows) {
      auto id = text(row["id"]);
      ids.push_back(id);
      s->state(db, row, vd, cd, states[id], outcome == "valid" ? "queued" : "not_committed", run);
      Json outcome = {{"state", states[id]},
                      {"context_id", group["context_id"]},
                      {"run_id", run},
                      {"diagnostics", errors[id]}};
      db.exec("INSERT INTO outcomes VALUES (?,?,?)", {run, id, outcome.dump()});
      for (const auto &e : errors[id])
        s->diagnostic(db, job["import_id"], e, row, group["id"], run,
                      e.value("classification", std::string("invalid")));
    }
    for (const auto &e : group_errors)
      s->diagnostic(db, job["import_id"], e, nullptr, group["id"], run, e["classification"]);
    db.exec("UPDATE admission_groups SET validation=?,commitment=?,run_id=?,revision=revision+1 "
            "WHERE id=?",
            {outcome, outcome == "valid" ? "queued" : "not_committed", run, group["id"]});
    db.exec("DELETE FROM dependencies WHERE group_id=?", {group["id"]});
    for (const auto &d : dependencies)
      db.exec("INSERT OR IGNORE INTO dependencies VALUES (?,?,?)", {group["id"], d[0], d[1]});
    s->counts(db, job["import_id"], vd, cd);
    auto at = s->event(db, job["import_id"], "validation." + outcome, Json::array({group["id"]}));
    s->history(db, ids, at);
    if (outcome == "valid")
      s->enqueue(db, "admit_group", group["id"], job["import_id"]);
    complete(db, job);
    s->fault("before_validation_commit");
    db.commit();
    s->fault("after_validation_commit");
  }
  void admit(const Json &job) {
    FileLock lock(s->directory / "locks/admission");
    Json group, rows, context;
    bool existing;
    {
      auto db = s->db();
      group = group_input(db, job, rows, context);
      lease(db, job);
      existing = !db.one("SELECT id FROM commits WHERE group_id=?", {group["id"]}).is_null();
      if (!existing) {
        ensure(group["validation"] == "valid" && group["commitment"] == "queued", "LEASE_LOST",
               true);
        sync(db);
      }
    }
    if (existing) {
      auto db = s->db(true);
      complete(db, job);
      db.commit();
      return;
    }
    auto ordered = causal_order(input_records(rows));
    ensure(sequence + ordered.size() <= limits.max_ledger_records, "LEDGER_LIMIT_EXCEEDED");
    auto now = std::max(s->now(), recorded_ns);
    ensure(now >= 0, "CLOCK_RANGE");
    auto prepared = ledger.prepare_batch(drafts(ordered, now));
    s->fault("after_admission_prepare");
    auto db = s->db(true);
    check_group(db, job, group);
    ensure(db.one("SELECT revision FROM ledger_state WHERE id=1")["revision"] == revision,
           "LEDGER_REVISION_CHANGED", true);
    auto run = "run-" + uuid();
    Json admission = {{"run_id", run},
                      {"kind", "admission"},
                      {"group_id", group["id"]},
                      {"context_id", group["context_id"]},
                      {"context", context},
                      {"validation_run_id", group["run_id"]},
                      {"engine_version", "luca/0.1"},
                      {"ledger_revision_examined", revision},
                      {"outcome", prepared ? "committed" : "rejected"},
                      {"finished_at", timestamp(s->now())}};
    db.exec("INSERT INTO validation_runs(id,import_id,group_id,body) VALUES (?,?,?,?)",
            {run, job["import_id"], group["id"], admission.dump()});
    Json vd = Json::object(), cd = Json::object(), ids = Json::array();
    for (const auto &row : rows)
      ids.push_back(row["id"]);
    if (!prepared) {
      auto error = core_error(prepared.error());
      const bool blocked = error["classification"] == "dependency";
      s->diagnostic(db, job["import_id"], error, nullptr, group["id"], run,
                    error["classification"]);
      for (const auto &row : rows)
        s->state(db, row, vd, cd, nullptr, blocked ? "not_committed" : "conflict");
      db.exec(
          "UPDATE admission_groups SET validation=?,commitment=?,revision=revision+1 WHERE id=?",
          {blocked ? "blocked" : "valid", blocked ? "not_committed" : "conflict", group["id"]});
      if (blocked)
        db.exec("INSERT OR IGNORE INTO dependencies VALUES (?,'record',?)",
                {group["id"], error["related_ids"][0]});
      s->counts(db, job["import_id"], vd, cd);
      auto at = s->event(db, job["import_id"], blocked ? "group.blocked" : "group.conflict",
                         Json::array({group["id"]}));
      s->history(db, ids, at);
      complete(db, job);
      db.commit();
      return;
    }
    auto commit = "commit-" + uuid();
    const auto count = prepared->records().size();
    Json receipt = {{"commit_id", commit},
                    {"import_id", job["import_id"]},
                    {"group_id", group["id"]},
                    {"member_hash", group["member_hash"]},
                    {"validation_run_id", group["run_id"]},
                    {"admission_run_id", run},
                    {"context", context},
                    {"engine_version", "luca/0.1"},
                    {"book_id", s->book},
                    {"prior_revision", std::to_string(revision)},
                    {"ledger_revision", std::to_string(revision + 1)},
                    {"record_count", count},
                    {"first_sequence", std::to_string(sequence + 1)},
                    {"last_sequence", std::to_string(sequence + count)},
                    {"recorded_ns", std::to_string(now)},
                    {"records_url", "/v1/books/" + quote(s->book) + "/imports/" +
                                        text(job["import_id"]) + "/records?commit_id=" + commit}};
    db.exec("INSERT INTO commits(id,import_id,group_id,body) VALUES (?,?,?,?)",
            {commit, job["import_id"], group["id"], receipt.dump()});
    for (const auto &record : prepared->records()) {
      auto bytes = serialization::canonical_bytes(record);
      db.exec("INSERT INTO ledger_records VALUES (?,?,?,?,?,?)",
              {record.acceptance_sequence().value(), record.record_id().value(),
               record.economic_event_id().value(), commit, serialization::detail::sha256_hex(bytes),
               Json::binary(std::vector<std::uint8_t>(
                   reinterpret_cast<const std::uint8_t *>(bytes.data()),
                   reinterpret_cast<const std::uint8_t *>(bytes.data() + bytes.size())))});
    }
    db.exec("UPDATE ledger_state SET revision=revision+1,last_sequence=?,recorded_ns=? WHERE id=1",
            {sequence + count, now});
    for (const auto &row : rows)
      s->state(db, row, vd, cd, nullptr, "committed", nullptr, commit);
    db.exec("UPDATE admission_groups SET commitment='committed',revision=revision+1 WHERE id=?",
            {group["id"]});
    s->counts(db, job["import_id"], vd, cd);
    auto at = s->event(db, job["import_id"], "group.committed", Json::array({group["id"]}));
    s->history(db, ids, at);
    complete(db, job);
    s->fault("before_admission_commit");
    db.commit();
    s->fault("after_admission_commit");
    auto accepted = ledger.publish(std::move(*prepared));
    ensure(accepted.has_value(), "CACHE_PUBLICATION_FAILED", true);
    sequence += count;
    ++revision;
    recorded_ns = now;
    s->fault("after_admission_publish");
  }
};
Worker::Worker(SqliteStore store, WorkerLimits limits)
    : impl_(std::make_unique<Impl>(std::move(store), limits)) {}
Worker::~Worker() = default;
Result<std::optional<WorkResult>> Worker::run_once() {
  return guarded([&]() -> std::optional<WorkResult> {
    auto &w = *impl_;
    std::lock_guard lock(w.mutex);
    w.s->wake();
    auto job = w.claim();
    if (job.is_null())
      return std::nullopt;
    WorkResult result{job["id"], job["kind"], "processed", ""};
    try {
      w.s->fault("after_claim");
      if (job["kind"] == "parse_import")
        w.parse_import(job);
      else if (job["kind"] == "validate_group")
        w.validate(job);
      else
        w.admit(job);
    } catch (...) {
      Failure error{"WORKER_UNAVAILABLE", true};
      try {
        throw;
      } catch (const Failure &e) {
        error = e;
      } catch (const std::exception &) {
      }
      w.ledger = LifecycleLedger{};
      w.sequence = 0;
      w.revision = 0;
      w.recorded_ns = 0;
      if (error.code != "LEASE_LOST")
        w.failure(job, error);
      result.outcome = "deferred";
      result.code = error.code;
    }
    return result;
  });
}
Result<std::size_t> Worker::drain(std::size_t maximum) {
  return guarded([&] {
    std::size_t count = 0;
    while (count < maximum) {
      auto result = take(run_once());
      if (!result)
        break;
      ++count;
    }
    return count;
  });
}
void Worker::run(std::stop_token stop, std::chrono::milliseconds idle) {
  std::mutex mutex;
  std::condition_variable_any wake;
  std::unique_lock lock(mutex);
  while (!stop.stop_requested()) {
    auto result = run_once();
    if (!result || !result->has_value())
      wake.wait_for(lock, stop, idle, [] { return false; });
  }
}
struct AsyncHost::Impl {
  struct Submission {
    Json metadata;
    std::string key, payload;
    std::promise<Result<Json>> promise;
  };
  SqliteStore store;
  std::size_t max_submissions, max_bytes, bytes = 0, active = 0;
  bool stopping = false;
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::unique_ptr<Submission>> queue;
  std::vector<std::jthread> workers;
  std::jthread receipts;
  Impl(SqliteStore s, std::size_t count, std::size_t maximum, std::size_t memory)
      : store(std::move(s)), max_submissions(maximum), max_bytes(memory) {
    ensure(count >= 1 && count <= 64 && maximum >= 1 && memory >= 1, "INVALID_EXECUTOR_LIMITS");
    // Construct workers before starting threads, so configuration failure cannot
    // terminate a thread or leave a partially running executor.
    std::vector<std::shared_ptr<Worker>> owners;
    for (std::size_t i = 0; i < count; ++i)
      owners.push_back(std::make_shared<Worker>(store));
    for (auto &owner : owners)
      workers.emplace_back([owner](std::stop_token stop) { owner->run(stop); });
    receipts = std::jthread([this] {
      while (true) {
        std::unique_ptr<Submission> item;
        {
          std::unique_lock lock(mutex);
          cv.wait(lock, [&] { return stopping || !queue.empty(); });
          if (queue.empty() && stopping)
            break;
          item = std::move(queue.front());
          queue.pop_front();
        }
        std::istringstream input(item->payload);
        auto result = store.receive(item->metadata, item->key, input, item->payload.size());
        item->promise.set_value(std::move(result));
        {
          std::lock_guard lock(mutex);
          bytes -= item->payload.size();
          --active;
        }
      }
    });
  }
  std::once_flag stopped;
  void stop() {
    std::call_once(stopped, [&] {
      {
        std::lock_guard lock(mutex);
        stopping = true;
      }
      cv.notify_all();
      if (receipts.joinable())
        receipts.join();
      for (auto &worker : workers)
        worker.request_stop();
      workers.clear();
    });
  }
  ~Impl() { stop(); }
};
AsyncHost::AsyncHost(SqliteStore store, std::size_t workers, std::size_t max_submissions,
                     std::size_t max_bytes)
    : impl_(std::make_unique<Impl>(std::move(store), workers, max_submissions, max_bytes)) {}
AsyncHost::~AsyncHost() = default;
void AsyncHost::stop() { impl_->stop(); }
Result<std::future<Result<Json>>> AsyncHost::submit(Json metadata, std::string key,
                                                    std::string payload) {
  auto &s = *impl_;
  std::lock_guard lock(s.mutex);
  if (s.stopping)
    return std::unexpected(Failure{"EXECUTOR_STOPPED"});
  if (s.active >= s.max_submissions || payload.size() > s.max_bytes - s.bytes)
    return std::unexpected(Failure{"CAPACITY_UNAVAILABLE", true});
  auto item = std::make_unique<Impl::Submission>();
  item->metadata = std::move(metadata);
  item->key = std::move(key);
  item->payload = std::move(payload);
  auto future = item->promise.get_future();
  s.bytes += item->payload.size();
  ++s.active;
  s.queue.push_back(std::move(item));
  s.cv.notify_one();
  return future;
}
} // namespace luca::ingestion
