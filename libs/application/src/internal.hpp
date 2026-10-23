#pragma once
#include "luca/application/sqlite.hpp"
namespace luca::application::detail {
inline void require(bool ok, const char *code) {
  if (!ok)
    throw ingestion::Failure{code};
}
template <class T> T take(Result<T> result) {
  if (!result)
    throw result.error();
  return std::move(*result);
}
std::uint64_t sequence(const Json &value);
std::string mapping_digest(const BookBinding &);
Json project(const BookBinding &, const Json &, bool lots);
} // namespace luca::application::detail
