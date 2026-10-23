#pragma once
#include "luca/application/sqlite.hpp"
#include <fstream>
#include <set>

namespace luca::app_host {
using application::Json;
inline Json read(const std::filesystem::path &path, std::size_t maximum = 1024 * 1024) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw ingestion::Failure{"LOCAL_REQUEST_FAILED"};
  std::string text;
  char c;
  while (input.get(c)) {
    if (text.size() == maximum)
      throw ingestion::Failure{"LIMIT_EXCEEDED"};
    text += c;
  }
  if (input.bad())
    throw ingestion::Failure{"LOCAL_REQUEST_FAILED"};
  return Json::parse(text);
}
inline void keys(const Json &value, std::initializer_list<const char *> allowed) {
  if (!value.is_object())
    throw ingestion::Failure{"INVALID_CONFIGURATION"};
  std::set<std::string> set;
  for (auto key : allowed)
    set.insert(key);
  for (const auto &[key, item] : value.items())
    if (!set.contains(key))
      throw ingestion::Failure{"INVALID_CONFIGURATION"};
}
// Configuration paths belong to the operator; no tool accepts a path or profile.
// Existing storage opening/migration semantics are preserved and documented.
inline std::unique_ptr<application::SqliteApplication> open(const std::filesystem::path &profile) {
  auto config = read(profile);
  keys(config, {"books"});
  if (!config.at("books").is_array() || config.at("books").empty() ||
      config.at("books").size() > 16)
    throw ingestion::Failure{"INVALID_CONFIGURATION"};
  std::vector<application::BookBinding> bindings;
  for (const auto &b : config.at("books")) {
    keys(b, {"root", "config", "accounts", "mapping_version", "inspect", "submit", "process",
             "submit_sources", "settlement_actor", "transaction_actor"});
    auto store = ingestion::SqliteStore::open(b.at("root").get<std::string>(), b.at("config"));
    if (!store)
      throw store.error();
    application::BookBinding binding{std::move(*store),
                                     {},
                                     b.value("mapping_version", std::string{}),
                                     b.value("inspect", true),
                                     b.value("submit", false),
                                     b.value("process", false),
                                     b.value("submit_sources", std::vector<std::string>{})};
    binding.settlement_actor = b.value("settlement_actor", std::string{"local-profile"});
    binding.transaction_actor = b.value("transaction_actor", std::string{"local-profile"});
    if (b.contains("accounts"))
      for (const auto &a : b.at("accounts")) {
        keys(a, {"account", "legal_entity", "custodian_account"});
        binding.accounts.push_back(
            {a.at("account"), a.at("legal_entity"), a.at("custodian_account")});
      }
    bindings.push_back(std::move(binding));
  }
  return std::make_unique<application::SqliteApplication>(std::move(bindings));
}
} // namespace luca::app_host
