// An embedding peer: calls the shared installed C++ operation directly. No CLI
// command parsing or MCP protocol is used; Python only orchestrates test books.
#include "application_host.hpp"
#include <iostream>
int main(int argc, char **argv) {
  if (argc != 4)
    return 2;
  auto app = luca::app_host::open(argv[1]);
  auto op = luca::application::operation(argv[2]);
  if (!op)
    return 2;
  auto result = app->call(*op, luca::app_host::read(argv[3]));
  auto code = result ? 0 : luca::application::exit_code(result.error());
  std::cout << luca::application::document(*op, std::move(result)).dump() << '\n';
  return code;
}
