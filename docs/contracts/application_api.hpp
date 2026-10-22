#pragma once

// DESIGN ONLY: syntax-checkable declarations, not installed or implemented.
// See docs/application-api.md. No JSON, SQLite, MCP or networking types here.
#include "luca/accounting/balances.hpp"
#include "luca/core.hpp"
#include "luca/lifecycle.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

namespace luca::application::v1 {
inline constexpr auto api_version = "luca.application/1";

struct TenantTag; struct PrincipalTag; struct ImportTag; struct PayloadTag;
struct ContextTag; struct GroupTag; struct RunTag; struct RevisionTag;
struct SnapshotTag; struct ReportTag; struct ResultTag; struct RowTag;
struct EvidenceTag; struct CursorTag;
struct StagedRecordTag;
using TenantId = Identifier<TenantTag>;
using PrincipalId = Identifier<PrincipalTag>;
using ImportId = Identifier<ImportTag>;
using PayloadId = Identifier<PayloadTag>;
using ContextId = Identifier<ContextTag>;
using GroupId = Identifier<GroupTag>;
using RunId = Identifier<RunTag>;
using Revision = Identifier<RevisionTag>;
using SnapshotId = Identifier<SnapshotTag>;
using ReportId = Identifier<ReportTag>;
using ResultId = Identifier<ResultTag>;
using RowId = Identifier<RowTag>;
using EvidenceId = Identifier<EvidenceTag>;
using Cursor = Identifier<CursorTag>;
using StagedRecordId = Identifier<StagedRecordTag>;

struct Error {
  std::string code, detail, request_id;
  bool retryable = false;
  std::optional<std::chrono::milliseconds> retry_after;
};
template<class T> using Result = std::expected<T, Error>;
// Outer failure: not queued. Inner completion: operation outcome, not job finish.
template<class T> using Task = Result<std::future<Result<T>>>;
struct CallOptions {
  std::string request_id;
  std::optional<std::chrono::steady_clock::time_point> deadline;
  std::stop_token stop;
};
struct CallerContext { TenantId tenant; PrincipalId principal; };
struct BookScope { TenantId tenant; BookId book; };
enum class Permission { inspect, submit, revalidate, run_reports, read_reports };
class Authorizer {
public:
  virtual ~Authorizer() = default;
  // Called per operation; source_id is supplied for source-scoped authorization.
  // Executed outside financial calculations and off the host's network loop.
  virtual Result<void> check(const CallerContext &, const BookScope &, Permission,
                            const std::optional<SourceId> &source_id) = 0;
};

struct PageRequest {
  std::optional<Cursor> cursor;
  std::uint32_t limit = 100;
};
template<class T> struct Page {
  SnapshotId snapshot_id;
  Timestamp expires_at;
  std::vector<T> items;
  std::optional<Cursor> next_cursor;
};

enum class Validation { pending, validating, valid, invalid, blocked };
enum class Commitment { not_committed, queued, committed, conflict };
enum class Enumeration { pending, running, complete, failed };
enum class Atomicity { independent_groups, whole_import };
struct ValidationCounts {
  std::uint64_t pending=0, validating=0, valid=0, invalid=0, blocked=0;
};
struct CommitmentCounts {
  std::uint64_t not_committed=0, queued=0, committed=0, conflict=0;
};
struct ImportMetadata {
  SourceId source_id;
  std::string format;
  ContextId validation_context_id;
  Atomicity atomicity = Atomicity::independent_groups;
  std::optional<std::string> client_reference;
};
struct PayloadReference {
  PayloadId payload_id;
  std::uint64_t bytes;
  std::string sha256;
  Timestamp expires_at;
};
struct InlinePayload { std::string utf8; }; // <= 64 KiB at the adapter boundary
using SubmissionPayload = std::variant<InlinePayload, PayloadId>;
struct SubmitImport {
  std::string idempotency_key;
  ImportMetadata metadata;
  SubmissionPayload payload;
};
struct ImportReceipt { ImportId import_id; Timestamp received_at; };
struct ImportStatus {
  ImportId import_id;
  Revision status_revision;
  Enumeration enumeration;
  ValidationCounts validation;
  CommitmentCounts commitment;
  std::optional<Error> enumeration_error;
};
struct SourceLocator {
  std::uint64_t offset, length;
  std::string sha256;
};
struct StagedRecord {
  StagedRecordId record_id;
  std::uint64_t ordinal;
  Validation validation;
  Commitment commitment;
  ContextId validation_context_id;
  std::optional<RunId> run_id;
  std::optional<GroupId> group_id;
  std::optional<EventId> canonical_record_id;
  std::optional<Revision> accepted_revision;
  std::uint64_t diagnostic_count;
  SourceLocator source;
};
struct Diagnostic {
  std::string diagnostic_id, code, field, message;
  std::optional<StagedRecordId> record_id;
  std::optional<GroupId> group_id;
  std::optional<RunId> run_id;
  std::string severity; // error or blocked; no role/authority semantics
};
struct RecordQuery {
  ImportId import_id;
  std::optional<Validation> validation;
  std::optional<Commitment> commitment;
  PageRequest page;
};
struct DiagnosticQuery {
  ImportId import_id;
  std::optional<StagedRecordId> record_id;
  PageRequest page;
};
struct GroupSelection { GroupId group_id; Revision expected_revision; };
struct RevalidateImport {
  ImportId import_id;
  std::string idempotency_key;
  ContextId validation_context_id;
  std::vector<GroupSelection> groups;
};
struct RevalidationReceipt { std::string revalidation_id; ImportId import_id; };

enum class ReportKind {
  positions, settled_cash, settlement_obligations, journal_lines, trial_balance
};
struct CurrentAtRequest {};
struct ExactRevision { Revision revision; };
using InputSelection = std::variant<CurrentAtRequest, ExactRevision>;
struct AvailableInputs {};
struct RequireImports { std::vector<ImportId> import_ids; Timestamp deadline; };
using InputReadiness = std::variant<AvailableInputs, RequireImports>;
struct Cutoffs {
  Timestamp economic_as_of, recorded_through;
  SettlementDate settlement_as_of_date;
};
struct ReportFilters {
  std::vector<AccountId> account_ids;
  std::vector<InstrumentId> instrument_ids;
  std::vector<Currency> currencies;
  std::vector<ChartAccountId> chart_account_ids;
};
struct RequestReport {
  std::string idempotency_key;
  ReportKind kind;
  ContextId calculation_context_id;
  Cutoffs cutoffs;
  InputSelection selection;
  InputReadiness readiness;
  ReportFilters filters;
};
struct ReportReceipt { ReportId report_id; Timestamp accepted_at; };
struct Coverage {
  SnapshotId snapshot_id;
  Timestamp captured_at;
  Revision accepted_revision;
  bool enumeration_complete;
  std::uint64_t pending, invalid, blocked, valid_uncommitted, admitted_after_selection;
};
struct CalculationIdentity {
  ContextId context_id;
  std::string engine_version, projection_version, normalization_version;
  std::string reference_snapshot_id;
  std::optional<AccountingPolicyIdentity> accounting_policy;
};
struct CurrencyControl { Currency currency; Money debits, credits; };
enum class ControlScope { none, whole_journals_before_chart_filter };
struct ReportManifest {
  ResultId result_id;
  BookId book_id;
  ReportKind kind;
  CalculationIdentity calculation;
  Cutoffs cutoffs;
  Coverage coverage;
  std::uint64_t row_count;
  ControlScope control_scope;
  std::vector<CurrencyControl> currency_controls;
  Timestamp expires_at;
};
enum class ReportState { waiting_for_inputs, queued, running, available, failed };
struct ReportStatus {
  ReportId report_id;
  ReportState state;
  std::optional<Coverage> selection;
  std::optional<ReportManifest> manifest;
  std::optional<Error> failure;
};
struct PositionRow { AccountId account; InstrumentId instrument; Quantity quantity; };
struct CashRow { AccountId account; Money amount; };
enum class SettlementDirection { receivable, payable };
struct SettlementRow {
  AccountId account;
  SettlementDate settlement_date;
  SettlementDirection direction;
  Money amount;
};
struct JournalRow {
  JournalEntryId entry_id;
  JournalLineId line_id;
  ChartAccountId chart_account;
  Money debit, credit;
};
struct TrialBalanceRow { ChartAccountId chart_account; Money debits, credits, net; };
struct ReportRow {
  RowId row_id;
  std::variant<PositionRow, CashRow, SettlementRow, JournalRow, TrialBalanceRow> value;
};
struct ReportPage {
  ReportId report_id;
  ResultId result_id;
  Page<ReportRow> rows;
};
struct EvidenceContribution {
  EvidenceId evidence_id;
  EventId canonical_record_id;
  EconomicEventId economic_id;
  std::optional<EventId> causal_record_id;
  std::optional<EvidenceId> causal_evidence_id;
  SourceRecordId source_record_id;
  ImportId import_id;
  StagedRecordId staged_record_id;
  std::optional<JournalEntryId> journal_entry_id;
  std::optional<JournalLineId> journal_line_id;
  SourceLocator source;
};
struct EvidencePage {
  ReportId report_id;
  ResultId result_id;
  std::string target_id; // row or evidence ID, always scoped to this result
  Page<EvidenceContribution> evidence;
};
struct ContextDescriptor { ContextId context_id; std::string description; };
struct Capabilities {
  std::string api_version;
  BookId book_id;
  std::vector<std::string> formats;
  std::vector<SourceId> source_ids;
  std::vector<ContextDescriptor> validation_contexts, calculation_contexts;
  std::vector<ReportKind> report_kinds;
  std::uint32_t max_page_size, max_inline_bytes;
  std::uint64_t max_payload_bytes, max_queued_bytes;
  std::uint32_t snapshot_ttl_seconds, result_ttl_seconds, command_ttl_seconds;
  bool stage_file_enabled;
  std::vector<std::string> input_root_ids;
};

class PayloadReader {
public:
  virtual ~PayloadReader() = default;
  // Zero bytes means EOF; blocking I/O runs only on the bounded I/O executor.
  virtual Result<std::size_t> read(std::span<std::byte> destination) = 0;
};
class BookSession {
public:
  virtual ~BookSession() = default;
  virtual Task<Capabilities> capabilities(CallOptions) = 0;
  virtual Task<PayloadReference> stage_payload(
      std::unique_ptr<PayloadReader>, std::uint64_t expected_bytes, CallOptions) = 0;
  virtual Task<ImportReceipt> submit_import(SubmitImport, CallOptions) = 0;
  virtual Task<ImportStatus> get_import_status(ImportId, CallOptions) = 0;
  virtual Task<Page<StagedRecord>> read_records(RecordQuery, CallOptions) = 0;
  virtual Task<Page<Diagnostic>> read_diagnostics(DiagnosticQuery, CallOptions) = 0;
  virtual Task<RevalidationReceipt> revalidate_import(RevalidateImport, CallOptions) = 0;
  virtual Task<ReportReceipt> request_report(RequestReport, CallOptions) = 0;
  virtual Task<ReportStatus> get_report_status(ReportId, CallOptions) = 0;
  virtual Task<ReportPage> read_report_page(ReportId, PageRequest, CallOptions) = 0;
  virtual Task<EvidencePage> get_evidence(
      ReportId, std::string target_id, PageRequest, CallOptions) = 0;
};
class Application {
public:
  virtual ~Application() = default;
  // Host establishes caller identity; the implementation enforces book access
  // here and on every subsequent operation. No caller-supplied "authorized" flag.
  virtual Task<std::shared_ptr<BookSession>> open_book(
      CallerContext, BookScope, CallOptions) = 0;
};
} // namespace luca::application::v1
