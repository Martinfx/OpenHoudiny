cmake_minimum_required(VERSION 3.20)
project(pgcore LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Prototype of the procedural-geometry core.
#
# Deliberately dependency-free: C++20 and the standard library only, so that
# `cmake -B build && cmake --build build && ctest --test-dir build` works on a
# bare machine. Production targets (TBB, OpenVDB, USD, LLVM) are listed in
# ARCHITECTURE.md; none of them are needed to evaluate the core invariants.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(PG_SANITIZE "Build with address+undefined sanitizers" OFF)
option(PG_SANITIZE_THREAD "Build with the thread sanitizer" OFF)

add_library(pgcore
  src/pg/core/Types.cpp
  src/pg/core/Attribute.cpp
  src/pg/core/Geometry.cpp
  src/pg/core/Parallel.cpp
  src/pg/core/Node.cpp
  src/pg/core/Graph.cpp
  src/pg/core/CookEngine.cpp
  src/pg/nodes/Generators.cpp
  src/pg/nodes/Modifiers.cpp
  src/pg/nodes/Expression.cpp
  src/pg/nodes/Wrangle.cpp
)
target_include_directories(pgcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
find_package(Threads REQUIRED)
target_link_libraries(pgcore PUBLIC Threads::Threads)

if(MSVC)
  target_compile_options(pgcore PRIVATE /W4)
else()
  target_compile_options(pgcore PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PG_SANITIZE)
  target_compile_options(pgcore PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(pgcore PUBLIC -fsanitize=address,undefined)
endif()
if(PG_SANITIZE_THREAD)
  target_compile_options(pgcore PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(pgcore PUBLIC -fsanitize=thread)
endif()

# --- tests -----------------------------------------------------------------
enable_testing()
add_executable(pgtests
  tests/test_main.cpp
  tests/test_attribute_cow.cpp
  tests/test_geometry.cpp
  tests/test_cook.cpp
  tests/test_cache.cpp
  tests/test_determinism.cpp
  tests/test_expression.cpp
)
target_link_libraries(pgtests PRIVATE pgcore)
add_test(NAME pgtests COMMAND pgtests)

# --- benchmark + demo ------------------------------------------------------
add_executable(pgbench bench/bench_main.cpp)
target_link_libraries(pgbench PRIVATE pgcore)

add_executable(pgdemo cli/demo_main.cpp)
target_link_libraries(pgdemo PRIVATE pgcore)
