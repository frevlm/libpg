cmake_minimum_required(VERSION 3.25)
project(libpg VERSION 0.4.0 LANGUAGES CXX)

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
  src/error.cpp
  src/migrate.cpp
  src/pool.cpp
  src/sql.cpp
)
target_include_directories(pg PUBLIC include)
target_compile_features(pg PUBLIC cxx_std_20)
target_link_libraries(pg PUBLIC PostgreSQL::PostgreSQL Threads::Threads)
target_compile_options(pg PRIVATE -Wall -Wextra -Wpedantic)

# The write-back cache (include/pg/writeback.hpp): counters in memory, postings
# written in batches by one background thread.
add_library(pg_writeback src/writeback.cpp)
target_link_libraries(pg_writeback PUBLIC pg)
target_compile_options(pg_writeback PRIVATE -Wall -Wextra -Wpedantic)

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
