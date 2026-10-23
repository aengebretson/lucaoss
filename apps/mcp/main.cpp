#include "application_host.hpp"
#include <iostream>
using namespace luca::application;
namespace {
Json error(Json id, int code, const char *message) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}
// Cap bytes before parsing or allocating an arbitrary newline-delimited frame.
std::optional<std::string> frame(std::istream &input) {
  std::string text;
  char c;
  while (input.get(c)) {
    if (c == '\n')
      return text;
    if (text.size() == 1024 * 1024)
      throw luca::ingestion::Failure{"FRAME_LIMIT_EXCEEDED"};
    text += c;
  }
  if (!text.empty())
    throw luca::ingestion::Failure{"TRUNCATED_FRAME"};
  return {};
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
      std::cout << "Usage: luca-mcp --profile FILE\nStandard newline-delimited stdio MCP; no "
                   "network listener.\nRead-only grants are the default. process=true owns native "
                   "background workers.\n";
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
      std::cout << "luca.foundations/1\n";
      return 0;
    }
    if (argc != 3 || std::string_view(argv[1]) != "--profile")
      throw luca::ingestion::Failure{"INVALID_ARGUMENTS"};
    auto app = luca::app_host::open(argv[2]);
    bool initialized = false, ready = false;
    while (auto line = frame(std::cin)) {
      Json request;
      try {
        request = Json::parse(*line);
      } catch (const Json::exception &) {
        std::cout << error(nullptr, -32700, "Parse error").dump() << std::endl;
        continue;
      }
      const auto id = request.is_object() ? request.value("id", Json()) : Json();
      if (!request.is_object() || request.value("jsonrpc", Json()) != "2.0" ||
          !request.contains("method") || !request["method"].is_string() ||
          (request.contains("id") && !(id.is_string() || id.is_number_integer()))) {
        std::cout << error(nullptr, -32600, "Invalid Request").dump() << std::endl;
        continue;
      }
      const auto method = request.at("method").get<std::string>();
      if (!request.contains("id")) {
        if (initialized && method == "notifications/initialized")
          ready = true;
        // Notifications, including cancellation, never retract durable work.
        continue;
      }
      auto params = request.value("params", Json::object());
      Json response;
      try {
        if (!params.is_object())
          response = error(id, -32602, "Invalid params");
        else if (method == "ping")
          response = {{"jsonrpc", "2.0"}, {"id", id}, {"result", Json::object()}};
        else if (method == "initialize") {
          if (initialized || !params.contains("protocolVersion") ||
              !params["protocolVersion"].is_string() || !params.contains("capabilities") ||
              !params["capabilities"].is_object() || !params.contains("clientInfo") ||
              !params["clientInfo"].is_object() || !params["clientInfo"].contains("name") ||
              !params["clientInfo"]["name"].is_string() ||
              !params["clientInfo"].contains("version") ||
              !params["clientInfo"]["version"].is_string())
            response = error(id, -32602, "Invalid initialization");
          else {
            auto started = app->start_workers();
            if (!started)
              throw started.error();
            initialized = true;
            const auto version = params.at("protocolVersion").get<std::string>();
            response = {{"jsonrpc", "2.0"},
                        {"id", id},
                        {"result",
                         {{"protocolVersion", version == "2025-06-18" ? version : "2025-11-25"},
                          {"capabilities", {{"tools", {{"listChanged", false}}}}},
                          {"serverInfo", {{"name", "luca-oss"}, {"version", "0.1.0"}}},
                          {"instructions",
                           "Durable import receipts precede asynchronous validation and admission. "
                           "Inspect retained status and diagnostics before asserting financial "
                           "completion. Lot results are economic LIFO projections only."}}}};
          }
        } else if (!ready)
          response = error(id, -32002, "Server not initialized");
        else if (method == "tools/list") {
          if (!params.empty())
            response = error(id, -32602, "Invalid tool-list cursor");
          else
            response = {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"tools", app->tools()}}}};
        } else if (method == "tools/call") {
          if (!params.contains("name") || !params["name"].is_string() ||
              (params.contains("arguments") && !params["arguments"].is_object()))
            response = error(id, -32602, "Invalid tool call");
          else {
            const auto tool = params.at("name").get<std::string>();
            auto op = operation(tool);
            const auto permitted = app->tools();
            const bool listed = std::any_of(permitted.begin(), permitted.end(),
                                            [&](const auto &t) { return t.at("name") == tool; });
            if (!op || !listed)
              response = error(id, -32602, "Unknown or unavailable tool");
            else {
              auto result = app->call(*op, params.value("arguments", Json::object()));
              const bool failed = !result;
              auto body = document(*op, std::move(result));
              response = {{"jsonrpc", "2.0"},
                          {"id", id},
                          {"result",
                           {{"content", Json::array({{{"type", "text"}, {"text", body.dump()}}})},
                            {"structuredContent", body},
                            {"isError", failed}}}};
            }
          }
        } else
          response = error(id, -32601, "Method not found");
      } catch (const luca::ingestion::Failure &) {
        response = error(id, -32603, "Application host unavailable");
      } catch (const std::exception &) {
        response = error(id, -32602, "Invalid params");
      }
      std::cout << response.dump() << std::endl;
    }
    app->stop();
    return 0;
  } catch (const luca::ingestion::Failure &e) {
    std::cerr << Json{{"code", e.code}, {"retryable", e.retryable}}.dump() << '\n';
    return exit_code(e);
  } catch (const std::exception &) {
    std::cerr << "{\"code\":\"INVALID_CONFIGURATION\"}\n";
    return 2;
  }
}
