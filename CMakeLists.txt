cmake_minimum_required(VERSION 3.25)
project(libpg VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Homebrew's libpq is keg-only; look there when nothing else is given.
if(APPLE AND NOT DEFINED PostgreSQL_ROOT AND EXISTS /opt/homebrew/opt/libpq)
  set(PostgreSQL_ROOT /opt/homebrew/opt/libpq)
endif()
find_package(PostgreSQL REQUIRED)
find_package(Threads REQUIRED)

add_library(pg
  src/codec.cpp
  src/engine.cpp
  src/error.cpp
  src/libpg_schema.cpp
  src/migrate.cpp
  src/pool.cpp
  src/prefetch.cpp
  src/sha256.cpp
  src/sql.cpp
  src/writeback.cpp
  src/writeback_listen.cpp
  src/writeback_writer.cpp
)
target_include_directories(pg PUBLIC include)
target_compile_features(pg PUBLIC cxx_std_20)
target_link_libraries(pg PUBLIC PostgreSQL::PostgreSQL Threads::Threads)
target_compile_options(pg PRIVATE -Wall -Wextra -Wpedantic)
# Uses the libpq 14-16 calls (PQpipelineSync, DEALLOCATE) even when newer ones exist,
# to test that path.
option(LIBPG_FORCE_LIBPQ16_PATH "Use the pre-libpq-17 code path of the engine" OFF)
if(LIBPG_FORCE_LIBPQ16_PATH)
  target_compile_definitions(pg PRIVATE LIBPG_FORCE_LIBPQ16_PATH)
endif()

# Reads a schema.sql into a scratch schema of a live database and writes the
# column tags of its tables (CONTEXT.md, 列标签).
add_executable(pg_tablegen tools/pg_tablegen.cpp)
target_link_libraries(pg_tablegen PRIVATE PostgreSQL::PostgreSQL)
target_compile_options(pg_tablegen PRIVATE -Wall -Wextra -Wpedantic)

option(LIBPG_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
if(LIBPG_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

option(LIBPG_BENCH "Build benchmarks" OFF)
if(LIBPG_BENCH)
  add_subdirectory(bench)
endif()
