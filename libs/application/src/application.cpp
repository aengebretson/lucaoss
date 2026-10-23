#include "internal.hpp"
#include "schemas.hpp"
#include <charconv>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

namespace luca::application {
using namespace detail;
namespace {
constexpr std::string_view names[] = {
    "books",         "capabilities",     "accounts",    "submit_import",  "import_status",
    "read_records",  "read_diagnostics", "read_record", "policy_history", "resolve_policy",
    "policy_result", "transactions",     "lots"};
const Json &catalog() {
  static const auto value = Json::parse(foundations_schemas).at("tools");
  return value;
}
// Deliberately limited to the closed schema vocabulary used by this catalog.
// Unknown schema keywords are caught by schema contract tests; no runtime schema
// or financial rules are accepted from a caller.
void validate(const Json &value, const Json &schema) {
  if (schema.contains("type")) {
    const auto t = schema.at("type").get<std::string>();
    require(t == "object"    ? value.is_object()
            : t == "string"  ? value.is_string()
            : t == "integer" ? value.is_number_integer()
            : t == "boolean" ? value.is_boolean()
                             : false,
            "INVALID_ARGUMENTS");
  }
  if (schema.contains("enum"))
    require(std::find(schema["enum"].begin(), schema["enum"].end(), value) != schema["enum"].end(),
            "INVALID_ARGUMENTS");
  if (value.is_object()) {
    for (const auto &key : schema.value("required", Json::array()))
      require(value.contains(key.get<std::string>()), "INVALID_ARGUMENTS");
    const auto properties = schema.value("properties", Json::object());
    for (const auto &[key, item] : value.items()) {
      require(properties.contains(key), "INVALID_ARGUMENTS");
      validate(item, properties.at(key));
    }
  }
  if (value.is_string()) {
    const auto &v = value.get_ref<const std::string &>();
    require(v.size() >= schema.value("minLength", std::size_t{0}) &&
                v.size() <= schema.value("maxLength", std::size_t{65536}),
            "INVALID_ARGUMENTS");
    if (schema.contains("pattern"))
      require(std::regex_match(v, std::regex(schema["pattern"].get<std::string>())),
              "INVALID_ARGUMENTS");
  }
  if (value.is_number_integer()) {
    require(!value.is_number_unsigned() || value.get<std::uint64_t>() <= INT64_MAX,
            "INVALID_ARGUMENTS");
    const auto n = value.get<std::int64_t>();
    require(n >= schema.value("minimum", INT64_MIN) && n <= schema.value("maximum", INT64_MAX),
            "INVALID_ARGUMENTS");
  }
}
Json account_rows(const BookBinding &b) {
  Json rows = Json::array();
  for (const auto &a : b.accounts)
    rows.push_back({{"account", a.account},
                    {"legal_entity", a.legal_entity},
                    {"custodian_account", a.custodian_account}});
  return rows;
}
} // namespace
std::uint64_t detail::sequence(const Json &value) {
  require(value.is_string(), "INVALID_ARGUMENTS");
  const auto text = value.get<std::string>();
  std::uint64_t n = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), n);
  require(error == std::errc{} && end == text.data() + text.size() && n <= INT64_MAX,
          "INVALID_ARGUMENTS");
  return n;
}
std::optional<Operation> operation(std::string_view text) {
  if (text.starts_with("luca_"))
    text.remove_prefix(5);
  for (std::size_t i = 0; i < std::size(names); ++i)
    if (names[i] == text)
      return static_cast<Operation>(i);
  return {};
}
std::string_view name(Operation op) {
  const auto i = static_cast<std::size_t>(op);
  return i < std::size(names) ? names[i] : "unknown";
}
struct SqliteApplication::Impl {
  std::vector<BookBinding> books;
  std::vector<std::jthread> workers;
  explicit Impl(std::vector<BookBinding> input) : books(std::move(input)) {
    std::set<std::string> seen;
    for (const auto &b : books) {
      auto c = take(b.store.capabilities());
      require(seen.insert(c.at("book_id")).second, "DUPLICATE_BOOK_BINDING");
      require(!b.submit || !b.submit_sources.empty(), "INVALID_APPLICATION_BINDING");
      require(b.mapping_version.size() <= 256, "INVALID_APPLICATION_BINDING");
      std::set<std::string> accounts;
      for (const auto &a : b.accounts)
        require(!a.account.empty() && a.account.size() <= 256 && !a.legal_entity.empty() &&
                    a.legal_entity.size() <= 256 && !a.custodian_account.empty() &&
                    a.custodian_account.size() <= 256 && accounts.insert(a.account).second,
                "INVALID_APPLICATION_BINDING");
      require(b.accounts.empty() || !b.mapping_version.empty(), "INVALID_APPLICATION_BINDING");
    }
  }
  const BookBinding &book(std::string_view id) const {
    for (const auto &b : books)
      if (take(b.store.capabilities()).at("book_id") == id)
        return b;
    throw ingestion::Failure{"BOOK_NOT_AUTHORIZED"};
  }
};
SqliteApplication::SqliteApplication(std::vector<BookBinding> books)
    : impl_(std::make_unique<Impl>(std::move(books))) {}
SqliteApplication::~SqliteApplication() { stop(); }
Json SqliteApplication::tools() const {
  auto tools = Json::array();
  const bool submit =
      std::any_of(impl_->books.begin(), impl_->books.end(), [](const auto &b) { return b.submit; });
  const bool inspect = std::any_of(impl_->books.begin(), impl_->books.end(),
                                   [](const auto &b) { return b.inspect; });
  for (const auto &t : catalog())
    if (t.at("name") == "luca_books" || (t.at("name") == "luca_submit_import" ? submit : inspect))
      tools.push_back(t);
  return tools;
}
Result<Json> SqliteApplication::call(Operation op, Json a) const {
  try {
    const auto index = static_cast<std::size_t>(op);
    require(index < catalog().size(), "INVALID_ARGUMENTS");
    validate(a, catalog().at(index).at("inputSchema"));
    if (op == Operation::books) {
      Json rows = Json::array();
      for (const auto &b : impl_->books)
        if (b.inspect || b.submit) {
          auto c = take(b.store.capabilities());
          rows.push_back({{"book_id", c.at("book_id")},
                          {"tenant_id", c.at("tenant_id")},
                          {"inspect", b.inspect},
                          {"submit", b.submit},
                          {"process", b.process}});
        }
      return Json{{"items", rows}};
    }
    const auto &b = impl_->book(a.at("book_id").get<std::string>());
    require(op == Operation::submit_import ? b.submit : b.inspect, "PERMISSION_DENIED");
    const auto &store = b.store;
    const auto id = a.value("import_id", std::string{});
    switch (op) {
    case Operation::capabilities: {
      auto result = take(store.capabilities());
      result["api_version"] = "luca.foundations/1";
      result["max_inline_bytes"] = 65536;
      result["max_adapter_page_size"] =
          std::min<std::size_t>(100, result.at("limits").at("max_page_size").get<std::size_t>());
      result["mapping_digest"] = detail::mapping_digest(b);
      result["max_projection_records"] = 10000;
      result["source_ids"] = b.submit_sources;
      result["operations"] = Json::array();
      for (const auto &t : tools())
        if (b.submit || t.at("name") != "luca_submit_import")
          result["operations"].push_back(t.at("name"));
      auto state = take(store.book_state());
      for (auto &[key, value] : state.items())
        if (value.is_number_integer())
          value = std::to_string(value.get<std::int64_t>());
      result["ledger_state"] = state;
      result["report_jobs_available"] = false;
      result["mapping_version"] = b.mapping_version;
      return result;
    }
    case Operation::accounts:
      return Json{{"items", account_rows(b)},
                  {"mapping_version", b.mapping_version},
                  {"authority", "configured_bindings"}};
    case Operation::submit_import: {
      const auto source = a.at("metadata").at("source_id").get<std::string>();
      require(std::find(b.submit_sources.begin(), b.submit_sources.end(), source) !=
                  b.submit_sources.end(),
              "SOURCE_NOT_AUTHORIZED");
      const auto body = a.at("payload_utf8").get<std::string>();
      require(body.size() <= 65536, "LIMIT_EXCEEDED");
      std::istringstream input(body);
      auto accepted = store.receive(a.at("metadata"), a.at("idempotency_key"), input, body.size());
      if (!accepted && accepted.error().code == "STORAGE_UNAVAILABLE")
        return std::unexpected(ingestion::Failure{"OUTCOME_UNKNOWN", true});
      auto receipt = take(std::move(accepted));
      // Existing receipt includes URL-shaped locators; no network service exists.
      receipt.erase("status_url");
      receipt.erase("records_url");
      receipt.erase("diagnostics_url");
      receipt["durable_acceptance"] = true;
      receipt["completion"] = "pending_or_in_progress";
      return receipt;
    }
    case Operation::import_status:
      return store.status(id);
    case Operation::read_records:
    case Operation::read_diagnostics:
      return store.page(
          op == Operation::read_records ? "records" : "diagnostics", id,
          a.value("filters", Json::object()), a.value("cursor", std::string{}),
          a.value(
              "limit",
              std::min<std::size_t>(
                  100,
                  take(store.capabilities()).at("limits").at("max_page_size").get<std::size_t>())));
    case Operation::read_record:
      return store.record(id, a.at("record_id"), a.value("run_id", std::string{}));
    case Operation::policy_history:
      return store.policy_history();
    case Operation::resolve_policy:
      return store.resolve_policy(a.at("target"), a.at("effective_at"),
                                  sequence(a.at("through_sequence")));
    case Operation::policy_result:
      return store.policy_result(a.at("evidence_id"));
    case Operation::transactions:
    case Operation::lots:
      return detail::project(b, a, op == Operation::lots);
    default:
      throw ingestion::Failure{"INVALID_ARGUMENTS"};
    }
  } catch (const ingestion::Failure &e) {
    return std::unexpected(e);
  } catch (const Json::exception &) {
    return std::unexpected(ingestion::Failure{"INVALID_ARGUMENTS"});
  } catch (const std::exception &) {
    return std::unexpected(ingestion::Failure{"INTERNAL_ERROR"});
  }
}
Result<std::size_t> SqliteApplication::drain(std::string id, std::size_t max_jobs) const {
  try {
    const auto &b = impl_->book(id);
    require(b.process, "PERMISSION_DENIED");
    require(max_jobs >= 1 && max_jobs <= 100000, "INVALID_ARGUMENTS");
    ingestion::Worker worker(b.store);
    return worker.drain(max_jobs);
  } catch (const ingestion::Failure &e) {
    return std::unexpected(e);
  } catch (const std::exception &) {
    return std::unexpected(ingestion::Failure{"INTERNAL_ERROR"});
  }
}
Result<void> SqliteApplication::start_workers() {
  try {
    require(impl_->workers.empty(), "WORKERS_ALREADY_STARTED");
    for (const auto &b : impl_->books)
      if (b.process) {
        auto worker = std::make_shared<ingestion::Worker>(b.store);
        impl_->workers.emplace_back([worker](std::stop_token stop) { worker->run(stop); });
      }
    return {};
  } catch (const ingestion::Failure &e) {
    stop();
    return std::unexpected(e);
  } catch (const std::exception &) {
    stop();
    return std::unexpected(ingestion::Failure{"INTERNAL_ERROR"});
  }
}
void SqliteApplication::stop() noexcept {
  for (auto &t : impl_->workers)
    t.request_stop();
  impl_->workers.clear();
}
int exit_code(const ingestion::Failure &e) {
  if (e.code == "OUTCOME_UNKNOWN")
    return 9;
  if (e.code == "INVALID_CURSOR")
    return 4;
  if (e.code.starts_with("INVALID_") || e.code == "UNSUPPORTED_CONTEXT_OR_FORMAT" ||
      e.code == "LIMIT_EXCEEDED" || e.code == "PROJECTION_LIMIT_EXCEEDED" ||
      e.code == "LOT_ACCOUNT_REQUIRED" || e.code == "ENTITY_POLICY_REQUIRED")
    return 2;
  if (e.code == "PERMISSION_DENIED" || e.code == "BOOK_NOT_AUTHORIZED" ||
      e.code == "SOURCE_NOT_AUTHORIZED")
    return 3;
  if (e.retryable)
    return 5;
  if (e.code == "NOT_FOUND" || e.code.find("CONFLICT") != std::string::npos ||
      e.code.find("CURSOR") != std::string::npos ||
      e.code.find("UNAVAILABLE") != std::string::npos || e.code == "POLICY_RESULT_NOT_FOUND")
    return 4;
  return 8;
}
Json document(Operation op, Result<Json> result) {
  Json out = {{"api_version", "luca.foundations/1"},
              {"operation", name(op)},
              {"ok", bool(result)},
              {"data", nullptr},
              {"error", nullptr}};
  if (result)
    out["data"] = std::move(*result);
  else {
    const auto &e = result.error();
    const auto code = exit_code(e);
    out["error"] = {
        {"code", e.code},
        {"retryable", e.retryable},
        {"exit_code", code},
        {"outcome", code == 9                        ? "unknown"
                    : op == Operation::submit_import ? "not_accepted"
                                                     : "read_failed"},
        {"retry", code == 9 ? "Retry the identical book, metadata, payload and idempotency key to "
                              "recover the receipt."
                  : e.retryable
                      ? "Retry with bounded backoff; preserve the same idempotency key and payload."
                      : "Inspect code and correct the request or configuration before retrying."}};
  }
  return out;
}
} // namespace luca::application
