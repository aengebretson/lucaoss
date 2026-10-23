# Versioned accounting policy configuration

`luca/accounting/policy.hpp` provides pure C++ `resolve_policy(target, revisions,
effective_at, through_sequence)`. It returns owned resolved values, both cutoffs,
selected revisions, field sources and a deterministic digest. No implicit clock,
storage or networking is used. `project_policy_lots` consumes and retains these
values, including for empty reports. Legacy `project_lifo_lots` remains supported
but does not claim durable configuration evidence.

## Resolution

Account override → custodian-account override → legal-entity default. Custodian
scope refers to an identified custody account, not every account at a bank.
Book/entity/subject identity is explicit. Each field resolves independently.
A revision replaces its scope's settings; null means inherit, not preserve an
older override. Entity defaults specify both fields. The latest effective date
not after the evaluation date wins, then the highest sequence at that date, within
the explicit policy-history cutoff. Duplicate IDs/sequences fail.

Only `lot_selection=LIFO` is implemented. Recognition is separately configured as
`unspecified`, `cash`, or `accrual`. Lot results explicitly report
`recognition_applied=false`; these preferences do not enable cash/accrual journals.
The digest includes target and selected definitions. Cutoffs remain separate
evidence. Lot-pool compatibility depends on the economic LIFO algorithm rather
than account-specific evidence or recognition preferences. Shared custody relief
and independent per-account provenance are both retained.

## Durable adapter and API

Optional SQLite schema 4 adds immutable revision history and content-addressed
result evidence. `put_policy(definition, expected_sequence, actor)` accepts exactly
`id`, `scope`, `legal_entity`, `subject`, `effective_from`, `lot_selection`, and
`recognition`. The store assigns a sequence. Exact retries recover their receipt;
conflicting reuse and stale expected sequences fail. Actor/effective date/sequence
remain in each revision. History is bounded to 10,000 revisions per book; exceeding
the bound fails explicitly.

`policy_history`, `resolve_policy`, `retain_policy_result` and `policy_result`
provide retrieval, resolution, retention and lookup. Retention re-resolves the
complete persisted history within its transaction, rejects omitted overrides and
unavailable ledger/policy cutoffs, and stores resolved evidence with projection,
mapping version, ledger sequence and result digest. The caller computes the
financial result; retention verifies policy provenance, not financial arithmetic.

Policy edits and closes share the admission fence. Edits effective in a closed
interval fail. Close requests review both ledger and policy sequences; receipts
pin both. Historical replay supplies both cutoffs. Changes never rewrite retained
results; new settings require a new projection result.

CLI commands: `policy-history`, `policy-result ID`, `policy-put --request FILE`,
`policy-resolve --request FILE`. Put payloads contain `revision`,
`expected_sequence`, `actor`; resolve payloads contain `target`, `effective_at`,
optional `through_sequence`. The host supplies trusted authorization.

## Migration and verification

Opening schemas 1–3 upgrades transactionally to 4; earlier binaries reject 4.
Rollback requires a coherent pre-upgrade backup. Enterprise seeds existing LIFO
configuration only where no prior closed period exists. Closed legacy books
without policy history require explicit reviewed migration; no policy is invented.

Tests cover override precedence/removal, effective dates, historical cutoffs,
restart, exact retries/stale edits, unsupported methods, entity isolation, shared
custody relief, omitted/forged evidence, empty results and closed-history fences.
This slice makes no new performance or journal-recognition coverage claim.
