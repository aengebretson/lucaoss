cmake_minimum_required(VERSION 3.24)
project(luca VERSION 0.1.0 LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/LucaOptions.cmake)
luca_compatible_option(BUILD_TESTS "Build LUCA tests" ${PROJECT_IS_TOP_LEVEL})
luca_compatible_option(BUILD_BENCHMARKS "Build reproducible performance benchmarks" OFF)
luca_compatible_option(BUILD_TOOLS "Build standalone LUCA tools" ${PROJECT_IS_TOP_LEVEL})

add_library(luca_ledger INTERFACE)
add_library(luca::ledger ALIAS luca_ledger)
set_target_properties(luca_ledger PROPERTIES EXPORT_NAME ledger)
target_compile_features(luca_ledger INTERFACE cxx_std_23)
target_include_directories(luca_ledger INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/ledger/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# New accounting functionality is opt-in; existing umbrella dependencies stay compatible.
add_library(luca_accounting INTERFACE)
add_library(luca::accounting ALIAS luca_accounting)
set_target_properties(luca_accounting PROPERTIES EXPORT_NAME accounting)
target_compile_features(luca_accounting INTERFACE cxx_std_23)
target_include_directories(luca_accounting INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/accounting/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(luca_accounting INTERFACE luca::ledger)

add_library(luca_portfolio INTERFACE)
add_library(luca::portfolio ALIAS luca_portfolio)
set_target_properties(luca_portfolio PROPERTIES EXPORT_NAME portfolio)
target_compile_features(luca_portfolio INTERFACE cxx_std_23)
target_include_directories(luca_portfolio INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/portfolio/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(luca_portfolio INTERFACE luca::ledger)

add_library(luca_reconciliation INTERFACE)
add_library(luca::reconciliation ALIAS luca_reconciliation)
set_target_properties(luca_reconciliation PROPERTIES EXPORT_NAME reconciliation)
target_compile_features(luca_reconciliation INTERFACE cxx_std_23)
target_include_directories(luca_reconciliation INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/reconciliation/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(luca_reconciliation INTERFACE
  luca::portfolio
  luca::ledger)

# Adapters are an explicit opt-in surface; the existing core target graph is unchanged.
add_library(luca_adapters INTERFACE)
add_library(luca::adapters ALIAS luca_adapters)
set_target_properties(luca_adapters PROPERTIES EXPORT_NAME adapters)
target_compile_features(luca_adapters INTERFACE cxx_std_23)
target_include_directories(luca_adapters INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/adapters/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(luca_adapters INTERFACE luca::reconciliation)

add_library(pacioli INTERFACE)
add_library(luca::luca ALIAS pacioli)
set_target_properties(pacioli PROPERTIES EXPORT_NAME luca)
target_compile_features(pacioli INTERFACE cxx_std_23)
target_include_directories(pacioli INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(pacioli INTERFACE
  luca::ledger
  luca::portfolio
  luca::reconciliation)

install(
  TARGETS
    luca_ledger
    luca_accounting
    luca_portfolio
    luca_reconciliation
    luca_adapters
    pacioli
  EXPORT LucaTargets)
install(
  DIRECTORY
    libs/ledger/include/
    libs/accounting/include/
    libs/portfolio/include/
    libs/reconciliation/include/
    libs/adapters/include/
    include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.hpp")
install(
  EXPORT LucaTargets
  FILE LucaTargets.cmake
  NAMESPACE luca::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Luca)

configure_package_config_file(
  cmake/LucaConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/LucaConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Luca)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/LucaConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(
  FILES
    ${CMAKE_CURRENT_BINARY_DIR}/LucaConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/LucaConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Luca)

option(LUCA_BUILD_INGESTION "Build optional native SQLite ingestion runtime" OFF)
if(LUCA_BUILD_INGESTION)
  if(NOT UNIX)
    message(FATAL_ERROR "Native ingestion currently requires POSIX local-filesystem semantics")
  endif()
  find_package(SQLite3 3.38 REQUIRED)
  add_subdirectory(libs/ingestion)
endif()

if(LUCA_BUILD_TOOLS)
  add_subdirectory(tools/exact-cash)
endif()

if(LUCA_BUILD_TESTS)
  enable_testing()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  find_package(Threads REQUIRED)
  if(UNIX)
    add_test(
      NAME luca_durable_intake_test
      COMMAND ${Python3_EXECUTABLE} -B -m unittest discover
        -s ${CMAKE_CURRENT_SOURCE_DIR}/tests/ingestion -p "test_*.py")
    set_tests_properties(luca_durable_intake_test PROPERTIES TIMEOUT 120)
    if(TARGET luca-ingest)
      set_tests_properties(luca_durable_intake_test PROPERTIES ENVIRONMENT "LUCA_INGESTION_CLI=$<TARGET_FILE:luca-ingest>")
    endif()
  endif()
  if(LUCA_BUILD_INGESTION)
    add_executable(luca_native_ingestion_test tests/unit/native_ingestion_test.cpp)
    target_link_libraries(luca_native_ingestion_test PRIVATE luca::ingestion_sqlite SQLite::SQLite3)
    target_compile_definitions(luca_native_ingestion_test PRIVATE LUCA_NATIVE_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/tests/ingestion/fixtures/submission-fingerprints.json")
    add_test(NAME luca_native_ingestion_test COMMAND luca_native_ingestion_test)
    add_executable(luca_policy_test tests/unit/policy_test.cpp)
    target_link_libraries(luca_policy_test PRIVATE luca::ingestion_sqlite)
    add_test(NAME luca_policy_test COMMAND luca_policy_test)
    set_tests_properties(luca_native_ingestion_test PROPERTIES TIMEOUT 180)
    if(TARGET luca-ingest)
      add_test(NAME luca_native_ingestion_consumer_test
        COMMAND ${CMAKE_COMMAND}
          "-DLUCA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
          "-DLUCA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
          "-DNATIVE_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/native-package-consumer"
          "-DGENERATOR=${CMAKE_GENERATOR}"
          "-DCOMPILER=${CMAKE_CXX_COMPILER}"
          "-DBUILD_TYPE=${CMAKE_BUILD_TYPE}"
          "-DDEPENDENCY_PREFIX=${CMAKE_PREFIX_PATH}"
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ingestion/package_consumer_test.cmake)
      set_tests_properties(luca_native_ingestion_consumer_test PROPERTIES TIMEOUT 120)
    endif()
  endif()
  add_executable(luca_lots_test tests/unit/lots_test.cpp)
  target_link_libraries(luca_lots_test PRIVATE luca::accounting luca::adapters Threads::Threads)
  target_compile_definitions(luca_lots_test PRIVATE LUCA_LOT_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/lifo-lots/trades.tsv" LUCA_DATED_VOID_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/dated-void/queries.tsv")
  add_test(NAME luca_lots_test COMMAND luca_lots_test)

  add_executable(luca_prepared_admission_test tests/unit/prepared_admission_test.cpp)
  target_link_libraries(luca_prepared_admission_test PRIVATE luca::adapters)
  add_test(NAME luca_prepared_admission_test COMMAND luca_prepared_admission_test)

  add_executable(luca_journal_balances_test tests/unit/journal_balances_test.cpp)
  target_link_libraries(luca_journal_balances_test PRIVATE luca::accounting Threads::Threads)
  target_compile_definitions(luca_journal_balances_test PRIVATE
    LUCA_JOURNAL_BALANCE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/journal-balances/walkthrough.tsv")
  add_test(NAME luca_journal_balances_test COMMAND luca_journal_balances_test)
  add_executable(pacioli_smoke_test tests/smoke.cpp)
  target_link_libraries(pacioli_smoke_test PRIVATE pacioli)
  add_test(NAME pacioli_smoke_test COMMAND pacioli_smoke_test)

  add_executable(pacioli_core_values_test tests/unit/core_values_test.cpp)
  target_link_libraries(pacioli_core_values_test PRIVATE pacioli)
  add_test(NAME pacioli_core_values_test COMMAND pacioli_core_values_test)

  add_executable(pacioli_canonical_serialization_test
    tests/unit/canonical_serialization_test.cpp)
  target_link_libraries(pacioli_canonical_serialization_test PRIVATE luca::luca)
  add_test(NAME pacioli_canonical_serialization_test
    COMMAND pacioli_canonical_serialization_test)

  add_executable(pacioli_lifecycle_serialization_test
    tests/unit/lifecycle_serialization_test.cpp)
  target_link_libraries(pacioli_lifecycle_serialization_test PRIVATE luca::luca)
  add_test(NAME pacioli_lifecycle_serialization_test
    COMMAND pacioli_lifecycle_serialization_test)

  add_executable(pacioli_portfolio_serialization_test
    tests/unit/portfolio_serialization_test.cpp)
  target_link_libraries(pacioli_portfolio_serialization_test PRIVATE luca::portfolio)
  add_test(NAME pacioli_portfolio_serialization_test
    COMMAND pacioli_portfolio_serialization_test)

  add_executable(pacioli_checkpoint_manifest_serialization_test
    tests/unit/checkpoint_manifest_serialization_test.cpp)
  target_compile_definitions(pacioli_checkpoint_manifest_serialization_test PRIVATE
    "LUCA_CHECKPOINT_MANIFEST_FIXTURE_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/serialization-checkpoints/valid-append.json\"")
  target_link_libraries(pacioli_checkpoint_manifest_serialization_test
    PRIVATE luca::portfolio)
  add_test(NAME pacioli_checkpoint_manifest_serialization_test
    COMMAND pacioli_checkpoint_manifest_serialization_test)

  add_executable(pacioli_checkpoint_decode_test
    tests/unit/checkpoint_decode_test.cpp)
  target_compile_definitions(pacioli_checkpoint_decode_test PRIVATE
    "LUCA_CHECKPOINT_DECODE_FIXTURE_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/serialization-checkpoints/valid-append.json\""
    "LUCA_CHECKPOINT_DECODE_LATE_FIXTURE_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/serialization-checkpoints/late-correction.json\"")
  target_link_libraries(pacioli_checkpoint_decode_test PRIVATE luca::portfolio)
  add_test(NAME pacioli_checkpoint_decode_test
    COMMAND pacioli_checkpoint_decode_test)

  add_executable(pacioli_checkpoint_resume_test
    tests/unit/checkpoint_resume_test.cpp)
  target_link_libraries(pacioli_checkpoint_resume_test PRIVATE luca::portfolio)
  add_test(NAME pacioli_checkpoint_resume_test
    COMMAND pacioli_checkpoint_resume_test)

  add_executable(pacioli_checkpoint_apply_test
    tests/unit/checkpoint_apply_test.cpp)
  target_link_libraries(pacioli_checkpoint_apply_test PRIVATE luca::portfolio)
  add_test(NAME pacioli_checkpoint_apply_test
    COMMAND pacioli_checkpoint_apply_test)

  add_executable(pacioli_checkpoint_result_test tests/unit/checkpoint_result_test.cpp)
  target_link_libraries(pacioli_checkpoint_result_test PRIVATE luca::portfolio)
  add_test(NAME pacioli_checkpoint_result_test COMMAND pacioli_checkpoint_result_test)

  add_executable(pacioli_settlement_date_projection_test
    tests/unit/settlement_date_projection_test.cpp)
  target_link_libraries(pacioli_settlement_date_projection_test PRIVATE luca::portfolio)
  add_test(NAME pacioli_settlement_date_projection_test
    COMMAND pacioli_settlement_date_projection_test)

  add_executable(luca_financial_algebra_test tests/unit/financial_algebra_test.cpp)
  target_link_libraries(luca_financial_algebra_test PRIVATE luca::reconciliation)
  add_test(NAME luca_financial_algebra_test COMMAND luca_financial_algebra_test)

  add_executable(luca_trade_reconciliation_test tests/unit/trade_reconciliation_test.cpp)
  target_link_libraries(luca_trade_reconciliation_test PRIVATE luca::reconciliation)
  add_test(NAME luca_trade_reconciliation_test COMMAND luca_trade_reconciliation_test)

  add_executable(luca_trade_csv_test tests/unit/trade_csv_test.cpp)
  target_link_libraries(luca_trade_csv_test PRIVATE luca::adapters)
  target_compile_definitions(luca_trade_csv_test PRIVATE
    LUCA_TRADE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/trade-reconciliation")
  add_test(NAME luca_trade_csv_test COMMAND luca_trade_csv_test)

  add_executable(pacioli_provenance_test tests/unit/provenance_test.cpp)
  target_link_libraries(pacioli_provenance_test PRIVATE pacioli)
  add_test(NAME pacioli_provenance_test COMMAND pacioli_provenance_test)

  add_executable(pacioli_economic_event_test tests/unit/economic_event_test.cpp)
  target_link_libraries(pacioli_economic_event_test PRIVATE pacioli)
  add_test(NAME pacioli_economic_event_test COMMAND pacioli_economic_event_test)

  add_executable(pacioli_ledger_test tests/unit/ledger_test.cpp)
  target_link_libraries(pacioli_ledger_test PRIVATE pacioli)
  add_test(NAME pacioli_ledger_test COMMAND pacioli_ledger_test)

  add_executable(pacioli_event_lifecycle_test tests/unit/event_lifecycle_test.cpp)
  target_link_libraries(pacioli_event_lifecycle_test PRIVATE pacioli)
  add_test(NAME pacioli_event_lifecycle_test COMMAND pacioli_event_lifecycle_test)

  add_executable(pacioli_lifecycle_projection_test tests/unit/lifecycle_projection_test.cpp)
  target_link_libraries(pacioli_lifecycle_projection_test PRIVATE pacioli)
  add_test(NAME pacioli_lifecycle_projection_test COMMAND pacioli_lifecycle_projection_test)

  add_executable(pacioli_position_projection_test tests/unit/position_projection_test.cpp)
  target_link_libraries(pacioli_position_projection_test PRIVATE pacioli)
  add_test(NAME pacioli_position_projection_test COMMAND pacioli_position_projection_test)

  add_executable(pacioli_position_reconciliation_test tests/unit/position_reconciliation_test.cpp)
  target_link_libraries(pacioli_position_reconciliation_test PRIVATE pacioli)
  add_test(NAME pacioli_position_reconciliation_test COMMAND pacioli_position_reconciliation_test)

  add_executable(pacioli_cash_reconciliation_test tests/unit/cash_reconciliation_test.cpp)
  target_link_libraries(pacioli_cash_reconciliation_test PRIVATE pacioli)
  add_test(NAME pacioli_cash_reconciliation_test COMMAND pacioli_cash_reconciliation_test)

  add_executable(pacioli_cash_projection_test tests/unit/cash_projection_test.cpp)
  target_link_libraries(pacioli_cash_projection_test PRIVATE pacioli)
  add_test(NAME pacioli_cash_projection_test COMMAND pacioli_cash_projection_test)

  add_executable(pacioli_exact_cash_reduction_test
    tests/unit/exact_cash_reduction_test.cpp)
  target_link_libraries(pacioli_exact_cash_reduction_test PRIVATE luca::portfolio)
  add_test(NAME pacioli_exact_cash_reduction_test
    COMMAND pacioli_exact_cash_reduction_test)

  add_executable(pacioli_exact_cash_comparison_test
    tests/unit/exact_cash_comparison_test.cpp)
  target_link_libraries(pacioli_exact_cash_comparison_test
    PRIVATE luca::reconciliation)
  add_test(NAME pacioli_exact_cash_comparison_test
    COMMAND pacioli_exact_cash_comparison_test)

  if(TARGET luca-exact-cash)
    add_test(
      NAME luca_exact_cash_cli_test
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli/exact_cash_cli_test.py
        $<TARGET_FILE:luca-exact-cash>
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/exact-cash-cli)
    add_test(
      NAME luca_exact_cash_python_test
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli/exact_cash_python_test.py
        $<TARGET_FILE:luca-exact-cash>
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/exact-cash-cli
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/exact-cash/python)
  endif()

  add_executable(pacioli_settlement_projection_test tests/unit/settlement_projection_test.cpp)
  target_link_libraries(pacioli_settlement_projection_test PRIVATE pacioli)
  add_test(NAME pacioli_settlement_projection_test COMMAND pacioli_settlement_projection_test)

  add_test(
    NAME pacioli_conformance_test
    COMMAND ${Python3_EXECUTABLE} -m unittest discover -s tests/conformance -p "test_*.py"
  )
  set_tests_properties(pacioli_conformance_test PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  set(CONFORMANCE_ARTIFACT ${CMAKE_CURRENT_BINARY_DIR}/conformance/fixtures.tsv)
  file(GLOB_RECURSE CONFORMANCE_FIXTURES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/*.json)
  add_custom_command(
    OUTPUT ${CONFORMANCE_ARTIFACT}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/generate_engine_fixture.py
      --root ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance
      --output ${CONFORMANCE_ARTIFACT}
    DEPENDS ${CONFORMANCE_FIXTURES}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/harness.py
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/generate_engine_fixture.py
    VERBATIM)
  add_executable(pacioli_conformance_engine_test
    tests/conformance/engine/conformance_engine_test.cpp
    ${CONFORMANCE_ARTIFACT})
  target_link_libraries(pacioli_conformance_engine_test PRIVATE pacioli)
  add_test(NAME pacioli_conformance_engine_test
    COMMAND pacioli_conformance_engine_test ${CONFORMANCE_ARTIFACT})

  add_test(
    NAME luca_package_consumer_test
    COMMAND ${CMAKE_COMMAND}
      -DLUCA_SOURCE_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -DLUCA_CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/package_consumer
      -DLUCA_CONSUMER_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/package_consumer
      -DLUCA_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/package_consumer_prefix
      -DLUCA_GENERATOR=${CMAKE_GENERATOR}
      -DLUCA_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
      -DLUCA_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}
      -DLUCA_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DLUCA_BUILD_CONFIG=$<CONFIG>
      -DLUCA_CTEST_COMMAND=${CMAKE_CTEST_COMMAND}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/package_consumer/run_test.cmake)

  add_test(
    NAME luca_package_options_test
    COMMAND ${CMAKE_COMMAND}
      -DLUCA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -DLUCA_OPTIONS_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/package_options
      -DLUCA_GENERATOR=${CMAKE_GENERATOR}
      -DLUCA_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
      -DLUCA_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}
      -DLUCA_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/parent_consumer/options.cmake)

  add_test(
    NAME luca_parent_consumer_test
    COMMAND ${CMAKE_COMMAND}
      -DLUCA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -DLUCA_PARENT_CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/parent_consumer
      -DLUCA_PARENT_CONSUMER_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/parent_consumer
      -DLUCA_GENERATOR=${CMAKE_GENERATOR}
      -DLUCA_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
      -DLUCA_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}
      -DLUCA_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DLUCA_BUILD_CONFIG=$<CONFIG>
      -DLUCA_CTEST_COMMAND=${CMAKE_CTEST_COMMAND}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/parent_consumer/run_test.cmake)

  # All financial test executables must retain their checks in optimized builds.
  # Apply this after registration so older assertion-based suites are included.
  get_property(luca_assertion_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(luca_test_target IN LISTS luca_assertion_targets)
    if(luca_test_target MATCHES "_test$")
      target_compile_options(${luca_test_target} PRIVATE
        "$<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>"
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>")
    endif()
  endforeach()
endif()

if(LUCA_BUILD_BENCHMARKS)
  add_executable(pacioli_benchmarks
    tests/benchmarks/benchmark_main.cpp
    tests/benchmarks/ledger_benchmark.cpp
    tests/benchmarks/projection_benchmark.cpp
    tests/benchmarks/reconciliation_benchmark.cpp
    tests/benchmarks/journal_balance_benchmark.cpp)
  target_link_libraries(pacioli_benchmarks PRIVATE pacioli luca::accounting)
  target_compile_definitions(pacioli_benchmarks PRIVATE
    LUCA_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    LUCA_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()

if(LUCA_BUILD_BENCHMARKS)
  add_executable(luca_lot_benchmark tests/benchmarks/lot_benchmark.cpp)
  target_link_libraries(luca_lot_benchmark PRIVATE luca::accounting)
endif()
