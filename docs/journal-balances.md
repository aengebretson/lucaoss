# Exact journal balances and trial-balance controls

Include `<luca/accounting/balances.hpp>` and link the opt-in `luca::accounting`
target. The header lives under `libs/accounting`; existing journal/projection
headers stay under `libs/ledger` for compatibility. The existing umbrella graph
is unchanged. The new accounting target depends only on `luca::ledger`.

## Contract and inputs

`reduce_journal_balances(contract, entries)` reduces whole validated `JournalEntry`
values. A contract fixes an explicit `BookId`, caller-supplied immutable input
revision, engine and journal-projection versions, accounting policy ID/version,
knowledge/economic cutoffs and independent settlement date cutoff. All identity
fields are nonempty tokens. Factories return `std::expected` with a stable
`JournalBalanceError`; no partial financial result escapes a failed reduction.

`BookId` and `ChartAccountId` are distinct strong types. Existing journal lines'
`AccountId` values identify chart accounts; the reducer converts them explicitly
to `ChartAccountId` and keys every row by `(book, chart account, currency)`.
One invocation concerns one explicit book. The caller must assign selected
journals to that book correctly before reduction; existing journals carry no
independent book ownership to infer or verify. The fixture consumer uses one
economic account (`fund-a`) and explicitly assigns its journals to `fund-a.book`.
Mixing books requires separate invocations and cannot be achieved by merging
results under different contracts.

Every entry must match the contract's accounting policy and lie inside its
knowledge and economic cutoffs. Settlement-phase recognition must not exceed
the settlement cutoff; other recognition dates must not exceed the economic
cutoff's UTC date. This validates supplied entries, not completeness: a result is
not proof that every journal belonging to the input revision was supplied. A
publishing host must verify coverage against its pinned input manifest.

The reducer does not resolve lifecycle chains, regenerate journals, infer source
normalization, calculate settlement calendars, fetch data, or select a policy.
Source interpretation and version correctness remain caller responsibilities.

## Results and evidence

An owned `JournalBalanceResult` exposes const views of:

- Sorted account/currency rows with exact positive-side debit and credit turnover
  and debit-positive signed net balances. Zero-net rows retain their evidence.
- Separate currency controls: gross debit/credit posting totals and sums of
  positive debit/credit net account balances. These are distinct measures; each
  pair balances independently in one currency.
- Sorted contributing `(journal entry ID, journal line ID)` references per row,
  and entry evidence retaining full record/event/source and reversal lineage.
- The complete calculation contract. Source vectors and journals can be destroyed
  without invalidating a result.

Currency balances are never converted or netted together. No rounding occurs:
the reducer uses existing scale-six `Money` arithmetic. Financial statement
classification, account normal-side conventions, closing/reopening, opening
balance import and production accounting policies are outside this operation.
The result has no new canonical wire schema or output-hash promise yet.

## Partitioning and actual algebraic laws

`merge_journal_balances(contract, partials)` accepts only results produced under
exactly the same contract. Partition **whole entries**, preserving both sides;
never split a journal into line partitions. Entries and line IDs must be globally
disjoint within the invocation. Repeated source IDs across different entries
are valid evidence; repeated entry/line contributions are errors.

Empty reduction/merge supplies the identity: the same contract, no rows,
controls or evidence. It invents no zero balance for an unspecified account.
Identity, associativity and commutativity apply on the explicit domain of one
compatible contract, disjoint entry/line identities, and each currency's gross
debit and credit totals fitting nonnegative `int64` scaled money. Positive
turnover totals are monotone, so every permitted grouping is representable under
this bound. Net balances are calculated once from those totals, with no ordered
signed cancellation. Even zero final net does not excuse gross-total overflow.
This is a separate operation contract from the existing signed exact-cash reducer.

Results sort rows, references and entry evidence deterministically; full and
partitioned reductions compare equal, including lineage. These laws do not
permit reordering unresolved lifecycle records or changing upstream journal
rounding. Corrections require newly selected journals under a new input/context
revision; do not merge old and replacement outputs or infer a subtraction inverse.
The API enables independent workers but supplies no scheduler or parallel runtime.

Errors distinguish invalid/incompatible contracts, policy mismatch, outside-cutoff
entries, duplicate entry IDs, duplicate line IDs and amount overflow. If invalid
inputs have several defects, the first diagnostic can follow traversal order;
the algebraic equivalence claim concerns valid representable inputs.

## Demonstration and verification

The [standalone consumer](../examples/journal-balances/README.md) projects the
existing contribution/purchase/correction/settlement/reversal history under
both fixture policies, then prints balances, controls and source evidence.
`tests/conformance/journal-balances/walkthrough.tsv` supplies independently stated
expectations for ten policy/time views. `luca_journal_balances_test` consumes that
file through the production C++ APIs, including real independent worker
reductions, regrouping, identity, sorted evidence, ownership and diagnostic
boundaries. Checks remain active in Debug and Release.

Installed and embedded consumers link the new public target. Benchmarks add
`journal_balances.full` and `journal_balances.merge_prepared_4`, retaining evidence
construction in timing. The latter excludes partial computation and is not an
end-to-end parallel speedup measurement. The existing synthetic microbenchmark
suite does not establish production ingestion/report service levels.

Actual executed evidence and remaining baseline failures belong in the PR and
handoff. Authoring or compiling these checks alone establishes no runtime
acceptance. Full portable accounting/reconciliation reporting and durable staging
remain the next separate increments of the minimal-core program.
