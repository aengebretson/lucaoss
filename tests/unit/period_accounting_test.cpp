#include "luca/accounting/period_pnl_rows.hpp"
#include "luca/serialization/canonical_decode.hpp"
#include <fstream>
#include <future>
#include <iostream>
#include <source_location>
#include <sstream>
using namespace luca;
using namespace luca::accounting;
void check(bool v, std::source_location at = std::source_location::current()) {
  if (!v) {
    std::cerr << "check failed at " << at.line() << '\n';
    std::abort();
  }
}
template <class T> auto take(T value, std::source_location at = std::source_location::current()) {
  if (!value) {
    if constexpr (requires { value.error().code; })
      std::cerr << value.error().code << '\n';
    check(false, at);
  }
  return *value;
}
const auto usd = take(Currency::from_code("USD"));
Money m(std::string_view s) { return take(Money::parse(s, usd)); }
Timestamp day(int n) {
  return Timestamp{std::chrono::sys_days{std::chrono::year{2026} / 1 / 1}} + std::chrono::days{n};
}
Provenance proof(std::string id) {
  return take(Provenance::create({SourceRecordId{"source:" + id}}, "hand-calculated", "1"));
}
EquityTrade trade(std::string id, std::string q, std::string price, std::string commission,
                  std::string fee, int n, std::string account = "a") {
  auto h = take(EventHeader::create(EventId{id}, AccountId{account}, day(n), proof(id)));
  return take(EquityTrade::create(
      h, InstrumentId{"XYZ"}, take(Quantity::parse(q)), take(Price::parse(price)), usd,
      take(SettlementDate::create(std::chrono::year{2026} / 1 / 31)),
      TradeCharges{{"c", "commission", "broker", "", "reported", m(commission)},
                   {"f", "exchange", "exchange", "", "reported", m(fee)}}));
}
struct Book {
  LifecycleLedger ledger;
  std::vector<SettlementEvent> settlements;
  std::vector<PolicyTarget> targets{{"book", "entity", "custody", "a"}};
  std::vector<KnownPolicyRevision> policies;
  explicit Book(RecognitionBasis basis = RecognitionBasis::accrual) {
    policies.push_back({{"p1", "entity", "entity", "operator", PolicyScope::legal_entity, day(-10),
                         1, LotSelection::lifo, basis},
                        day(-10)});
  }
  void contribution(std::string id = "capital", std::string account = "a", int at = 0) {
    auto h = take(EventHeader::create(EventId{id}, AccountId{account}, day(at), proof(id)));
    take(ledger.accept(LifecycleRecordDraft::originate(
        EconomicEventId{id}, day(at), EconomicEvent{CashMovement::create(h, m("1000"))})));
  }
  void add(EquityTrade t, int recorded = -1) {
    take(ledger.accept(LifecycleRecordDraft::originate(
        EconomicEventId{t.header().id().value()},
        recorded < 0 ? t.header().effective_at() : day(recorded), EconomicEvent{t})));
  }
  void settle(std::string id, std::string record, std::string amount, int at,
              std::optional<std::string> reverses = {}, int recorded = -1) {
    const auto *r = ledger.find(EventId{record});
    check(r);
    SettlementEvent e{id,
                      r->account().value(),
                      record,
                      r->economic_event_id().value(),
                      day(at),
                      day(recorded < 0 ? at : recorded),
                      settlements.size() + 1,
                      ledger.size(),
                      m(amount),
                      {SourceRecordId{"bank:" + id}},
                      reverses};
    auto v = validate_settlement(ledger, settlements, e);
    if (!v)
      std::cerr << v.error().code << ' ' << id << '\n';
    check(bool(v));
    settlements.push_back(e);
  }
  PeriodContext context(int from = 0, int until = 20, int known = 30) const {
    return {day(from),
            day(until),
            day(known),
            ledger.size(),
            policies.size(),
            settlements.size(),
            "UTC",
            std::string(period_chart_version),
            CorrectionRecognition::recorded_time};
  }
  PeriodAccountingReport report(int from = 0, int until = 20, int known = 30,
                                std::optional<ClosedPeriodAnchor> closed = {}) const {
    return take(project_period_accounting(
        ledger, targets, policies, settlements, context(from, until, known),
        {ChargeTreatment::capitalize_opening, CashRecognition::both_legs}, closed));
  }
};
Money value(const PeriodAccountingReport &r, std::string account, bool closing = true) {
  for (const auto &b : r.balances)
    if (b.key.account.value() == account)
      return closing ? b.closing : b.opening;
  return m("0");
}
void verify(const PeriodAccountingReport &r) {
  for (const auto &b : r.balances)
    check(take(b.opening.add(b.movement)) == b.closing);
  for (const auto &e : r.effects)
    check(e.journal.debit_total() == e.journal.credit_total());
  for (const auto *endpoint : {&r.opening, &r.closing})
    for (const auto &e : endpoint->journals)
      check(e.debit_total() == e.credit_total());
  for (const auto &t : r.statements.closing_balance_sheet)
    check(t.equation_difference.scaled_value() == 0);
  check(!r.statements.unrealized && !r.statements.other_income && !r.statements.expenses &&
        !r.statements.fx && !r.statements.cash_flow);
  for (const auto &p : r.pnl) {
    check(take(p.opening.economic.add(p.movement.economic)) == p.closing.economic);
    check(take(p.opening.recognized.add(p.movement.recognized)) == p.closing.recognized);
    auto economic = Money::from_scaled(0, p.currency),
         recognized = Money::from_scaled(0, p.currency);
    for (const auto &d : r.daily_pnl)
      if (d.currency == p.currency) {
        economic = take(economic.add(d.movement.economic));
        recognized = take(recognized.add(d.movement.recognized));
      }
    check(economic == p.movement.economic);
    check(recognized == p.movement.recognized);
  }
}
Book charged(RecognitionBasis basis) {
  Book b(basis);
  b.contribution();
  b.add(trade("b1", "10", "10", "1", "1", 1));
  b.add(trade("b2", "5", "12", "0.5", "0.5", 2));
  b.add(trade("s1", "-8", "15", "1", "1", 3));
  b.add(trade("s2", "-10", "14", "1", "1", 4));
  b.add(trade("c1", "2", "11", "0.2", "0.2", 5));
  b.add(trade("c2", "2", "9", "0.2", "0.2", 6));
  b.settle("b1-half", "b1", "-51", 2, {}, 6);
  b.settle("b2-full", "b2", "-61", 3, {}, 6);
  b.settle("s1-full", "s1", "118", 5, {}, 6);
  b.settle("s2-full", "s2", "138", 6, {}, 6);
  b.settle("b1-rest", "b1", "-51", 7);
  b.settle("c1-full", "c1", "-22.4", 7);
  b.settle("c2-full", "c2", "-18.4", 8);
  return b;
}
int main() {
  auto b = charged(RecognitionBasis::accrual);
  std::ifstream queries(LUCA_PERIOD_QUERIES);
  check(queries.good());
  std::string query;
  while (std::getline(queries, query)) {
    if (query.empty() || query.front() == '#')
      continue;
    std::istringstream in(query);
    std::string basis, economic, recognized, cash, long_carrying, short_carrying;
    int from, until, known;
    check(bool(in >> basis >> from >> until >> known >> economic >> recognized >> cash >>
               long_carrying >> short_carrying));
    auto book = charged(basis == "cash" ? RecognitionBasis::cash : RecognitionBasis::accrual);
    auto result = book.report(from, until, known);
    verify(result);
    check(result.pnl[0].movement.economic == m(economic));
    check(result.pnl[0].movement.recognized == m(recognized));
    check(value(result, "asset.cash") == m(cash));
    check(value(result, "asset.equity-securities") == m(long_carrying));
    check(value(result, "liability.short-equities") == take(m("0").subtract(m(short_carrying))));
  }
  auto full = b.report();
  verify(full);
  check(full == b.report());
  check(full.supported_complete);
  check(value(full, "asset.cash") == m("1052.2"));
  check(value(full, "asset.equity-securities") == m("9.2"));
  check(value(full, "income.realized-pnl") == m("-61.4"));
  check(full.pnl[0].movement.economic == m("61.4"));
  auto selected = take(select_period_pnl(full));
  check(selected.rows.size() == 1);
  check(selected.totals == full.pnl);
  check(full.statements.closing_balance_sheet[0].unclosed_earnings == m("61.4"));
  check(full.statements.closing_balance_sheet[0].contributed_capital == m("1000"));
  auto a = b.report(0, 5), c = b.report(5, 20);
  verify(a);
  verify(c);
  for (const auto &row : full.balances) {
    auto movement = [&](const auto &r) {
      for (const auto &v : r.balances)
        if (v.key == row.key)
          return v.movement;
      return m("0");
    };
    check(take(movement(a).add(movement(c))) == row.movement);
  }
  std::vector<std::string> combined, all;
  for (const auto *r : {&a, &c})
    for (const auto &e : r->effects)
      combined.push_back(e.journal.journal_entry_id().value());
  for (const auto &e : full.effects)
    all.push_back(e.journal.journal_entry_id().value());
  check(combined == all);
  auto composed_effects = a.effects;
  composed_effects.insert(composed_effects.end(), c.effects.begin(), c.effects.end());
  check(composed_effects == full.effects);
  // Half-open boundaries include exactly from, exclude exactly until.
  auto one = b.report(3, 4);
  verify(one);
  check(one.pnl[0].movement.economic == m("26.4"));
  auto cash = charged(RecognitionBasis::cash);
  auto partial = cash.report(0, 7);
  verify(partial);
  check(partial.pnl[0].closing.recognized == m("32.175"));
  check(partial.pnl[0].closing.economic == m("61.4"));
  check(partial.pnl[0].closing.deferred == m("29.225"));
  check(!partial.supported_complete);
  auto settled = cash.report(7, 9);
  verify(settled);
  check(settled.pnl[0].movement.economic == m("0"));
  check(settled.pnl[0].movement.recognized == m("29.225"));
  cash.settle("refund", "b1", "51", 9, "b1-half");
  auto refund = cash.report(9, 10);
  verify(refund);
  check(refund.pnl[0].movement.recognized == m("-19.425"));
  check(value(refund, "asset.cash") == m("1103.2"));
  cash.settle("repay", "b1", "-51", 10);
  check(cash.report(10, 11).pnl[0].movement.recognized == m("19.425"));
  auto retained = cash.report(0, 11, 10);
  auto anchor = retain_period_anchor(retained);
  take(cash.ledger.accept(
      LifecycleRecordDraft::correct(EconomicEventId{"b1"}, EventId{"b1"}, day(11),
                                    EconomicEvent{trade("b1-v2", "10", "11", "1", "1", 1)})));
  auto revised = cash.report(11, 12, 30, anchor);
  verify(revised);
  check(revised.pnl[0].movement.economic == m("-10"));
  check(revised.pnl[0].closing.economic == m("51.4"));
  check(revised.pnl[0].closing.recognized == m("48.824107"));
  check(revised.pnl[0].closing.deferred == m("2.575893"));
  check(value(revised, "asset.cash") == m("1052.2"));
  check(cash.report(0, 11).history_digest == retained.history_digest);
  // Current knowledge of a recorded-date correction preserves prior recognized
  // periods. Historical knowledge still excludes the correction's version.
  auto old_knowledge = cash.report(11, 12, 10);
  verify(old_knowledge);
  check(old_knowledge.effects.empty());
  check(old_knowledge.pnl[0].closing.economic == m("61.4"));
  bool lineage = false, dependent = false;
  for (const auto &e : revised.effects) {
    for (const auto &r : e.journal.lineage().record_ids())
      lineage = lineage || r.value() == "b1-v2";
    dependent = dependent || (e.before && e.before->active_record_id().value() == "s2");
  }
  check(lineage && dependent);
  take(cash.ledger.accept(LifecycleRecordDraft::cancel(EventId{"void-c2"}, EconomicEventId{"c2"},
                                                       EventId{"c2"}, AccountId{"a"}, day(12),
                                                       proof("void-c2"), day(13))));
  auto before_void = cash.report(12, 13), after_void = cash.report(13, 14);
  verify(before_void);
  verify(after_void);
  check(before_void.effects.empty());
  check(after_void.pnl[0].movement.economic == m("-4.6"));
  check(value(after_void, "asset.cash") == m("1052.2"));
  check(value(after_void, "control.settlement-suspense") == m("18.4"));
  check(cash.report(13, 14, 11).pnl[0].movement.economic == m("0"));
  cash.settle("void-refund", "c2", "18.4", 14, "c2-full");
  auto void_refund = cash.report(14, 15);
  verify(void_refund);
  check(value(void_refund, "control.settlement-suspense") == m("0"));
  check(value(void_refund, "asset.cash") == m("1070.6"));
  // A late source affecting retained closed history is an exception. The caller
  // retains this error and its inputs; the original report remains byte-identical.
  auto closed_book = charged(RecognitionBasis::accrual);
  auto historical = closed_book.report(0, 10, 10);
  auto retained_copy = historical;
  closed_book.add(trade("late", "1", "1", "0", "0", 2), 12);
  auto exception =
      project_period_accounting(closed_book.ledger, closed_book.targets, closed_book.policies,
                                closed_book.settlements, closed_book.context(10, 20),
                                {ChargeTreatment::capitalize_opening, CashRecognition::both_legs},
                                retain_period_anchor(historical));
  check(!exception && exception.error().code == "CLOSED_HISTORY_EXCEPTION");
  check(exception.error().expected_digest != exception.error().actual_digest);
  check(exception.error().actual_digest == closed_book.report(0, 10).history_digest);
  check(historical == retained_copy);
  auto invalid_context = b.context();
  invalid_context.reporting_timezone = "America/Chicago";
  auto unsupported =
      project_period_accounting(b.ledger, b.targets, b.policies, b.settlements, invalid_context,
                                {ChargeTreatment::capitalize_opening, CashRecognition::both_legs});
  check(!unsupported && unsupported.error().code == "UNSUPPORTED_REPORTING_TIMEZONE");
  Book missing(RecognitionBasis::unspecified);
  missing.contribution();
  auto missing_policy = project_period_accounting(
      missing.ledger, missing.targets, missing.policies, missing.settlements, missing.context(),
      {ChargeTreatment::capitalize_opening, CashRecognition::both_legs});
  check(!missing_policy && missing_policy.error().code == "RECOGNITION_POLICY_REQUIRED");
  auto limit = project_period_accounting(
      b.ledger, b.targets, b.policies, b.settlements, b.context(),
      {ChargeTreatment::capitalize_opening, CashRecognition::both_legs}, {}, {1, 1});
  check(!limit && limit.error().code == "PERIOD_REPLAY_LIMIT_EXCEEDED");
  // Backdated void and its linked replacement have distinct recognition gates:
  // explicit void day9, replacement recorded day11. Neither changes bank cash.
  auto replacement = charged(RecognitionBasis::cash);
  auto replacement_old = replacement.report(0, 10, 10);
  std::array edits{
      LifecycleRecordDraft::cancel(EventId{"replace-void"}, EconomicEventId{"c2"}, EventId{"c2"},
                                   AccountId{"a"}, day(11), proof("replace-void"), day(9)),
      LifecycleRecordDraft::correct(EconomicEventId{"c2"}, EventId{"replace-void"}, day(11),
                                    EconomicEvent{trade("c2-new", "2", "8", "0.2", "0.2", 6)})};
  take(replacement.ledger.accept_batch(edits));
  auto exact_context = replacement.context(9, 10, 11);
  exact_context.until = day(9) + std::chrono::nanoseconds{1};
  auto exact_void = take(project_period_accounting(
      replacement.ledger, replacement.targets, replacement.policies, replacement.settlements,
      exact_context, {ChargeTreatment::capitalize_opening, CashRecognition::both_legs}));
  check(exact_void.pnl[0].movement.economic == m("-4.6"));
  exact_context.recorded_through = day(11) - std::chrono::nanoseconds{1};
  auto before_recording = take(project_period_accounting(
      replacement.ledger, replacement.targets, replacement.policies, replacement.settlements,
      exact_context, {ChargeTreatment::capitalize_opening, CashRecognition::both_legs}));
  check(before_recording.effects.empty());
  auto replacement_void = replacement.report(9, 10), replacement_new = replacement.report(11, 12);
  verify(replacement_void);
  verify(replacement_new);
  check(replacement_void.pnl[0].movement.economic == m("-4.6"));
  check(value(replacement_void, "control.settlement-suspense") == m("18.4"));
  check(replacement_new.pnl[0].movement.economic == m("5.6"));
  check(replacement_new.pnl[0].closing.economic == m("62.4"));
  check(value(replacement_new, "asset.equity-securities") == m("8.2"));
  check(value(replacement_new, "asset.cash") == m("1052.2"));
  check(value(replacement_new, "control.settlement-suspense") == m("0"));
  check(replacement.report(9, 10, 10).pnl[0].movement.economic == m("0"));
  auto blocked_replacement =
      project_period_accounting(replacement.ledger, replacement.targets, replacement.policies,
                                replacement.settlements, replacement.context(10, 20),
                                {ChargeTreatment::capitalize_opening, CashRecognition::both_legs},
                                retain_period_anchor(replacement_old));
  check(!blocked_replacement && blocked_replacement.error().code == "CLOSED_HISTORY_EXCEPTION");
  auto replacement_restored = take(
      serialization::decode_lifecycle_ledger(serialization::canonical_bytes(replacement.ledger)));
  check(take(project_period_accounting(
            replacement_restored, replacement.targets, replacement.policies,
            replacement.settlements, replacement.context(),
            {ChargeTreatment::capitalize_opening, CashRecognition::both_legs})) ==
        replacement.report());
  auto future_replacement = charged(RecognitionBasis::accrual);
  std::array future_edits{
      LifecycleRecordDraft::cancel(EventId{"future-void"}, EconomicEventId{"c2"}, EventId{"c2"},
                                   AccountId{"a"}, day(9), proof("future-void"), day(11)),
      LifecycleRecordDraft::correct(EconomicEventId{"c2"}, EventId{"future-void"}, day(9),
                                    EconomicEvent{trade("future-new", "2", "8", "0.2", "0.2", 6)})};
  take(future_replacement.ledger.accept_batch(future_edits));
  check(future_replacement.report(9, 10).effects.empty());
  check(future_replacement.report(11, 12).pnl[0].movement.economic == m("1"));
  // A dated void of a not-yet-recognized corrected version removes the
  // economic chain on its explicit date; the later correction cannot resurrect it.
  auto ancestor_void = charged(RecognitionBasis::accrual);
  take(ancestor_void.ledger.accept(LifecycleRecordDraft::correct(
      EconomicEventId{"c2"}, EventId{"c2"}, day(11),
      EconomicEvent{trade("ancestor-v2", "2", "8", "0.2", "0.2", 6)})));
  take(ancestor_void.ledger.accept(LifecycleRecordDraft::cancel(
      EventId{"ancestor-void"}, EconomicEventId{"c2"}, EventId{"ancestor-v2"}, AccountId{"a"},
      day(12), proof("ancestor-void"), day(9))));
  auto ancestor_result = ancestor_void.report(9, 10);
  verify(ancestor_result);
  check(ancestor_result.pnl[0].movement.economic == m("-4.6"));
  check(ancestor_void.report(11, 12).effects.empty());
  for (const auto &e : ancestor_result.effects) {
    bool void_source = false, target_version = false, void_cause = false;
    for (const auto &r : e.journal.lineage().record_ids()) {
      void_source |= r.value() == "ancestor-void";
      target_version |= r.value() == "ancestor-v2";
    }
    for (const auto &cause : e.causes)
      void_cause |= cause.id == "ancestor-void" && cause.kind == MovementKind::dated_void;
    check(void_source && target_version && void_cause);
  }
  // An unchanged retained prefix is required even if amounts still match.
  auto tampered = charged(RecognitionBasis::accrual);
  auto protected_report = tampered.report(0, 10, 10);
  tampered.policies[0].revision.actor = "changed-evidence";
  auto immutable = project_period_accounting(
      tampered.ledger, tampered.targets, tampered.policies, tampered.settlements,
      tampered.context(10, 20), {ChargeTreatment::capitalize_opening, CashRecognition::both_legs},
      retain_period_anchor(protected_report));
  check(!immutable && immutable.error().code == "CLOSED_INPUT_EVIDENCE_CHANGED");
  auto extended_policy = charged(RecognitionBasis::accrual);
  auto policy_closed = extended_policy.report(0, 10, 10);
  extended_policy.policies.push_back(
      {{"late-same-policy", "entity", "entity", "later-actor", PolicyScope::legal_entity, day(-5),
        2, LotSelection::lifo, RecognitionBasis::accrual},
       day(11)});
  auto policy_exception = project_period_accounting(
      extended_policy.ledger, extended_policy.targets, extended_policy.policies,
      extended_policy.settlements, extended_policy.context(10, 20),
      {ChargeTreatment::capitalize_opening, CashRecognition::both_legs},
      retain_period_anchor(policy_closed));
  check(!policy_exception && policy_exception.error().code == "CLOSED_HISTORY_EXCEPTION");
  Book empty_book;
  auto empty_closed = empty_book.report(0, 10, 10);
  empty_book.policies.push_back(
      {{"empty-late-policy", "entity", "entity", "later-actor", PolicyScope::legal_entity, day(-5),
        2, LotSelection::lifo, RecognitionBasis::accrual},
       day(11)});
  auto empty_policy_exception = project_period_accounting(
      empty_book.ledger, empty_book.targets, empty_book.policies, empty_book.settlements,
      empty_book.context(10, 20), {ChargeTreatment::capitalize_opening, CashRecognition::both_legs},
      retain_period_anchor(empty_closed));
  check(!empty_policy_exception &&
        empty_policy_exception.error().code == "CLOSED_HISTORY_EXCEPTION");
  // Policy knowledge is part of effect knowledge, even when effective before
  // the first source boundary. Equity activity never asserts actual settlement.
  auto late_policy = charged(RecognitionBasis::accrual);
  late_policy.policies[0].recorded_at = day(10);
  auto late_policy_report = late_policy.report();
  verify(late_policy_report);
  for (const auto &e : late_policy_report.effects) {
    check(e.journal.recorded_at() >= day(10));
    if (e.journal.event_type() == JournalEventType::equity_trade)
      check(e.journal.recognition_phase() == RecognitionPhase::period_adjustment);
  }
  // Currencies remain separate all the way through daily P&L and statements.
  Book multi;
  multi.contribution();
  auto eur = take(Currency::from_code("EUR"));
  auto eh = take(
      EventHeader::create(EventId{"euro-capital"}, AccountId{"a"}, day(1), proof("euro-capital")));
  take(multi.ledger.accept(LifecycleRecordDraft::originate(
      EconomicEventId{"euro-capital"}, day(1),
      EconomicEvent{CashMovement::create(eh, take(Money::parse("200", eur)))})));
  auto multi_report = multi.report();
  verify(multi_report);
  check(multi_report.statements.closing_balance_sheet.size() == 2);
  // A closing account can relieve another account's lot in the same custody
  // pool. Recognition preferences do not change lot selection or carrying cost.
  Book shared(RecognitionBasis::cash);
  shared.targets.push_back({"book", "entity", "custody", "b"});
  shared.contribution();
  shared.add(trade("shared-buy", "10", "10", "0", "0", 1));
  shared.add(trade("shared-sale", "-4", "15", "0", "0", 2, "b"));
  shared.policies.push_back({{"p-account-b",
                              "entity",
                              "b",
                              "operator",
                              PolicyScope::account,
                              day(-5),
                              2,
                              {},
                              RecognitionBasis::accrual},
                             day(-5)});
  auto shared_report = shared.report();
  verify(shared_report);
  auto shared_rows = take(select_period_pnl(shared_report));
  check(shared_rows.rows.size() == 2);
  check(shared_rows.totals == shared_report.pnl);
  auto only_b = take(select_period_pnl(shared_report, {"b", "XYZ"}));
  check(only_b.rows.size() == 1);
  check(only_b.totals[0].movement.economic == m("20"));
  check(only_b.totals[0].movement.recognized == m("20"));
  check(take(select_period_pnl(shared_report, {"a", "XYZ"})).totals[0].movement.economic == m("0"));
  check(take(select_period_pnl(replacement.report())).totals == replacement.report().pnl);
  // Policy/cash IDs cannot be interpreted as ledger IDs, including unknown
  // future lifecycle records that happen to have identical text identities.
  Book collision;
  collision.contribution();
  collision.add(trade("collision-buy", "10", "10", "1", "1", 1));
  collision.settle("same-cash-id", "collision-buy", "-102", 2);
  collision.policies.push_back(
      {{"same-policy-id", "entity", "entity", "operator", PolicyScope::legal_entity, day(2), 2,
        LotSelection::lifo, RecognitionBasis::accrual},
       day(2)});
  collision.add(trade("same-cash-id", "1", "1", "0", "0", 4), 100);
  collision.add(trade("same-policy-id", "1", "1", "0", "0", 4), 100);
  auto collision_result = collision.report(2, 3, 30);
  verify(collision_result);
  for (const auto &effect : collision_result.effects) {
    check(effect.journal.recorded_at() <= day(30));
    for (const auto &record : effect.journal.lineage().record_ids())
      check(record.value() != "same-cash-id" && record.value() != "same-policy-id");
  }
  // Canonical restart reconstructs immutable inputs; retries reproduce evidence.
  auto restored =
      take(serialization::decode_lifecycle_ledger(serialization::canonical_bytes(cash.ledger)));
  auto replay = take(project_period_accounting(
      restored, cash.targets, cash.policies, cash.settlements, cash.context(),
      {ChargeTreatment::capitalize_opening, CashRecognition::both_legs}));
  check(replay == cash.report());
  // Full books are independent execution partitions; neither temporal shards nor
  // arbitrary pieces of a dependent custody pool are supported merge operands.
  auto first = std::async(std::launch::async, [&] { return b.report(); });
  auto other_book = charged(RecognitionBasis::cash);
  other_book.targets[0].book = "other-book";
  auto second = std::async(std::launch::async, [&] { return other_book.report(); });
  check(first.get() == b.report());
  check(second.get() == other_book.report());
  check(other_book.report().report_id != b.report().report_id);
  // Unknown future policy remains excluded by knowledge even in a pinned prefix.
  auto changed_policy = charged(RecognitionBasis::cash);
  changed_policy.policies.push_back(
      {{"p2", "entity", "entity", "operator", PolicyScope::legal_entity, day(6), 2,
        LotSelection::lifo, RecognitionBasis::accrual},
       day(9)});
  auto before_policy = changed_policy.report(6, 7, 8),
       after_policy = changed_policy.report(6, 7, 9);
  verify(before_policy);
  verify(after_policy);
  check(before_policy.pnl[0].closing.recognized == m("32.175"));
  check(after_policy.pnl[0].closing.recognized == m("61.4"));
  bool policy_effect = false;
  for (const auto &effect : after_policy.effects)
    for (const auto &cause : effect.causes)
      policy_effect = policy_effect || cause.kind == MovementKind::policy_change;
  check(policy_effect);
  std::cout << "period accounting checks passed\n";
}
