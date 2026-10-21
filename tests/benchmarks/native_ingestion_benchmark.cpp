#include "luca/ingestion/sqlite.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
using namespace luca::ingestion;
namespace {
template <class T> T take(Result<T> result) {
  if (!result)
    throw result.error();
  return std::move(*result);
}
Json config(std::string book) {
  return {{"tenant_id", "example-tenant"},
          {"book_id", book},
          {"source_ids", Json::array({"custodian-a"})},
          {"contexts",
           {{"cash-equity-v1-ref42",
             {{"adapter", "luca.transactions.jsonl/1"},
              {"normalization", "source-book-identity/1"},
              {"rules", "cash-equity/1"},
              {"engine", "luca/0.1"},
              {"reference_snapshot", "ref42"}}}}}};
}
Json meta() {
  return {{"source_id", "custodian-a"},
          {"format", "luca.transactions.jsonl/1"},
          {"validation_context_id", "cash-equity-v1-ref42"}};
}
std::string payload(std::size_t count, std::string prefix, std::size_t group, bool mixed = false) {
  std::string result;
  for (std::size_t i = 0; i < count; ++i) {
    Json row = {{"record_id", prefix + "-r" + std::to_string(i)},
                {"economic_event_id", prefix + "-e" + std::to_string(i)},
                {"action", "originate"},
                {"account_id", "portfolio-a"},
                {"event",
                 {{"type", "cash_movement"},
                  {"effective_at", "2026-10-20T00:00:00Z"},
                  {"currency", "USD"},
                  {"amount", mixed && i % 20 == 0 ? "invalid" : "1.000000"}}}};
    if (group > 1)
      row["atomic_group"] = "g" + std::to_string(i / group);
    result += row.dump() + "\n";
  }
  return result;
}
SqliteStore open(const std::filesystem::path &path, std::string book) {
  auto store = take(SqliteStore::open(path, config(book)));
  take(store.register_reference(
      "ref42", {{"accounts", Json::array({"portfolio-a"})}, {"instruments", Json::array()}}));
  return store;
}
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}
Json process(Worker &worker) {
  Json seconds = Json::object(), jobs = Json::object();
  while (true) {
    auto at = Clock::now();
    auto result = take(worker.run_once());
    auto duration = elapsed(at);
    if (!result)
      break;
    if (result->outcome != "processed")
      throw Failure{result->code};
    seconds[result->kind] = seconds.value(result->kind, 0.0) + duration;
    jobs[result->kind] = jobs.value(result->kind, 0) + 1;
  }
  return {{"job_seconds", seconds}, {"jobs", jobs}};
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: native_ingestion_benchmark OUTPUT.json\n";
    return 2;
  }
  char pattern[] = "/tmp/luca-native-benchmark-XXXXXX";
  auto *directory = ::mkdtemp(pattern);
  if (!directory)
    return 1;
  std::filesystem::path root = directory;
  try {
    Json result = {{"schema", "luca.native-ingestion-benchmark/1"},
                   {"language", "C++23"},
                   {"scope",
                    "Native durable receipt and direct in-process validation/admission; same "
                    "synthetic cash/group workloads as Python baseline; no HTTP/report/GPU timing"},
                   {"samples", Json::array()},
                   {"concurrency", Json::array()}};
    for (const std::size_t count : {1000, 10000, 100000})
      for (unsigned repetition = 0; repetition < 3; ++repetition) {
        auto store = open(root, "book-" + std::to_string(count) + "-" + std::to_string(repetition));
        auto body = payload(count, "initial", 256);
        std::istringstream input(body);
        auto at = Clock::now();
        auto receipt = take(store.receive(meta(), "initial", input, body.size()));
        auto receipt_seconds = elapsed(at);
        if (take(store.status(receipt["import_id"]))["enumeration"]["state"] != "pending")
          throw Failure{"BENCHMARK_RECEIPT_SCOPE"};
        Worker worker(store);
        at = Clock::now();
        auto stages = process(worker);
        auto processing_seconds = elapsed(at);
        if (!take(store.status(receipt["import_id"]))["all_records_committed"] ||
            take(store.book_state())["last_sequence"] != count)
          throw Failure{"BENCHMARK_RESULT"};
        auto extra = payload(256, "incremental", 256);
        std::istringstream extra_input(extra);
        auto increment = take(store.receive(meta(), "incremental", extra_input, extra.size()));
        at = Clock::now();
        auto incremental = process(worker);
        auto incremental_seconds = elapsed(at);
        if (!take(store.status(increment["import_id"]))["all_records_committed"] ||
            take(store.book_state())["last_sequence"] != count + 256)
          throw Failure{"BENCHMARK_INCREMENTAL_RESULT"};
        Json sample = {{"records", count},
                       {"group_size", 256},
                       {"repetition", repetition},
                       {"bytes", body.size()},
                       {"receipt_seconds", receipt_seconds},
                       {"processing_seconds", processing_seconds},
                       {"incremental_records", 256},
                       {"incremental_processing_seconds", incremental_seconds},
                       {"incremental", incremental},
                       {"profile", take(store.capabilities())["durability_profile"]},
                       {"sqlite", take(store.capabilities())["sqlite_version"]},
                       {"hash_backend", take(store.capabilities())["hash_backend"]}};
        sample.update(stages);
        result["samples"].push_back(sample);
      }
    for (bool separate : {false, true})
      for (const std::size_t workers : {1, 2, 4}) {
        std::vector<SqliteStore> stores;
        for (std::size_t i = 0; i < workers; ++i)
          stores.push_back(open(root, "parallel-" + std::to_string(separate) + "-" +
                                          std::to_string(workers) + "-" +
                                          std::to_string(separate ? i : 0)));
        std::vector<std::pair<std::size_t, std::string>> imports;
        for (std::size_t i = 0; i < 4; ++i) {
          auto body = payload(100, "i" + std::to_string(i), 1, true);
          std::istringstream input(body);
          auto receipt = take(
              stores[i % workers].receive(meta(), "i" + std::to_string(i), input, body.size()));
          imports.emplace_back(i % workers, receipt["import_id"]);
        }
        std::vector<Json> outcomes(workers);
        std::vector<std::jthread> threads;
        auto at = Clock::now();
        for (std::size_t i = 0; i < workers; ++i)
          threads.emplace_back([&, i] {
            Worker worker(stores[i]);
            outcomes[i] = process(worker);
          });
        threads.clear();
        const auto duration = elapsed(at);
        for (const auto &[index, id] : imports) {
          auto state = take(stores[index].status(id));
          if (state["validation_counts"]["invalid"] != 5 ||
              state["commitment_record_counts"]["committed"] != 95)
            throw Failure{"BENCHMARK_MIXED_RESULT"};
        }
        result["concurrency"].push_back({{"workers", workers},
                                         {"separate_books", separate},
                                         {"records", 400},
                                         {"invalid_records", 20},
                                         {"wall_seconds", duration},
                                         {"runs", outcomes}});
      }
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    result["peak_rss_platform_units"] = usage.ru_maxrss;
    std::ofstream output(argv[1]);
    output << result.dump(2) << '\n';
    if (!output)
      throw Failure{"OUTPUT_WRITE_FAILED"};
    std::filesystem::remove_all(root);
    std::cout << "9 native samples and 6 partition workloads passed\n";
    return 0;
  } catch (const Failure &error) {
    std::cerr << error.code << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  std::filesystem::remove_all(root);
  return 1;
}
