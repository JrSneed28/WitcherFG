cmake_minimum_required(VERSION 3.24)
project(WitcherFG LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "WitcherFG requires 64-bit MSVC")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
set(WFG_GENERATED_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
add_custom_target(wfg_build_identity
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_identity.py"
    --metadata "${CMAKE_CURRENT_SOURCE_DIR}/release/project.toml"
    --root "${CMAKE_CURRENT_SOURCE_DIR}" --build-type "$<CONFIG>"
    --output "${WFG_GENERATED_DIRECTORY}/wfg/build_info.hpp"
  BYPRODUCTS "${WFG_GENERATED_DIRECTORY}/wfg/build_info.hpp" "${WFG_GENERATED_DIRECTORY}/wfg/build_info.json"
  VERBATIM)

add_library(wfg_core STATIC src/pe.cpp src/compatibility.cpp src/configuration.cpp src/diagnostics.cpp src/windows_process.cpp
  src/debug_session.cpp src/transaction.cpp src/runtime_observation.cpp)
add_dependencies(wfg_core wfg_build_identity)
target_include_directories(wfg_core PUBLIC include "${WFG_GENERATED_DIRECTORY}")
target_link_libraries(wfg_core PRIVATE bcrypt advapi32 psapi shlwapi xmllite ole32 wintrust crypt32 version)
target_compile_options(wfg_core PRIVATE /W4 /permissive-)
target_compile_definitions(wfg_core PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)

add_library(wfg_support STATIC src/support_bundle.cpp src/support_storage_windows.cpp)
target_include_directories(wfg_support PUBLIC include)
target_link_libraries(wfg_support PUBLIC wfg_core PRIVATE shell32 ole32)
target_compile_options(wfg_support PRIVATE /W4 /permissive-)
target_compile_definitions(wfg_support PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)

add_executable(wfg_launcher src/main.cpp)
set_target_properties(wfg_launcher PROPERTIES OUTPUT_NAME WitcherFG)
target_link_libraries(wfg_launcher PRIVATE wfg_core wfg_support)
target_compile_options(wfg_launcher PRIVATE /W4 /permissive-)

add_executable(wfg_tests tests/test_main.cpp tests/pe_tests.cpp tests/core_tests.cpp tests/compatibility_tests.cpp
  tests/configuration_tests.cpp tests/diagnostics_tests.cpp tests/process_tests.cpp tests/debug_tests.cpp tests/observation_tests.cpp)
target_compile_definitions(wfg_tests PRIVATE WFG_CONFIG_SNAPSHOT)
target_include_directories(wfg_tests PRIVATE include tests)
target_link_libraries(wfg_tests PRIVATE wfg_core)
target_compile_options(wfg_tests PRIVATE /W4 /permissive-)

enable_testing()
foreach(group IN ITEMS pe compatibility process debug configuration diagnostics observation)
  add_test(NAME wfg_${group} COMMAND wfg_tests --filter ${group})
  set_tests_properties(wfg_${group} PROPERTIES LABELS ${group})
endforeach()
set_tests_properties(wfg_configuration PROPERTIES LABELS "configuration;config")

add_executable(wfg_support_tests tests/test_main.cpp tests/support_tests.cpp
  src/support_bundle.cpp src/support_storage_windows.cpp)
target_include_directories(wfg_support_tests PRIVATE include tests)
target_link_libraries(wfg_support_tests PRIVATE wfg_core shell32 ole32 advapi32)
target_compile_options(wfg_support_tests PRIVATE /W4 /permissive-)
target_compile_definitions(wfg_support_tests PRIVATE WFG_SUPPORT_TESTING
  _WIN32_WINNT=0x0A00 WINVER=0x0A00)
set(WFG_SUPPORT_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/support-fixture-$<CONFIG>.zip")
set(WFG_SUPPORT_NATIVE_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/support-native-fixture-$<CONFIG>.zip")
set(WFG_SUPPORT_TEST_ENV
  "WFG_SUPPORT_FIXTURE=${WFG_SUPPORT_FIXTURE}"
  "WFG_SUPPORT_NATIVE_FIXTURE=${WFG_SUPPORT_NATIVE_FIXTURE}")
add_test(NAME wfg_support COMMAND wfg_support_tests --filter support.)
set_tests_properties(wfg_support PROPERTIES LABELS support
  ENVIRONMENT "${WFG_SUPPORT_TEST_ENV}" FIXTURES_SETUP wfg_support_archives)
add_test(NAME wfg_support_zip COMMAND "${Python3_EXECUTABLE}" -m unittest discover
  -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p support_zip_tests.py)
set_tests_properties(wfg_support_zip PROPERTIES LABELS support
  ENVIRONMENT "${WFG_SUPPORT_TEST_ENV}" FIXTURES_REQUIRED wfg_support_archives)

add_executable(wfg_transaction_tests tests/test_main.cpp tests/transaction_tests.cpp)
target_include_directories(wfg_transaction_tests PRIVATE include tests)
target_link_libraries(wfg_transaction_tests PRIVATE wfg_core)
target_compile_options(wfg_transaction_tests PRIVATE /W4 /permissive-)
add_test(NAME wfg_transaction COMMAND wfg_transaction_tests --filter transaction)
set_tests_properties(wfg_transaction PROPERTIES LABELS transaction)

add_executable(wfg_debug_win32_boundary tests/test_main.cpp tests/fixtures/debug_win32_backend.cpp src/pe.cpp)
target_include_directories(wfg_debug_win32_boundary PRIVATE include tests)
target_compile_options(wfg_debug_win32_boundary PRIVATE /W4 /permissive-)
target_compile_definitions(wfg_debug_win32_boundary PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
add_test(NAME wfg_debug_win32_boundary COMMAND wfg_debug_win32_boundary --filter debug.native_)
set_tests_properties(wfg_debug_win32_boundary PROPERTIES LABELS debug)

add_executable(wfg_debug_api_smoke tests/fixtures/debug_api_smoke.cpp)
target_link_libraries(wfg_debug_api_smoke PRIVATE wfg_core)
target_compile_options(wfg_debug_api_smoke PRIVATE /W4 /permissive-)
add_test(NAME wfg_debug_api_smoke COMMAND wfg_debug_api_smoke)
set_tests_properties(wfg_debug_api_smoke PROPERTIES LABELS debug)

add_test(NAME wfg_build_identity COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_identity_tests.py"
  --native-runner "$<TARGET_FILE:wfg_tests>" --generated-json "${WFG_GENERATED_DIRECTORY}/wfg/build_info.json" BuildIdentityTests)
set_tests_properties(wfg_build_identity PROPERTIES LABELS "identity;diagnostics")
add_test(NAME wfg_configuration_snapshot COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_identity_tests.py"
  --native-runner "$<TARGET_FILE:wfg_tests>" NativeConfigurationTests)
set_tests_properties(wfg_configuration_snapshot PROPERTIES LABELS "configuration;config")

add_test(NAME wfg_runner_unknown_filter COMMAND ${CMAKE_COMMAND}
  -DTEST_EXECUTABLE=$<TARGET_FILE:wfg_tests> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/runner_tests.cmake)
set_tests_properties(wfg_runner_unknown_filter PROPERTIES LABELS pe)
