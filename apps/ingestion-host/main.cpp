#include "luca/ingestion/sqlite.hpp"
#include <csignal>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>
using namespace luca::ingestion;
namespace {
volatile std::sig_atomic_t stopped = 0;
void stop_signal(int) { stopped = 1; }
Json read(const std::string &file) {
  std::ifstream input(file);
  if (!input)
    throw Failure{"LOCAL_REQUEST_FAILED"};
  return Json::parse(input);
}
template <class T> T take(Result<T> result) {
  if (!result)
    throw result.error();
  return std::move(*result);
}
} // namespace
int main(int argc, char **argv) {
  try {
    std::map<std::string, std::string> options;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg.starts_with("--")) {
        if (i + 1 == argc)
          throw Failure{"INVALID_ARGUMENTS"};
        options[arg] = argv[++i];
      } else
        positional.push_back(arg);
    }
    if (!options.contains("--root") || !options.contains("--config") || positional.empty()) {
      std::cerr
          << "Usage: luca-ingest --root DIR --config FILE COMMAND [ID] [--request JSONFILE]\n";
      return 2;
    }
    auto config = read(options.at("--config"));
    auto store = take(SqliteStore::open(options.at("--root"), config));
    const auto &command = positional[0];
    auto id = positional.size() > 1 ? positional[1] : "";
    Json result;
    const auto limit = options.contains("--limit") ? std::stoull(options.at("--limit")) : 100;
    const auto after = options.contains("--after") ? std::stoull(options.at("--after")) : 0;
    auto request = [&] { return read(options.at("--request")); };
    if (command == "capabilities")
      result = take(store.capabilities());
    else if(command == "policy-history")result=take(store.policy_history());
    else if(command == "policy-result")result=take(store.policy_result(id));
    else if(command == "policy-put") {
      auto r=request();result=take(store.put_policy(r.at("revision"),r.at("expected_sequence"),r.at("actor")));
    }
    else if(command == "policy-resolve") {
      auto r=request();std::optional<std::uint64_t> through;
      if(r.contains("through_sequence"))through=r.at("through_sequence");
      result=take(store.resolve_policy(r.at("target"),r.at("effective_at"),through));
    }
    else if (command == "receive") {
      auto metadata = read(options.at("--metadata"));
      std::ifstream input(options.at("--payload"), std::ios::binary);
      if (!input)
        throw Failure{"PAYLOAD_UNAVAILABLE"};
      result = take(store.receive(metadata, options.at("--key"), input,
                                  std::filesystem::file_size(options.at("--payload"))));
    } else if (command == "worker") {
      Worker worker(store);
      auto mode = options.contains("--mode") ? options.at("--mode") : "once";
      if (mode == "drain")
        result = {{"jobs", take(worker.drain())}};
      else if (mode == "watch") {
        std::signal(SIGINT, stop_signal);
        std::signal(SIGTERM, stop_signal);
        std::jthread running([&](std::stop_token stop) { worker.run(stop); });
        while (!stopped)
          std::this_thread::sleep_for(std::chrono::milliseconds{25});
        running.request_stop();
        running.join();
        result = {{"state", "stopped"}};
      } else if (mode == "once") {
        auto job = take(worker.run_once());
        result = job ? Json{{"work_id", job->work_id},
                            {"kind", job->kind},
                            {"outcome", job->outcome},
                            {"code", job->code}}
                     : Json();
      } else
        throw Failure{"INVALID_ARGUMENTS"};
    } else if (command == "status")
      result = take(store.status(id));
    else if (command == "group")
      result = take(store.group(id, positional.at(2)));
    else if (command == "record")
      result = take(store.record(id, positional.at(2),
                                 options.contains("--run-id") ? options.at("--run-id") : ""));
    else if (command == "records" || command == "diagnostics" || command == "validation-runs" ||
             command == "commits")
      result = take(store.page(command == "validation-runs" ? "validation_runs" : command, id,
                               options.contains("--filters") ? Json::parse(options.at("--filters"))
                                                             : Json::object(),
                               options.contains("--cursor") ? options.at("--cursor") : "", limit));
    else if (command == "events")
      result = take(store.events(id, after, limit));
    else if (command == "work-attempts")
      result = take(store.work_attempts(id, after, limit));
    else if (command == "ledger-state")
      result = take(store.book_state());
    else if (command == "ledger-records")
      result = take(store.ledger_records(after, limit));
    else if (command == "register-reference")
      result = take(store.register_reference(
          id, options.contains("--definition") ? read(options.at("--definition")) : request()));
    else if (command == "register-context")
      result = take(store.register_context(
          id, options.contains("--definition") ? read(options.at("--definition")) : request()));
    else if (command == "revalidate")
      result = take(store.revalidate(id, options.at("--key"), request()));
    else if (command == "upload-create") {
      auto r = request();
      result =
          take(store.create_upload(r["key"], r["expected_bytes"], r["sha256"], r["chunk_bytes"]));
    } else if (command == "upload-status")
      result = take(store.upload_status(id));
    else if (command == "upload-put") {
      std::ifstream input(options.at("--payload"), std::ios::binary);
      if (!input)
        throw Failure{"PAYLOAD_UNAVAILABLE"};
      auto size = std::filesystem::file_size(options.at("--payload"));
      if (size > 4 * 1024 * 1024)
        throw Failure{"LIMIT_EXCEEDED"};
      std::string bytes(std::istreambuf_iterator<char>{input}, {});
      result = take(
          store.put_chunk(id, std::stoull(options.at("--index")), bytes, options.at("--sha256")));
    } else if (command == "upload-verify")
      result = take(store.verify_upload(id));
    else if (command == "receive-upload")
      result = take(store.receive_upload(read(options.at("--metadata")), options.at("--key"), id));
    else if (command == "recover")
      result = take(store.recover(options.contains("--after") ? options.at("--after") : "", limit));
    else if (command == "payload") {
      const auto output = options.at("--output");
      if (std::filesystem::exists(output))
        throw Failure{"OUTPUT_EXISTS"};
      std::ofstream target(output, std::ios::binary | std::ios::noreplace);
      result = take(store.payload(
          id, target, options.contains("--offset") ? std::stoull(options.at("--offset")) : 0,
          options.contains("--length")
              ? std::optional<std::uint64_t>{std::stoull(options.at("--length"))}
              : std::nullopt));
    } else
      throw Failure{"INVALID_ARGUMENTS"};
    std::cout << result.dump() << '\n';
    return 0;
  } catch (const Failure &e) {
    std::cerr << Json{{"code", e.code}, {"retryable", e.retryable}}.dump() << '\n';
  } catch (const std::exception &) {
    std::cerr << "{\"code\":\"LOCAL_REQUEST_FAILED\",\"retryable\":false}\n";
  }
  return 1;
}
