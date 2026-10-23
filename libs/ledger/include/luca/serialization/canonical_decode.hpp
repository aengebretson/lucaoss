#pragma once

#include "luca/serialization/canonical.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace luca::serialization {

enum class DecodeDiagnosticCategory {
  schema_shape,
  unsupported_version,
  canonical_encoding,
  duplicate_identity,
  deterministic_ordering,
  lineage_reference_missing,
  incompatible_account,
  incompatible_event_relationship,
  conflicting_lifecycle_successor,
};

[[nodiscard]] constexpr std::string_view category_name(DecodeDiagnosticCategory category) noexcept {
  switch (category) {
  case DecodeDiagnosticCategory::schema_shape:
    return "schema_shape";
  case DecodeDiagnosticCategory::unsupported_version:
    return "unsupported_version";
  case DecodeDiagnosticCategory::canonical_encoding:
    return "canonical_encoding";
  case DecodeDiagnosticCategory::duplicate_identity:
    return "duplicate_identity";
  case DecodeDiagnosticCategory::deterministic_ordering:
    return "deterministic_ordering";
  case DecodeDiagnosticCategory::lineage_reference_missing:
    return "lineage_reference_missing";
  case DecodeDiagnosticCategory::incompatible_account:
    return "incompatible_account";
  case DecodeDiagnosticCategory::incompatible_event_relationship:
    return "incompatible_event_relationship";
  case DecodeDiagnosticCategory::conflicting_lifecycle_successor:
    return "conflicting_lifecycle_successor";
  }
  return "canonical_encoding";
}

class DecodeError {
public:
  [[nodiscard]] DecodeDiagnosticCategory category() const noexcept { return category_; }
  [[nodiscard]] std::string_view category_name() const noexcept {
    return serialization::category_name(category_);
  }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] const std::string &message() const noexcept { return message_; }

  DecodeError(DecodeDiagnosticCategory category, std::size_t offset, std::string message)
      : category_(category), offset_(offset), message_(std::move(message)) {}

private:
  DecodeDiagnosticCategory category_;
  std::size_t offset_;
  std::string message_;
};

namespace decode_detail {

// The typed v1 schemas are intentionally bounded. Limits prevent attacker-
// controlled counts from becoming allocation requests even when a caller
// supplies an unusually large backing span.
inline constexpr std::size_t maximum_input_bytes = 64U * 1024U * 1024U;
inline constexpr std::size_t maximum_text_bytes = 16U * 1024U * 1024U;
inline constexpr std::size_t maximum_collection_items = 1'000'000U;
inline constexpr std::uint32_t maximum_map_members = 64U;

[[nodiscard]] inline bool raw_bytes_less(std::string_view left, std::string_view right) noexcept {
  return std::lexicographical_compare(
      left.begin(), left.end(), right.begin(), right.end(), [](char left_byte, char right_byte) {
        return static_cast<unsigned char>(left_byte) < static_cast<unsigned char>(right_byte);
      });
}

class Reader {
public:
  explicit Reader(std::span<const std::byte> input) : input_(input) {}

  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

  [[nodiscard]] DecodeError error(DecodeDiagnosticCategory category, std::string message) const {
    return DecodeError{category, offset_, std::move(message)};
  }

  [[nodiscard]] std::expected<void, DecodeError> read_header() {
    if (input_.size() > maximum_input_bytes) {
      return std::unexpected(error(DecodeDiagnosticCategory::canonical_encoding,
                                   "LCB1 input exceeds the v1 decoder byte limit"));
    }
    constexpr std::string_view header = "LCB1";
    if (remaining() < header.size()) {
      return std::unexpected(
          error(DecodeDiagnosticCategory::canonical_encoding, "LCB1 header is truncated"));
    }
    for (const char character : header) {
      if (read_octet_unchecked() != static_cast<std::uint8_t>(character)) {
        return std::unexpected(error(DecodeDiagnosticCategory::canonical_encoding,
                                     "LCB1 header is invalid or unsupported"));
      }
    }
    return {};
  }

  [[nodiscard]] std::expected<std::uint32_t, DecodeError> read_map_count() {
    auto tag = expect_tag(0x06U, "map");
    if (!tag)
      return std::unexpected(tag.error());
    auto count = read_u32();
    if (!count)
      return std::unexpected(count.error());
    if (*count > maximum_map_members || *count > remaining() / 5U) {
      return std::unexpected(error(DecodeDiagnosticCategory::canonical_encoding,
                                   "LCB1 map count exceeds the bounded input"));
    }
    return *count;
  }

  [[nodiscard]] std::expected<void, DecodeError> read_map(std::uint32_t expected_members,
                                                          std::string_view schema_name) {
    auto count = read_map_count();
    if (!count)
      return std::unexpected(count.error());
    if (*count != expected_members) {
      return std::unexpected(
          error(DecodeDiagnosticCategory::schema_shape,
                std::string{schema_name} + " has a missing or extra map member"));
    }
    return {};
  }

  [[nodiscard]] bool next_is_null() const noexcept {
    return remaining() != 0U && std::to_integer<std::uint8_t>(input_[offset_]) == 0x00U;
  }

  [[nodiscard]] std::expected<void, DecodeError> read_null() { return expect_tag(0x00U, "null"); }

  [[nodiscard]] std::expected<std::size_t, DecodeError>
  read_array_count(std::string_view field_name) {
    auto tag = expect_tag(0x05U, "array");
    if (!tag)
      return std::unexpected(tag.error());
    auto count = read_u64();
    if (!count)
      return std::unexpected(count.error());
    if (*count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
        *count > maximum_collection_items || *count > remaining()) {
      return std::unexpected(error(DecodeDiagnosticCategory::canonical_encoding,
                                   std::string{field_name} + " count exceeds the bounded input"));
    }
    return static_cast<std::size_t>(*count);
  }

  [[nodiscard]] std::expected<std::string_view, DecodeError> read_text() {
    auto tag = expect_tag(0x04U, "text");
    if (!tag)
      return std::unexpected(tag.error());
    auto length = read_u32();
    if (!length)
      return std::unexpected(length.error());
    if (*length > maximum_text_bytes || *length > remaining()) {
      return std::unexpected(error(DecodeDiagnosticCategory::canonical_encoding,
                                   "LCB1 text length exceeds the bounded input"));
    }
    const auto result = text_at(offset_, *length);
    offset_ += *length;
    if (auto valid = validate_utf8(result, "LCB1 text"); !valid)
      return std::unexpected(valid.error());
    return result;
  }

  [[nodiscard]] std::expected<void, DecodeError> read_key(std::string_view expected,
                                                          std::string_view &previous) {
    auto length = read_u32();
    if (!length)
      return std::unexpected(length.error());
    if (*length > maximum_text_bytes || *length > remaining()) {
      return std::unexpected(error(DecodeDiagnosticCategory::canonical_encoding,
                                   "LCB1 map-key length exceeds the bounded input"));
    }
    const auto key = text_at(offset_, *length);
    offset_ += *length;
    if (auto valid = validate_utf8(key, "LCB1 map key"); !valid)
      return std::unexpected(valid.error());

    if (!previous.empty() && !raw_bytes_less(previous, key)) {
      const auto category = previous == key ? DecodeDiagnosticCategory::duplicate_identity
                                            : DecodeDiagnosticCategory::deterministic_ordering;
      return std::unexpected(
          error(category, previous == key ? "LCB1 map keys must be unique"
                                          : "LCB1 map keys are not in canonical raw-byte order"));
    }
    previous = key;
    if (key != expected) {
      return std::unexpected(error(DecodeDiagnosticCategory::schema_shape,
                                   "LCB1 map has an unknown, missing, or reordered member"));
    }
    return {};
  }

  [[nodiscard]] std::expected<void, DecodeError> finish() const {
    if (offset_ != input_.size()) {
      return std::unexpected(
          error(DecodeDiagnosticCategory::canonical_encoding, "LCB1 value has trailing bytes"));
    }
    return {};
  }

private:
  [[nodiscard]] std::size_t remaining() const noexcept { return input_.size() - offset_; }

  [[nodiscard]] std::uint8_t read_octet_unchecked() noexcept {
    return std::to_integer<std::uint8_t>(input_[offset_++]);
  }

  [[nodiscard]] std::expected<void, DecodeError> expect_tag(std::uint8_t expected,
                                                            std::string_view expected_name) {
    if (remaining() == 0) {
      return std::unexpected(error(DecodeDiagnosticCategory::canonical_encoding,
                                   "LCB1 value is truncated before a tag"));
    }
    const auto actual = read_octet_unchecked();
    if (actual > 0x06U) {
      return std::unexpected(error(DecodeDiagnosticCategory::canonical_encoding,
                                   "LCB1 value contains an unknown tag"));
    }
    if (actual != expected) {
      return std::unexpected(error(DecodeDiagnosticCategory::schema_shape,
                                   "LCB1 value is not the expected " + std::string{expected_name}));
    }
    return {};
  }

  [[nodiscard]] std::expected<std::uint32_t, DecodeError> read_u32() {
    if (remaining() < 4U) {
      return std::unexpected(error(DecodeDiagnosticCategory::canonical_encoding,
                                   "LCB1 unsigned 32-bit value is truncated"));
    }
    std::uint32_t result = 0;
    for (unsigned index = 0; index < 4U; ++index)
      result = static_cast<std::uint32_t>((result << 8U) | read_octet_unchecked());
    return result;
  }

  [[nodiscard]] std::expected<std::uint64_t, DecodeError> read_u64() {
    if (remaining() < 8U) {
      return std::unexpected(error(DecodeDiagnosticCategory::canonical_encoding,
                                   "LCB1 unsigned 64-bit value is truncated"));
    }
    std::uint64_t result = 0;
    for (unsigned index = 0; index < 8U; ++index)
      result = (result << 8U) | read_octet_unchecked();
    return result;
  }

  [[nodiscard]] std::string_view text_at(std::size_t offset, std::size_t length) const noexcept {
    const auto *characters = reinterpret_cast<const char *>(input_.data() + offset);
    return {characters, length};
  }

  [[nodiscard]] std::expected<void, DecodeError> validate_utf8(std::string_view value,
                                                               std::string_view field) const {
    switch (detail::unicode_nfc::validate(value)) {
    case detail::unicode_nfc::ValidationResult::valid:
      return {};
    case detail::unicode_nfc::ValidationResult::invalid_utf8:
      return std::unexpected(error(DecodeDiagnosticCategory::canonical_encoding,
                                   std::string{field} + " must be valid UTF-8"));
    case detail::unicode_nfc::ValidationResult::not_nfc:
      return std::unexpected(error(DecodeDiagnosticCategory::canonical_encoding,
                                   std::string{field} + " must be NFC-normalized"));
    }
    return std::unexpected(
        error(DecodeDiagnosticCategory::canonical_encoding, std::string{field} + " is invalid"));
  }

  std::span<const std::byte> input_;
  std::size_t offset_{};
};

[[nodiscard]] inline std::expected<std::string_view, DecodeError>
read_required_text(Reader &reader, std::string_view field) {
  auto value = reader.read_text();
  if (!value)
    return std::unexpected(value.error());
  if (value->empty() || value->find('\0') != std::string_view::npos) {
    return std::unexpected(
        reader.error(DecodeDiagnosticCategory::canonical_encoding,
                     std::string{field} + " must be non-empty text without NUL"));
  }
  return *value;
}

[[nodiscard]] inline std::expected<std::int64_t, DecodeError>
parse_signed_decimal(Reader &reader, std::string_view value, std::string_view field) {
  if (value.empty() || value.front() == '+' || value == "-0" ||
      (value.size() > 1U && value.front() == '0') ||
      (value.front() == '-' && (value.size() == 1U || value[1] == '0'))) {
    return std::unexpected(
        reader.error(DecodeDiagnosticCategory::canonical_encoding,
                     std::string{field} + " is not canonical signed decimal text"));
  }
  std::int64_t parsed{};
  const auto [end, result] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result != std::errc{} || end != value.data() + value.size()) {
    return std::unexpected(
        reader.error(DecodeDiagnosticCategory::canonical_encoding,
                     std::string{field} + " is outside signed 64-bit canonical decimal"));
  }
  return parsed;
}

[[nodiscard]] inline std::expected<std::uint64_t, DecodeError>
parse_positive_decimal(Reader &reader, std::string_view value, std::string_view field) {
  if (value.empty() || value.front() < '1' || value.front() > '9') {
    return std::unexpected(
        reader.error(DecodeDiagnosticCategory::canonical_encoding,
                     std::string{field} + " is not canonical positive decimal text"));
  }
  std::uint64_t parsed{};
  const auto [end, result] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result != std::errc{} || end != value.data() + value.size()) {
    return std::unexpected(
        reader.error(DecodeDiagnosticCategory::canonical_encoding,
                     std::string{field} + " is outside unsigned 64-bit canonical decimal"));
  }
  return parsed;
}

[[nodiscard]] inline bool decimal_digits(std::string_view value) noexcept {
  for (const char digit : value) {
    if (digit < '0' || digit > '9')
      return false;
  }
  return true;
}

[[nodiscard]] inline unsigned parse_small_decimal(std::string_view value) noexcept {
  unsigned result = 0;
  for (const char digit : value)
    result = result * 10U + static_cast<unsigned>(digit - '0');
  return result;
}

[[nodiscard]] inline std::expected<SettlementDate, DecodeError>
parse_date(Reader &reader, std::string_view value, std::string_view field) {
  if (value.size() != 10U || value[4] != '-' || value[7] != '-' ||
      !decimal_digits(value.substr(0, 4)) || !decimal_digits(value.substr(5, 2)) ||
      !decimal_digits(value.substr(8, 2))) {
    return std::unexpected(
        reader.error(DecodeDiagnosticCategory::canonical_encoding,
                     std::string{field} + " is not a canonical settlement date"));
  }
  const auto date = std::chrono::year{static_cast<int>(parse_small_decimal(value.substr(0, 4)))} /
                    std::chrono::month{parse_small_decimal(value.substr(5, 2))} /
                    std::chrono::day{parse_small_decimal(value.substr(8, 2))};
  const auto result = SettlementDate::create(date);
  if (!result || static_cast<int>(date.year()) == 0) {
    return std::unexpected(reader.error(DecodeDiagnosticCategory::canonical_encoding,
                                        std::string{field} + " is not a valid v1 date"));
  }
  return *result;
}

[[nodiscard]] inline std::expected<Timestamp, DecodeError>
parse_timestamp(Reader &reader, std::string_view value, std::string_view field) {
  using namespace std::chrono;
  if (value.size() != 30U || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':' || value[19] != '.' || value[29] != 'Z' ||
      !decimal_digits(value.substr(0, 4)) || !decimal_digits(value.substr(5, 2)) ||
      !decimal_digits(value.substr(8, 2)) || !decimal_digits(value.substr(11, 2)) ||
      !decimal_digits(value.substr(14, 2)) || !decimal_digits(value.substr(17, 2)) ||
      !decimal_digits(value.substr(20, 9))) {
    return std::unexpected(reader.error(DecodeDiagnosticCategory::canonical_encoding,
                                        std::string{field} + " is not a canonical timestamp"));
  }
  const auto date = year{static_cast<int>(parse_small_decimal(value.substr(0, 4)))} /
                    month{parse_small_decimal(value.substr(5, 2))} /
                    day{parse_small_decimal(value.substr(8, 2))};
  const auto hour = parse_small_decimal(value.substr(11, 2));
  const auto minute = parse_small_decimal(value.substr(14, 2));
  const auto second = parse_small_decimal(value.substr(17, 2));
  const auto nanosecond = parse_small_decimal(value.substr(20, 9));
  if (!date.ok() || static_cast<int>(date.year()) == 0 || hour > 23U || minute > 59U ||
      second > 59U) {
    return std::unexpected(reader.error(DecodeDiagnosticCategory::canonical_encoding,
                                        std::string{field} + " is not a valid v1 timestamp"));
  }

  constexpr auto nanoseconds_per_day = duration_cast<nanoseconds>(days{1}).count();
  constexpr auto minimum_count = Timestamp::duration::min().count();
  constexpr auto maximum_count = Timestamp::duration::max().count();
  constexpr auto floor_days = [nanoseconds_per_day](std::int64_t count) {
    auto result = count / nanoseconds_per_day;
    if (count % nanoseconds_per_day < 0)
      --result;
    return result;
  };
  constexpr auto day_remainder = [nanoseconds_per_day](std::int64_t count) {
    auto result = count % nanoseconds_per_day;
    if (result < 0)
      result += nanoseconds_per_day;
    return result;
  };

  constexpr auto minimum_day = floor_days(minimum_count);
  constexpr auto maximum_day = floor_days(maximum_count);
  constexpr auto minimum_time = day_remainder(minimum_count);
  constexpr auto maximum_time = day_remainder(maximum_count);
  const auto parsed_day = sys_days{date}.time_since_epoch().count();
  const auto time_of_day = static_cast<std::int64_t>(hour) * 3'600'000'000'000LL +
                           static_cast<std::int64_t>(minute) * 60'000'000'000LL +
                           static_cast<std::int64_t>(second) * 1'000'000'000LL +
                           static_cast<std::int64_t>(nanosecond);
  if (parsed_day < minimum_day || parsed_day > maximum_day ||
      (parsed_day == minimum_day && time_of_day < minimum_time) ||
      (parsed_day == maximum_day && time_of_day > maximum_time)) {
    return std::unexpected(
        reader.error(DecodeDiagnosticCategory::canonical_encoding,
                     std::string{field} + " is outside the representable v1 timestamp range"));
  }

  // On the minimum representable day, converting sys_days to nanoseconds first would
  // overflow even though adding the time of day produces a representable Timestamp.
  const auto timestamp_count = parsed_day >= 0 ? parsed_day * nanoseconds_per_day + time_of_day
                                               : (parsed_day + 1) * nanoseconds_per_day +
                                                     (time_of_day - nanoseconds_per_day);
  return Timestamp{nanoseconds{timestamp_count}};
}

} // namespace decode_detail

namespace lifecycle_decode_detail {

[[nodiscard]] inline std::expected<std::string, DecodeError>
read_identifier(decode_detail::Reader &reader, std::string_view field) {
  auto value = decode_detail::read_required_text(reader, field);
  if (!value)
    return std::unexpected(value.error());
  return std::string{*value};
}

[[nodiscard]] inline std::expected<std::optional<std::string>, DecodeError>
read_optional_identifier(decode_detail::Reader &reader, std::string_view field) {
  if (reader.next_is_null()) {
    if (auto value = reader.read_null(); !value)
      return std::unexpected(value.error());
    return std::nullopt;
  }
  auto value = read_identifier(reader, field);
  if (!value)
    return std::unexpected(value.error());
  return std::optional<std::string>{std::move(*value)};
}

[[nodiscard]] inline std::expected<Currency, DecodeError>
read_currency(decode_detail::Reader &reader, std::string_view field) {
  auto text = reader.read_text();
  if (!text)
    return std::unexpected(text.error());
  const auto currency = Currency::from_code(*text);
  if (!currency) {
    return std::unexpected(
        reader.error(DecodeDiagnosticCategory::canonical_encoding,
                     std::string{field} + " must be three upper-case ASCII letters"));
  }
  return *currency;
}

[[nodiscard]] inline std::expected<void, DecodeError> require_version(decode_detail::Reader &reader,
                                                                      std::string_view expected,
                                                                      std::string_view value_name) {
  auto schema = reader.read_text();
  if (!schema)
    return std::unexpected(schema.error());
  if (*schema != expected) {
    return std::unexpected(
        reader.error(DecodeDiagnosticCategory::unsupported_version,
                     std::string{value_name} + " schema version is not supported"));
  }
  return {};
}

[[nodiscard]] inline std::expected<Money, DecodeError> read_money(decode_detail::Reader &reader) {
  auto shape = reader.read_map(4U, "luca.money.v1");
  if (!shape)
    return std::unexpected(shape.error());
  std::string_view previous;

  auto key = reader.read_key("currency", previous);
  if (!key)
    return std::unexpected(key.error());
  auto currency = read_currency(reader, "money currency");
  if (!currency)
    return std::unexpected(currency.error());

  key = reader.read_key("scale", previous);
  if (!key)
    return std::unexpected(key.error());
  auto scale = reader.read_text();
  if (!scale)
    return std::unexpected(scale.error());
  if (*scale != "6") {
    return std::unexpected(reader.error(DecodeDiagnosticCategory::unsupported_version,
                                        "money scale is not supported"));
  }

  key = reader.read_key("scaled_value", previous);
  if (!key)
    return std::unexpected(key.error());
  auto scaled_text = reader.read_text();
  if (!scaled_text)
    return std::unexpected(scaled_text.error());
  auto scaled = decode_detail::parse_signed_decimal(reader, *scaled_text, "money scaled value");
  if (!scaled)
    return std::unexpected(scaled.error());

  key = reader.read_key("schema_version", previous);
  if (!key)
    return std::unexpected(key.error());
  if (auto version = require_version(reader, "luca.money.v1", "money"); !version)
    return std::unexpected(version.error());
  return Money::from_scaled(*scaled, *currency);
}

template <class Value>
[[nodiscard]] inline std::expected<Value, DecodeError>
read_fixed_point(decode_detail::Reader &reader, std::string_view schema,
                 std::string_view value_name) {
  auto shape = reader.read_map(3U, schema);
  if (!shape)
    return std::unexpected(shape.error());
  std::string_view previous;

  auto key = reader.read_key("scale", previous);
  if (!key)
    return std::unexpected(key.error());
  auto scale = reader.read_text();
  if (!scale)
    return std::unexpected(scale.error());
  if (*scale != "8") {
    return std::unexpected(reader.error(DecodeDiagnosticCategory::unsupported_version,
                                        std::string{value_name} + " scale is not supported"));
  }

  key = reader.read_key("scaled_value", previous);
  if (!key)
    return std::unexpected(key.error());
  auto scaled_text = reader.read_text();
  if (!scaled_text)
    return std::unexpected(scaled_text.error());
  auto scaled = decode_detail::parse_signed_decimal(reader, *scaled_text,
                                                    std::string{value_name} + " scaled value");
  if (!scaled)
    return std::unexpected(scaled.error());

  key = reader.read_key("schema_version", previous);
  if (!key)
    return std::unexpected(key.error());
  if (auto version = require_version(reader, schema, value_name); !version)
    return std::unexpected(version.error());
  return Value::from_scaled(*scaled);
}

[[nodiscard]] inline std::expected<Provenance, DecodeError>
read_provenance(decode_detail::Reader &reader) {
  auto shape = reader.read_map(5U, "luca.provenance.v1");
  if (!shape)
    return std::unexpected(shape.error());
  std::string_view previous;

  auto key = reader.read_key("schema_version", previous);
  if (!key)
    return std::unexpected(key.error());
  if (auto version = require_version(reader, "luca.provenance.v1", "provenance"); !version)
    return std::unexpected(version.error());

  key = reader.read_key("source_record_ids", previous);
  if (!key)
    return std::unexpected(key.error());
  auto source_count = reader.read_array_count("provenance source-record array");
  if (!source_count)
    return std::unexpected(source_count.error());
  if (*source_count == 0U) {
    return std::unexpected(reader.error(DecodeDiagnosticCategory::schema_shape,
                                        "provenance source-record array must not be empty"));
  }
  std::vector<SourceRecordId> sources;
  std::unordered_set<std::string> source_identities;
  sources.reserve(*source_count);
  source_identities.reserve(*source_count);
  for (std::size_t index = 0; index < *source_count; ++index) {
    auto source = read_identifier(reader, "provenance source-record identity");
    if (!source)
      return std::unexpected(source.error());
    if (!source_identities.emplace(*source).second) {
      return std::unexpected(reader.error(DecodeDiagnosticCategory::duplicate_identity,
                                          "provenance source-record identity is duplicated"));
    }
    sources.emplace_back(std::move(*source));
  }

  key = reader.read_key("transformation_metadata", previous);
  if (!key)
    return std::unexpected(key.error());
  std::optional<std::string> metadata;
  if (reader.next_is_null()) {
    if (auto value = reader.read_null(); !value)
      return std::unexpected(value.error());
  } else {
    auto value = reader.read_text();
    if (!value)
      return std::unexpected(value.error());
    if (value->find('\0') != std::string_view::npos) {
      return std::unexpected(reader.error(DecodeDiagnosticCategory::canonical_encoding,
                                          "provenance metadata must not contain NUL"));
    }
    metadata = std::string{*value};
  }

  key = reader.read_key("transformation_name", previous);
  if (!key)
    return std::unexpected(key.error());
  auto name = read_identifier(reader, "provenance transformation name");
  if (!name)
    return std::unexpected(name.error());

  key = reader.read_key("transformation_version", previous);
  if (!key)
    return std::unexpected(key.error());
  auto transformation_version = read_identifier(reader, "provenance transformation version");
  if (!transformation_version)
    return std::unexpected(transformation_version.error());

  auto provenance = Provenance::create(std::move(sources), std::move(*name),
                                       std::move(*transformation_version), std::move(metadata));
  if (!provenance) {
    return std::unexpected(reader.error(DecodeDiagnosticCategory::schema_shape,
                                        "provenance violates the v1 domain invariants"));
  }
  return std::move(*provenance);
}

[[nodiscard]] inline std::expected<EventHeader, DecodeError>
read_event_header(decode_detail::Reader &reader) {
  auto shape = reader.read_map(5U, "luca.event-header.v1");
  if (!shape)
    return std::unexpected(shape.error());
  std::string_view previous;

  auto key = reader.read_key("account", previous);
  if (!key)
    return std::unexpected(key.error());
  auto account = read_identifier(reader, "event-header account");
  if (!account)
    return std::unexpected(account.error());

  key = reader.read_key("effective_at", previous);
  if (!key)
    return std::unexpected(key.error());
  auto timestamp_text = reader.read_text();
  if (!timestamp_text)
    return std::unexpected(timestamp_text.error());
  auto effective_at = decode_detail::parse_timestamp(reader, *timestamp_text, "effective time");
  if (!effective_at)
    return std::unexpected(effective_at.error());

  key = reader.read_key("provenance", previous);
  if (!key)
    return std::unexpected(key.error());
  auto provenance = read_provenance(reader);
  if (!provenance)
    return std::unexpected(provenance.error());

  key = reader.read_key("record_id", previous);
  if (!key)
    return std::unexpected(key.error());
  auto record_id = read_identifier(reader, "event-header record identity");
  if (!record_id)
    return std::unexpected(record_id.error());

  key = reader.read_key("schema_version", previous);
  if (!key)
    return std::unexpected(key.error());
  if (auto version = require_version(reader, "luca.event-header.v1", "event-header"); !version)
    return std::unexpected(version.error());

  auto header = EventHeader::create(EventId{std::move(*record_id)}, AccountId{std::move(*account)},
                                    *effective_at, std::move(*provenance));
  if (!header) {
    return std::unexpected(reader.error(DecodeDiagnosticCategory::schema_shape,
                                        "event header violates the v1 domain invariants"));
  }
  return std::move(*header);
}

[[nodiscard]] inline std::expected<EconomicEvent, DecodeError>
read_cash_movement(decode_detail::Reader &reader) {
  std::string_view previous;
  auto key = reader.read_key("amount", previous);
  if (!key)
    return std::unexpected(key.error());
  auto amount = read_money(reader);
  if (!amount)
    return std::unexpected(amount.error());

  key = reader.read_key("header", previous);
  if (!key)
    return std::unexpected(key.error());
  auto header = read_event_header(reader);
  if (!header)
    return std::unexpected(header.error());

  key = reader.read_key("schema_version", previous);
  if (!key)
    return std::unexpected(key.error());
  if (auto version = require_version(reader, "luca.economic-event.v1", "economic-event"); !version)
    return std::unexpected(version.error());

  key = reader.read_key("variant", previous);
  if (!key)
    return std::unexpected(key.error());
  auto variant = reader.read_text();
  if (!variant)
    return std::unexpected(variant.error());
  if (*variant != "cash_movement") {
    return std::unexpected(reader.error(DecodeDiagnosticCategory::schema_shape,
                                        "cash-movement shape has an invalid variant tag"));
  }
  return EconomicEvent{CashMovement::create(std::move(*header), *amount)};
}

[[nodiscard]] inline std::expected<EconomicEvent, DecodeError>
read_equity_trade(decode_detail::Reader &reader, bool charged = false) {
  std::optional<TradeCharges> charges;
  if (charged) {
    std::string_view previous;
    auto key = reader.read_key("charges", previous);
    if (!key) return std::unexpected(key.error());
    auto count = reader.read_array_count("charges");
    if (!count) return std::unexpected(count.error());
    if (*count > 64) return std::unexpected(reader.error(DecodeDiagnosticCategory::schema_shape, "too many charges"));
    charges.emplace();
    for (std::uint64_t i = 0; i < *count; ++i) {
      auto members = reader.read_map_count();
      if (!members) return std::unexpected(members.error());
      if (*members != 6) return std::unexpected(reader.error(DecodeDiagnosticCategory::schema_shape, "invalid charge shape"));
      previous = {};
      key = reader.read_key("amount", previous);
      if (!key) return std::unexpected(key.error());
      auto amount = read_money(reader);
      if (!amount) return std::unexpected(amount.error());
      std::vector<std::string> texts;
      for (const auto name : {"category", "id", "payee", "schedule_version", "state"}) {
        key = reader.read_key(name, previous);
        if (!key) return std::unexpected(key.error());
        auto value = reader.read_text();
        if (!value) return std::unexpected(value.error());
        texts.emplace_back(*value);
      }
      charges->push_back({texts[1],texts[0],texts[2],texts[3],texts[4],*amount});
    }
  }
  std::string_view previous;
  auto key = reader.read_key("header", previous);
  if (!key)
    return std::unexpected(key.error());
  auto header = read_event_header(reader);
  if (!header)
    return std::unexpected(header.error());

  key = reader.read_key("instrument", previous);
  if (!key)
    return std::unexpected(key.error());
  auto instrument = read_identifier(reader, "equity-trade instrument");
  if (!instrument)
    return std::unexpected(instrument.error());

  key = reader.read_key("price", previous);
  if (!key)
    return std::unexpected(key.error());
  auto price = read_fixed_point<Price>(reader, "luca.price.v1", "price");
  if (!price)
    return std::unexpected(price.error());

  key = reader.read_key("quantity", previous);
  if (!key)
    return std::unexpected(key.error());
  auto quantity = read_fixed_point<Quantity>(reader, "luca.quantity.v1", "quantity");
  if (!quantity)
    return std::unexpected(quantity.error());

  key = reader.read_key("quote_currency", previous);
  if (!key)
    return std::unexpected(key.error());
  auto quote_currency = read_currency(reader, "equity-trade quote currency");
  if (!quote_currency)
    return std::unexpected(quote_currency.error());

  key = reader.read_key("schema_version", previous);
  if (!key)
    return std::unexpected(key.error());
  if (auto version = require_version(reader, charged ? "luca.economic-event.v2" : "luca.economic-event.v1", "economic-event"); !version)
    return std::unexpected(version.error());

  key = reader.read_key("settlement_date", previous);
  if (!key)
    return std::unexpected(key.error());
  auto date_text = reader.read_text();
  if (!date_text)
    return std::unexpected(date_text.error());
  auto settlement_date = decode_detail::parse_date(reader, *date_text, "settlement date");
  if (!settlement_date)
    return std::unexpected(settlement_date.error());

  key = reader.read_key("variant", previous);
  if (!key)
    return std::unexpected(key.error());
  auto variant = reader.read_text();
  if (!variant)
    return std::unexpected(variant.error());
  if (*variant != "equity_trade") {
    return std::unexpected(reader.error(DecodeDiagnosticCategory::schema_shape,
                                        "equity-trade shape has an invalid variant tag"));
  }

  auto trade = EquityTrade::create(std::move(*header), InstrumentId{std::move(*instrument)},
                                   *quantity, *price, *quote_currency, *settlement_date, std::move(charges));
  if (!trade) {
    return std::unexpected(reader.error(DecodeDiagnosticCategory::schema_shape,
                                        "equity trade violates the v1 domain invariants"));
  }
  return EconomicEvent{std::move(*trade)};
}

[[nodiscard]] inline std::expected<EconomicEvent, DecodeError>
read_economic_event(decode_detail::Reader &reader) {
  auto count = reader.read_map_count();
  if (!count)
    return std::unexpected(count.error());
  if (*count == 4U)
    return read_cash_movement(reader);
  if (*count == 8U || *count == 9U)
    return read_equity_trade(reader, *count == 9U);
  return std::unexpected(reader.error(DecodeDiagnosticCategory::schema_shape,
                                      "economic-event has a missing or extra map member"));
}

[[nodiscard]] inline std::expected<LifecycleAction, DecodeError>
read_lifecycle_action(decode_detail::Reader &reader) {
  auto action = reader.read_text();
  if (!action)
    return std::unexpected(action.error());
  if (*action == "originate")
    return LifecycleAction::originate;
  if (*action == "correct")
    return LifecycleAction::correct;
  if (*action == "cancel")
    return LifecycleAction::cancel;
  if (*action == "reverse")
    return LifecycleAction::reverse;
  return std::unexpected(
      reader.error(DecodeDiagnosticCategory::schema_shape, "lifecycle action is not supported"));
}

struct DecodedLifecycleRecord {
  std::uint64_t acceptance_sequence;
  LifecycleRecordDraft draft;
};

[[nodiscard]] inline std::expected<DecodedLifecycleRecord, DecodeError>
read_lifecycle_record(decode_detail::Reader &reader) {
  auto shape = reader.read_map_count();
  if (!shape)
    return std::unexpected(shape.error());
  std::string_view previous;

  auto key = reader.read_key("acceptance_sequence", previous);
  if (!key)
    return std::unexpected(key.error());
  if (*shape != 10 && *shape != 11)
    return std::unexpected(reader.error(DecodeDiagnosticCategory::schema_shape, "Invalid lifecycle field count"));
  auto sequence_text = reader.read_text();
  if (!sequence_text)
    return std::unexpected(sequence_text.error());
  auto sequence =
      decode_detail::parse_positive_decimal(reader, *sequence_text, "acceptance sequence");
  if (!sequence)
    return std::unexpected(sequence.error());

  key = reader.read_key("account", previous);
  if (!key)
    return std::unexpected(key.error());
  auto account = read_identifier(reader, "lifecycle-record account");
  if (!account)
    return std::unexpected(account.error());

  key = reader.read_key("action", previous);
  if (!key)
    return std::unexpected(key.error());
  auto action = read_lifecycle_action(reader);
  if (!action)
    return std::unexpected(action.error());

  std::optional<Timestamp> cancellation_at;
  if (*shape == 11) {
    key = reader.read_key("cancellation_at", previous);
    if (!key) return std::unexpected(key.error());
    auto text = reader.read_text();
    if (!text) return std::unexpected(text.error());
    auto stamp = decode_detail::parse_timestamp(reader, *text, "void effective time");
    if (!stamp) return std::unexpected(stamp.error());
    cancellation_at = *stamp;
    if (*action != LifecycleAction::cancel)
      return std::unexpected(reader.error(DecodeDiagnosticCategory::schema_shape, "Only cancellation has a void date"));
  }
  key = reader.read_key("causal_record_id", previous);
  if (!key)
    return std::unexpected(key.error());
  auto causal_record_id = read_optional_identifier(reader, "causal record identity");
  if (!causal_record_id)
    return std::unexpected(causal_record_id.error());

  key = reader.read_key("economic_event_id", previous);
  if (!key)
    return std::unexpected(key.error());
  auto economic_event_id = read_identifier(reader, "economic-event identity");
  if (!economic_event_id)
    return std::unexpected(economic_event_id.error());

  key = reader.read_key("event", previous);
  if (!key)
    return std::unexpected(key.error());
  std::optional<EconomicEvent> event;
  if (reader.next_is_null()) {
    if (auto value = reader.read_null(); !value)
      return std::unexpected(value.error());
  } else {
    auto value = read_economic_event(reader);
    if (!value)
      return std::unexpected(value.error());
    event = std::move(*value);
  }

  key = reader.read_key("provenance", previous);
  if (!key)
    return std::unexpected(key.error());
  auto provenance = read_provenance(reader);
  if (!provenance)
    return std::unexpected(provenance.error());

  key = reader.read_key("record_id", previous);
  if (!key)
    return std::unexpected(key.error());
  auto record_id = read_identifier(reader, "lifecycle record identity");
  if (!record_id)
    return std::unexpected(record_id.error());

  key = reader.read_key("recorded_at", previous);
  if (!key)
    return std::unexpected(key.error());
  auto recorded_text = reader.read_text();
  if (!recorded_text)
    return std::unexpected(recorded_text.error());
  auto recorded_at = decode_detail::parse_timestamp(reader, *recorded_text, "recorded time");
  if (!recorded_at)
    return std::unexpected(recorded_at.error());

  key = reader.read_key("schema_version", previous);
  if (!key)
    return std::unexpected(key.error());
  if (auto version = require_version(reader, *shape == 11 ? "luca.lifecycle-record.v2" : "luca.lifecycle-record.v1", "lifecycle-record");
      !version)
    return std::unexpected(version.error());

  if (event) {
    const auto &event_header = header(*event);
    if (event_header.id().value() != *record_id || event_header.account().value() != *account ||
        event_header.provenance() != *provenance) {
      return std::unexpected(
          reader.error(DecodeDiagnosticCategory::schema_shape,
                       "lifecycle record and event header contain inconsistent redundant fields"));
    }
  }

  const EventId typed_record_id{std::move(*record_id)};
  const EconomicEventId typed_economic_event_id{std::move(*economic_event_id)};
  const AccountId typed_account{std::move(*account)};
  std::optional<LifecycleRecordDraft> draft;
  switch (*action) {
  case LifecycleAction::originate:
    if (*causal_record_id || !event) {
      return std::unexpected(reader.error(
          DecodeDiagnosticCategory::schema_shape,
          "originating lifecycle record requires an event and a null causal identity"));
    }
    draft =
        LifecycleRecordDraft::originate(typed_economic_event_id, *recorded_at, std::move(*event));
    break;
  case LifecycleAction::correct:
    if (!*causal_record_id || !event) {
      return std::unexpected(
          reader.error(DecodeDiagnosticCategory::schema_shape,
                       "correcting lifecycle record requires an event and a causal identity"));
    }
    draft = LifecycleRecordDraft::correct(typed_economic_event_id,
                                          EventId{std::move(**causal_record_id)}, *recorded_at,
                                          std::move(*event));
    break;
  case LifecycleAction::cancel:
    if (!*causal_record_id || event) {
      return std::unexpected(
          reader.error(DecodeDiagnosticCategory::schema_shape,
                       "cancelling lifecycle record requires a causal identity and a null event"));
    }
    draft = LifecycleRecordDraft::cancel(typed_record_id, typed_economic_event_id,
                                         EventId{std::move(**causal_record_id)}, typed_account,
                                         *recorded_at, std::move(*provenance), cancellation_at);
    break;
  case LifecycleAction::reverse:
    if (!*causal_record_id || !event) {
      return std::unexpected(
          reader.error(DecodeDiagnosticCategory::schema_shape,
                       "reversing lifecycle record requires an event and a causal identity"));
    }
    draft = LifecycleRecordDraft::reverse(typed_economic_event_id,
                                          EventId{std::move(**causal_record_id)}, *recorded_at,
                                          std::move(*event));
    break;
  }
  return DecodedLifecycleRecord{*sequence, std::move(*draft)};
}

[[nodiscard]] inline DecodeDiagnosticCategory
lifecycle_category(LifecycleDiagnosticCategory category) noexcept {
  switch (category) {
  case LifecycleDiagnosticCategory::invalid_record:
    return DecodeDiagnosticCategory::schema_shape;
  case LifecycleDiagnosticCategory::duplicate_identity:
    return DecodeDiagnosticCategory::duplicate_identity;
  case LifecycleDiagnosticCategory::deterministic_ordering:
  case LifecycleDiagnosticCategory::sequence_overflow:
    return DecodeDiagnosticCategory::deterministic_ordering;
  case LifecycleDiagnosticCategory::causal_reference_missing:
  case LifecycleDiagnosticCategory::causal_self_reference:
  case LifecycleDiagnosticCategory::causal_cycle:
  case LifecycleDiagnosticCategory::causal_reference_unavailable:
    return DecodeDiagnosticCategory::lineage_reference_missing;
  case LifecycleDiagnosticCategory::incompatible_account:
    return DecodeDiagnosticCategory::incompatible_account;
  case LifecycleDiagnosticCategory::incompatible_event_relationship:
    return DecodeDiagnosticCategory::incompatible_event_relationship;
  case LifecycleDiagnosticCategory::conflicting_lifecycle_successor:
    return DecodeDiagnosticCategory::conflicting_lifecycle_successor;
  }
  return DecodeDiagnosticCategory::schema_shape;
}

[[nodiscard]] inline std::expected<LifecycleLedger, DecodeError>
read_lifecycle_ledger(decode_detail::Reader &reader) {
  auto shape = reader.read_map(2U, "luca.lifecycle-record-sequence.v1");
  if (!shape)
    return std::unexpected(shape.error());
  std::string_view previous;

  auto key = reader.read_key("records", previous);
  if (!key)
    return std::unexpected(key.error());
  auto record_count = reader.read_array_count("lifecycle-record array");
  if (!record_count)
    return std::unexpected(record_count.error());
  if (*record_count == 0U) {
    return std::unexpected(reader.error(DecodeDiagnosticCategory::schema_shape,
                                        "lifecycle-record sequence must not be empty"));
  }

  LifecycleLedger ledger;
  for (std::size_t index = 0; index < *record_count; ++index) {
    auto decoded = read_lifecycle_record(reader);
    if (!decoded)
      return std::unexpected(decoded.error());
    const auto expected_sequence = static_cast<std::uint64_t>(index) + 1U;
    if (decoded->acceptance_sequence != expected_sequence) {
      return std::unexpected(
          reader.error(DecodeDiagnosticCategory::deterministic_ordering,
                       "lifecycle acceptance sequence must be contiguous and begin at one"));
    }
    auto accepted = ledger.accept(decoded->draft);
    if (!accepted) {
      return std::unexpected(reader.error(lifecycle_category(accepted.error().category()),
                                          accepted.error().message()));
    }
  }

  key = reader.read_key("schema_version", previous);
  if (!key)
    return std::unexpected(key.error());
  if (auto version =
          require_version(reader, "luca.lifecycle-record-sequence.v1", "lifecycle-record-sequence");
      !version)
    return std::unexpected(version.error());
  return ledger;
}

} // namespace lifecycle_decode_detail

// Decode an owned canonical record for append-log recovery; admission still checks
// continuity and all lifecycle relationships against the actual accepted prefix.
using DecodedLifecycleRecord = lifecycle_decode_detail::DecodedLifecycleRecord;
[[nodiscard]] inline std::expected<DecodedLifecycleRecord, DecodeError>
decode_lifecycle_record(std::span<const std::byte> input) {
  decode_detail::Reader reader{input};
  if (auto header = reader.read_header(); !header) return std::unexpected(header.error());
  auto record = lifecycle_decode_detail::read_lifecycle_record(reader);
  if (!record) return std::unexpected(record.error());
  if (auto complete = reader.finish(); !complete) return std::unexpected(complete.error());
  return record;
}

[[nodiscard]] inline std::expected<LifecycleLedger, DecodeError>
decode_lifecycle_ledger(std::span<const std::byte> input) {
  decode_detail::Reader reader{input};
  if (auto header = reader.read_header(); !header)
    return std::unexpected(header.error());
  auto ledger = lifecycle_decode_detail::read_lifecycle_ledger(reader);
  if (!ledger)
    return std::unexpected(ledger.error());
  if (auto complete = reader.finish(); !complete)
    return std::unexpected(complete.error());
  return ledger;
}

} // namespace luca::serialization
