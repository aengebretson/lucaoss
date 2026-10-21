#pragma once
#include "luca/adapters/transaction.hpp"
#include "luca/ingestion/sqlite.hpp"
#include "luca/serialization/canonical_decode.hpp"
#include "schema.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <sqlite3.h>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace luca::ingestion::detail {
[[noreturn]] inline void fail(std::string code, bool retry = false) {
  throw Failure{std::move(code), retry};
}
inline void ensure(bool condition, std::string code, bool retry = false) {
  if (!condition)
    fail(std::move(code), retry);
}
template <class F> auto guarded(F &&f) -> Result<decltype(f())> {
  try {
    return f();
  } catch (const Failure &e) {
    return std::unexpected(e);
  } catch (const Json::exception &) {
    return std::unexpected(Failure{"INVALID_ENVELOPE"});
  } catch (const std::exception &) {
    return std::unexpected(Failure{"STORAGE_UNAVAILABLE", true});
  }
}
template <class T> T take(Result<T> result) {
  if (!result)
    throw result.error();
  return std::move(*result);
}
inline std::string text(const Json &x) { return x.is_null() ? "" : x.get<std::string>(); }
inline std::string token(const Json &x, std::size_t maximum = 256) {
  ensure(x.is_string() && !x.get_ref<const std::string &>().empty() &&
             x.get_ref<const std::string &>().size() <= maximum,
         "INVALID_ENVELOPE");
  auto result = x.get<std::string>();
  (void)Json(result).dump();
  return result;
}
inline std::int64_t integer(const Json &x, std::int64_t low, std::int64_t high) {
  ensure(x.is_number_integer(), "LIMIT_EXCEEDED");
  if (x.is_number_unsigned())
    ensure(x.get<std::uint64_t>() <= static_cast<std::uint64_t>(INT64_MAX), "LIMIT_EXCEEDED");
  auto value = x.get<std::int64_t>();
  ensure(value >= low && value <= high, "LIMIT_EXCEEDED");
  return value;
}
inline std::string hex(std::span<const std::byte> data) {
  static constexpr char chars[] = "0123456789abcdef";
  std::string out;
  out.reserve(data.size() * 2);
  for (auto b : data) {
    auto c = std::to_integer<unsigned>(b);
    out += chars[c >> 4];
    out += chars[c & 15];
  }
  return out;
}
inline std::string hex(std::string_view data) { return hex(std::as_bytes(std::span{data})); }
inline std::string unhex(std::string_view data) {
  ensure(data.size() % 2 == 0, "INVALID_CURSOR");
  std::string result;
  auto digit = [](char c) {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    fail("INVALID_CURSOR");
  };
  for (std::size_t i = 0; i < data.size(); i += 2)
    result += static_cast<char>(digit(data[i]) * 16 + digit(data[i + 1]));
  return result;
}
// Streaming SHA-256 reuses the financial codec's compression primitive. No
// OpenSSL, shell commands, or unbounded whole-file allocation is required.
class Hash {
  std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::array<std::byte, 64> block_{};
  std::size_t used_ = 0;
  std::uint64_t size_ = 0;

public:
  void update(std::span<const std::byte> data) {
    size_ += data.size();
    for (auto b : data) {
      block_[used_++] = b;
      if (used_ == 64) {
        serialization::detail::sha256_compress(state_, block_.data());
        used_ = 0;
      }
    }
  }
  void update(std::string_view data) { update(std::as_bytes(std::span{data})); }
  std::string finish() {
    const auto bits = size_ * 8;
    block_[used_++] = std::byte{0x80};
    if (used_ > 56) {
      std::fill(block_.begin() + used_, block_.end(), std::byte{});
      serialization::detail::sha256_compress(state_, block_.data());
      used_ = 0;
    }
    std::fill(block_.begin() + used_, block_.end(), std::byte{});
    for (unsigned i = 0; i < 8; ++i)
      block_[63 - i] = static_cast<std::byte>(bits >> (i * 8));
    serialization::detail::sha256_compress(state_, block_.data());
    std::array<std::byte, 32> out{};
    for (unsigned i = 0; i < 8; ++i)
      for (unsigned j = 0; j < 4; ++j)
        out[i * 4 + j] = static_cast<std::byte>(state_[i] >> (24 - j * 8));
    return hex(out);
  }
};
inline std::string sha(std::string_view value) {
  Hash hash;
  hash.update(value);
  return hash.finish();
}
inline std::string framed(std::initializer_list<Json> fields) {
  Hash hash;
  for (const auto &field : fields) {
    if (field.is_null()) {
      hash.update(std::string_view{"\0", 1});
      continue;
    }
    const auto value = field.get<std::string>();
    std::array<std::byte, 9> prefix{};
    prefix[0] = std::byte{1};
    for (unsigned i = 0; i < 8; ++i)
      prefix[8 - i] = static_cast<std::byte>(value.size() >> (i * 8));
    hash.update(prefix);
    hash.update(value);
  }
  return hash.finish();
}
struct Fd {
  int value = -1;
  explicit Fd(int fd) : value(fd) { ensure(fd >= 0, "STORAGE_UNAVAILABLE", true); }
  ~Fd() {
    if (value >= 0)
      ::close(value);
  }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
};
inline void sync_file(int fd) {
  ensure(::fsync(fd) == 0, "STORAGE_UNAVAILABLE", true);
#ifdef __APPLE__
  ensure(::fcntl(fd, F_FULLFSYNC) == 0, "STORAGE_UNAVAILABLE", true);
#endif
}
inline void sync_dir(const std::filesystem::path &path) {
  Fd fd(::open(path.c_str(), O_RDONLY | O_DIRECTORY));
  ensure(::fsync(fd.value) == 0, "STORAGE_UNAVAILABLE", true);
}
inline void directories(const std::filesystem::path &path) {
  if (!std::filesystem::exists(path)) {
    directories(path.parent_path());
    ensure(::mkdir(path.c_str(), 0700) == 0 || errno == EEXIST, "STORAGE_UNAVAILABLE", true);
    sync_dir(path.parent_path());
  }
}
inline void write_all(int fd, std::string_view data) {
  while (!data.empty()) {
    auto n = ::write(fd, data.data(), data.size());
    if (n < 0 && errno == EINTR)
      continue;
    ensure(n > 0, "STORAGE_UNAVAILABLE", true);
    data.remove_prefix(static_cast<std::size_t>(n));
  }
}
inline std::string uuid() {
  std::array<std::byte, 16> bytes;
  Fd random(::open("/dev/urandom", O_RDONLY));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    auto n = ::read(random.value, bytes.data() + offset, bytes.size() - offset);
    if (n < 0 && errno == EINTR)
      continue;
    ensure(n > 0, "STORAGE_UNAVAILABLE", true);
    offset += static_cast<std::size_t>(n);
  }
  return hex(bytes);
}
inline std::string timestamp(std::int64_t ns) {
  return serialization::detail::canonical_timestamp(Timestamp{std::chrono::nanoseconds{ns}});
}
inline std::string quote(std::string_view raw) {
  const char *h = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : raw) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~')
      out += c;
    else {
      out += '%';
      out += h[c >> 4];
      out += h[c & 15];
    }
  }
  return out;
}
struct FileLock {
  Fd fd;
  FileLock(const std::filesystem::path &file, bool blocking = true)
      : fd(::open(file.c_str(), O_CREAT | O_RDWR, 0600)) {
    ensure(::flock(fd.value, LOCK_EX | (blocking ? 0 : LOCK_NB)) == 0, "LOCK_BUSY", true);
  }
};
class Db {
  sqlite3 *db_ = nullptr;
  bool complete_ = false;
  std::filesystem::path directory_;

public:
  Db(const std::filesystem::path &file, std::string mode, bool write, bool init = false)
      : directory_(file.parent_path()) {
    if (sqlite3_open(file.c_str(), &db_) != SQLITE_OK) {
      if (db_)
        sqlite3_close(db_);
      db_ = nullptr;
      fail("STORAGE_UNAVAILABLE", true);
    }
    try {
      sqlite3_busy_timeout(db_, 10000);
      if (init)
        exec("PRAGMA journal_mode=" + mode);
      ensure(text(one("PRAGMA journal_mode")["journal_mode"]) == mode, "DURABILITY_UNAVAILABLE");
      exec("PRAGMA synchronous=" + std::string(mode == "wal" ? "FULL" : "EXTRA"));
      exec("PRAGMA foreign_keys=ON");
#ifdef __APPLE__
      exec("PRAGMA fullfsync=ON");
      exec("PRAGMA checkpoint_fullfsync=ON");
#endif
      exec(write ? "BEGIN IMMEDIATE" : "BEGIN");
    } catch (...) {
      sqlite3_close(db_);
      db_ = nullptr;
      throw;
    }
  }
  ~Db() {
    if (db_) {
      if (!complete_)
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
      sqlite3_close(db_);
    }
  }
  Db(const Db &) = delete;
  Db &operator=(const Db &) = delete;
  Json query(std::string_view sql, std::span<const Json> values) {
    sqlite3_stmt *raw = nullptr;
    ensure(sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &raw, nullptr) ==
               SQLITE_OK,
           "STORAGE_UNAVAILABLE", true);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    int index = 1;
    for (const auto &value : values) {
      int rc = SQLITE_OK;
      if (value.is_null())
        rc = sqlite3_bind_null(raw, index);
      else if (value.is_binary()) {
        const auto &b = value.get_binary();
        rc = sqlite3_bind_blob64(raw, index, b.data(), b.size(), SQLITE_TRANSIENT);
      } else if (value.is_number_integer())
        rc = sqlite3_bind_int64(raw, index, value.get<std::int64_t>());
      else {
        const auto s = value.get<std::string>();
        rc = sqlite3_bind_text64(raw, index, s.data(), s.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
      }
      ensure(rc == SQLITE_OK, "STORAGE_UNAVAILABLE", true);
      ++index;
    }
    Json result = Json::array();
    int rc;
    while ((rc = sqlite3_step(raw)) == SQLITE_ROW) {
      Json row = Json::object();
      for (int i = 0; i < sqlite3_column_count(raw); ++i) {
        const auto *name = sqlite3_column_name(raw, i);
        const auto type = sqlite3_column_type(raw, i);
        if (type == SQLITE_NULL)
          row[name] = nullptr;
        else if (type == SQLITE_INTEGER)
          row[name] = sqlite3_column_int64(raw, i);
        else if (type == SQLITE_BLOB) {
          const auto *data = static_cast<const std::uint8_t *>(sqlite3_column_blob(raw, i));
          const int size = sqlite3_column_bytes(raw, i);
          row[name] = Json::binary(std::vector<std::uint8_t>(data, data + size));
        } else
          row[name] = std::string(reinterpret_cast<const char *>(sqlite3_column_text(raw, i)),
                                  sqlite3_column_bytes(raw, i));
      }
      result.push_back(std::move(row));
    }
    ensure(rc == SQLITE_DONE, "STORAGE_UNAVAILABLE", true);
    return result;
  }
  Json rows(std::string_view sql, std::initializer_list<Json> values = {}) {
    return query(sql, std::span<const Json>{values.begin(), values.size()});
  }
  Json one(std::string_view sql, std::initializer_list<Json> values = {}) {
    auto result = rows(sql, values);
    return result.empty() ? Json() : result[0];
  }
  void exec(std::string_view sql, std::initializer_list<Json> values = {}) {
    (void)rows(sql, values);
  }
  void commit() {
    exec("COMMIT");
    complete_ = true;
    sync_dir(directory_);
  }
};
inline Json parse(const Json &value) { return Json::parse(value.get_ref<const std::string &>()); }
inline Json defaults() {
  return {{"max_payload_bytes", 256 * 1024 * 1024},
          {"max_retained_bytes", 4LL * 1024 * 1024 * 1024},
          {"max_pending_imports", 1024},
          {"max_chunk_bytes", 4 * 1024 * 1024},
          {"max_chunks", 8192},
          {"max_page_size", 1000},
          {"upload_ttl_seconds", 86400}};
}
inline bool patched_wal() {
  const int v = sqlite3_libversion_number();
  return v >= 3051003 || (v >= 3050007 && v < 3051000) || (v >= 3044006 && v < 3045000);
}
} // namespace luca::ingestion::detail
namespace luca::ingestion {
struct SqliteStore::Impl {
  std::filesystem::path directory, database;
  Json config, limits;
  std::string tenant, book, mode;
  Hooks hooks;
  Impl(std::filesystem::path root, Json cfg, Hooks h);
  std::int64_t now() const {
    return hooks.clock ? hooks.clock()
                       : std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
  }
  void fault(std::string_view phase) const {
    if (hooks.checkpoint)
      hooks.checkpoint(phase);
  }
  detail::Db db(bool write = false) const { return {database, mode, write}; }
  std::filesystem::path part(const std::string &id) const { return directory / "transfers" / id; }
  std::filesystem::path payload_path(const std::string &id) const {
    return directory / "payloads" / id;
  }
  detail::FileLock guard(const std::string &id, bool blocking = true) const;
  Json metadata(Json input) const;
  std::string fingerprint(const Json &metadata, const std::string &digest,
                          std::uint64_t bytes) const;
  Json context(detail::Db &db, const std::string &id) const;
  Json import(detail::Db &db, const std::string &id) const;
  Json upload(detail::Db &db, const std::string &id) const;
  void reserve(detail::Db &db, const std::string &id, std::uint64_t size, const std::string &kind,
               Json digest = nullptr, Json chunk = nullptr, Json key = nullptr,
               Json request = nullptr) const;
  bool cleanup(const std::string &id, bool erase = false) const;
  void install(const std::string &id) const;
  Json attach(detail::Db &db, const std::string &id, const Json &metadata, const std::string &key,
              const std::string &digest, std::uint64_t size) const;
  void enqueue(detail::Db &db, std::string kind, std::string subject, std::string import_id) const;
  std::int64_t event(detail::Db &db, const std::string &import_id, std::string type,
                     Json subjects = Json::array()) const;
  void history(detail::Db &db, const Json &ids, std::int64_t revision) const;
  void counts(detail::Db &db, const std::string &id, const Json &vd, const Json &cd) const;
  void state(detail::Db &db, const Json &row, Json &vd, Json &cd, Json validation = nullptr,
             Json commitment = nullptr, Json run = nullptr, Json commit = nullptr) const;
  bool pipeline(detail::Db &db, const std::string &id) const;
  void release(detail::Db &db, const std::string &id) const;
  void wake() const;
  Json record_public(detail::Db &db, const Json &row, Json state = nullptr) const;
  Json status(detail::Db &db, const std::string &id) const;
  void diagnostic(detail::Db &db, std::string import_id, Json diagnostic, Json record = nullptr,
                  Json group = nullptr, Json run = nullptr,
                  std::string classification = "invalid") const;
};
} // namespace luca::ingestion
