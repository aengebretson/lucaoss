#include "luca/portfolio/checkpoint_result.hpp"
#include "luca/portfolio/checkpoint_decode.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <limits>
#include <optional>
#include <ranges>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace luca;

void check(bool condition, const std::source_location location = std::source_location::current()) {
  if (!condition) {
    std::fprintf(stderr, "check failed at %s:%u\n", location.file_name(), location.line());
    std::abort();
  }
}

template <class Value, class Error>
Value require(std::expected<Value, Error> result,
              const std::source_location location = std::source_location::current()) {
  check(result.has_value(), location);
  return std::move(*result);
}

Timestamp timestamp(int year, unsigned month, unsigned day, std::chrono::nanoseconds time) {
  return Timestamp{std::chrono::sys_days{std::chrono::year{year} / std::chrono::month{month} /
                                         std::chrono::day{day}} +
                   time};
}

SettlementDate date(int year, unsigned month, unsigned day) {
  return require(SettlementDate::create(std::chrono::year{year} / std::chrono::month{month} /
                                        std::chrono::day{day}));
}

Currency currency(std::string_view code) { return require(Currency::from_code(code)); }

Provenance provenance(std::string_view source) {
  return require(
      Provenance::create({SourceRecordId{std::string{source}}}, "checkpoint-result-test", "1"));
}

EconomicEvent cash_event(std::string_view record_id, std::string_view account,
                         Timestamp effective_at, Money amount, std::string_view source) {
  const auto header =
      require(EventHeader::create(EventId{std::string{record_id}}, AccountId{std::string{account}},
                                  effective_at, provenance(source)));
  return CashMovement::create(header, amount);
}

EconomicEvent cash_event(std::string_view record_id, std::string_view account,
                         Timestamp effective_at, std::string_view amount, std::string_view source,
                         Currency denomination = currency("USD")) {
  return cash_event(record_id, account, effective_at, require(Money::parse(amount, denomination)),
                    source);
}

EconomicEvent trade_event(std::string_view record_id, std::string_view account,
                          Timestamp effective_at, Quantity quantity, Price price,
                          SettlementDate settlement_date, std::string_view source,
                          std::string_view instrument = "instrument-a",
                          Currency quote_currency = currency("USD")) {
  const auto header =
      require(EventHeader::create(EventId{std::string{record_id}}, AccountId{std::string{account}},
                                  effective_at, provenance(source)));
  return require(EquityTrade::create(header, InstrumentId{std::string{instrument}}, quantity, price,
                                     quote_currency, settlement_date));
}

EconomicEvent trade_event(std::string_view record_id, std::string_view account,
                          Timestamp effective_at, std::string_view quantity_value,
                          std::string_view price_value, SettlementDate settlement_date,
                          std::string_view source, std::string_view instrument = "instrument-a") {
  return trade_event(record_id, account, effective_at, require(Quantity::parse(quantity_value)),
                     require(Price::parse(price_value)), settlement_date, source, instrument);
}

void accept(LifecycleLedger &ledger, const LifecycleRecordDraft &draft) {
  check(ledger.accept(draft).has_value());
}

CheckpointEvaluationContext context() {
  return require(CheckpointEvaluationContext::create(
      timestamp(2026, 1, 31, 23h), timestamp(2026, 1, 31, 23h), date(2026, 1, 5)));
}

PortfolioState project_state(const LifecycleLedger &ledger,
                             const CheckpointEvaluationContext &evaluation_context) {
  const auto resolution =
      ledger.resolve(evaluation_context.recorded_through(), evaluation_context.economic_as_of());
  auto projected = project_lifecycle(
      resolution, LifecycleProjectionContext{evaluation_context.economic_as_of(),
                                             evaluation_context.settlement_as_of_date().value()});
  check(projected.positions.has_value());
  check(projected.settled_cash.has_value());
  check(projected.open_settlement_obligations.has_value());
  return PortfolioState{std::move(*projected.positions), std::move(*projected.settled_cash),
                        std::move(*projected.open_settlement_obligations)};
}

CheckpointIdentity identity(std::string_view id, std::string_view version = "1") {
  return require(CheckpointIdentity::create(id, version));
}

Sha256Digest digest(std::string_view value) { return require(Sha256Digest::create(value)); }

CheckpointManifest
make_manifest(const LifecycleLedger &prefix_ledger, const PortfolioState &checkpoint_state,
              const CheckpointEvaluationContext &evaluation_context,
              std::vector<AccountId> partition_accounts = {AccountId{"acct-main"}}) {
  const auto records = prefix_ledger.records();
  check(!records.empty());
  const auto resolution = prefix_ledger.resolve(evaluation_context.recorded_through(),
                                                evaluation_context.economic_as_of());
  check(!resolution.active_events().empty());

  std::vector<EventId> lifecycle_ids;
  std::vector<SourceRecordId> source_ids;
  std::unordered_set<std::string_view> seen_sources;
  lifecycle_ids.reserve(records.size());
  for (const auto &record : records) {
    lifecycle_ids.push_back(record.record_id());
    for (const auto &source : record.provenance().source_records()) {
      if (seen_sources.emplace(source.value()).second)
        source_ids.push_back(source);
    }
  }

  std::vector<EventId> active_ids;
  active_ids.reserve(resolution.active_events().size());
  for (const auto &active : resolution.active_events())
    active_ids.push_back(active.record().record_id());

  const auto input_digest = digest(serialization::canonical_digest(prefix_ledger));
  const auto state_digest = digest(serialization::canonical_digest(checkpoint_state));
  const auto prefix = require(CheckpointEventPrefix::create(
      1, records.size(), records.size(), records.back().record_id(), input_digest));
  const auto partition = require(AccountSetPartition::create(partition_accounts));
  const auto lineage = require(CheckpointLineage::create(lifecycle_ids, active_ids, source_ids));
  const auto &last = resolution.active_events().back().record();
  const auto watermark = require(ResolvedEventWatermark::create(
      *last.effective_at(), last.acceptance_sequence().value(), last.record_id()));
  return require(CheckpointManifest::create(identity("luca.portfolio-state"), "luca-engine-1",
                                            identity("luca.portfolio-default"), partition, prefix,
                                            evaluation_context, state_digest, watermark, lineage));
}

struct RequestOverrides {
  std::optional<Sha256Digest> manifest_digest;
  std::optional<CheckpointIdentity> projection;
  std::optional<std::string> engine;
  std::optional<CheckpointIdentity> policy;
  std::optional<AccountSetPartition> partition;
  std::optional<CheckpointEvaluationContext> evaluation_context;
  std::optional<CheckpointEventPrefix> prefix;
  std::optional<Sha256Digest> state_digest;
};

CheckpointResumeRequest make_request(const CheckpointManifest &manifest,
                                     const RequestOverrides &overrides = {}) {
  return require(CheckpointResumeRequest::create(
      CheckpointResumeRequest::schema_version, CheckpointResumeRequest::serialization_version,
      overrides.manifest_digest.value_or(digest(serialization::canonical_digest(manifest))),
      overrides.projection.value_or(manifest.projection()),
      overrides.engine.value_or(manifest.engine_version()),
      overrides.policy.value_or(manifest.policy()),
      overrides.partition.value_or(manifest.partition()),
      overrides.evaluation_context.value_or(manifest.evaluation_context()),
      overrides.prefix.value_or(manifest.event_prefix()),
      overrides.state_digest.value_or(manifest.canonical_state_digest())));
}

struct Fixture {
  LifecycleLedger ledger;
  std::size_t prefix_size;
  PortfolioState checkpoint_state;
  CheckpointManifest manifest;
  CheckpointResumeRequest request;
};

void append_prefix(LifecycleLedger &ledger,
                   Money cash = require(Money::parse("1000", currency("USD"))),
                   Quantity quantity = require(Quantity::parse("5")),
                   Price price = require(Price::parse("10")),
                   SettlementDate settlement_date = date(2026, 1, 10)) {
  accept(ledger, LifecycleRecordDraft::originate(
                     EconomicEventId{"prefix-cash-economic"}, timestamp(2026, 1, 1, 10h),
                     cash_event("prefix-cash", "acct-main", timestamp(2026, 1, 1, 9h), cash,
                                "prefix-cash-source")));
  accept(ledger, LifecycleRecordDraft::originate(
                     EconomicEventId{"prefix-trade-economic"}, timestamp(2026, 1, 2, 10h),
                     trade_event("prefix-trade", "acct-main", timestamp(2026, 1, 2, 9h), quantity,
                                 price, settlement_date, "prefix-trade-source")));
}

Fixture ordinary_fixture(bool include_other_account = false) {
  LifecycleLedger ledger;
  append_prefix(ledger);
  const auto evaluation_context = context();
  const auto state = project_state(ledger, evaluation_context);
  const auto manifest =
      make_manifest(ledger, state, evaluation_context,
                    include_other_account
                        ? std::vector<AccountId>{AccountId{"acct-main"}, AccountId{"acct-other"}}
                        : std::vector<AccountId>{AccountId{"acct-main"}});
  const auto request = make_request(manifest);
  const auto prefix_size = ledger.size();
  accept(ledger, LifecycleRecordDraft::originate(
                     EconomicEventId{"suffix-cash-economic"}, timestamp(2026, 1, 3, 10h),
                     cash_event("suffix-origin", "acct-main", timestamp(2026, 1, 3, 9h), "100",
                                "suffix-origin-source")));
  return Fixture{std::move(ledger), prefix_size, state, manifest, request};
}

Fixture lifecycle_fixture() {
  auto fixture = ordinary_fixture();
  auto &ledger = fixture.ledger;
  accept(ledger, LifecycleRecordDraft::correct(EconomicEventId{"suffix-cash-economic"},
                                               EventId{"suffix-origin"}, timestamp(2026, 1, 4, 10h),
                                               cash_event("suffix-correction", "acct-main",
                                                          timestamp(2026, 1, 3, 11h), "200",
                                                          "suffix-correction-source")));
  accept(ledger, LifecycleRecordDraft::originate(
                     EconomicEventId{"suffix-reversed-economic"}, timestamp(2026, 1, 5, 10h),
                     cash_event("suffix-reversed", "acct-main", timestamp(2026, 1, 4, 9h), "50",
                                "suffix-reversed-source")));
  accept(ledger, LifecycleRecordDraft::reverse(
                     EconomicEventId{"suffix-reversal-economic"}, EventId{"suffix-reversed"},
                     timestamp(2026, 1, 6, 10h),
                     cash_event("suffix-reversal", "acct-main", timestamp(2026, 1, 5, 9h), "-50",
                                "suffix-reversal-source")));
  accept(ledger, LifecycleRecordDraft::originate(
                     EconomicEventId{"suffix-trade-economic"}, timestamp(2026, 1, 7, 10h),
                     trade_event("suffix-trade", "acct-main", timestamp(2026, 1, 6, 9h), "3", "10",
                                 date(2026, 1, 4), "suffix-trade-source")));
  accept(ledger, LifecycleRecordDraft::correct(
                     EconomicEventId{"suffix-trade-economic"}, EventId{"suffix-trade"},
                     timestamp(2026, 1, 8, 10h),
                     trade_event("suffix-trade-correction", "acct-main", timestamp(2026, 1, 6, 10h),
                                 "4", "10", date(2026, 1, 4), "suffix-trade-correction-source")));
  accept(ledger, LifecycleRecordDraft::originate(
                     EconomicEventId{"suffix-sale-economic"}, timestamp(2026, 1, 9, 10h),
                     trade_event("suffix-sale", "acct-main", timestamp(2026, 1, 7, 9h), "-2", "12",
                                 date(2026, 1, 10), "suffix-sale-source")));
  accept(ledger, LifecycleRecordDraft::originate(
                     EconomicEventId{"suffix-cancelled-economic"}, timestamp(2026, 1, 10, 10h),
                     cash_event("suffix-cancelled", "acct-main", timestamp(2026, 1, 8, 9h), "10",
                                "suffix-cancelled-source")));
  accept(ledger, LifecycleRecordDraft::cancel(
                     EventId{"suffix-cancellation"}, EconomicEventId{"suffix-cancelled-economic"},
                     EventId{"suffix-cancelled"}, AccountId{"acct-main"},
                     timestamp(2026, 1, 11, 10h), provenance("suffix-cancellation-source")));
  accept(ledger, LifecycleRecordDraft::originate(
                     EconomicEventId{"suffix-eur-economic"}, timestamp(2026, 1, 12, 10h),
                     cash_event("suffix-eur", "acct-main", timestamp(2026, 1, 9, 9h), "7",
                                "suffix-eur-source", currency("EUR"))));
  return fixture;
}

void verify_evidence(const CheckpointApplicationResult &result, const LifecycleLedger &ledger,
                     const CheckpointManifest &prior) {
  const auto full = project_state(ledger, prior.evaluation_context());
  check(result.state == full);
  check(result.manifest == make_manifest(ledger, full, prior.evaluation_context()));
  check(result.manifest.event_prefix().canonical_input_digest().value() ==
        serialization::canonical_digest(ledger));
  check(result.manifest.canonical_state_digest().value() == serialization::canonical_digest(full));
  check(result.manifest.projection() == prior.projection());
  check(result.manifest.engine_version() == prior.engine_version());
  check(result.manifest.policy() == prior.policy());
  check(result.manifest.partition() == prior.partition());
  check(result.manifest.evaluation_context() == prior.evaluation_context());
  const auto bytes = serialization::canonical_bytes(result.manifest);
  const auto decoded = require(serialization::decode_checkpoint_manifest(bytes));
  check(decoded == result.manifest);
  check(serialization::canonical_bytes(decoded) == bytes);
  check(serialization::canonical_digest(decoded) == serialization::canonical_digest(result.manifest));
}

void full_replay_and_multiple_continuations() {
  auto fixture = lifecycle_fixture();
  const auto original_manifest = fixture.manifest;
  const auto original_state = fixture.checkpoint_state;
  const auto original_request = fixture.request;
  const auto input_bytes = serialization::canonical_bytes(fixture.ledger);
  const auto prefix = fixture.ledger.records().first(fixture.prefix_size);
  const auto suffix = fixture.ledger.records().subspan(fixture.prefix_size);
  const auto first = require(apply_checkpoint_suffix_with_manifest(
      fixture.request, fixture.manifest, fixture.checkpoint_state, prefix, suffix));
  verify_evidence(first, fixture.ledger, fixture.manifest);
  check(first.state == require(apply_checkpoint_suffix(
                           fixture.request, fixture.manifest, fixture.checkpoint_state, prefix, suffix)));
  const auto again = require(apply_checkpoint_suffix_with_manifest(
      fixture.request, fixture.manifest, fixture.checkpoint_state, prefix, suffix));
  check(serialization::canonical_bytes(first.state) == serialization::canonical_bytes(again.state));
  check(serialization::canonical_bytes(first.manifest) == serialization::canonical_bytes(again.manifest));
  check(first.state.positions().front().quantity() == require(Quantity::parse("7")));
  check(first.state.settled_cash()[0].amount() == require(Money::parse("7", currency("EUR"))));
  check(first.state.settled_cash()[1].amount() == require(Money::parse("1160", currency("USD"))));
  check(first.state.open_settlement_obligations().size() == 2);
  check(first.state.open_settlement_obligations()[0].amount() == require(Money::parse("24", currency("USD"))));
  check(first.state.open_settlement_obligations()[1].amount() == require(Money::parse("50", currency("USD"))));
  const std::vector<EventId> active{
      EventId{"prefix-cash"}, EventId{"prefix-trade"}, EventId{"suffix-correction"},
      EventId{"suffix-reversed"}, EventId{"suffix-reversal"}, EventId{"suffix-trade-correction"},
      EventId{"suffix-sale"}, EventId{"suffix-eur"}};
  check(std::ranges::equal(first.manifest.lineage().active_record_ids(), active));
  check(first.manifest.lineage().lifecycle_record_ids().size() == 12);
  check(first.manifest.lineage().source_record_ids().size() == 12);
  check(fixture.manifest == original_manifest);
  check(fixture.checkpoint_state == original_state);
  check(fixture.request == original_request);
  check(serialization::canonical_bytes(fixture.ledger) == input_bytes);

  const auto boundary = fixture.ledger.size();
  // Repeat an existing source: evidence uses first occurrence order, not sorting
  // and not concatenation of source lists across continuations.
  accept(fixture.ledger, LifecycleRecordDraft::originate(
      EconomicEventId{"second-economic"}, timestamp(2026, 1, 13, 10h),
      cash_event("second", "acct-main", timestamp(2026, 1, 10, 9h), "-160", "prefix-cash-source")));
  const auto second = require(apply_checkpoint_suffix_with_manifest(
      make_request(first.manifest), first.manifest, first.state,
      fixture.ledger.records().first(boundary), fixture.ledger.records().subspan(boundary)));
  verify_evidence(second, fixture.ledger, fixture.manifest);
  check(second.manifest.event_prefix().record_count() == 13);
  check(second.manifest.lineage().source_record_ids().size() == 12);
  check(second.manifest.resolved_event_watermark().acceptance_sequence() == 13);
  check(second.state.settled_cash()[1].amount() == require(Money::parse("1000", currency("USD"))));
  const auto third_boundary = fixture.ledger.size();
  accept(fixture.ledger, LifecycleRecordDraft::originate(
      EconomicEventId{"third-economic"}, timestamp(2026, 1, 14, 10h),
      cash_event("third", "acct-main", timestamp(2026, 1, 11, 9h), "-7", "third-source", currency("EUR"))));
  const auto third = require(apply_checkpoint_suffix_with_manifest(
      make_request(second.manifest), second.manifest, second.state,
      fixture.ledger.records().first(third_boundary), fixture.ledger.records().subspan(third_boundary)));
  verify_evidence(third, fixture.ledger, fixture.manifest);
  check(third.state.settled_cash().size() == 1);
}

void context_selection_and_watermark_are_not_last_acceptance() {
  auto fixture = ordinary_fixture();
  // Correction accepted after the knowledge cutoff must remain historical
  // evidence while the original suffix head remains selected.
  accept(fixture.ledger, LifecycleRecordDraft::correct(
      EconomicEventId{"suffix-cash-economic"}, EventId{"suffix-origin"}, timestamp(2026, 2, 1, 10h),
      cash_event("future-correction", "acct-main", timestamp(2026, 1, 4, 9h), "200", "future-source")));
  const auto result = require(apply_checkpoint_suffix_with_manifest(
      fixture.request, fixture.manifest, fixture.checkpoint_state,
      fixture.ledger.records().first(2), fixture.ledger.records().subspan(2)));
  verify_evidence(result, fixture.ledger, fixture.manifest);
  check(result.manifest.event_prefix().last_record_id() == EventId{"future-correction"});
  check(result.manifest.resolved_event_watermark().record_id() == EventId{"suffix-origin"});
  check(result.manifest.resolved_event_watermark().acceptance_sequence() == 3);
  check(result.state.settled_cash().front().amount() == require(Money::parse("1100", currency("USD"))));

  auto cancelled = ordinary_fixture();
  accept(cancelled.ledger, LifecycleRecordDraft::cancel(
      EventId{"cancel"}, EconomicEventId{"suffix-cash-economic"}, EventId{"suffix-origin"},
      AccountId{"acct-main"}, timestamp(2026, 1, 4, 10h), provenance("cancel-source")));
  const auto inert = require(apply_checkpoint_suffix_with_manifest(
      cancelled.request, cancelled.manifest, cancelled.checkpoint_state,
      cancelled.ledger.records().first(2), cancelled.ledger.records().subspan(2)));
  verify_evidence(inert, cancelled.ledger, cancelled.manifest);
  check(inert.state == cancelled.checkpoint_state);
  check(inert.manifest.resolved_event_watermark() == cancelled.manifest.resolved_event_watermark());
  check(inert.manifest.event_prefix().record_count() == 4);
}

void explicit_context_inputs_are_preserved() {
  auto fixture = ordinary_fixture();
  const auto input = require(CheckpointInput::create("synthetic-input", "7", digest(std::string(64, 'a'))));
  const auto base = context();
  const auto rich_context = require(CheckpointEvaluationContext::create(
      base.recorded_through(), base.economic_as_of(), base.settlement_as_of_date(),
      {input}, {input}, {input}, {input}, {input}));
  LifecycleLedger prefix;
  append_prefix(prefix);
  const auto standard = make_manifest(prefix, fixture.checkpoint_state, rich_context);
  const auto manifest = require(CheckpointManifest::create(
      identity("reporting-projection", "9"), "engine-19", identity("policy", "42"),
      standard.partition(), standard.event_prefix(), rich_context,
      standard.canonical_state_digest(), standard.resolved_event_watermark(), standard.lineage()));
  const auto result = require(apply_checkpoint_suffix_with_manifest(
      make_request(manifest), manifest, fixture.checkpoint_state,
      fixture.ledger.records().first(2), fixture.ledger.records().subspan(2)));
  check(result.manifest.evaluation_context() == rich_context);
  check(result.manifest.projection() == manifest.projection());
  check(result.manifest.engine_version() == manifest.engine_version());
  check(result.manifest.policy() == manifest.policy());
}

void tied_economic_time_continues_in_acceptance_order() {
  auto fixture = ordinary_fixture();
  // The ordering watermark is (effective time, acceptance sequence): a new
  // payload at the prefix's last effective time still follows that prefix.
  accept(fixture.ledger, LifecycleRecordDraft::originate(
      EconomicEventId{"same-time-economic"}, timestamp(2026, 1, 4, 10h),
      cash_event("same-time", "acct-main", timestamp(2026, 1, 2, 9h), "7", "same-time-source")));
  const auto result = require(apply_checkpoint_suffix_with_manifest(
      fixture.request, fixture.manifest, fixture.checkpoint_state,
      fixture.ledger.records().first(2), fixture.ledger.records().subspan(2)));
  verify_evidence(result, fixture.ledger, fixture.manifest);
  check(result.state.settled_cash().front().amount() ==
        require(Money::parse("1107", currency("USD"))));
  check(result.manifest.lineage().active_record_ids()[2] == EventId{"same-time"});
  check(result.manifest.resolved_event_watermark().record_id() == EventId{"suffix-origin"});
}

void multiple_account_partition_is_preserved() {
  auto fixture = ordinary_fixture(true);
  accept(fixture.ledger, LifecycleRecordDraft::originate(
      EconomicEventId{"other-account-economic"}, timestamp(2026, 1, 4, 10h),
      cash_event("other-account", "acct-other", timestamp(2026, 1, 4, 9h), "19",
                 "other-account-source", currency("EUR"))));
  const auto result = require(apply_checkpoint_suffix_with_manifest(
      fixture.request, fixture.manifest, fixture.checkpoint_state,
      fixture.ledger.records().first(2), fixture.ledger.records().subspan(2)));
  const auto full = project_state(fixture.ledger, context());
  check(result.state == full);
  check(result.manifest == make_manifest(fixture.ledger, full, context(),
      {AccountId{"acct-main"}, AccountId{"acct-other"}}));
  check(result.manifest.partition() == fixture.manifest.partition());
  // A second continuation must accept the exact refreshed partition and
  // preserve currency/account separation through the returned evidence.
  const auto boundary = fixture.ledger.size();
  accept(fixture.ledger, LifecycleRecordDraft::originate(
      EconomicEventId{"other-account-second-economic"}, timestamp(2026, 1, 5, 10h),
      cash_event("other-account-second", "acct-other", timestamp(2026, 1, 5, 9h), "3",
                 "other-account-second-source", currency("EUR"))));
  const auto second = require(apply_checkpoint_suffix_with_manifest(
      make_request(result.manifest), result.manifest, result.state,
      fixture.ledger.records().first(boundary), fixture.ledger.records().subspan(boundary)));
  check(second.state == project_state(fixture.ledger, context()));
  check(second.manifest.partition() == fixture.manifest.partition());
}

void expect_forwarded_resume_error(const Fixture &fixture, const CheckpointResumeRequest &request,
                                  std::span<const LifecycleRecord> suffix,
                                  CheckpointResumeDiagnosticCategory category) {
  const auto state = fixture.checkpoint_state;
  const auto manifest = fixture.manifest;
  const auto request_copy = request;
  const auto records = serialization::canonical_bytes(fixture.ledger);
  const auto prefix = fixture.ledger.records().first(fixture.prefix_size);
  const auto legacy = apply_checkpoint_suffix(request, manifest, state, prefix, suffix);
  const auto result = apply_checkpoint_suffix_with_manifest(request, manifest, state, prefix, suffix);
  check(!legacy && !result);
  const auto *error = std::get_if<CheckpointResumeError>(&result.error());
  check(error != nullptr);
  check(error->category() == category);
  check(error->message() == std::get<CheckpointResumeError>(legacy.error()).message());
  check(fixture.checkpoint_state == state);
  check(fixture.manifest == manifest);
  check(request == request_copy);
  check(serialization::canonical_bytes(fixture.ledger) == records);
}

void errors_are_forwarded_without_mutation() {
  auto fixture = ordinary_fixture();
  const auto suffix = fixture.ledger.records().subspan(2);
  RequestOverrides policy;
  policy.policy = identity("different-policy");
  expect_forwarded_resume_error(fixture, make_request(fixture.manifest, policy), suffix,
                               CheckpointResumeDiagnosticCategory::incompatible_policy);
  RequestOverrides changed_context;
  changed_context.evaluation_context = require(CheckpointEvaluationContext::create(
      timestamp(2026, 2, 1, 23h), context().economic_as_of(), context().settlement_as_of_date()));
  expect_forwarded_resume_error(fixture, make_request(fixture.manifest, changed_context), suffix,
                               CheckpointResumeDiagnosticCategory::incompatible_context);
  RequestOverrides hash;
  hash.manifest_digest = digest(std::string(64, '0'));
  expect_forwarded_resume_error(fixture, make_request(fixture.manifest, hash), suffix,
                               CheckpointResumeDiagnosticCategory::digest_mismatch);
  expect_forwarded_resume_error(fixture, fixture.request, {},
                               CheckpointResumeDiagnosticCategory::prefix_continuity);
  // Each prefix-targeting operation must preserve the original late-knowledge diagnostic.
  for (int action = 0; action != 3; ++action) {
    auto late = ordinary_fixture();
    if (action == 0)
      accept(late.ledger, LifecycleRecordDraft::correct(
          EconomicEventId{"prefix-cash-economic"}, EventId{"prefix-cash"}, timestamp(2026, 1, 4, 10h),
          cash_event("late", "acct-main", timestamp(2026, 1, 4, 9h), "1200", "late-source")));
    else if (action == 1)
      accept(late.ledger, LifecycleRecordDraft::cancel(
          EventId{"late"}, EconomicEventId{"prefix-cash-economic"}, EventId{"prefix-cash"},
          AccountId{"acct-main"}, timestamp(2026, 1, 4, 10h), provenance("late-source")));
    else
      accept(late.ledger, LifecycleRecordDraft::reverse(
          EconomicEventId{"late-economic"}, EventId{"prefix-cash"}, timestamp(2026, 1, 4, 10h),
          cash_event("late", "acct-main", timestamp(2026, 1, 4, 9h), "-1000", "late-source")));
    expect_forwarded_resume_error(late, late.request, late.ledger.records().subspan(2),
                                 CheckpointResumeDiagnosticCategory::late_lifecycle_knowledge);
  }
  auto early = ordinary_fixture();
  accept(early.ledger, LifecycleRecordDraft::originate(
      EconomicEventId{"early-economic"}, timestamp(2026, 1, 4, 10h),
      cash_event("early", "acct-main", timestamp(2026, 1, 1, 9h), "1", "early-source")));
  expect_forwarded_resume_error(early, early.request, early.ledger.records().subspan(2),
                               CheckpointResumeDiagnosticCategory::late_lifecycle_knowledge);
  auto outside = ordinary_fixture();
  accept(outside.ledger, LifecycleRecordDraft::originate(
      EconomicEventId{"outside-economic"}, timestamp(2026, 1, 4, 10h),
      cash_event("outside", "acct-other", timestamp(2026, 1, 4, 9h), "1", "outside-source")));
  expect_forwarded_resume_error(outside, outside.request, outside.ledger.records().subspan(2),
                               CheckpointResumeDiagnosticCategory::incompatible_partition);
}

void intermediate_overflow_is_not_offset_away() {
  LifecycleLedger ledger;
  append_prefix(ledger, Money::from_scaled(std::numeric_limits<std::int64_t>::max(), currency("USD")));
  const auto state = project_state(ledger, context());
  const auto manifest = make_manifest(ledger, state, context());
  const auto request = make_request(manifest);
  accept(ledger, LifecycleRecordDraft::originate(
      EconomicEventId{"plus-economic"}, timestamp(2026, 1, 3, 10h),
      cash_event("plus", "acct-main", timestamp(2026, 1, 3, 9h), "0.000001", "plus-source")));
  accept(ledger, LifecycleRecordDraft::originate(
      EconomicEventId{"minus-economic"}, timestamp(2026, 1, 4, 10h),
      cash_event("minus", "acct-main", timestamp(2026, 1, 4, 9h), "-0.000001", "minus-source")));
  const auto bytes = serialization::canonical_bytes(ledger);
  const auto result = apply_checkpoint_suffix_with_manifest(
      request, manifest, state, ledger.records().first(2), ledger.records().subspan(2));
  check(!result);
  check(std::get<CashProjectionError>(result.error()) == CashProjectionError::amount_overflow);
  const auto full = project_lifecycle(ledger.resolve(context().recorded_through(), context().economic_as_of()),
      LifecycleProjectionContext{context().economic_as_of(), context().settlement_as_of_date().value()});
  check(!full.settled_cash && full.settled_cash.error() == CashProjectionError::amount_overflow);
  check(serialization::canonical_bytes(ledger) == bytes);
  check(serialization::canonical_digest(state) == manifest.canonical_state_digest().value());
}

void canonical_construction_failure_is_explicit() {
  auto fixture = ordinary_fixture();
  // Core provenance allows this duplicate, while canonical v1 rejects it.
  const auto duplicate = require(Provenance::create(
      {SourceRecordId{"duplicate"}, SourceRecordId{"duplicate"}}, "synthetic", "1"));
  const auto header = require(EventHeader::create(EventId{"unencodable"}, AccountId{"acct-main"},
      timestamp(2026, 1, 4, 9h), duplicate));
  accept(fixture.ledger, LifecycleRecordDraft::originate(
      EconomicEventId{"unencodable-economic"}, timestamp(2026, 1, 4, 10h),
      CashMovement::create(header, require(Money::parse("1", currency("USD"))))));
  const auto state = fixture.checkpoint_state;
  const auto manifest = fixture.manifest;
  const std::vector<LifecycleRecord> records{fixture.ledger.records().begin(), fixture.ledger.records().end()};
  const auto result = apply_checkpoint_suffix_with_manifest(
      fixture.request, fixture.manifest, fixture.checkpoint_state,
      fixture.ledger.records().first(2), fixture.ledger.records().subspan(2));
  check(!result);
  const auto *error = std::get_if<CheckpointResultConstructionError>(&result.error());
  check(error != nullptr);
  check(error->category == CheckpointResultConstructionCategory::canonical_encoding);
  check(!error->message.empty());
  check(fixture.checkpoint_state == state && fixture.manifest == manifest);
  check(std::ranges::equal(fixture.ledger.records(), records));
}

void economic_order_and_source_order_are_explicit() {
  auto fixture = ordinary_fixture();
  const auto provenance_value = require(Provenance::create(
      {SourceRecordId{"z-source"}, SourceRecordId{"prefix-cash-source"}, SourceRecordId{"a-source"}},
      "checkpoint-result-test", "1"));
  const auto header = require(EventHeader::create(EventId{"later-economic"}, AccountId{"acct-main"},
      timestamp(2026, 1, 6, 9h), provenance_value));
  accept(fixture.ledger, LifecycleRecordDraft::originate(
      EconomicEventId{"later-economic-id"}, timestamp(2026, 1, 4, 10h),
      CashMovement::create(header, require(Money::parse("1", currency("USD"))))));
  accept(fixture.ledger, LifecycleRecordDraft::originate(
      EconomicEventId{"last-accepted-economic"}, timestamp(2026, 1, 5, 10h),
      cash_event("last-accepted", "acct-main", timestamp(2026, 1, 4, 9h), "2", "a-source")));
  const auto result = require(apply_checkpoint_suffix_with_manifest(
      fixture.request, fixture.manifest, fixture.checkpoint_state,
      fixture.ledger.records().first(2), fixture.ledger.records().subspan(2)));
  verify_evidence(result, fixture.ledger, fixture.manifest);
  const std::vector<SourceRecordId> sources{SourceRecordId{"prefix-cash-source"},
      SourceRecordId{"prefix-trade-source"}, SourceRecordId{"suffix-origin-source"},
      SourceRecordId{"z-source"}, SourceRecordId{"a-source"}};
  check(std::ranges::equal(result.manifest.lineage().source_record_ids(), sources));
  check(result.manifest.event_prefix().last_record_id() == EventId{"last-accepted"});
  check(result.manifest.resolved_event_watermark().record_id() == EventId{"later-economic"});
  check(result.manifest.resolved_event_watermark().acceptance_sequence() == 4);
}

void dated_replacement_checkpoint_conformance() {
  std::ifstream cases{LUCA_DATED_CHECKPOINT_CASES};
  check(cases.good());
  int year;
  unsigned month, day, hour;
  std::string active_id, amount;
  std::size_t count = 0;
  while (cases >> year >> month >> day >> hour >> active_id >> amount) {
    ++count;
    const auto evaluation = require(CheckpointEvaluationContext::create(
        context().recorded_through(), timestamp(year, month, day, std::chrono::hours{hour}),
        context().settlement_as_of_date()));
    for (const bool amendments_in_prefix : {true, false}) {
      LifecycleLedger ledger;
      append_prefix(ledger);
      const auto append_amendments = [&] {
        accept(ledger, LifecycleRecordDraft::originate(
                           EconomicEventId{"dated-economic"}, timestamp(2026, 1, 3, 10h),
                           cash_event("dated-original", "acct-main", timestamp(2026, 1, 3, 9h),
                                      "10", "dated-source")));
        const auto recorded = timestamp(2026, 1, 4, 10h);
        const std::array amendments{
            LifecycleRecordDraft::cancel(EventId{"dated-void"}, EconomicEventId{"dated-economic"},
                                         EventId{"dated-original"}, AccountId{"acct-main"},
                                         recorded, provenance("void-source"),
                                         timestamp(2026, 2, 1, 0h)),
            LifecycleRecordDraft::correct(
                EconomicEventId{"dated-economic"}, EventId{"dated-void"}, recorded,
                cash_event("dated-replacement", "acct-main", timestamp(2026, 1, 3, 9h), "20",
                           "replacement-source"))};
        require(ledger.accept_batch(amendments));
      };
      if (amendments_in_prefix)
        append_amendments();
      const auto state = project_state(ledger, evaluation);
      const auto manifest = make_manifest(ledger, state, evaluation);
      const auto request = make_request(manifest);
      const auto prefix_size = ledger.size();
      if (!amendments_in_prefix)
        append_amendments();
      else
        accept(ledger, LifecycleRecordDraft::originate(
                           EconomicEventId{"after-dated-economic"}, timestamp(2026, 1, 5, 10h),
                           cash_event("after-dated", "acct-main", timestamp(2026, 1, 5, 9h), "5",
                                      "after-source")));
      const auto result = require(apply_checkpoint_suffix_with_manifest(
          request, manifest, state, ledger.records().first(prefix_size),
          ledger.records().subspan(prefix_size)));
      verify_evidence(result, ledger, manifest);
      const auto resolution =
          ledger.resolve(evaluation.recorded_through(), evaluation.economic_as_of());
      const auto selected = std::ranges::find_if(resolution.active_events(), [&](const auto &e) {
        return e.record().economic_event_id() == EconomicEventId{"dated-economic"};
      });
      check(selected != resolution.active_events().end());
      check(selected->record().record_id() == EventId{active_id});
      check(std::get<CashMovement>(selected->event()).amount() ==
            require(Money::parse(amount, currency("USD"))));
    }
  }
  check(cases.eof() && count == 2);
}

void factory_failure_retains_its_diagnostic() {
  auto fixture = ordinary_fixture();
  // UTF-8 decomposed e + acute is accepted by the event value, but v1
  // manifest identity factories require NFC text and must report invalid_text.
  const std::string non_nfc = "record-e\xcc\x81";
  accept(fixture.ledger, LifecycleRecordDraft::originate(
      EconomicEventId{"non-nfc-economic"}, timestamp(2026, 1, 4, 10h),
      cash_event(non_nfc, "acct-main", timestamp(2026, 1, 4, 9h), "1", "nfc-source")));
  const auto result = apply_checkpoint_suffix_with_manifest(
      fixture.request, fixture.manifest, fixture.checkpoint_state,
      fixture.ledger.records().first(2), fixture.ledger.records().subspan(2));
  check(!result);
  const auto *error = std::get_if<CheckpointError>(&result.error());
  check(error != nullptr);
  check(error->category() == CheckpointDiagnosticCategory::invalid_text);
}

} // namespace

int main() {
  dated_replacement_checkpoint_conformance();
  full_replay_and_multiple_continuations();
  context_selection_and_watermark_are_not_last_acceptance();
  explicit_context_inputs_are_preserved();
  tied_economic_time_continues_in_acceptance_order();
  multiple_account_partition_is_preserved();
  economic_order_and_source_order_are_explicit();
  factory_failure_retains_its_diagnostic();
  errors_are_forwarded_without_mutation();
  intermediate_overflow_is_not_offset_away();
  canonical_construction_failure_is_explicit();
}
