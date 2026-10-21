foreach(required_variable IN ITEMS
    LUCA_SOURCE_DIR
    LUCA_PARENT_CONSUMER_SOURCE_DIR
    LUCA_PARENT_CONSUMER_BINARY_DIR
    LUCA_GENERATOR
    LUCA_CXX_COMPILER
    LUCA_CTEST_COMMAND)
  if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

set(opt_in_binary_dir "${LUCA_PARENT_CONSUMER_BINARY_DIR}_with_luca_tests")
file(REMOVE_RECURSE
  "${LUCA_PARENT_CONSUMER_BINARY_DIR}"
  "${opt_in_binary_dir}")

set(configure_command
  "${CMAKE_COMMAND}"
  -S "${LUCA_PARENT_CONSUMER_SOURCE_DIR}"
  -B "${LUCA_PARENT_CONSUMER_BINARY_DIR}"
  -G "${LUCA_GENERATOR}"
  "-DLUCA_SOURCE_DIR=${LUCA_SOURCE_DIR}"
  "-DCMAKE_CXX_COMPILER=${LUCA_CXX_COMPILER}")
if(DEFINED LUCA_GENERATOR_PLATFORM AND NOT "${LUCA_GENERATOR_PLATFORM}" STREQUAL "")
  list(APPEND configure_command -A "${LUCA_GENERATOR_PLATFORM}")
endif()
if(DEFINED LUCA_GENERATOR_TOOLSET AND NOT "${LUCA_GENERATOR_TOOLSET}" STREQUAL "")
  list(APPEND configure_command -T "${LUCA_GENERATOR_TOOLSET}")
endif()
if(DEFINED LUCA_BUILD_CONFIG AND NOT "${LUCA_BUILD_CONFIG}" STREQUAL "")
  list(APPEND configure_command "-DCMAKE_BUILD_TYPE=${LUCA_BUILD_CONFIG}")
endif()

execute_process(
  COMMAND ${configure_command}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR
    "Parent consumer configure failed (${configure_result})\n"
    "${configure_output}\n${configure_error}")
endif()
if("${configure_output}\n${configure_error}" MATCHES "Python3")
  message(FATAL_ERROR
    "Default parent configure unexpectedly mentioned Python3\n"
    "${configure_output}\n${configure_error}")
endif()

execute_process(
  COMMAND "${LUCA_CTEST_COMMAND}"
    --test-dir "${LUCA_PARENT_CONSUMER_BINARY_DIR}"
    --build-config "${LUCA_BUILD_CONFIG}" -N
  RESULT_VARIABLE inventory_result
  OUTPUT_VARIABLE inventory_output
  ERROR_VARIABLE inventory_error)
if(NOT inventory_result EQUAL 0)
  message(FATAL_ERROR
    "Parent test inventory failed (${inventory_result})\n"
    "${inventory_output}\n${inventory_error}")
endif()
foreach(parent_test IN ITEMS
    luca_parent_journal_balances_run
    luca_parent_consumer_run
    luca_parent_ledger_consumer_run
    luca_parent_portfolio_consumer_run
    luca_parent_reconciliation_consumer_run
    luca_parent_exact_cash_consumer_run
    luca_parent_exact_cash_comparison_consumer_run
    luca_parent_legacy_consumer_run
    luca_parent_adapters_consumer_run)
  if(NOT inventory_output MATCHES "${parent_test}")
    message(FATAL_ERROR
      "Parent default test inventory is missing ${parent_test}\n"
      "${inventory_output}\n${inventory_error}")
  endif()
endforeach()
if(NOT inventory_output MATCHES "Total Tests: 9")
  message(FATAL_ERROR
    "Parent default test inventory was not limited to its nine consumer tests\n"
    "${inventory_output}\n${inventory_error}")
endif()
if(inventory_output MATCHES "pacioli_|luca_package_consumer|benchmark")
  message(FATAL_ERROR
    "Parent default test inventory contains nested LUCA tests\n"
    "${inventory_output}\n${inventory_error}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${LUCA_PARENT_CONSUMER_BINARY_DIR}"
    --config "${LUCA_BUILD_CONFIG}"
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR
    "Parent consumer build failed (${build_result})\n${build_output}\n${build_error}")
endif()

execute_process(
  COMMAND "${LUCA_CTEST_COMMAND}"
    --test-dir "${LUCA_PARENT_CONSUMER_BINARY_DIR}"
    --build-config "${LUCA_BUILD_CONFIG}" --output-on-failure
  RESULT_VARIABLE test_result
  OUTPUT_VARIABLE test_output
  ERROR_VARIABLE test_error)
if(NOT test_result EQUAL 0)
  message(FATAL_ERROR
    "Parent consumer execution failed (${test_result})\n${test_output}\n${test_error}")
endif()

set(opt_in_configure_command
  "${CMAKE_COMMAND}"
  -S "${LUCA_PARENT_CONSUMER_SOURCE_DIR}"
  -B "${opt_in_binary_dir}"
  -G "${LUCA_GENERATOR}"
  "-DLUCA_SOURCE_DIR=${LUCA_SOURCE_DIR}"
  "-DCMAKE_CXX_COMPILER=${LUCA_CXX_COMPILER}"
  -DPACIOLI_BUILD_TESTS=ON
  -DLUCA_PARENT_EXPECT_TESTS=ON
  -DLUCA_PARENT_CONSUMER_ALLOW_PYTHON=ON)
if(DEFINED LUCA_GENERATOR_PLATFORM AND NOT "${LUCA_GENERATOR_PLATFORM}" STREQUAL "")
  list(APPEND opt_in_configure_command -A "${LUCA_GENERATOR_PLATFORM}")
endif()
if(DEFINED LUCA_GENERATOR_TOOLSET AND NOT "${LUCA_GENERATOR_TOOLSET}" STREQUAL "")
  list(APPEND opt_in_configure_command -T "${LUCA_GENERATOR_TOOLSET}")
endif()
if(DEFINED LUCA_BUILD_CONFIG AND NOT "${LUCA_BUILD_CONFIG}" STREQUAL "")
  list(APPEND opt_in_configure_command "-DCMAKE_BUILD_TYPE=${LUCA_BUILD_CONFIG}")
endif()
execute_process(
  COMMAND ${opt_in_configure_command}
  RESULT_VARIABLE opt_in_configure_result
  OUTPUT_VARIABLE opt_in_configure_output
  ERROR_VARIABLE opt_in_configure_error)
if(NOT opt_in_configure_result EQUAL 0)
  message(FATAL_ERROR
    "Opt-in parent configure failed (${opt_in_configure_result})\n"
    "${opt_in_configure_output}\n${opt_in_configure_error}")
endif()

execute_process(
  COMMAND "${LUCA_CTEST_COMMAND}" --test-dir "${opt_in_binary_dir}"
    --build-config "${LUCA_BUILD_CONFIG}" -N
  RESULT_VARIABLE opt_in_inventory_result
  OUTPUT_VARIABLE opt_in_inventory_output
  ERROR_VARIABLE opt_in_inventory_error)
if(NOT opt_in_inventory_result EQUAL 0)
  message(FATAL_ERROR
    "Opt-in parent test inventory failed (${opt_in_inventory_result})\n"
    "${opt_in_inventory_output}\n${opt_in_inventory_error}")
endif()
foreach(expected_test IN ITEMS
    pacioli_smoke_test
    pacioli_conformance_test
    luca_package_consumer_test)
  if(NOT opt_in_inventory_output MATCHES "${expected_test}")
    message(FATAL_ERROR
      "Opt-in parent test inventory is missing ${expected_test}\n"
      "${opt_in_inventory_output}\n${opt_in_inventory_error}")
  endif()
endforeach()

message(STATUS
  "Parent consumer passed without Python discovery or nested LUCA tests\n"
  "${test_output}\n"
  "Opt-in parent configuration exposed the LUCA test suite")
