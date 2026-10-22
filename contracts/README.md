# Application and MCP contracts

Design artifacts only: no new runtime target, MCP server or unified CLI is
implemented here. Financial/domain behavior is defined by the native C++ core;
these schemas define adapter documents, not canonical ledger encoding.

- [Application contract](../docs/application-api.md)
- [C++ declaration sketch](../docs/contracts/application_api.hpp)
- [Optional SQLite host construction](../docs/contracts/sqlite_application.hpp)
- [CLI contract](../docs/cli.md)
- [Common definitions and operation exchanges](application/v1.schema.json)
- [Generated, self-contained MCP tool catalog](mcp/v1.tools.json)
- [Illustrative valid exchanges](examples/application-v1.json)
- [Rejected schema cases](examples/invalid-v1.json)

Edit the common schema and its `x-luca-mcp-tools` descriptors, then regenerate the
catalog. Input and output schemas contain only local `$defs` references; an MCP
client does not need to fetch the common schema. The catalog is a package
artifact, not a complete protocol response: a server must negotiate a supported
MCP protocol version and wrap discovery/results correctly. Tool annotations are
hints, not authorization controls. Examples illustrate independent operation
shapes; they are not a financial golden history or verified journal policy.

Developer checks (Python is used only for schema maintenance, not the runtime):

```sh
python3 -m venv /tmp/luca-contract-checks
/tmp/luca-contract-checks/bin/python -m pip install 'jsonschema[format]==4.26.0'
/tmp/luca-contract-checks/bin/python contracts/check.py
# After intentionally editing the common contract:
/tmp/luca-contract-checks/bin/python contracts/check.py --write-catalog
```

The SQLite construction sketch additionally requires the already optional
ingestion component's nlohmann/json headers. It does not make them a dependency
of the shared C++ application header.

Syntax-check the C++ draft with the existing financial headers and a supported
C++23 compiler; include it as a header rather than a main translation unit:

```sh
clang++ -std=c++23 -Wall -Wextra -Werror -fsyntax-only -x c++ \
  -I libs/ledger/include -I libs/accounting/include -I libs/portfolio/include \
  -include docs/contracts/application_api.hpp /dev/null
```

Checks cover schema validity, catalog equivalence, adapter examples and rejected
shapes. They do not establish durability, financial correctness, runtime
performance, installed ABI compatibility or integration with an MCP host.
