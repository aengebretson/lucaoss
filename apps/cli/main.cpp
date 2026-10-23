#include "application_host.hpp"
#include <iostream>
#include <map>
using namespace luca::application;
namespace {
constexpr auto help = R"(luca — existing OSS application operations, JSON output
Usage: luca --profile FILE COMMAND [--request JSONFILE]
  books | capabilities | accounts
  import submit | import status | import records | import errors | import record
  policy history | policy resolve | policy result
  transactions | lots | transactions query
  report (collection: journals, trial_balance, obligations, actual_settlements, reconciliation)
  settlement submit | settlement refund | settlement status
  worker run --book ID [--max-jobs N]   bounded drain; requires process grant
  schemas                             permitted MCP schemas for this profile
  --help | --version                   no profile, storage, migration or worker

Requests are JSON objects matching the MCP tool inputSchema, including book_id.
For import submit, --file JSONL may supply payload_utf8 alongside --request.
Submission returns a durable receipt only; validation/admission are asynchronous.
CLI submission never starts a worker. Use worker run or an MCP process configured
with process=true. See docs/cli-mcp-foundations.md for grants, limits and examples.
Exit: 0 success (including inspected invalid data), 2 request, 3 scope/grant,
4 missing/conflicting/stale resource, 5 retryable, 8 internal, 9 outcome unknown.
)";
}
int main(int argc, char **argv) {
  Operation op = Operation::capabilities;
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
      std::cout << help;
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
      std::cout << "luca.foundations/1\n";
      return 0;
    }
    std::map<std::string, std::string> options;
    std::vector<std::string> words;
    const std::set<std::string> permitted = {"--profile", "--request", "--file", "--book",
                                             "--max-jobs"};
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg.starts_with("--")) {
        if (!permitted.contains(arg) || options.contains(arg) || i + 1 == argc)
          throw luca::ingestion::Failure{"INVALID_ARGUMENTS"};
        options[arg] = argv[++i];
      } else
        words.push_back(arg);
    }
    std::string command;
    for (const auto &w : words) {
      if (!command.empty())
        command += ' ';
      command += w;
    }
    const std::map<std::string, Operation> mapping = {
        {"books", Operation::books},
        {"capabilities", Operation::capabilities},
        {"accounts", Operation::accounts},
        {"import submit", Operation::submit_import},
        {"import status", Operation::import_status},
        {"import records", Operation::read_records},
        {"import errors", Operation::read_diagnostics},
        {"import record", Operation::read_record},
        {"policy history", Operation::policy_history},
        {"policy resolve", Operation::resolve_policy},
        {"policy result", Operation::policy_result},
        {"transactions", Operation::transactions},
        {"lots", Operation::lots},
        {"report", Operation::accounting_report},
        {"transactions query", Operation::query_transactions},
        {"settlement submit", Operation::submit_settlement},
        {"settlement refund", Operation::submit_refund},
        {"settlement status", Operation::settlement_status}};
    if (mapping.contains(command))
      op = mapping.at(command);
    else if (command != "schemas" && command != "worker run")
      throw luca::ingestion::Failure{"INVALID_ARGUMENTS"};
    if (!options.contains("--profile"))
      throw luca::ingestion::Failure{"INVALID_ARGUMENTS"};
    if (options.contains("--file") && op != Operation::submit_import)
      throw luca::ingestion::Failure{"INVALID_ARGUMENTS"};
    if (command != "worker run" && (options.contains("--book") || options.contains("--max-jobs")))
      throw luca::ingestion::Failure{"INVALID_ARGUMENTS"};
    auto request = options.contains("--request") ? luca::app_host::read(options.at("--request"))
                                                 : Json::object();
    if (options.contains("--file")) {
      if (request.contains("payload_utf8"))
        throw luca::ingestion::Failure{"INVALID_ARGUMENTS"};
      std::ifstream input(options.at("--file"), std::ios::binary);
      if (!input)
        throw luca::ingestion::Failure{"LOCAL_REQUEST_FAILED"};
      std::string body;
      char c;
      while (input.get(c)) {
        if (body.size() == 65536)
          throw luca::ingestion::Failure{"LIMIT_EXCEEDED"};
        body += c;
      }
      if (input.bad())
        throw luca::ingestion::Failure{"LOCAL_REQUEST_FAILED"};
      request["payload_utf8"] = std::move(body);
    }
    auto app = luca::app_host::open(options.at("--profile"));
    if (command == "schemas") {
      if (options.size() != 1)
        throw luca::ingestion::Failure{"INVALID_ARGUMENTS"};
      std::cout << Json{{"tools", app->tools()}}.dump() << '\n';
      return 0;
    }
    if (command == "worker run") {
      if (!options.contains("--book") || options.contains("--request") ||
          options.contains("--file"))
        throw luca::ingestion::Failure{"INVALID_ARGUMENTS"};
      auto max = options.contains("--max-jobs") ? options.at("--max-jobs") : "10000";
      if (max.empty() || max.find_first_not_of("0123456789") != std::string::npos)
        throw luca::ingestion::Failure{"INVALID_ARGUMENTS"};
      auto result = app->drain(options.at("--book"), std::stoull(max));
      if (!result)
        throw result.error();
      std::cout << Json{{"jobs", *result},
                        {"completion", "bounded_drain; blocked_or_future_work_may_remain"}}
                       .dump()
                << '\n';
      return 0;
    }
    auto result = app->call(op, std::move(request));
    const int code = result ? 0 : exit_code(result.error());
    std::cout << document(op, std::move(result)).dump() << '\n';
    return code;
  } catch (const luca::ingestion::Failure &e) {
    std::cout << document(op, std::unexpected(e)).dump() << '\n';
    return exit_code(e);
  } catch (const std::exception &) {
    luca::ingestion::Failure e{"INVALID_ARGUMENTS"};
    std::cout << document(op, std::unexpected(e)).dump() << '\n';
    return 2;
  }
}
