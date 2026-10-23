#pragma once

#include "luca/lifecycle.hpp"
#include "luca/serialization/unicode_nfc.hpp"

#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace luca::serialization {

using CanonicalBytes = std::vector<std::byte>;

namespace detail {

inline void append_octet(CanonicalBytes &output, std::uint8_t value) {
  output.push_back(static_cast<std::byte>(value));
}

inline void append_u32(CanonicalBytes &output, std::uint32_t value) {
  append_octet(output, static_cast<std::uint8_t>(value >> 24));
  append_octet(output, static_cast<std::uint8_t>(value >> 16));
  append_octet(output, static_cast<std::uint8_t>(value >> 8));
  append_octet(output, static_cast<std::uint8_t>(value));
}

inline void append_u64(CanonicalBytes &output, std::uint64_t value) {
  append_octet(output, static_cast<std::uint8_t>(value >> 56));
  append_octet(output, static_cast<std::uint8_t>(value >> 48));
  append_octet(output, static_cast<std::uint8_t>(value >> 40));
  append_octet(output, static_cast<std::uint8_t>(value >> 32));
  append_octet(output, static_cast<std::uint8_t>(value >> 24));
  append_octet(output, static_cast<std::uint8_t>(value >> 16));
  append_octet(output, static_cast<std::uint8_t>(value >> 8));
  append_octet(output, static_cast<std::uint8_t>(value));
}

inline void append_ascii(CanonicalBytes &output, std::string_view value) {
  for (const char character : value)
    append_octet(output, static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
}

inline void validate_text(std::string_view value) {
  switch (unicode_nfc::validate(value)) {
  case unicode_nfc::ValidationResult::valid:
    return;
  case unicode_nfc::ValidationResult::invalid_utf8:
    throw std::invalid_argument("LCB1 text must be valid UTF-8");
  case unicode_nfc::ValidationResult::not_nfc:
    throw std::invalid_argument("LCB1 text must be NFC-normalized");
  }
}

inline void append_text(CanonicalBytes &output, std::string_view value) {
  if (value.size() > std::numeric_limits<std::uint32_t>::max())
    throw std::length_error("LCB1 text exceeds the unsigned 32-bit length limit");
  validate_text(value);
  append_octet(output, 0x04);
  append_u32(output, static_cast<std::uint32_t>(value.size()));
  append_ascii(output, value);
}

inline void append_nul_free_text(CanonicalBytes &output, std::string_view value) {
  if (value.find('\0') != std::string_view::npos)
    throw std::invalid_argument("LCB1 schema text must not contain NUL");
  append_text(output, value);
}

inline void append_required_text(CanonicalBytes &output, std::string_view value) {
  if (value.empty())
    throw std::invalid_argument("LCB1 required schema text must not be empty");
  append_nul_free_text(output, value);
}

inline void append_identifier(CanonicalBytes &output, std::string_view value) {
  if (value.empty())
    throw std::invalid_argument("LCB1 identifier must not be empty");
  if (value.find('\0') != std::string_view::npos)
    throw std::invalid_argument("LCB1 identifier must not contain NUL");
  append_text(output, value);
}

inline void append_key(CanonicalBytes &output, std::string_view key) {
  if (key.size() > std::numeric_limits<std::uint32_t>::max())
    throw std::length_error("LCB1 map key exceeds the unsigned 32-bit length limit");
  append_u32(output, static_cast<std::uint32_t>(key.size()));
  append_ascii(output, key);
}

inline void append_map(CanonicalBytes &output, std::uint32_t member_count) {
  append_octet(output, 0x06);
  append_u32(output, member_count);
}

inline void append_array(CanonicalBytes &output, std::uint64_t item_count) {
  append_octet(output, 0x05);
  append_u64(output, item_count);
}

inline std::string canonical_decimal(std::int64_t value) {
  std::array<char, 32> buffer{};
  const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  (void)error; // A 32-byte buffer holds every signed 64-bit decimal representation.
  return {buffer.data(), end};
}

inline std::string canonical_decimal(std::uint64_t value) {
  std::array<char, 32> buffer{};
  const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  (void)error; // A 32-byte buffer holds every unsigned 64-bit decimal representation.
  return {buffer.data(), end};
}

inline void append_padded_decimal(std::string &output, unsigned value, std::size_t width) {
  std::array<char, 16> buffer{};
  const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  (void)error;
  const auto size = static_cast<std::size_t>(end - buffer.data());
  output.append(width - size, '0');
  output.append(buffer.data(), size);
}

inline std::string canonical_date(SettlementDate value) {
  const auto date = value.value();
  const auto year = static_cast<int>(date.year());
  if (year < 1 || year > 9999)
    throw std::out_of_range("LCB1 settlement date year must be between 0001 and 9999");

  std::string output;
  output.reserve(10);
  append_padded_decimal(output, static_cast<unsigned>(year), 4);
  output.push_back('-');
  append_padded_decimal(output, static_cast<unsigned>(date.month()), 2);
  output.push_back('-');
  append_padded_decimal(output, static_cast<unsigned>(date.day()), 2);
  return output;
}

inline std::string canonical_timestamp(Timestamp value) {
  using namespace std::chrono;

  const auto day = floor<days>(value);
  const year_month_day date{day};
  const auto year = static_cast<int>(date.year());
  if (year < 1 || year > 9999)
    throw std::out_of_range("LCB1 timestamp year must be between 0001 and 9999");

  // The midnight before Timestamp::min() is outside the nanosecond range. A
  // remainder from the truncated day stays representable and can be normalized.
  const auto since_epoch = value.time_since_epoch();
  const auto truncated_day = duration_cast<days>(since_epoch);
  auto since_midnight = since_epoch - duration_cast<Timestamp::duration>(truncated_day);
  if (since_midnight < Timestamp::duration::zero())
    since_midnight += duration_cast<Timestamp::duration>(days{1});
  const hh_mm_ss time{since_midnight};

  std::string output;
  output.reserve(30);
  append_padded_decimal(output, static_cast<unsigned>(year), 4);
  output.push_back('-');
  append_padded_decimal(output, static_cast<unsigned>(date.month()), 2);
  output.push_back('-');
  append_padded_decimal(output, static_cast<unsigned>(date.day()), 2);
  output.push_back('T');
  append_padded_decimal(output, static_cast<unsigned>(time.hours().count()), 2);
  output.push_back(':');
  append_padded_decimal(output, static_cast<unsigned>(time.minutes().count()), 2);
  output.push_back(':');
  append_padded_decimal(output, static_cast<unsigned>(time.seconds().count()), 2);
  output.push_back('.');
  append_padded_decimal(output, static_cast<unsigned>(time.subseconds().count()), 9);
  output.push_back('Z');
  return output;
}

inline void append_fixed_point(CanonicalBytes &output, std::string_view schema_version,
                               std::string_view scale, std::int64_t scaled_value) {
  append_map(output, 3);
  append_key(output, "scale");
  append_text(output, scale);
  append_key(output, "scaled_value");
  append_text(output, canonical_decimal(scaled_value));
  append_key(output, "schema_version");
  append_text(output, schema_version);
}

inline void append_money(CanonicalBytes &output, Money value) {
  append_map(output, 4);
  append_key(output, "currency");
  append_text(output, value.currency().code());
  append_key(output, "scale");
  append_text(output, "6");
  append_key(output, "scaled_value");
  append_text(output, canonical_decimal(value.scaled_value()));
  append_key(output, "schema_version");
  append_text(output, "luca.money.v1");
}

inline void append_quantity(CanonicalBytes &output, Quantity value) {
  append_fixed_point(output, "luca.quantity.v1", "8", value.scaled_value());
}

inline void append_price(CanonicalBytes &output, Price value) {
  append_fixed_point(output, "luca.price.v1", "8", value.scaled_value());
}

inline void append_provenance(CanonicalBytes &output, const Provenance &value) {
  const auto sources = value.source_records();
  for (std::size_t left = 0; left < sources.size(); ++left) {
    for (std::size_t right = left + 1; right < sources.size(); ++right) {
      if (sources[left] == sources[right])
        throw std::invalid_argument("LCB1 provenance source record identities must be unique");
    }
  }

  append_map(output, 5);
  append_key(output, "schema_version");
  append_text(output, "luca.provenance.v1");
  append_key(output, "source_record_ids");
  append_array(output, static_cast<std::uint64_t>(sources.size()));
  for (const auto &source_record : sources)
    append_identifier(output, source_record.value());
  append_key(output, "transformation_metadata");
  if (value.transformation_metadata())
    append_nul_free_text(output, *value.transformation_metadata());
  else
    append_octet(output, 0x00);
  append_key(output, "transformation_name");
  append_required_text(output, value.transformation_name());
  append_key(output, "transformation_version");
  append_required_text(output, value.transformation_version());
}

inline void append_event_header(CanonicalBytes &output, const EventHeader &value) {
  append_map(output, 5);
  append_key(output, "account");
  append_identifier(output, value.account().value());
  append_key(output, "effective_at");
  append_text(output, canonical_timestamp(value.effective_at()));
  append_key(output, "provenance");
  append_provenance(output, value.provenance());
  append_key(output, "record_id");
  append_identifier(output, value.id().value());
  append_key(output, "schema_version");
  append_text(output, "luca.event-header.v1");
}

inline void append_cash_movement(CanonicalBytes &output, const CashMovement &value) {
  append_map(output, 4);
  append_key(output, "amount");
  append_money(output, value.amount());
  append_key(output, "header");
  append_event_header(output, value.header());
  append_key(output, "schema_version");
  append_text(output, "luca.economic-event.v1");
  append_key(output, "variant");
  append_text(output, "cash_movement");
}

inline void append_equity_trade(CanonicalBytes &output, const EquityTrade &value) {
  append_map(output, value.charges() ? 9 : 8);
  if (value.charges()) {
    append_key(output, "charges");
    append_array(output, value.charges()->size());
    for (const auto& c : *value.charges()) {
      append_map(output, 6);
      append_key(output, "amount"); append_money(output, c.amount);
      append_key(output, "category"); append_required_text(output, c.category);
      append_key(output, "id"); append_identifier(output, c.id);
      append_key(output, "payee"); append_required_text(output, c.payee);
      append_key(output, "schedule_version"); append_nul_free_text(output, c.schedule_version);
      append_key(output, "state"); append_required_text(output, c.state);
    }
  }
  append_key(output, "header");
  append_event_header(output, value.header());
  append_key(output, "instrument");
  append_identifier(output, value.instrument().value());
  append_key(output, "price");
  append_price(output, value.price());
  append_key(output, "quantity");
  append_quantity(output, value.quantity());
  append_key(output, "quote_currency");
  append_text(output, value.quote_currency().code());
  append_key(output, "schema_version");
  append_text(output, value.charges() ? "luca.economic-event.v2" : "luca.economic-event.v1");
  append_key(output, "settlement_date");
  append_text(output, canonical_date(value.settlement_date()));
  append_key(output, "variant");
  append_text(output, "equity_trade");
}

inline void append_economic_event(CanonicalBytes &output, const EconomicEvent &value) {
  std::visit(
      [&output](const auto &event) {
        using Event = std::remove_cvref_t<decltype(event)>;
        if constexpr (std::same_as<Event, CashMovement>)
          append_cash_movement(output, event);
        else
          append_equity_trade(output, event);
      },
      value);
}

inline std::string_view lifecycle_action_name(LifecycleAction action) noexcept {
  switch (action) {
  case LifecycleAction::originate:
    return "originate";
  case LifecycleAction::correct:
    return "correct";
  case LifecycleAction::cancel:
    return "cancel";
  case LifecycleAction::reverse:
    return "reverse";
  }
  return "";
}

inline void append_lifecycle_record(CanonicalBytes &output, const LifecycleRecord &value) {
  append_map(output, value.cancellation_at() ? 11 : 10);
  append_key(output, "acceptance_sequence");
  append_text(output, canonical_decimal(value.acceptance_sequence().value()));
  append_key(output, "account");
  append_identifier(output, value.account().value());
  append_key(output, "action");
  append_text(output, lifecycle_action_name(value.action()));
  if (value.cancellation_at()) {
    append_key(output, "cancellation_at");
    append_text(output, canonical_timestamp(*value.cancellation_at()));
  }
  append_key(output, "causal_record_id");
  if (value.causal_record_id())
    append_identifier(output, value.causal_record_id()->value());
  else
    append_octet(output, 0x00);
  append_key(output, "economic_event_id");
  append_identifier(output, value.economic_event_id().value());
  append_key(output, "event");
  if (value.event())
    append_economic_event(output, *value.event());
  else
    append_octet(output, 0x00);
  append_key(output, "provenance");
  append_provenance(output, value.provenance());
  append_key(output, "record_id");
  append_identifier(output, value.record_id().value());
  append_key(output, "recorded_at");
  append_text(output, canonical_timestamp(value.recorded_at()));
  append_key(output, "schema_version");
  append_text(output, value.cancellation_at() ? "luca.lifecycle-record.v2" : "luca.lifecycle-record.v1");
}

inline CanonicalBytes top_level_bytes() {
  CanonicalBytes output;
  output.reserve(256);
  append_ascii(output, "LCB1");
  return output;
}

inline CanonicalBytes fixed_point_bytes(std::string_view schema_version, std::string_view scale,
                                        std::int64_t scaled_value) {
  auto output = top_level_bytes();
  append_fixed_point(output, schema_version, scale, scaled_value);
  return output;
}

inline constexpr std::array<std::uint32_t, 64> sha256_round_constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

inline std::uint32_t load_u32(const std::byte *input) {
  return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[0])) << 24) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[1])) << 16) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[2])) << 8) |
         static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[3]));
}

inline void sha256_compress(std::array<std::uint32_t, 8> &state, const std::byte *block) {
  std::array<std::uint32_t, 64> words{};
  for (std::size_t index = 0; index < 16; ++index) {
    words[index] = load_u32(block + (index * 4));
  }
  for (std::size_t index = 16; index < words.size(); ++index) {
    const auto sigma0 = std::rotr(words[index - 15], 7) ^ std::rotr(words[index - 15], 18) ^
                        (words[index - 15] >> 3);
    const auto sigma1 = std::rotr(words[index - 2], 17) ^ std::rotr(words[index - 2], 19) ^
                        (words[index - 2] >> 10);
    words[index] = words[index - 16] + sigma0 + words[index - 7] + sigma1;
  }

  auto a = state[0];
  auto b = state[1];
  auto c = state[2];
  auto d = state[3];
  auto e = state[4];
  auto f = state[5];
  auto g = state[6];
  auto h = state[7];

  for (std::size_t index = 0; index < words.size(); ++index) {
    const auto choice = (e & f) ^ ((~e) & g);
    const auto majority = (a & b) ^ (a & c) ^ (b & c);
    const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    const auto temporary1 = h + sum1 + choice + sha256_round_constants[index] + words[index];
    const auto temporary2 = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

inline std::array<std::byte, 32> sha256(std::span<const std::byte> input) {
  std::array<std::uint32_t, 8> state{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                     0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

  std::size_t offset = 0;
  while (input.size() - offset >= 64) {
    sha256_compress(state, input.data() + offset);
    offset += 64;
  }

  std::array<std::byte, 128> tail{};
  const auto remainder = input.size() - offset;
  for (std::size_t index = 0; index < remainder; ++index)
    tail[index] = input[offset + index];
  tail[remainder] = std::byte{0x80};

  const std::uint64_t bit_length = static_cast<std::uint64_t>(input.size()) * 8U;
  const std::size_t tail_size = remainder < 56 ? 64 : 128;
  for (std::size_t index = 0; index < 8; ++index) {
    tail[tail_size - 1 - index] = static_cast<std::byte>(bit_length >> (index * 8));
  }
  sha256_compress(state, tail.data());
  if (tail_size == 128)
    sha256_compress(state, tail.data() + 64);

  std::array<std::byte, 32> digest{};
  for (std::size_t index = 0; index < state.size(); ++index) {
    digest[index * 4] = static_cast<std::byte>(state[index] >> 24);
    digest[index * 4 + 1] = static_cast<std::byte>(state[index] >> 16);
    digest[index * 4 + 2] = static_cast<std::byte>(state[index] >> 8);
    digest[index * 4 + 3] = static_cast<std::byte>(state[index]);
  }
  return digest;
}

inline std::string sha256_hex(std::span<const std::byte> input) {
  constexpr std::string_view digits = "0123456789abcdef";
  const auto digest = sha256(input);
  std::string output;
  output.reserve(digest.size() * 2);
  for (const auto octet : digest) {
    const auto value = std::to_integer<std::uint8_t>(octet);
    output.push_back(digits[value >> 4]);
    output.push_back(digits[value & 0x0f]);
  }
  return output;
}

} // namespace detail

[[nodiscard]] inline CanonicalBytes canonical_bytes(const Money &value) {
  auto output = detail::top_level_bytes();
  detail::append_money(output, value);
  return output;
}

[[nodiscard]] inline CanonicalBytes canonical_bytes(const Quantity &value) {
  return detail::fixed_point_bytes("luca.quantity.v1", "8", value.scaled_value());
}

[[nodiscard]] inline CanonicalBytes canonical_bytes(const Price &value) {
  return detail::fixed_point_bytes("luca.price.v1", "8", value.scaled_value());
}

[[nodiscard]] inline CanonicalBytes canonical_bytes(const Provenance &value) {
  auto output = detail::top_level_bytes();
  detail::append_provenance(output, value);
  return output;
}

[[nodiscard]] inline CanonicalBytes canonical_bytes(const EventHeader &value) {
  auto output = detail::top_level_bytes();
  detail::append_event_header(output, value);
  return output;
}

[[nodiscard]] inline CanonicalBytes canonical_bytes(const CashMovement &value) {
  auto output = detail::top_level_bytes();
  detail::append_cash_movement(output, value);
  return output;
}

[[nodiscard]] inline CanonicalBytes canonical_bytes(const EquityTrade &value) {
  auto output = detail::top_level_bytes();
  detail::append_equity_trade(output, value);
  return output;
}

[[nodiscard]] inline CanonicalBytes canonical_bytes(const EconomicEvent &value) {
  auto output = detail::top_level_bytes();
  detail::append_economic_event(output, value);
  return output;
}

[[nodiscard]] inline CanonicalBytes canonical_bytes(const LifecycleRecord &value) {
  auto output = detail::top_level_bytes();
  detail::append_lifecycle_record(output, value);
  return output;
}

// A LifecycleLedger is the typed sequence boundary: successful acceptance has
// already established contiguous uint64_t sequence values beginning at one.
[[nodiscard]] inline CanonicalBytes canonical_bytes(const LifecycleLedger &value) {
  if (value.empty())
    throw std::invalid_argument("LCB1 lifecycle record sequence must not be empty");
  auto output = detail::top_level_bytes();
  detail::append_map(output, 2);
  detail::append_key(output, "records");
  detail::append_array(output, static_cast<std::uint64_t>(value.records().size()));
  for (const auto &record : value.records())
    detail::append_lifecycle_record(output, record);
  detail::append_key(output, "schema_version");
  detail::append_text(output, "luca.lifecycle-record-sequence.v1");
  return output;
}

[[nodiscard]] inline std::string canonical_digest(const Money &value) {
  return detail::sha256_hex(canonical_bytes(value));
}

[[nodiscard]] inline std::string canonical_digest(const Quantity &value) {
  return detail::sha256_hex(canonical_bytes(value));
}

[[nodiscard]] inline std::string canonical_digest(const Price &value) {
  return detail::sha256_hex(canonical_bytes(value));
}

template <class Value>
requires std::same_as<Value, Provenance> || std::same_as<Value, EventHeader> ||
    std::same_as<Value, CashMovement> || std::same_as<Value, EquityTrade> ||
    std::same_as<Value, EconomicEvent> || std::same_as<Value, LifecycleRecord> ||
    std::same_as<Value, LifecycleLedger>
[[nodiscard]] inline std::string canonical_digest(const Value &value) {
  return detail::sha256_hex(canonical_bytes(value));
}

} // namespace luca::serialization
