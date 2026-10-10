cmake_minimum_required(VERSION 3.24)
project(WitcherFG LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "WitcherFG requires 64-bit MSVC")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(wfg_core STATIC src/pe.cpp)
target_include_directories(wfg_core PUBLIC include)
target_compile_options(wfg_core PRIVATE /W4 /permissive-)

add_executable(wfg_launcher src/main.cpp)
set_target_properties(wfg_launcher PROPERTIES OUTPUT_NAME WitcherFG)
target_link_libraries(wfg_launcher PRIVATE wfg_core)
target_compile_options(wfg_launcher PRIVATE /W4 /permissive-)

add_executable(wfg_tests tests/test_main.cpp tests/pe_tests.cpp)
target_include_directories(wfg_tests PRIVATE include tests)
target_link_libraries(wfg_tests PRIVATE wfg_core)
target_compile_options(wfg_tests PRIVATE /W4 /permissive-)

enable_testing()
foreach(group IN ITEMS pe compatibility process debug transaction launcher)
  add_test(NAME wfg_${group} COMMAND wfg_tests --filter ${group})
  set_tests_properties(wfg_${group} PROPERTIES LABELS ${group})
endforeach()
