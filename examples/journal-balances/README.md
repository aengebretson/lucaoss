# Journal-to-trial-balance walkthrough

This independent installed-package consumer uses the same synthetic lifecycle
history as `examples/settlement-date-accounting`. It projects both accounting
policies at five historical views and calls the public journal-balance reducer.
Each output identifies its explicit book/policy, account/currency debit/credit
turnover, debit-positive net balances, trial-balance controls, contributing
entry/line IDs and source-record lineage.

From an installed package:

```sh
cmake -S examples/journal-balances -B build/journal-balance-example \
  -DCMAKE_PREFIX_PATH=/absolute/path/to/luca-install
cmake --build build/journal-balance-example
```

When software execution is enabled, run
`build/journal-balance-example/luca_journal_balance_walkthrough`.
All printed monetary values are exact scaled integers with six decimal places;
`100000000000` is `100000.000000`. The corrected settled purchase leaves cash
`95600.000000` and securities `4400.000000`. After reversal settlement, cash
returns to `100000.000000` and security/settlement controls net to zero.
Both sides of the closing trial balance total `100000.000000` in these views.

The fixture assigns one economic account to one book explicitly. This example
has no transport/storage host, canonical report wire format, production policy,
or external-observation comparison. See [the contract](../../docs/journal-balances.md).
