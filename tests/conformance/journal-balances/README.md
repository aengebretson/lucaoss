# Journal-balance conformance expectations

`walkthrough.tsv` is read by the production C++ `luca_journal_balances_test`.
It is not a `scenario.json` fixture for the older portfolio-only translator.
Rows are keyed by the five ordered evaluations in the existing accounting
walkthrough and `td`/`sd` policy. Money columns are signed scale-six integers:
row count, gross debit turnover, total positive closing balances, then cash,
securities, payable, receivable and contributed-capital net balances.

Credit balances are negative. Omitted accounts read as zero for comparison;
an existing zero-net account still retains a row and its evidence, reflected
in the explicit row count. Gross debit equals gross credit; total debit net
balances equal total credit net balances independently in USD.

Hand calculation: the original purchase is 5000, correction makes it 4400,
settlement reduces cash from 100000 to 95600, and the exact reversal restores
100000 cash after settlement. Trade-date policy recognizes payable/receivable
controls before settlement; settlement-date policy defers that recognition.
