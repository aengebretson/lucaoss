#include "internal.hpp"
#include <regex>
namespace luca::ingestion {
using namespace detail;
namespace {
std::string copied(std::istream &input, std::uint64_t bytes, int output = -1) {
  Hash hash;
  std::array<char, 65536> buffer;
  while (bytes) {
    auto count = std::min<std::uint64_t>(bytes, buffer.size());
    input.read(buffer.data(), static_cast<std::streamsize>(count));
    ensure(input.gcount() == static_cast<std::streamsize>(count), "PAYLOAD_LENGTH_MISMATCH");
    std::string_view block(buffer.data(), count);
    hash.update(block);
    if (output >= 0)
      write_all(output, block);
    bytes -= count;
  }
  ensure(input.peek() == std::char_traits<char>::eof() && !input.bad(), "PAYLOAD_LENGTH_MISMATCH");
  return hash.finish();
}
void digest_ok(std::string_view value) {
  ensure(value.size() == 64 &&
             std::all_of(value.begin(), value.end(),
                         [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }),
         "INVALID_DIGEST");
}
void live(const Json &row, std::int64_t now) {
  ensure(row["state"] != "expired" &&
             (row["state"] == "attached" || row["expires_ns"].get<std::int64_t>() > now),
         "UPLOAD_EXPIRED");
}
} // namespace
SqliteStore::Impl::Impl(std::filesystem::path root, Json cfg, Hooks h)
    : config(std::move(cfg)), hooks(std::move(h)) {
  tenant = token(config.at("tenant_id"));
  book = token(config.at("book_id"));
  ensure(config.at("source_ids").is_array() && !config["source_ids"].empty() &&
             config.at("contexts").is_object() && !config["contexts"].empty(),
         "INVALID_CONFIGURATION");
  for (const auto &source : config["source_ids"])
    token(source);
  limits = defaults();
  if (config.contains("limits"))
    for (const auto &[key, value] : config["limits"].items()) {
      ensure(limits.contains(key), "INVALID_CONFIGURATION");
      limits[key] = integer(value, 1, 9007199254740991LL);
    }
  for (const auto &[id, value] : config["contexts"].items()) {
    token(id);
    ensure(value.is_object() && value.size() == 5, "INVALID_CONFIGURATION");
    for (const auto *key : {"adapter", "normalization", "rules", "engine", "reference_snapshot"})
      token(value.at(key));
  }
  directory = std::filesystem::absolute(root) / framed({"shard/1", tenant, book});
  database = directory / "intake.sqlite3";
  directories(directory);
  for (const auto *name : {"payloads", "transfers", "locks"})
    directories(directory / name);
  FileLock initialization(directory / "locks/initialization");
  const auto profile = directory / "storage-profile.json";
  if (std::filesystem::exists(profile)) {
    std::ifstream file(profile);
    Json body;
    file >> body;
    ensure(body["schema"] == 1, "UNSUPPORTED_STORAGE_PROFILE");
    mode = token(body["journal_mode"]);
    ensure(mode == "wal" || mode == "delete", "UNSUPPORTED_STORAGE_PROFILE");
  } else {
    ensure(!std::filesystem::exists(database), "UNRECOGNIZED_DATABASE");
    mode = patched_wal() ? "wal" : "delete";
    auto temp = directory / "storage-profile.tmp";
    Fd file(::open(temp.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600));
    write_all(file.value, Json{{"schema", 1}, {"journal_mode", mode}}.dump());
    sync_file(file.value);
    std::filesystem::rename(temp, profile);
    sync_dir(directory);
  }
  ensure(mode != "wal" || patched_wal(), "UNPATCHED_WAL_RUNTIME");
  Db db(database, mode, true, true);
  auto version = db.one("PRAGMA user_version")["user_version"].get<int>();
  Json expected = {
      {"tenant", tenant}, {"book", book}, {"limits", limits}, {"contexts", config["contexts"]}};
  if (version == 0) {
    ensure(db.rows("SELECT name FROM sqlite_master WHERE type='table'").empty(),
           "UNRECOGNIZED_DATABASE");
    for (auto sql : schema_one)
      db.exec(sql);
    db.exec("INSERT INTO config VALUES (1,?)", {expected.dump()});
    version = 1;
  }
  ensure(version >= 1 && version <= 6, "UNSUPPORTED_SCHEMA");
  auto stored = parse(db.one("SELECT value FROM config WHERE id=1")["value"]);
  for (const auto *key : {"tenant", "book", "limits"})
    ensure(stored[key] == expected[key], "CONFIGURATION_CONFLICT");
  for (const auto &[id, context] : config["contexts"].items())
    ensure(stored["contexts"].contains(id) && stored["contexts"][id] == context,
           "CONFIGURATION_CONFLICT");
  if (version == 1)
    for (auto sql : schema_two)
      db.exec(sql);
  if (version < 3)
    for (auto sql : schema_three) db.exec(sql);
  if (version < 4)
    for (auto sql : schema_four) db.exec(sql);
  if (version < 5)
    for (auto sql : schema_five) db.exec(sql);
  if (version < 6) {
    db.exec("ALTER TABLE policy_revisions ADD COLUMN recorded_ns INTEGER");
    db.exec("PRAGMA user_version=6");
  }
  db.commit();
}
Result<SqliteStore> SqliteStore::open(std::filesystem::path root, Json config, Hooks hooks) {
  return guarded([&] {
    return SqliteStore(
        std::make_shared<Impl>(std::move(root), std::move(config), std::move(hooks)));
  });
}
std::filesystem::path SqliteStore::path() const { return impl_->directory; }
FileLock SqliteStore::Impl::guard(const std::string &id, bool blocking) const {
  ensure(id.size() == 32 &&
             std::all_of(id.begin(), id.end(),
                         [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }),
         "NOT_FOUND");
  return {directory / "locks" / id.substr(0, 3), blocking};
}
Json SqliteStore::Impl::context(Db &db, const std::string &id) const {
  auto cfg = parse(db.one("SELECT value FROM config WHERE id=1")["value"]);
  ensure(cfg["contexts"].contains(id), "UNSUPPORTED_CONTEXT_OR_FORMAT");
  return cfg["contexts"][id];
}
Json SqliteStore::Impl::import(Db &db, const std::string &id) const {
  auto row = db.one("SELECT * FROM imports WHERE id=?", {id});
  ensure(!row.is_null(), "NOT_FOUND");
  return row;
}
Json SqliteStore::Impl::upload(Db &db, const std::string &id) const {
  auto row = db.one("SELECT * FROM transfers WHERE id=? AND kind='upload'", {id});
  ensure(!row.is_null(), "NOT_FOUND");
  return row;
}
Json SqliteStore::Impl::metadata(Json data) const {
  ensure(data.is_object(), "INVALID_ENVELOPE");
  const std::set<std::string> allowed = {"source_id", "format",           "validation_context_id",
                                         "atomicity", "client_reference", "replaces"};
  for (const auto &[key, value] : data.items())
    ensure(allowed.contains(key), "INVALID_ENVELOPE");
  const auto source = token(data.at("source_id"));
  ensure(std::find(config["source_ids"].begin(), config["source_ids"].end(), source) !=
             config["source_ids"].end(),
         "SOURCE_NOT_AUTHORIZED");
  auto db = this->db();
  auto ctx = context(db, token(data.at("validation_context_id")));
  ensure(data.at("format") == ctx["adapter"], "UNSUPPORTED_CONTEXT_OR_FORMAT");
  data["atomicity"] = data.value("atomicity", std::string("independent_groups"));
  ensure(data["atomicity"] == "independent_groups" || data["atomicity"] == "whole_import",
         "INVALID_ENVELOPE");
  if (data.contains("client_reference"))
    token(data["client_reference"]);
  if (data.contains("replaces")) {
    auto &r = data["replaces"];
    ensure(r.is_object() && r.size() == 2 && r.contains("import_id") && r.contains("record_ids"),
           "INVALID_ENVELOPE");
    token(r["import_id"]);
    ensure(r["record_ids"].is_array() && r["record_ids"].size() <= 1000, "INVALID_ENVELOPE");
    std::set<std::string> ids;
    for (auto &id : r["record_ids"])
      ids.insert(token(id));
    r["record_ids"] = ids;
  }
  ensure(data.dump().size() <= 16384, "LIMIT_EXCEEDED");
  return data;
}
std::string SqliteStore::Impl::fingerprint(const Json &data, const std::string &digest,
                                           std::uint64_t bytes) const {
  auto replacement = data.value("replaces", Json());
  return framed({"submission/1", tenant, book, data["source_id"], data["format"],
                 data["validation_context_id"], data["atomicity"],
                 data.value("client_reference", Json()),
                 replacement.is_null() ? Json() : replacement["import_id"],
                 replacement.is_null() ? Json() : Json(replacement["record_ids"].dump()), digest,
                 std::to_string(bytes)});
}
void SqliteStore::Impl::reserve(Db &db, const std::string &id, std::uint64_t size,
                                const std::string &kind, Json digest, Json chunk, Json key,
                                Json request) const {
  auto used = db.one("SELECT * FROM capacity WHERE id=1");
  ensure(size <= limits["max_payload_bytes"].get<std::uint64_t>(), "LIMIT_EXCEEDED");
  ensure(size <= limits["max_retained_bytes"].get<std::uint64_t>() -
                     used["bytes"].get<std::uint64_t>() &&
             used["slots"] < limits["max_pending_imports"],
         "CAPACITY_UNAVAILABLE", true);
  const auto at = now(), ttl = limits["upload_ttl_seconds"].get<std::int64_t>();
  ensure(at >= 0 && ttl <= (INT64_MAX - at) / 1000000000, "CLOCK_RANGE");
  db.exec("INSERT INTO "
          "transfers(id,kind,state,size,digest,chunk_size,upload_key,request_hash,created_ns,"
          "expires_ns,reserved) VALUES (?,?,?,?,?,?,?,?,?,?,1)",
          {id, kind, kind == "direct" ? "receiving" : "uploading", size, digest, chunk, key,
           request, at, at + ttl * 1000000000});
  db.exec("UPDATE capacity SET bytes=bytes+?,slots=slots+1 WHERE id=1", {size});
}
void SqliteStore::Impl::install(const std::string &id) const {
  if (std::filesystem::exists(part(id))) {
    ensure(::chmod(part(id).c_str(), 0400) == 0, "STORAGE_UNAVAILABLE", true);
    Fd file(::open(part(id).c_str(), O_RDONLY));
    sync_file(file.value);
    std::filesystem::rename(part(id), payload_path(id));
    sync_dir(directory / "payloads");
    sync_dir(directory / "transfers");
  } else
    ensure(std::filesystem::is_regular_file(payload_path(id)), "PAYLOAD_UNAVAILABLE");
}
bool SqliteStore::Impl::cleanup(const std::string &id, bool erase) const {
  auto db = this->db(true);
  auto row = db.one("SELECT * FROM transfers WHERE id=?", {id});
  if (row.is_null() || row["state"] == "attached")
    return false;
  for (auto file : {part(id), payload_path(id)})
    if (std::filesystem::remove(file))
      sync_dir(file.parent_path());
  if (row["reserved"] == 1)
    db.exec("UPDATE capacity SET bytes=bytes-?,slots=slots-? WHERE id=1",
            {row["size"], row["queue_reserved"]});
  db.exec("UPDATE work SET state='complete' WHERE kind='verify_upload' AND subject_id=?", {id});
  db.exec("UPDATE transfers SET reserved=0,queue_reserved=0,state='expired' WHERE id=?", {id});
  if (erase)
    db.exec("DELETE FROM transfers WHERE id=?", {id});
  db.commit();
  return true;
}
Json SqliteStore::Impl::attach(Db &db, const std::string &id, const Json &data,
                               const std::string &key, const std::string &digest,
                               std::uint64_t size) const {
  auto fingerprint = this->fingerprint(data, digest, size);
  auto old =
      db.one("SELECT * FROM imports WHERE source=? AND request_key=?", {data["source_id"], key});
  if (!old.is_null()) {
    ensure(old["fingerprint"] == fingerprint, "IDEMPOTENCY_CONFLICT");
    return parse(old["receipt"]);
  }
  const auto import_id = "imp-" + uuid();
  const auto at = timestamp(now());
  Json receipt = {{"import_id", import_id},
                  {"receipt", "durable"},
                  {"received_at", at},
                  {"payload", {{"algorithm", "sha256"}, {"digest", digest}, {"bytes", size}}},
                  {"durability_profile", "local-fsync-" + mode + "-v1"},
                  {"status_url", "/v1/books/" + quote(book) + "/imports/" + import_id}};
  db.exec("INSERT INTO imports VALUES (?,?,?,?,?,?,?,?)",
          {import_id, data["source_id"], key, fingerprint, data.dump(),
           context(db, data["validation_context_id"]).dump(), receipt.dump(), id});
  db.exec("UPDATE transfers SET state='attached',digest=? WHERE id=?", {digest, id});
  enqueue(db, "parse_import", import_id, import_id);
  Json event = {{"event_id", uuid()},
                {"import_id", import_id},
                {"revision", "1"},
                {"type", "import.received"},
                {"occurred_at", at},
                {"subject_ids", Json::array({import_id})},
                {"links", {{"status", receipt["status_url"]}}}};
  db.exec("INSERT INTO events VALUES (?,1,?)", {import_id, event.dump()});
  fault("before_receipt_commit");
  return receipt;
}
Result<Json> SqliteStore::receive(Json data, std::string key, std::istream &input,
                                  std::uint64_t bytes) const {
  return guarded([&] {
    auto &s = *impl_;
    data = s.metadata(std::move(data));
    token(key, 128);
    integer(bytes, 0, s.limits["max_payload_bytes"]);
    {
      auto db = s.db();
      auto old = db.one("SELECT * FROM imports WHERE source=? AND request_key=?",
                        {data["source_id"], key});
      if (!old.is_null()) {
        ensure(old["fingerprint"] == s.fingerprint(data, copied(input, bytes), bytes),
               "IDEMPOTENCY_CONFLICT");
        return parse(old["receipt"]);
      }
    }
    auto id = uuid();
    auto lock = s.guard(id);
    try {
      {
        auto db = s.db(true);
        s.reserve(db, id, bytes, "direct");
        db.commit();
      }
      s.fault("after_reservation");
      std::string digest;
      {
        Fd file(::open(s.part(id).c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600));
        digest = copied(input, bytes, file.value);
        sync_file(file.value);
      }
      s.fault("after_payload_flush");
      s.install(id);
      s.fault("after_payload_install");
      Json receipt;
      {
        auto db = s.db(true);
        receipt = s.attach(db, id, data, key, digest, bytes);
        db.commit();
      }
      s.fault("after_receipt_commit");
      s.cleanup(id, true);
      return receipt;
    } catch (...) {
      try {
        s.cleanup(id, true);
      } catch (...) {
      }
      throw;
    }
  });
}
Result<Json> SqliteStore::create_upload(std::string key, std::uint64_t bytes, std::string digest,
                                        std::uint64_t chunk) const {
  return guarded([&] {
    auto &s = *impl_;
    token(key, 128);
    digest_ok(digest);
    integer(bytes, 0, s.limits["max_payload_bytes"]);
    integer(chunk, 1, s.limits["max_chunk_bytes"]);
    ensure(bytes / chunk + (bytes % chunk != 0) <= s.limits["max_chunks"].get<std::uint64_t>(),
           "LIMIT_EXCEEDED");
    auto request = framed({"upload/1", std::to_string(bytes), digest, std::to_string(chunk)});
    std::string id;
    {
      auto db = s.db(true);
      auto row = db.one("SELECT * FROM transfers WHERE upload_key=?", {key});
      if (!row.is_null()) {
        ensure(row["request_hash"] == request, "IDEMPOTENCY_CONFLICT");
        id = text(row["id"]);
      } else {
        id = uuid();
        s.reserve(db, id, bytes, "upload", digest, chunk, key, request);
        if (!bytes) {
          db.exec("UPDATE transfers SET state='verifying' WHERE id=?", {id});
          db.exec(
              "INSERT INTO work(id,kind,subject_id,state) VALUES (?,'verify_upload',?,'queued')",
              {uuid(), id});
        }
      }
      db.commit();
    }
    return take(upload_status(id));
  });
}
Result<Json> SqliteStore::upload_status(std::string id) const {
  return guarded([&] {
    auto &s = *impl_;
    auto db = s.db();
    auto row = s.upload(db, id);
    Json ranges = Json::array();
    for (auto &item :
         db.rows("SELECT number FROM chunks WHERE transfer_id=? ORDER BY number", {id})) {
      auto n = item["number"].get<std::int64_t>();
      if (!ranges.empty() && ranges.back()[1].get<std::int64_t>() + 1 == n)
        ranges.back()[1] = n;
      else
        ranges.push_back(Json::array({n, n}));
    }
    auto state = row["state"];
    if (state != "attached" && row["expires_ns"].get<std::int64_t>() <= s.now())
      state = "expired";
    return Json{{"upload_id", id},
                {"state", state},
                {"expected_bytes", row["size"]},
                {"sha256", row["digest"]},
                {"chunk_bytes", row["chunk_size"]},
                {"received_ranges", ranges},
                {"expires_at", timestamp(row["expires_ns"])},
                {"error", state == "failed"
                              ? Json{{"code", "CHECKSUM_MISMATCH"}, {"retryable", false}}
                              : Json()}};
  });
}
Result<Json> SqliteStore::put_chunk(std::string id, std::uint64_t index, std::string_view data,
                                    std::string digest) const {
  return guarded([&] {
    auto &s = *impl_;
    digest_ok(digest);
    ensure(sha(data) == digest, "CHECKSUM_MISMATCH");
    integer(index, 0, s.limits["max_chunks"].get<std::int64_t>() - 1);
    ensure(data.size() <= s.limits["max_chunk_bytes"], "LIMIT_EXCEEDED");
    auto lock = s.guard(id);
    Json row;
    {
      auto db = s.db();
      row = s.upload(db, id);
      live(row, s.now());
      auto size = row["size"].get<std::uint64_t>(), chunk = row["chunk_size"].get<std::uint64_t>();
      ensure(index < size / chunk + (size % chunk != 0) &&
                 data.size() == std::min(chunk, size - index * chunk),
             "CHUNK_LENGTH_MISMATCH");
      auto old = db.one("SELECT digest FROM chunks WHERE transfer_id=? AND number=?", {id, index});
      if (!old.is_null()) {
        ensure(old["digest"] == digest, "IDEMPOTENCY_CONFLICT");
        return Json{{"state", "retained"}};
      }
      ensure(row["state"] == "uploading", "UPLOAD_FROZEN");
    }
    {
      Fd file(::open(s.part(id).c_str(), O_CREAT | O_RDWR, 0600));
      ensure(::lseek(file.value, static_cast<off_t>(index * row["chunk_size"].get<std::uint64_t>()),
                     SEEK_SET) >= 0,
             "STORAGE_UNAVAILABLE", true);
      write_all(file.value, data);
      sync_file(file.value);
    }
    sync_dir(s.directory / "transfers");
    s.fault("after_chunk_flush");
    {
      auto db = s.db(true);
      db.exec("INSERT INTO chunks VALUES (?,?,?)", {id, index, digest});
      auto count = db.one("SELECT count(*) AS n FROM chunks WHERE transfer_id=?", {id})["n"]
                       .get<std::uint64_t>();
      auto size = row["size"].get<std::uint64_t>(), chunk = row["chunk_size"].get<std::uint64_t>();
      if (count == size / chunk + (size % chunk != 0)) {
        db.exec("UPDATE transfers SET state='verifying' WHERE id=?", {id});
        db.exec("INSERT INTO work(id,kind,subject_id,state) VALUES (?,'verify_upload',?,'queued')",
                {uuid(), id});
      }
      db.commit();
    }
    s.fault("after_chunk_commit");
    return Json{{"state", "retained"}};
  });
}
Result<Json> SqliteStore::verify_upload(std::string id) const {
  return guarded([&] {
    auto &s = *impl_;
    auto lock = s.guard(id);
    Json row;
    {
      auto db = s.db();
      row = s.upload(db, id);
      live(row, s.now());
    }
    if (row["state"] == "ready" || row["state"] == "attached")
      return take(upload_status(id));
    ensure(row["state"] == "verifying", "UPLOAD_NOT_READY", row["state"] == "uploading");
    auto path = std::filesystem::exists(s.payload_path(id)) ? s.payload_path(id) : s.part(id);
    if (row["size"] == 0 && !std::filesystem::exists(path)) {
      Fd file(::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600));
      sync_file(file.value);
    }
    std::ifstream input(path, std::ios::binary);
    ensure(input.good(), "PAYLOAD_UNAVAILABLE");
    auto digest = copied(input, row["size"]);
    if (digest != text(row["digest"])) {
      auto db = s.db(true);
      db.exec("UPDATE transfers SET state='failed' WHERE id=?", {id});
      db.exec("UPDATE work SET state='complete' WHERE kind='verify_upload' AND subject_id=?", {id});
      db.commit();
      fail("CHECKSUM_MISMATCH");
    }
    s.install(id);
    s.fault("after_upload_install");
    {
      auto db = s.db(true);
      db.exec("UPDATE transfers SET state='ready' WHERE id=?", {id});
      db.exec("UPDATE work SET state='complete' WHERE kind='verify_upload' AND subject_id=?", {id});
      db.commit();
    }
    return take(upload_status(id));
  });
}
Result<Json> SqliteStore::receive_upload(Json data, std::string key, std::string id) const {
  return guarded([&] {
    auto &s = *impl_;
    data = s.metadata(std::move(data));
    token(key, 128);
    auto lock = s.guard(id);
    Json receipt;
    {
      auto db = s.db(true);
      auto row = s.upload(db, id);
      auto old = db.one("SELECT * FROM imports WHERE source=? AND request_key=?",
                        {data["source_id"], key});
      if (!old.is_null()) {
        ensure(old["fingerprint"] == s.fingerprint(data, row["digest"], row["size"]),
               "IDEMPOTENCY_CONFLICT");
        return parse(old["receipt"]);
      }
      live(row, s.now());
      ensure(row["state"] != "attached", "UPLOAD_FROZEN");
      ensure(row["state"] == "ready", "UPLOAD_NOT_READY",
             row["state"] == "uploading" || row["state"] == "verifying");
      receipt = s.attach(db, id, data, key, row["digest"], row["size"]);
      db.commit();
    }
    s.fault("after_receipt_commit");
    return receipt;
  });
}
Result<Json> SqliteStore::recover(std::string after, std::size_t limit) const {
  return guarded([&] {
    auto &s = *impl_;
    integer(limit, 1, s.limits["max_page_size"]);
    Json rows;
    {
      auto db = s.db();
      rows = db.rows("SELECT * FROM transfers WHERE state!='attached' AND reserved=1 AND id>? "
                     "ORDER BY id LIMIT ?",
                     {after, limit});
    }
    Json reclaimed = Json::array();
    for (const auto &row : rows) {
      if (row["kind"] != "direct" && row["expires_ns"].get<std::int64_t>() > s.now())
        continue;
      auto id = text(row["id"]);
      try {
        auto lock = s.guard(id, false);
        if (s.cleanup(id))
          reclaimed.push_back(id);
      } catch (const Failure &e) {
        if (e.code != "LOCK_BUSY")
          throw;
      }
    }
    return Json{{"reclaimed", reclaimed},
                {"next_after", rows.size() == limit ? rows.back()["id"] : Json()}};
  });
}
Result<Json> SqliteStore::payload(std::string id, std::ostream &output, std::uint64_t offset,
                                  std::optional<std::uint64_t> length) const {
  return guarded([&] {
    auto &s = *impl_;
    Json row;
    {
      auto db = s.db();
      row = s.import(db, id);
    }
    auto size = parse(row["receipt"])["payload"]["bytes"].get<std::uint64_t>();
    ensure(offset <= size, "LIMIT_EXCEEDED");
    auto remaining = length.value_or(size - offset);
    ensure(remaining <= size - offset, "LIMIT_EXCEEDED");
    std::ifstream input(s.payload_path(row["transfer_id"]), std::ios::binary);
    ensure(input.good(), "PAYLOAD_UNAVAILABLE");
    input.seekg(static_cast<std::streamoff>(offset));
    std::array<char, 65536> buffer;
    while (remaining) {
      const auto n = std::min<std::uint64_t>(remaining, buffer.size());
      input.read(buffer.data(), n);
      ensure(input.gcount() == static_cast<std::streamsize>(n), "PAYLOAD_UNAVAILABLE");
      output.write(buffer.data(), n);
      ensure(output.good(), "OUTPUT_WRITE_FAILED");
      remaining -= n;
    }
    return Json{{"bytes", length.value_or(size - offset)}};
  });
}
} // namespace luca::ingestion
