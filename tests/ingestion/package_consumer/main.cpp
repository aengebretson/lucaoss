#include <iostream>
#include <luca/ingestion/sqlite.hpp>
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  using namespace luca::ingestion;
  Json config = {{"tenant_id", "consumer"},
                 {"book_id", "book"},
                 {"source_ids", Json::array({"source"})},
                 {"contexts",
                  {{"context",
                    {{"adapter", "luca.transactions.jsonl/1"},
                     {"normalization", "source-book-identity/1"},
                     {"rules", "cash-equity/1"},
                     {"engine", "luca/0.1"},
                     {"reference_snapshot", "refs"}}}}}};
  auto opened = SqliteStore::open(argv[1], config);
  if (!opened)
    return 3;
  auto refs = opened->register_reference(
      "refs", {{"accounts", Json::array({"account"})}, {"instruments", Json::array()}});
  if (!refs)
    return 4;
  AsyncHost host(*opened, 1);
  Json metadata = {{"source_id", "source"},
                   {"format", "luca.transactions.jsonl/1"},
                   {"validation_context_id", "context"}};
  auto future =
      host.submit(metadata, "submission",
                  "{\"record_id\":\"r\",\"economic_event_id\":\"e\",\"action\":\"originate\","
                  "\"account_id\":\"account\",\"event\":{\"type\":\"cash_movement\",\"effective_"
                  "at\":\"2026-10-20T00:00:00Z\",\"currency\":\"USD\",\"amount\":\"100\"}}\n");
  if (!future)
    return 5;
  auto receipt = future->get();
  if (!receipt)
    return 6;
  host.stop();
  Worker worker(*opened);
  if (!worker.drain())
    return 7;
  auto status = opened->status((*receipt)["import_id"]);
  if (!status || !(*status)["all_records_committed"])
    return 8;
  std::cout << "Installed native library accepted one record without Python\n";
}
