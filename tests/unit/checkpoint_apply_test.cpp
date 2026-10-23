#include "luca/portfolio.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
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
      Provenance::create({SourceRecordId{std::string{source}}}, "checkpoint-apply-test", "1"));
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

void expect_resume_error(const std::expected<PortfolioState, CheckpointApplyError> &result,
                         CheckpointResumeDiagnosticCategory category,
                         const std::source_location location = std::source_location::current()) {
  check(!result.has_value(), location);
  const auto *error = std::get_if<CheckpointResumeError>(&result.error());
  check(error != nullptr, location);
  check(error->category() == category, location);
  check(error->category_name() == category_name(category), location);
}

void request_factory_categories_remain_stable() {
  const auto fixture = ordinary_fixture();
  const auto &manifest = fixture.manifest;
  const auto manifest_digest = digest(serialization::canonical_digest(manifest));
  const auto create = [&](std::string_view schema_version, std::string_view serialization_version,
                          std::string_view engine_version) {
    return CheckpointResumeRequest::create(
        schema_version, serialization_version, manifest_digest, manifest.projection(),
        engine_version, manifest.policy(), manifest.partition(), manifest.evaluation_context(),
        manifest.event_prefix(), manifest.canonical_state_digest());
  };

  const auto unsupported =
      create("luca.checkpoint-resume.v2", CheckpointResumeRequest::serialization_version,
             manifest.engine_version());
  check(!unsupported.has_value());
  check(unsupported.error().category() == CheckpointResumeDiagnosticCategory::unsupported_version);
  const auto invalid_shape = create(CheckpointResumeRequest::schema_version,
                                    CheckpointResumeRequest::serialization_version, "");
  check(!invalid_shape.has_value());
  check(invalid_shape.error().category() == CheckpointResumeDiagnosticCategory::schema_shape);
}

void full_replay_equals_checkpoint_plus_lifecycle_suffix() {
  const auto fixture = lifecycle_fixture();
  const auto prefix = fixture.ledger.records().first(fixture.prefix_size);
  const auto suffix = fixture.ledger.records().subspan(fixture.prefix_size);
  const auto original_state = fixture.checkpoint_state;
  const std::vector<LifecycleRecord> original_records{fixture.ledger.records().begin(),
                                                      fixture.ledger.records().end()};

  const auto first = apply_checkpoint_suffix(fixture.request, fixture.manifest,
                                             fixture.checkpoint_state, prefix, suffix);
  const auto second = apply_checkpoint_suffix(fixture.request, fixture.manifest,
                                              fixture.checkpoint_state, prefix, suffix);
  check(first.has_value());
  check(second.has_value());
  const auto full = project_state(fixture.ledger, fixture.manifest.evaluation_context());
  check(*first == full);
  check(*second == full);
  check(serialization::canonical_bytes(*first) == serialization::canonical_bytes(full));
  check(serialization::canonical_digest(*first) == serialization::canonical_digest(full));
  check(serialization::canonical_bytes(*first) == serialization::canonical_bytes(*second));
  check(serialization::canonical_digest(*first) == serialization::canonical_digest(*second));

  check(first->positions().size() == 1);
  check(first->positions().front().quantity() == require(Quantity::parse("7")));
  check(first->settled_cash().size() == 2);
  check(first->settled_cash()[0].key().currency() == currency("EUR"));
  check(first->settled_cash()[0].amount() == require(Money::parse("7", currency("EUR"))));
  check(first->settled_cash()[1].key().currency() == currency("USD"));
  check(first->settled_cash()[1].amount() == require(Money::parse("1160", currency("USD"))));
  check(first->open_settlement_obligations().size() == 2);
  check(first->open_settlement_obligations()[0].key().direction() ==
        SettlementDirection::receivable);
  check(first->open_settlement_obligations()[0].amount() ==
        require(Money::parse("24", currency("USD"))));
  check(first->open_settlement_obligations()[1].key().direction() == SettlementDirection::payable);
  check(first->open_settlement_obligations()[1].amount() ==
        require(Money::parse("50", currency("USD"))));
  check(fixture.checkpoint_state == original_state);
  check(std::ranges::equal(fixture.ledger.records(), original_records));
}

void zero_position_and_cash_balances_are_removed() {
  LifecycleLedger ledger;
  append_prefix(ledger, require(Money::parse("10", currency("USD"))), require(Quantity::parse("5")),
                require(Price::parse("10")), date(2026, 1, 4));
  const auto evaluation_context = context();
  const auto state = project_state(ledger, evaluation_context);
  const auto manifest = make_manifest(ledger, state, evaluation_context);
  const auto request = make_request(manifest);
  const auto prefix_size = ledger.size();
  accept(ledger, LifecycleRecordDraft::originate(
                     EconomicEventId{"zero-cash-economic"}, timestamp(2026, 1, 3, 10h),
                     cash_event("zero-cash", "acct-main", timestamp(2026, 1, 3, 9h), "-10",
                                "zero-cash-source")));
  accept(ledger, LifecycleRecordDraft::originate(
                     EconomicEventId{"zero-position-economic"}, timestamp(2026, 1, 4, 10h),
                     trade_event("zero-position", "acct-main", timestamp(2026, 1, 4, 9h), "-5",
                                 "10", date(2026, 1, 4), "zero-position-source")));

  const auto applied =
      apply_checkpoint_suffix(request, manifest, state, ledger.records().first(prefix_size),
                              ledger.records().subspan(prefix_size));
  check(applied.has_value());
  check(applied->positions().empty());
  check(applied->settled_cash().empty());
  check(applied->open_settlement_obligations().empty());
  check(*applied == project_state(ledger, evaluation_context));
}

void identity_context_hash_partition_and_continuity_failures_are_forwarded() {
  const auto fixture = ordinary_fixture();
  const auto prefix = fixture.ledger.records().first(fixture.prefix_size);
  const auto suffix = fixture.ledger.records().subspan(fixture.prefix_size);
  const auto zero = digest(std::string(64, '0'));
  const auto original_state = fixture.checkpoint_state;
  const auto original_manifest = fixture.manifest;
  const auto original_request = fixture.request;
  const std::vector<LifecycleRecord> original_records{fixture.ledger.records().begin(),
                                                      fixture.ledger.records().end()};

  RequestOverrides projection;
  projection.projection = identity("luca.portfolio-state", "2");
  expect_resume_error(apply_checkpoint_suffix(make_request(fixture.manifest, projection),
                                              fixture.manifest, fixture.checkpoint_state, prefix,
                                              suffix),
                      CheckpointResumeDiagnosticCategory::incompatible_projection);
  RequestOverrides engine;
  engine.engine = "luca-engine-2";
  expect_resume_error(apply_checkpoint_suffix(make_request(fixture.manifest, engine),
                                              fixture.manifest, fixture.checkpoint_state, prefix,
                                              suffix),
                      CheckpointResumeDiagnosticCategory::incompatible_engine);
  RequestOverrides policy;
  policy.policy = identity("luca.portfolio-default", "2");
  expect_resume_error(apply_checkpoint_suffix(make_request(fixture.manifest, policy),
                                              fixture.manifest, fixture.checkpoint_state, prefix,
                                              suffix),
                      CheckpointResumeDiagnosticCategory::incompatible_policy);
  RequestOverrides partition;
  partition.partition = require(AccountSetPartition::create({AccountId{"acct-other"}}));
  expect_resume_error(apply_checkpoint_suffix(make_request(fixture.manifest, partition),
                                              fixture.manifest, fixture.checkpoint_state, prefix,
                                              suffix),
                      CheckpointResumeDiagnosticCategory::incompatible_partition);
  RequestOverrides changed_context;
  changed_context.evaluation_context = require(CheckpointEvaluationContext::create(
      timestamp(2026, 2, 1, 23h), fixture.manifest.evaluation_context().economic_as_of(),
      fixture.manifest.evaluation_context().settlement_as_of_date()));
  expect_resume_error(apply_checkpoint_suffix(make_request(fixture.manifest, changed_context),
                                              fixture.manifest, fixture.checkpoint_state, prefix,
                                              suffix),
                      CheckpointResumeDiagnosticCategory::incompatible_context);
  RequestOverrides changed_prefix;
  changed_prefix.prefix = require(CheckpointEventPrefix::create(
      1, 1, 1, EventId{"prefix-cash"}, fixture.manifest.event_prefix().canonical_input_digest()));
  expect_resume_error(apply_checkpoint_suffix(make_request(fixture.manifest, changed_prefix),
                                              fixture.manifest, fixture.checkpoint_state, prefix,
                                              suffix),
                      CheckpointResumeDiagnosticCategory::incompatible_prefix);
  RequestOverrides changed_manifest_digest;
  changed_manifest_digest.manifest_digest = zero;
  expect_resume_error(
      apply_checkpoint_suffix(make_request(fixture.manifest, changed_manifest_digest),
                              fixture.manifest, fixture.checkpoint_state, prefix, suffix),
      CheckpointResumeDiagnosticCategory::digest_mismatch);

  auto tampered_state = fixture.checkpoint_state;
  tampered_state =
      PortfolioState{{Position{PositionKey{AccountId{"acct-main"}, InstrumentId{"instrument-a"}},
                               require(Quantity::parse("4"))}},
                     std::vector<CashBalance>{fixture.checkpoint_state.settled_cash().begin(),
                                              fixture.checkpoint_state.settled_cash().end()},
                     std::vector<SettlementObligation>{
                         fixture.checkpoint_state.open_settlement_obligations().begin(),
                         fixture.checkpoint_state.open_settlement_obligations().end()}};
  expect_resume_error(
      apply_checkpoint_suffix(fixture.request, fixture.manifest, tampered_state, prefix, suffix),
      CheckpointResumeDiagnosticCategory::digest_mismatch);
  expect_resume_error(apply_checkpoint_suffix(fixture.request, fixture.manifest,
                                              fixture.checkpoint_state, prefix, {}),
                      CheckpointResumeDiagnosticCategory::prefix_continuity);

  LifecycleLedger outside_ledger;
  append_prefix(outside_ledger);
  accept(outside_ledger, LifecycleRecordDraft::originate(
                             EconomicEventId{"outside-economic"}, timestamp(2026, 1, 3, 10h),
                             cash_event("outside-record", "acct-other", timestamp(2026, 1, 3, 9h),
                                        "1", "outside-source")));
  expect_resume_error(
      apply_checkpoint_suffix(fixture.request, fixture.manifest, fixture.checkpoint_state,
                              outside_ledger.records().first(fixture.prefix_size),
                              outside_ledger.records().subspan(fixture.prefix_size)),
      CheckpointResumeDiagnosticCategory::incompatible_partition);
  check(fixture.checkpoint_state == original_state);
  check(fixture.manifest == original_manifest);
  check(fixture.request == original_request);
  check(std::ranges::equal(fixture.ledger.records(), original_records));
}

LifecycleLedger relationship_ledger(std::string_view account, std::string_view economic_id,
                                    bool insert_before_correction = false,
                                    std::string_view correction_id = "relationship-correction") {
  LifecycleLedger ledger;
  accept(ledger, LifecycleRecordDraft::originate(
                     EconomicEventId{"filler-economic-1"}, timestamp(2026, 1, 1, 1h),
                     cash_event("filler-record-1", "acct-main", timestamp(2026, 1, 1, 1h), "1",
                                "filler-source-1")));
  accept(ledger, LifecycleRecordDraft::originate(
                     EconomicEventId{"filler-economic-2"}, timestamp(2026, 1, 2, 1h),
                     cash_event("filler-record-2", "acct-main", timestamp(2026, 1, 2, 1h), "1",
                                "filler-source-2")));
  accept(ledger, LifecycleRecordDraft::originate(
                     EconomicEventId{std::string{economic_id}}, timestamp(2026, 1, 3, 10h),
                     cash_event("suffix-origin", account, timestamp(2026, 1, 3, 9h), "100",
                                "relationship-origin-source")));
  if (insert_before_correction) {
    accept(ledger,
           LifecycleRecordDraft::originate(
               EconomicEventId{"relationship-unrelated-economic"}, timestamp(2026, 1, 3, 11h),
               cash_event("relationship-unrelated", "acct-main", timestamp(2026, 1, 3, 10h), "1",
                          "relationship-unrelated-source")));
  }
  accept(ledger,
         LifecycleRecordDraft::correct(
             EconomicEventId{std::string{economic_id}}, EventId{"suffix-origin"},
             timestamp(2026, 1, 4, 10h),
             cash_event(correction_id, account, timestamp(2026, 1, 4, 9h), "200", correction_id)));
  return ledger;
}

void lifecycle_and_late_knowledge_failures_are_forwarded() {
  const auto fixture = ordinary_fixture(true);
  const auto prefix = fixture.ledger.records().first(fixture.prefix_size);
  const auto suffix_origin = fixture.ledger.records()[fixture.prefix_size];

  LifecycleLedger late_ledger;
  append_prefix(late_ledger);
  accept(late_ledger,
         LifecycleRecordDraft::correct(EconomicEventId{"prefix-cash-economic"},
                                       EventId{"prefix-cash"}, timestamp(2026, 1, 3, 10h),
                                       cash_event("late-prefix-correction", "acct-main",
                                                  timestamp(2026, 1, 3, 9h), "1100",
                                                  "late-prefix-correction-source")));
  expect_resume_error(apply_checkpoint_suffix(fixture.request, fixture.manifest,
                                              fixture.checkpoint_state,
                                              late_ledger.records().first(fixture.prefix_size),
                                              late_ledger.records().subspan(fixture.prefix_size)),
                      CheckpointResumeDiagnosticCategory::late_lifecycle_knowledge);

  const auto cross_account = relationship_ledger("acct-other", "suffix-cash-economic");
  const std::vector<LifecycleRecord> cross_account_suffix{suffix_origin,
                                                          cross_account.records()[3]};
  expect_resume_error(apply_checkpoint_suffix(fixture.request, fixture.manifest,
                                              fixture.checkpoint_state, prefix,
                                              cross_account_suffix),
                      CheckpointResumeDiagnosticCategory::incompatible_account);

  const auto changed_identity = relationship_ledger("acct-main", "changed-economic");
  const std::vector<LifecycleRecord> changed_identity_suffix{suffix_origin,
                                                             changed_identity.records()[3]};
  expect_resume_error(apply_checkpoint_suffix(fixture.request, fixture.manifest,
                                              fixture.checkpoint_state, prefix,
                                              changed_identity_suffix),
                      CheckpointResumeDiagnosticCategory::incompatible_event_relationship);

  const auto first_successor =
      relationship_ledger("acct-main", "suffix-cash-economic", false, "first-successor");
  const auto second_successor =
      relationship_ledger("acct-main", "suffix-cash-economic", true, "second-successor");
  const std::vector<LifecycleRecord> conflicting_suffix{suffix_origin, first_successor.records()[3],
                                                        second_successor.records()[4]};
  expect_resume_error(apply_checkpoint_suffix(fixture.request, fixture.manifest,
                                              fixture.checkpoint_state, prefix, conflicting_suffix),
                      CheckpointResumeDiagnosticCategory::conflicting_lifecycle_successor);
}

void arithmetic_failures_return_no_partial_state() {
  const auto evaluation_context = context();

  LifecycleLedger cash_ledger;
  append_prefix(cash_ledger,
                Money::from_scaled(std::numeric_limits<std::int64_t>::max(), currency("USD")),
                require(Quantity::parse("1")), Price::from_scaled(0), date(2026, 1, 4));
  const auto cash_state = project_state(cash_ledger, evaluation_context);
  const auto cash_manifest = make_manifest(cash_ledger, cash_state, evaluation_context);
  const auto cash_request = make_request(cash_manifest);
  const auto cash_prefix_size = cash_ledger.size();
  accept(cash_ledger,
         LifecycleRecordDraft::originate(
             EconomicEventId{"cash-overflow-economic"}, timestamp(2026, 1, 3, 10h),
             cash_event("cash-overflow", "acct-main", timestamp(2026, 1, 3, 9h),
                        Money::from_scaled(1, currency("USD")), "cash-overflow-source")));
  accept(cash_ledger,
         LifecycleRecordDraft::originate(
             EconomicEventId{"cash-offset-economic"}, timestamp(2026, 1, 4, 10h),
             cash_event("cash-offset", "acct-main", timestamp(2026, 1, 4, 9h),
                        Money::from_scaled(-1, currency("USD")), "cash-offset-source")));
  const auto original_cash_state = cash_state;
  const std::vector<LifecycleRecord> original_cash_records{cash_ledger.records().begin(),
                                                           cash_ledger.records().end()};
  const auto cash_result = apply_checkpoint_suffix(cash_request, cash_manifest, cash_state,
                                                   cash_ledger.records().first(cash_prefix_size),
                                                   cash_ledger.records().subspan(cash_prefix_size));
  check(!cash_result.has_value());
  check(std::get<CashProjectionError>(cash_result.error()) == CashProjectionError::amount_overflow);
  const auto full_cash = project_lifecycle(
      cash_ledger.resolve(evaluation_context.recorded_through(),
                          evaluation_context.economic_as_of()),
      LifecycleProjectionContext{evaluation_context.economic_as_of(),
                                 evaluation_context.settlement_as_of_date().value()});
  check(!full_cash.settled_cash.has_value());
  check(full_cash.settled_cash.error() == CashProjectionError::amount_overflow);
  check(cash_state == original_cash_state);
  check(std::ranges::equal(cash_ledger.records(), original_cash_records));

  LifecycleLedger position_ledger;
  append_prefix(position_ledger, require(Money::parse("1", currency("USD"))),
                Quantity::from_scaled(std::numeric_limits<std::int64_t>::max()),
                Price::from_scaled(0), date(2026, 1, 4));
  const auto position_state = project_state(position_ledger, evaluation_context);
  const auto position_manifest = make_manifest(position_ledger, position_state, evaluation_context);
  const auto position_request = make_request(position_manifest);
  const auto position_prefix_size = position_ledger.size();
  accept(position_ledger,
         LifecycleRecordDraft::originate(
             EconomicEventId{"position-overflow-economic"}, timestamp(2026, 1, 3, 10h),
             trade_event("position-overflow", "acct-main", timestamp(2026, 1, 3, 9h),
                         Quantity::from_scaled(1), Price::from_scaled(0), date(2026, 1, 4),
                         "position-overflow-source")));
  accept(position_ledger,
         LifecycleRecordDraft::originate(
             EconomicEventId{"position-offset-economic"}, timestamp(2026, 1, 4, 10h),
             trade_event("position-offset", "acct-main", timestamp(2026, 1, 4, 9h),
                         Quantity::from_scaled(-1), Price::from_scaled(0), date(2026, 1, 4),
                         "position-offset-source")));
  const auto original_position_state = position_state;
  const std::vector<LifecycleRecord> original_position_records{position_ledger.records().begin(),
                                                               position_ledger.records().end()};
  const auto position_result =
      apply_checkpoint_suffix(position_request, position_manifest, position_state,
                              position_ledger.records().first(position_prefix_size),
                              position_ledger.records().subspan(position_prefix_size));
  check(!position_result.has_value());
  check(std::get<PositionProjectionError>(position_result.error()) ==
        PositionProjectionError::quantity_overflow);
  const auto full_position = project_lifecycle(
      position_ledger.resolve(evaluation_context.recorded_through(),
                              evaluation_context.economic_as_of()),
      LifecycleProjectionContext{evaluation_context.economic_as_of(),
                                 evaluation_context.settlement_as_of_date().value()});
  check(!full_position.positions.has_value());
  check(full_position.positions.error() == PositionProjectionError::quantity_overflow);
  check(position_state == original_position_state);
  check(std::ranges::equal(position_ledger.records(), original_position_records));

  auto projection_fixture = ordinary_fixture();
  accept(projection_fixture.ledger,
         LifecycleRecordDraft::originate(
             EconomicEventId{"valuation-overflow-economic"}, timestamp(2026, 1, 4, 10h),
             trade_event("valuation-overflow", "acct-main", timestamp(2026, 1, 4, 9h),
                         Quantity::from_scaled(std::numeric_limits<std::int64_t>::max()),
                         Price::from_scaled(std::numeric_limits<std::int64_t>::max()),
                         date(2026, 2, 1), "valuation-overflow-source")));
  const auto projection_result = apply_checkpoint_suffix(
      projection_fixture.request, projection_fixture.manifest, projection_fixture.checkpoint_state,
      projection_fixture.ledger.records().first(projection_fixture.prefix_size),
      projection_fixture.ledger.records().subspan(projection_fixture.prefix_size));
  check(!projection_result.has_value());
  check(std::get<SettlementProjectionError>(projection_result.error()) ==
        SettlementProjectionError::valuation_overflow);
}

void dated_void_checkpoint_replay() {
  for(bool future:{false,true}) {
    LifecycleLedger ledger;append_prefix(ledger);
    auto state=project_state(ledger,context());
    auto manifest=make_manifest(ledger,state,context());
    auto request=make_request(manifest);const auto prefix_size=ledger.records().size();
    accept(ledger,LifecycleRecordDraft::originate(EconomicEventId{"dated-cash"},timestamp(2026,1,3,10h),
      cash_event("dated-cash","acct-main",timestamp(2026,1,3,9h),"10","dated-cash-source")));
    accept(ledger,LifecycleRecordDraft::cancel(EventId{"dated-void"},EconomicEventId{"dated-cash"},
      EventId{"dated-cash"},AccountId{"acct-main"},timestamp(2026,1,4,10h),provenance("dated-void-source"),
      future ? timestamp(2026,2,1,0h):timestamp(2026,1,30,0h)));
    const auto result=require(apply_checkpoint_suffix(request,manifest,state,
      ledger.records().first(prefix_size),ledger.records().subspan(prefix_size)));
    check(result==project_state(ledger,context()));
    // A checkpoint prefix containing a future void preserves its selected head.
    auto next_manifest=make_manifest(ledger,result,context());
    auto next_request=make_request(next_manifest);const auto next_size=ledger.records().size();
    accept(ledger,LifecycleRecordDraft::originate(EconomicEventId{"after-void"},timestamp(2026,1,5,10h),
      cash_event("after-void","acct-main",timestamp(2026,1,6,9h),"1","after-void-source")));
    check(require(apply_checkpoint_suffix(next_request,next_manifest,result,
      ledger.records().first(next_size),ledger.records().subspan(next_size)))==project_state(ledger,context()));
  }
}

} // namespace

int main() {
  dated_void_checkpoint_replay();
  request_factory_categories_remain_stable();
  full_replay_equals_checkpoint_plus_lifecycle_suffix();
  zero_position_and_cash_balances_are_removed();
  identity_context_hash_partition_and_continuity_failures_are_forwarded();
  lifecycle_and_late_knowledge_failures_are_forwarded();
  arithmetic_failures_return_no_partial_state();
}
