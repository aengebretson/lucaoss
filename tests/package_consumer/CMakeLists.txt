cmake_minimum_required(VERSION 3.24)
project(luca_package_consumer LANGUAGES CXX)

find_package(Luca 0.1 CONFIG REQUIRED)

set(LUCA_EXPECTED_INSTALL_PREFIX "" CACHE PATH
  "Expected isolated LUCA installation prefix")
if(NOT LUCA_EXPECTED_INSTALL_PREFIX)
  message(FATAL_ERROR "LUCA_EXPECTED_INSTALL_PREFIX is required")
endif()
get_filename_component(
  LUCA_EXPECTED_INSTALL_PREFIX "${LUCA_EXPECTED_INSTALL_PREFIX}" ABSOLUTE)
set(expected_include_dir "${LUCA_EXPECTED_INSTALL_PREFIX}/include")

foreach(luca_target IN ITEMS
    luca::accounting
    luca::ledger
    luca::portfolio
    luca::reconciliation
    luca::adapters
    luca::luca)
  if(NOT TARGET ${luca_target})
    message(FATAL_ERROR "Installed package did not expose ${luca_target}")
  endif()

  get_target_property(is_imported ${luca_target} IMPORTED)
  if(NOT is_imported)
    message(FATAL_ERROR "${luca_target} is not an installed imported target")
  endif()

  get_target_property(compile_features ${luca_target} INTERFACE_COMPILE_FEATURES)
  if(NOT "cxx_std_23" IN_LIST compile_features)
    message(FATAL_ERROR "${luca_target} does not require C++23")
  endif()

  get_target_property(include_dirs ${luca_target} INTERFACE_INCLUDE_DIRECTORIES)
  if(NOT include_dirs STREQUAL expected_include_dir)
    message(FATAL_ERROR
      "${luca_target} exposes unexpected installed include directories: "
      "${include_dirs}; expected ${expected_include_dir}")
  endif()
endforeach()

get_target_property(accounting_links luca::accounting INTERFACE_LINK_LIBRARIES)
if(NOT accounting_links STREQUAL "luca::ledger")
  message(FATAL_ERROR "luca::accounting has unexpected dependencies: ${accounting_links}")
endif()
add_executable(luca_package_journal_balances_consumer ../../examples/journal-balances/main.cpp)
target_link_libraries(luca_package_journal_balances_consumer PRIVATE luca::accounting)
add_test(NAME luca_package_journal_balances_run COMMAND luca_package_journal_balances_consumer)
set_tests_properties(luca_package_journal_balances_run PROPERTIES
  PASS_REGULAR_EXPRESSION "trial_balance_currency=USD debit_balances_scaled=100000000000 credit_balances_scaled=100000000000")

get_target_property(portfolio_links luca::portfolio INTERFACE_LINK_LIBRARIES)
if(NOT portfolio_links STREQUAL "luca::ledger")
  message(FATAL_ERROR
    "luca::portfolio has unexpected dependencies: ${portfolio_links}")
endif()
get_target_property(
  reconciliation_links luca::reconciliation INTERFACE_LINK_LIBRARIES)
if(NOT reconciliation_links STREQUAL "luca::portfolio;luca::ledger")
  message(FATAL_ERROR
    "luca::reconciliation has unexpected dependencies: ${reconciliation_links}")
endif()
get_target_property(adapters_links luca::adapters INTERFACE_LINK_LIBRARIES)
if(NOT adapters_links STREQUAL "luca::reconciliation")
  message(FATAL_ERROR "luca::adapters has unexpected dependencies: ${adapters_links}")
endif()
get_target_property(umbrella_links luca::luca INTERFACE_LINK_LIBRARIES)
if(NOT umbrella_links STREQUAL
    "luca::ledger;luca::portfolio;luca::reconciliation")
  message(FATAL_ERROR "luca::luca has unexpected dependencies: ${umbrella_links}")
endif()

add_executable(luca_package_consumer main.cpp)
target_link_libraries(luca_package_consumer PRIVATE luca::luca)

add_executable(luca_package_ledger_consumer ledger.cpp)
target_link_libraries(luca_package_ledger_consumer PRIVATE luca::ledger)

add_executable(luca_package_portfolio_consumer main.cpp)
target_link_libraries(luca_package_portfolio_consumer PRIVATE luca::portfolio)

add_executable(luca_package_reconciliation_consumer reconciliation.cpp)
target_link_libraries(
  luca_package_reconciliation_consumer PRIVATE luca::reconciliation)

add_executable(luca_package_exact_cash_consumer exact_cash_reduction.cpp)
target_link_libraries(luca_package_exact_cash_consumer PRIVATE luca::portfolio)

add_executable(luca_package_exact_cash_comparison_consumer
  exact_cash_comparison.cpp)
target_link_libraries(luca_package_exact_cash_comparison_consumer
  PRIVATE luca::reconciliation)

enable_testing()
add_test(NAME luca_package_consumer_run COMMAND luca_package_consumer)
set_tests_properties(luca_package_consumer_run PROPERTIES
  PASS_REGULAR_EXPRESSION "cash_scaled=125000000 currency=USD")
add_test(
  NAME luca_package_ledger_consumer_run
  COMMAND luca_package_ledger_consumer)
set_tests_properties(luca_package_ledger_consumer_run PROPERTIES
  PASS_REGULAR_EXPRESSION "ledger_entries=1")
add_test(
  NAME luca_package_portfolio_consumer_run
  COMMAND luca_package_portfolio_consumer)
set_tests_properties(luca_package_portfolio_consumer_run PROPERTIES
  PASS_REGULAR_EXPRESSION "cash_scaled=125000000 currency=USD")
add_test(
  NAME luca_package_reconciliation_consumer_run
  COMMAND luca_package_reconciliation_consumer)
set_tests_properties(luca_package_reconciliation_consumer_run PROPERTIES
  PASS_REGULAR_EXPRESSION "position_breaks=0")
add_test(
  NAME luca_package_exact_cash_consumer_run
  COMMAND luca_package_exact_cash_consumer)
set_tests_properties(luca_package_exact_cash_consumer_run PROPERTIES
  PASS_REGULAR_EXPRESSION
    "exact_cash_scaled=800000000 currency=USD policy=package-consumer.cash-policy")
add_test(
  NAME luca_package_exact_cash_comparison_consumer_run
  COMMAND luca_package_exact_cash_comparison_consumer)
set_tests_properties(luca_package_exact_cash_comparison_consumer_run PROPERTIES
  PASS_REGULAR_EXPRESSION
    "exact_cash_break=amount_mismatch difference_scaled=-10000000 currency=USD operation=compare.cash.exact")

# Compile the unchanged forwarding surface through the installed umbrella.
add_executable(luca_package_legacy_consumer legacy.cpp)
target_link_libraries(luca_package_legacy_consumer PRIVATE luca::luca)
add_test(NAME luca_package_legacy_consumer_run COMMAND luca_package_legacy_consumer)

add_executable(luca_package_walkthrough ../../examples/package-consumer/main.cpp)
target_link_libraries(luca_package_walkthrough PRIVATE luca::reconciliation)
add_test(NAME luca_package_walkthrough_run COMMAND luca_package_walkthrough)
set_tests_properties(luca_package_walkthrough_run PROPERTIES PASS_REGULAR_EXPRESSION
  "position_scaled=1000000000 cash_before_scaled=1000000000 payable_before_scaled=200000000 cash_scaled=800000000 open_after=0[\r\n]+cash_break=amount_mismatch difference_scaled=-10000000 currency=USD observation_source=observation.bank.1[\r\n]+event=deposit-1 source=source.deposit.1[\r\n]+event=trade-1 source=source.trade.1")

# CSV adapters are an explicit opt-in and do not widen the compatible umbrella.
add_executable(luca_package_adapters_consumer adapters.cpp)
target_link_libraries(luca_package_adapters_consumer PRIVATE luca::adapters)
add_test(NAME luca_package_adapters_consumer_run COMMAND luca_package_adapters_consumer)
set_tests_properties(luca_package_adapters_consumer_run PROPERTIES
  PASS_REGULAR_EXPRESSION "trade_observations=1 unexpected_observations=1")
