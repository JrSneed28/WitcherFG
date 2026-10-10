cmake_minimum_required(VERSION 3.24)
project(WitcherFG LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "WitcherFG requires 64-bit MSVC")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(wfg_launcher INTERFACE)
set_target_properties(wfg_launcher PROPERTIES OUTPUT_NAME WitcherFG)
target_include_directories(wfg_launcher INTERFACE include)

add_executable(wfg_tests src/pe.cpp tests/test_main.cpp tests/pe_tests.cpp)
target_include_directories(wfg_tests PRIVATE include tests)
target_compile_options(wfg_tests PRIVATE /W4 /permissive-)

enable_testing()
foreach(group IN ITEMS pe compatibility process debug transaction launcher)
  add_test(NAME wfg_${group} COMMAND wfg_tests --filter ${group})
  set_tests_properties(wfg_${group} PROPERTIES LABELS ${group})
endforeach()
