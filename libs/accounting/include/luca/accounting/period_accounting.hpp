#pragma once
#include "luca/accounting/trade_accounting.hpp"

namespace luca::accounting {
inline constexpr std::string_view period_accounting_version = "period-accounting/1";
inline constexpr std::string_view period_chart_version = "cash-equity-chart/1";
// No implicit correction-date policy. Linked dated replacements retain the
// void-effective gate as well as the recorded boundary.
enum class CorrectionRecognition { recorded_time };
enum class MovementKind {
  source_activity,
  actual_settlement,
  refund,
  correction,
  dated_void,
  replacement,
  policy_change,
  undated_void
};
struct KnownPolicyRevision {
  PolicyRevision revision;
  Timestamp recorded_at;
  bool operator==(const KnownPolicyRevision &) const = default;
};
struct PeriodContext {
  Timestamp from, until, recorded_through; // half-open recognition interval [from, until)
  std::uint64_t accepted_through, policy_through, settlement_through;
  std::string reporting_timezone, mapping_version;
  CorrectionRecognition corrections;
  bool operator==(const PeriodContext &) const = default;
};
struct PeriodLimits {
  std::size_t max_boundaries = 4096;
  std::size_t max_replay_work = 2000000; // records + settlements, times boundaries
};
struct PeriodError {
  std::string code, id, expected_digest, actual_digest;
};
struct ActivityCause {
  MovementKind kind;
  std::string id, economic_event;
  Timestamp recorded_at;
  std::optional<Timestamp> effective_at, void_effective_at;
  bool operator==(const ActivityCause &) const = default;
};
struct ActivityEffect {
  JournalEntry journal; // derived, balanced; never an authoritative source event
  std::optional<JournalEntry> before, after;
  std::vector<ActivityCause> causes;
  std::vector<ResolvedPolicy> policies_before, policies_after;
  std::vector<std::string> settlement_ids;
  bool operator==(const ActivityEffect &) const = default;
};
struct PeriodMovement {
  std::vector<MovementKind> kinds; // an atomic boundary can have several causes
  Money net;                       // debit-positive
  std::vector<JournalEntryId> entries;
  bool operator==(const PeriodMovement &) const = default;
};
struct PeriodBalance {
  JournalBalanceKey key;
  Money opening, movement, closing;
  std::vector<PeriodMovement> classified;
  bool operator==(const PeriodBalance &) const = default;
};
struct PeriodPnlAmounts {
  Money economic, recognized, deferred; // gains positive; deferred is economic - recognized
  bool operator==(const PeriodPnlAmounts &) const = default;
};
struct ActivityPnl {
  Currency currency;
  PeriodPnlAmounts opening, movement, closing;
  bool operator==(const ActivityPnl &) const = default;
};
struct DailyActivityPnl {
  std::chrono::year_month_day day; // UTC; sparse days with activity, including net-zero changes
  Currency currency;
  PeriodPnlAmounts movement;
  std::vector<JournalEntryId> entries;
  bool operator==(const DailyActivityPnl &) const = default;
};
struct PeriodDiagnostic {
  std::string code, id;
  bool affects_supported_coverage;
  bool operator==(const PeriodDiagnostic &) const = default;
};
enum class StatementClass {
  asset,
  liability,
  contributed_capital,
  realized_income,
  deferred_control,
  settlement_control
};
struct StatementMapping {
  std::string account;
  StatementClass section;
  bool operator==(const StatementMapping &) const = default;
};
inline const std::array<StatementMapping, 9> &period_statement_mapping() {
  static const std::array<StatementMapping, 9> mapping{
      {{"asset.cash", StatementClass::asset},
       {"asset.equity-securities", StatementClass::asset},
       {"asset.trade-receivable", StatementClass::asset},
       {"liability.trade-payable", StatementClass::liability},
       {"liability.short-equities", StatementClass::liability},
       {"equity.contributed-capital", StatementClass::contributed_capital},
       {"income.realized-pnl", StatementClass::realized_income},
       {"control.deferred-realized-pnl", StatementClass::deferred_control},
       {"control.settlement-suspense", StatementClass::settlement_control}}};
  return mapping;
}
struct StatementLine {
  JournalBalanceKey key;
  StatementClass section;
  Money opening, movement, closing; // same debit-positive values as trial balance
  bool operator==(const StatementLine &) const = default;
};
struct StatementTotals {
  Currency currency;
  Money assets, liabilities, contributed_capital, unclosed_earnings;
  // Credit-positive controls are disclosed separately; no statutory classification inferred.
  Money deferred_control, settlement_control, equation_difference;
  bool operator==(const StatementTotals &) const = default;
};
struct BoundedStatements {
  std::vector<StatementMapping> mapping;
  std::vector<StatementLine> comparative;
  std::vector<StatementTotals> opening_balance_sheet, closing_balance_sheet;
  std::vector<ActivityPnl> income_statement;
  // Null means unsupported, never a fabricated zero amount.
  std::optional<std::vector<Money>> unrealized, other_income, expenses, fx, cash_flow;
  bool operator==(const BoundedStatements &) const = default;
};
struct ClosedPeriodAnchor {
  Timestamp until; // exclusive, the retained report's closing cutoff is until - 1ns
  std::string report_id, input_digest, history_digest;
  std::uint64_t accepted_through, policy_through, settlement_through;
  bool operator==(const ClosedPeriodAnchor &) const = default;
};
struct PeriodInputEvidence {
  std::vector<LifecycleRecord> ledger;
  std::vector<KnownPolicyRevision> policy;
  std::vector<PolicyTarget> targets;
  std::vector<SettlementEvent> settlements;
  RecognitionRules rules;
  std::string ledger_digest, policy_digest, settlement_digest, input_digest;
  bool operator==(const PeriodInputEvidence &) const = default;
};
struct PeriodAccountingReport {
  PeriodContext context;
  PeriodInputEvidence inputs;
  AccountingReport opening, closing;   // endpoints use the period recognition contract
  std::vector<ActivityEffect> effects; // only [from, until); sorted by boundary then ID
  std::vector<PeriodBalance> balances;
  std::vector<ActivityPnl> pnl;
  std::vector<DailyActivityPnl> daily_pnl;
  BoundedStatements statements;
  std::vector<PeriodDiagnostic> diagnostics;
  bool supported_complete;
  std::string report_id, history_digest;
  bool operator==(const PeriodAccountingReport &) const = default;
};
inline ClosedPeriodAnchor retain_period_anchor(const PeriodAccountingReport &report) {
  return {report.context.until,
          report.report_id,
          report.inputs.input_digest,
          report.history_digest,
          report.context.accepted_through,
          report.context.policy_through,
          report.context.settlement_through};
}

namespace period_accounting_detail {
using namespace trade_accounting_detail;
using Lines = std::map<std::pair<std::string, Currency>, Money>;
inline void post(Lines &out, std::string account, Money value) {
  auto [it, inserted] = out.emplace(std::pair{std::move(account), value.currency()}, value);
  if (!inserted)
    it->second = add(it->second, value);
}
inline Lines signed_lines(const JournalEntry &entry) {
  Lines out;
  for (const auto &line : entry.lines())
    post(out, line.account_id().value(),
         line.side() == JournalSide::debit ? line.amount() : neg(line.amount()));
  return out;
}
inline std::string encode_lines(const Lines &lines) {
  std::string out;
  for (const auto &[key, amount] : lines)
    if (amount.scaled_value()) {
      frame(out, key.first);
      frame(out, key.second.code());
      frame(out, std::to_string(amount.scaled_value()));
    }
  return out;
}
inline std::string encode_cause(const ActivityCause &cause) {
  std::string out;
  frame(out, std::to_string(static_cast<int>(cause.kind)));
  frame(out, cause.id);
  frame(out, cause.economic_event);
  frame(out, std::to_string(cause.recorded_at.time_since_epoch().count()));
  for (auto at : {cause.effective_at, cause.void_effective_at})
    frame(out, at ? std::to_string(at->time_since_epoch().count()) : "absent");
  return out;
}
inline std::map<Currency, Money> economic(const AccountingReport &report) {
  std::map<Currency, Money> out;
  for (const auto &relief : report.lots.reliefs) {
    auto [it, inserted] =
        out.emplace(relief.net_pnl.currency(), Money::from_scaled(0, relief.net_pnl.currency()));
    it->second = add(it->second, relief.net_pnl);
  }
  return out;
}
inline Lines balances(const AccountingReport &report) {
  Lines out;
  for (const auto &row : report.trial_balance.balances())
    post(out, row.key.account.value(), row.net);
  return out;
}
inline Money get(const Lines &lines, std::string account, Currency currency) {
  auto it = lines.find({account, currency});
  return it == lines.end() ? Money::from_scaled(0, currency) : it->second;
}
inline Money get(const std::map<Currency, Money> &values, Currency currency) {
  auto it = values.find(currency);
  return it == values.end() ? Money::from_scaled(0, currency) : it->second;
}
inline std::vector<MovementKind> kinds(std::span<const ActivityCause> causes) {
  std::set<MovementKind> unique;
  for (const auto &cause : causes)
    unique.insert(cause.kind);
  return {unique.begin(), unique.end()};
}
inline PeriodPnlAmounts pnl_amounts(Money economic_value, Money recognized) {
  return {economic_value, recognized, sub(economic_value, recognized)};
}
} // namespace period_accounting_detail

// Bounded full replay reference implementation. Inputs must be complete pinned
// prefixes and complete custody pools. Only independent full pools/books may be
// partitioned. There is no I/O, persistence, live-policy selection or clock here.
inline std::expected<PeriodAccountingReport, PeriodError> project_period_accounting(
    const LifecycleLedger &ledger, std::span<const PolicyTarget> supplied_targets,
    std::span<const KnownPolicyRevision> policy, std::span<const SettlementEvent> settlements,
    PeriodContext context, RecognitionRules rules, std::optional<ClosedPeriodAnchor> closed = {},
    PeriodLimits limits = {}) {
  using namespace period_accounting_detail;
  auto fail = [](std::string code, std::string id = "", std::string expected = "",
                 std::string actual = "") {
    return std::unexpected(
        PeriodError{std::move(code), std::move(id), std::move(expected), std::move(actual)});
  };
  if (context.from == Timestamp::min() || context.until <= context.from ||
      supplied_targets.empty() || context.accepted_through != ledger.size() ||
      context.policy_through != policy.size() || context.settlement_through != settlements.size() ||
      !limits.max_boundaries || !limits.max_replay_work ||
      context.corrections != CorrectionRecognition::recorded_time)
    return fail("INVALID_PERIOD_CONTEXT");
  if (context.reporting_timezone != "UTC")
    return fail("UNSUPPORTED_REPORTING_TIMEZONE");
  if (context.mapping_version != period_chart_version)
    return fail("UNSUPPORTED_STATEMENT_MAPPING");
  if (closed &&
      (closed->until == Timestamp::min() || closed->until > context.from ||
       closed->report_id.empty() || closed->input_digest.empty() || closed->history_digest.empty()))
    return fail("CLOSED_PERIOD_RETAINED_REPORT_REQUIRED");
  if (rules.charges != ChargeTreatment::capitalize_opening ||
      rules.cash != CashRecognition::both_legs)
    return fail("INVALID_RECOGNITION_RULES");
  auto settlement_check = validate_settlement_history(ledger, settlements);
  if (!settlement_check)
    return fail(settlement_check.error().code, settlement_check.error().id);
  try {
    std::vector<PolicyTarget> targets(supplied_targets.begin(), supplied_targets.end());
    std::ranges::sort(targets, {}, &PolicyTarget::account);
    std::set<std::string> accounts;
    std::string target_encoding;
    for (const auto &target : targets) {
      if (target.book != targets.front().book || !accounts.insert(target.account).second)
        return fail("INVALID_PERIOD_TARGETS");
      for (const auto &field :
           {target.book, target.legal_entity, target.custodian_account, target.account})
        frame(target_encoding, field);
    }
    // Every account in the supplied ledger belongs to this complete book slice.
    // This prevents silently omitting another account sharing its custody lots.
    for (const auto &r : ledger.records())
      if (!accounts.contains(r.account().value()))
        return fail("PERIOD_ACCOUNT_BINDING_REQUIRED", r.account().value());
    std::string ledger_encoding = "luca.period-ledger/1", policy_encoding = "luca.period-policy/1",
                settlement_encoding = "luca.period-settlements/1";
    std::string prefix_ledger = digest(ledger_encoding), prefix_policy = digest(policy_encoding),
                prefix_settlement = digest(settlement_encoding);
    if (closed &&
        (closed->accepted_through > ledger.size() || closed->policy_through > policy.size() ||
         closed->settlement_through > settlements.size()))
      return fail("CLOSED_INPUT_PREFIX_UNAVAILABLE", closed->report_id);
    for (const auto &r : ledger.records()) {
      frame(ledger_encoding, serialization::canonical_digest(r));
      if (closed && r.acceptance_sequence().value() == closed->accepted_through)
        prefix_ledger = digest(ledger_encoding);
    }
    std::uint64_t sequence = 0;
    std::set<std::string> policy_ids;
    Timestamp last_policy_knowledge = Timestamp::min();
    std::vector<PolicyRevision> known_policy;
    for (const auto &p : policy) {
      if (p.revision.sequence != ++sequence || p.recorded_at < last_policy_knowledge ||
          !valid_policy_revision(p.revision) || !policy_ids.insert(p.revision.id).second)
        return fail("INVALID_PERIOD_POLICY_HISTORY", p.revision.id);
      last_policy_knowledge = p.recorded_at;
      // Frame every revision, including superseded overrides.
      const auto &r = p.revision;
      for (const auto &s : {r.id, r.legal_entity, r.subject, r.actor})
        frame(policy_encoding, s);
      frame(policy_encoding, std::to_string(static_cast<int>(r.scope)));
      frame(policy_encoding, std::to_string(r.sequence));
      frame(policy_encoding, std::to_string(r.effective_from.time_since_epoch().count()));
      frame(policy_encoding, std::to_string(p.recorded_at.time_since_epoch().count()));
      frame(policy_encoding,
            r.lot_selection ? std::to_string(static_cast<int>(*r.lot_selection)) : "inherit");
      frame(policy_encoding,
            r.recognition ? std::to_string(static_cast<int>(*r.recognition)) : "inherit");
      if (closed && r.sequence == closed->policy_through)
        prefix_policy = digest(policy_encoding);
      if (p.recorded_at <= context.recorded_through)
        known_policy.push_back(r);
    }
    for (const auto &e : settlements) {
      for (const auto &s : {e.id, e.account, e.trade_record, e.economic_event})
        frame(settlement_encoding, s);
      frame(settlement_encoding, std::to_string(e.sequence));
      frame(settlement_encoding, std::to_string(e.ledger_through));
      frame(settlement_encoding, std::to_string(e.effective_at.time_since_epoch().count()));
      frame(settlement_encoding, std::to_string(e.recorded_at.time_since_epoch().count()));
      frame(settlement_encoding, serialization::canonical_digest(e.cash));
      frame(settlement_encoding, e.reverses.value_or(""));
      for (const auto &source : e.sources)
        frame(settlement_encoding, source.value());
      if (closed && e.sequence == closed->settlement_through)
        prefix_settlement = digest(settlement_encoding);
    }
    const auto ledger_digest = digest(ledger_encoding), policy_digest = digest(policy_encoding),
               settlement_digest = digest(settlement_encoding);
    auto bind_inputs = [&](const std::string &ledger_hash, const std::string &policy_hash,
                           const std::string &settlement_hash) {
      std::string encoding = std::string(period_accounting_version);
      for (const auto &field : {ledger_hash, policy_hash, settlement_hash, target_encoding,
                                context.mapping_version, context.reporting_timezone})
        frame(encoding, field);
      frame(encoding, std::to_string(static_cast<int>(context.corrections)));
      frame(encoding, "capitalize-opening/both-legs/1");
      return digest(encoding);
    };
    const auto input_digest = bind_inputs(ledger_digest, policy_digest, settlement_digest);
    if (closed) {
      const auto prefix_digest = bind_inputs(prefix_ledger, prefix_policy, prefix_settlement);
      if (prefix_digest != closed->input_digest)
        return fail("CLOSED_INPUT_EVIDENCE_CHANGED", closed->report_id, closed->input_digest,
                    prefix_digest);
    }
    // Context-independent policy identity permits exact journal reduction; actual
    // resolved revisions remain on every effect and in the owned input evidence.
    auto identity = AccountingPolicyIdentity::create(
        AccountingPolicyId{"luca.period-accounting"},
        digest(std::string(period_chart_version) + ":recorded-corrections/1"));
    if (!identity)
      return fail("INVALID_PERIOD_POLICY_IDENTITY");
    std::map<Timestamp, std::vector<ActivityCause>> boundaries;
    auto add_boundary = [&](Timestamp at, ActivityCause cause) {
      if (at < context.until)
        boundaries[at].push_back(std::move(cause));
    };
    for (const auto &r : ledger.records()) {
      if (r.recorded_at() > context.recorded_through)
        continue;
      if (r.action() == LifecycleAction::reverse)
        return fail("PERIOD_REVERSAL_REQUIRES_CANCEL_OR_CORRECTION", r.record_id().value());
      const auto *parent = r.causal_record_id() ? ledger.find(*r.causal_record_id()) : nullptr;
      const bool replacement = r.action() == LifecycleAction::correct && parent &&
                               parent->action() == LifecycleAction::cancel &&
                               parent->cancellation_at();
      const auto kind =
          replacement                              ? MovementKind::replacement
          : r.action() == LifecycleAction::correct ? MovementKind::correction
          : r.action() == LifecycleAction::cancel
              ? (r.cancellation_at() ? MovementKind::dated_void : MovementKind::undated_void)
              : MovementKind::source_activity;
      const auto at = replacement
                          ? std::max(*parent->cancellation_at(), r.recorded_at())
                          : r.cancellation_at().value_or(r.action() == LifecycleAction::originate
                                                             ? *r.effective_at()
                                                             : r.recorded_at());
      ActivityCause cause{kind,
                          r.record_id().value(),
                          r.economic_event_id().value(),
                          r.recorded_at(),
                          r.effective_at(),
                          r.cancellation_at()};
      add_boundary(at, cause);
      // A correction may move its payload into the future. Its old contribution
      // disappears at recording and its replacement begins at its effective time.
      if (r.effective_at() && *r.effective_at() > at)
        add_boundary(*r.effective_at(), cause);
    }
    for (const auto &e : settlements)
      if (e.recorded_at <= context.recorded_through)
        add_boundary(e.effective_at,
                     {e.reverses ? MovementKind::refund : MovementKind::actual_settlement,
                      e.id,
                      e.economic_event,
                      e.recorded_at,
                      e.effective_at,
                      {}});
    // A policy revision remeasures the supported snapshot at effective_from.
    // Unknown revisions never enter historical knowledge queries.
    const auto first_financial =
        boundaries.empty() ? context.from - std::chrono::nanoseconds{1} : boundaries.begin()->first;
    for (const auto &p : policy)
      if (p.recorded_at <= context.recorded_through && p.revision.effective_from >= first_financial)
        add_boundary(p.revision.effective_from, {MovementKind::policy_change,
                                                 p.revision.id,
                                                 "",
                                                 p.recorded_at,
                                                 p.revision.effective_from,
                                                 {}});
    const auto before_from = context.from - std::chrono::nanoseconds{1};
    const auto before_until = context.until - std::chrono::nanoseconds{1};
    boundaries[before_from];
    boundaries[before_until];
    if (closed)
      boundaries[closed->until - std::chrono::nanoseconds{1}];
    const auto work_size = ledger.size() + settlements.size() + policy.size() + targets.size();
    if (boundaries.size() > limits.max_boundaries ||
        (work_size && boundaries.size() > limits.max_replay_work / work_size))
      return fail("PERIOD_REPLAY_LIMIT_EXCEEDED");
    auto snapshot = [&](Timestamp at) -> std::expected<AccountingReport, PeriodError> {
      std::vector<ResolvedPolicy> resolved;
      for (const auto &target : targets) {
        auto p = resolve_policy(target, known_policy, at, context.policy_through);
        if (!p)
          return fail(p.error().code, target.account);
        resolved.push_back(*p);
      }
      auto selection = ledger.resolve(context.recorded_through, at, at);
      auto report = trade_accounting_detail::project_selected(
          ledger, resolved, settlements,
          {context.recorded_through, at, context.accepted_through, context.settlement_through,
           context.mapping_version},
          rules, &selection);
      if (!report)
        return fail(report.error().code, report.error().record_id);
      return std::move(*report);
    };
    std::optional<AccountingReport> prior, opening, closing;
    std::vector<ActivityEffect> effects;
    std::vector<PeriodDiagnostic> diagnostics{{"UNREALIZED_VALUATION_UNSUPPORTED", "", false},
                                              {"OTHER_INCOME_UNSUPPORTED", "", false},
                                              {"EXPENSE_CLASSIFICATION_UNSUPPORTED", "", false},
                                              {"FX_TRANSLATION_UNSUPPORTED", "", false},
                                              {"CASH_FLOW_CLASSIFICATION_UNSUPPORTED", "", false},
                                              {"BOUNDED_CASH_EQUITY_STATEMENTS", "", false}};
    bool complete = true;
    std::string history = "luca.period-history/1";
    auto history_fingerprint = [&](const AccountingReport &endpoint) {
      auto encoding = history;
      frame(encoding, "closing-evidence/1");
      for (const auto &p : endpoint.lots.resolved_policies)
        frame(encoding, p.digest);
      frame(encoding, encode_lines(balances(endpoint)));
      for (const auto &[currency, amount] : economic(endpoint)) {
        frame(encoding, currency.code());
        frame(encoding, std::to_string(amount.scaled_value()));
      }
      return digest(encoding);
    };
    std::map<std::pair<std::chrono::sys_days, Currency>, DailyActivityPnl> daily;
    auto previous_economic = std::map<Currency, Money>{};
    for (auto &[at, causes] : boundaries) {
      std::ranges::sort(causes, [](const auto &a, const auto &b) {
        return std::tuple{a.kind, a.id} < std::tuple{b.kind, b.id};
      });
      auto next_result = snapshot(at);
      if (!next_result)
        return std::unexpected(next_result.error());
      auto next = std::move(*next_result);
      // Initial policy-only boundaries before all entity defaults exist are not
      // valid snapshots; callers must supply defaults before the first boundary.
      std::map<std::string, const JournalEntry *> before, after;
      if (prior)
        for (const auto &entry : prior->journals)
          before.emplace(entry.journal_entry_id().value(), &entry);
      for (const auto &entry : next.journals)
        after.emplace(entry.journal_entry_id().value(), &entry);
      std::set<std::string> entry_ids;
      for (const auto *index : {&before, &after})
        for (const auto &[id, entry] : *index)
          entry_ids.insert(id);
      std::map<Currency, Money> recognized_delta;
      std::map<Currency, std::vector<JournalEntryId>> boundary_entries;
      for (const auto &id : entry_ids) {
        const auto *old = before.contains(id) ? before.at(id) : nullptr;
        const auto *now = after.contains(id) ? after.at(id) : nullptr;
        Lines delta;
        if (old)
          for (const auto &[key, amount] : signed_lines(*old))
            post(delta, key.first, neg(amount));
        if (now)
          for (const auto &[key, amount] : signed_lines(*now))
            post(delta, key.first, amount);
        if (encode_lines(delta).empty())
          continue;
        std::set<EventId> records;
        std::set<EconomicEventId> economics;
        std::set<SourceRecordId> sources;
        auto known_at = Timestamp::min();
        for (const auto *entry : {old, now})
          if (entry) {
            records.insert(entry->lineage().record_ids().begin(),
                           entry->lineage().record_ids().end());
            economics.insert(entry->lineage().economic_event_ids().begin(),
                             entry->lineage().economic_event_ids().end());
            sources.insert(entry->lineage().source_record_ids().begin(),
                           entry->lineage().source_record_ids().end());
            known_at = std::max(known_at, entry->recorded_at());
          }
        std::vector<ActivityCause> relevant;
        std::set<std::string> relevant_policy_ids, relevant_policy_digests;
        std::set<std::string> participating_accounts;
        for (const auto &record_id : records)
          if (const auto *r = ledger.find(record_id))
            participating_accounts.insert(r->account().value());
        for (const auto *report : {prior ? &*prior : nullptr, &next})
          if (report)
            for (const auto &p : report->lots.resolved_policies)
              if (participating_accounts.contains(p.target.account)) {
                relevant_policy_digests.insert(p.digest);
                for (const auto &revision : p.revisions) {
                  relevant_policy_ids.insert(revision.id);
                  for (const auto &known : policy)
                    if (known.revision.id == revision.id)
                      known_at = std::max(known_at, known.recorded_at);
                }
              }
        for (const auto &cause : causes)
          if ((cause.economic_event.empty() && relevant_policy_ids.contains(cause.id)) ||
              economics.contains(EconomicEventId{cause.economic_event})) {
            relevant.push_back(cause);
            known_at = std::max(known_at, cause.recorded_at);
          }
        // A changed dependent journal may have neither the direct source ID nor
        // its economic ID in its old lineage; carry the boundary's full causes.
        if (relevant.empty())
          relevant = causes;
        for (const auto &cause : relevant) {
          known_at = std::max(known_at, cause.recorded_at);
          // Ledger, settlement and policy identities have independent namespaces.
          if (cause.kind == MovementKind::actual_settlement || cause.kind == MovementKind::refund ||
              cause.kind == MovementKind::policy_change)
            continue;
          for (const auto *r = ledger.find(EventId{cause.id}); r;
               r = r->causal_record_id() ? ledger.find(*r->causal_record_id()) : nullptr) {
            records.insert(r->record_id());
            sources.insert(r->provenance().source_records().begin(),
                           r->provenance().source_records().end());
            known_at = std::max(known_at, r->recorded_at());
          }
        }
        auto lineage = JournalLineage::create({records.begin(), records.end()},
                                              {economics.begin(), economics.end()},
                                              {sources.begin(), sources.end()});
        if (!lineage)
          return fail("PERIOD_LINEAGE_INVARIANT", id);
        std::string identity_encoding = std::string(period_accounting_version);
        frame(identity_encoding, targets.front().book);
        frame(identity_encoding, context.mapping_version);
        frame(identity_encoding, std::to_string(at.time_since_epoch().count()));
        frame(identity_encoding, id);
        frame(identity_encoding, old ? encode_lines(signed_lines(*old)) : "absent");
        frame(identity_encoding, now ? encode_lines(signed_lines(*now)) : "absent");
        for (const auto &cause : relevant)
          frame(identity_encoding, encode_cause(cause));
        for (const auto &policy_digest : relevant_policy_digests)
          frame(identity_encoding, policy_digest);
        const auto effect_id = "activity:" + digest(identity_encoding);
        std::vector<JournalLine> lines;
        for (const auto &[key, amount] : delta)
          if (amount.scaled_value()) {
            auto line = JournalLine::create(
                JournalLineId{effect_id + ":" + key.first}, JournalEntryId{effect_id},
                AccountId{key.first},
                amount.scaled_value() > 0 ? JournalSide::debit : JournalSide::credit,
                amount.scaled_value() > 0 ? amount : neg(amount), *lineage);
            if (!line)
              return fail("PERIOD_LINE_INVARIANT", id);
            lines.push_back(*line);
            if (key.first == "income.realized-pnl") {
              auto [it, inserted] =
                  recognized_delta.emplace(key.second, Money::from_scaled(0, key.second));
              it->second = sub(it->second, amount);
            }
          }
        const auto &model = now ? *now : *old;
        auto day = JournalDate::create(
            std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(at)});
        if (!day)
          return fail("PERIOD_DATE_INVARIANT", id);
        auto entry =
            JournalEntry::create(JournalEntryId{effect_id}, *identity, model.event_type(),
                                 model.active_record_id(), at, known_at, *day,
                                 model.event_type() == JournalEventType::cash_movement
                                     ? RecognitionPhase::immediate
                                     : RecognitionPhase::period_adjustment,
                                 0, model.settlement_context(), *lineage, std::move(lines));
        if (!entry)
          return fail("PERIOD_ENTRY_INVARIANT", id);
        boundary_entries[entry->lines().front().amount().currency()].push_back(
            entry->journal_entry_id());
        frame(history, effect_id);
        frame(history, encode_lines(delta));
        if (at >= context.from) {
          std::set<std::string> cash_ids;
          for (const auto *report : {prior ? &*prior : nullptr, &next})
            if (report)
              for (const auto &trace : report->trace)
                if (trace.entry_id == id)
                  cash_ids.insert(trace.settlement_ids.begin(), trace.settlement_ids.end());
          auto before_policies =
              prior ? prior->lots.resolved_policies : std::vector<ResolvedPolicy>{};
          // Evidence describes the strict-before boundary, independent of any
          // query endpoint sentinel inserted between financial boundaries.
          for (auto &p : before_policies)
            p.effective_at = at - std::chrono::nanoseconds{1};
          effects.push_back({std::move(*entry),
                             old ? std::optional{*old} : std::nullopt,
                             now ? std::optional{*now} : std::nullopt,
                             relevant,
                             std::move(before_policies),
                             next.lots.resolved_policies,
                             {cash_ids.begin(), cash_ids.end()}});
        }
      }
      auto next_economic = economic(next);
      std::set<Currency> currencies;
      for (const auto *values : {&previous_economic, &next_economic, &recognized_delta})
        for (const auto &[currency, amount] : *values)
          currencies.insert(currency);
      for (const auto &line : next.trial_balance.balances())
        currencies.insert(line.key.currency);
      for (auto currency : currencies) {
        const auto economic_delta =
            sub(get(next_economic, currency), get(previous_economic, currency));
        if (economic_delta.scaled_value()) {
          frame(history, currency.code());
          frame(history, std::to_string(economic_delta.scaled_value()));
        }
        if (at >= context.from && !causes.empty()) {
          const auto day = std::chrono::floor<std::chrono::days>(at);
          auto zero = Money::from_scaled(0, currency);
          auto [it, inserted] = daily.emplace(
              std::pair{day, currency},
              DailyActivityPnl{std::chrono::year_month_day{day}, currency, {zero, zero, zero}, {}});
          it->second.movement.economic = add(it->second.movement.economic, economic_delta);
          it->second.movement.recognized =
              add(it->second.movement.recognized, get(recognized_delta, currency));
          it->second.movement.deferred =
              sub(it->second.movement.economic, it->second.movement.recognized);
          for (const auto &entry_id : boundary_entries[currency])
            it->second.entries.push_back(entry_id);
        }
      }
      if (!causes.empty()) {
        frame(history, std::to_string(at.time_since_epoch().count()));
        for (const auto &cause : causes)
          frame(history, encode_cause(cause));
        // Closed history binds resolved evidence even when new backdated policy
        // has identical financial settings and therefore no adjustment amount.
        for (const auto &p : next.lots.resolved_policies)
          frame(history, p.digest);
      }
      if (closed && at == closed->until - std::chrono::nanoseconds{1} &&
          history_fingerprint(next) != closed->history_digest)
        return fail("CLOSED_HISTORY_EXCEPTION", closed->report_id, closed->history_digest,
                    history_fingerprint(next));
      if (at >= before_from && (!next.lots.charges_complete || !next.breaks.empty())) {
        complete = false;
        if (!next.lots.charges_complete)
          diagnostics.push_back({"CHARGES_PROVISIONAL", "", true});
        for (const auto &b : next.breaks)
          diagnostics.push_back({b.code, b.record_id, true});
      }
      if (at == before_from)
        opening = next;
      if (at == before_until)
        closing = next;
      previous_economic = next_economic;
      prior = std::move(next);
    }
    if (!opening || !closing)
      return fail("PERIOD_ENDPOINT_INVARIANT");
    std::ranges::sort(effects, [](const auto &a, const auto &b) {
      return std::tuple{a.journal.effective_at(), a.journal.journal_entry_id()} <
             std::tuple{b.journal.effective_at(), b.journal.journal_entry_id()};
    });
    const auto a = balances(*opening), b = balances(*closing);
    std::set<std::pair<std::string, Currency>> keys;
    for (const auto *values : {&a, &b})
      for (const auto &[key, amount] : *values)
        keys.insert(key);
    for (const auto &effect : effects)
      for (const auto &line : effect.journal.lines())
        keys.insert({line.account_id().value(), line.amount().currency()});
    std::vector<PeriodBalance> rows;
    for (const auto &key : keys) {
      auto zero = Money::from_scaled(0, key.second);
      std::map<std::vector<MovementKind>, PeriodMovement> grouped;
      auto movement = zero;
      for (const auto &effect : effects) {
        const auto lines = signed_lines(effect.journal);
        auto it = lines.find(key);
        if (it == lines.end() || !it->second.scaled_value())
          continue;
        const auto classification = kinds(effect.causes);
        auto [group, inserted] =
            grouped.emplace(classification, PeriodMovement{classification, zero, {}});
        group->second.net = add(group->second.net, it->second);
        group->second.entries.push_back(effect.journal.journal_entry_id());
        movement = add(movement, it->second);
      }
      const auto av = get(a, key.first, key.second), bv = get(b, key.first, key.second);
      if (add(av, movement) != bv)
        return fail("PERIOD_ROLLFORWARD_INVARIANT", key.first);
      std::vector<PeriodMovement> classified;
      for (auto &[kind, group] : grouped)
        classified.push_back(std::move(group));
      rows.push_back({{BookId{targets.front().book}, ChartAccountId{key.first}, key.second},
                      av,
                      movement,
                      bv,
                      std::move(classified)});
    }
    const auto ae = economic(*opening), be = economic(*closing);
    std::set<Currency> currencies;
    for (const auto &row : rows)
      currencies.insert(row.key.currency);
    std::vector<ActivityPnl> pnl;
    for (auto currency : currencies) {
      const auto ap = pnl_amounts(get(ae, currency), neg(get(a, "income.realized-pnl", currency)));
      const auto bp = pnl_amounts(get(be, currency), neg(get(b, "income.realized-pnl", currency)));
      pnl.push_back({currency,
                     ap,
                     {sub(bp.economic, ap.economic), sub(bp.recognized, ap.recognized),
                      sub(bp.deferred, ap.deferred)},
                     bp});
    }
    std::vector<DailyActivityPnl> daily_rows;
    for (auto &[key, row] : daily) {
      std::ranges::sort(row.entries);
      daily_rows.push_back(std::move(row));
    }
    BoundedStatements statements;
    statements.mapping.assign(period_statement_mapping().begin(), period_statement_mapping().end());
    for (const auto &row : rows) {
      auto mapping = std::ranges::find(statements.mapping, row.key.account.value(),
                                       &StatementMapping::account);
      if (mapping == statements.mapping.end())
        return fail("STATEMENT_ACCOUNT_UNMAPPED", row.key.account.value());
      statements.comparative.push_back(
          {row.key, mapping->section, row.opening, row.movement, row.closing});
    }
    auto totals = [&](bool closing_endpoint) {
      std::vector<StatementTotals> out;
      for (auto currency : currencies) {
        auto zero = Money::from_scaled(0, currency);
        StatementTotals t{currency, zero, zero, zero, zero, zero, zero, zero};
        for (const auto &row : statements.comparative)
          if (row.key.currency == currency) {
            auto amount = closing_endpoint ? row.closing : row.opening;
            auto *value = row.section == StatementClass::asset       ? &t.assets
                          : row.section == StatementClass::liability ? &t.liabilities
                          : row.section == StatementClass::contributed_capital
                              ? &t.contributed_capital
                          : row.section == StatementClass::realized_income  ? &t.unclosed_earnings
                          : row.section == StatementClass::deferred_control ? &t.deferred_control
                                                                            : &t.settlement_control;
            *value = add(*value, row.section == StatementClass::asset ? amount : neg(amount));
          }
        t.equation_difference = sub(
            sub(sub(sub(sub(t.assets, t.liabilities), t.contributed_capital), t.unclosed_earnings),
                t.deferred_control),
            t.settlement_control);
        if (t.deferred_control.scaled_value() || t.settlement_control.scaled_value()) {
          diagnostics.push_back(
              {"UNCLASSIFIED_STATEMENT_CONTROLS", std::string(currency.code()), true});
          complete = false;
        }
        out.push_back(t);
      }
      return out;
    };
    statements.opening_balance_sheet = totals(false);
    statements.closing_balance_sheet = totals(true);
    for (const auto &t : statements.opening_balance_sheet)
      if (t.equation_difference.scaled_value())
        return fail("STATEMENT_BALANCE_INVARIANT");
    for (const auto &t : statements.closing_balance_sheet)
      if (t.equation_difference.scaled_value())
        return fail("STATEMENT_BALANCE_INVARIANT");
    statements.income_statement = pnl;
    std::ranges::sort(diagnostics, [](const auto &a, const auto &b) {
      return std::tuple{a.code, a.id, a.affects_supported_coverage} <
             std::tuple{b.code, b.id, b.affects_supported_coverage};
    });
    diagnostics.erase(std::unique(diagnostics.begin(), diagnostics.end()), diagnostics.end());
    const auto history_digest = history_fingerprint(*closing);
    std::string result_encoding = input_digest;
    frame(result_encoding, std::to_string(context.from.time_since_epoch().count()));
    frame(result_encoding, std::to_string(context.until.time_since_epoch().count()));
    frame(result_encoding, std::to_string(context.recorded_through.time_since_epoch().count()));
    frame(result_encoding, history_digest);
    return PeriodAccountingReport{context,
                                  {{ledger.records().begin(), ledger.records().end()},
                                   {policy.begin(), policy.end()},
                                   targets,
                                   {settlements.begin(), settlements.end()},
                                   rules,
                                   ledger_digest,
                                   policy_digest,
                                   settlement_digest,
                                   input_digest},
                                  std::move(*opening),
                                  std::move(*closing),
                                  std::move(effects),
                                  std::move(rows),
                                  std::move(pnl),
                                  std::move(daily_rows),
                                  std::move(statements),
                                  std::move(diagnostics),
                                  complete,
                                  "period:" + digest(result_encoding),
                                  history_digest};
  } catch (const ArithmeticFailure &) {
    return fail("PERIOD_AMOUNT_OVERFLOW");
  }
}
} // namespace luca::accounting
