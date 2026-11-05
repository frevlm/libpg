# Benchmarks: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLIBPG_BENCH=ON; run with bench/run.sh.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  message(WARNING "libpg bench: CMAKE_BUILD_TYPE is '${CMAKE_BUILD_TYPE}'; numbers are only meaningful with Release")
endif()

add_executable(pg_bench
  pg_bench.cpp
  harness.cpp
  scn_raw.cpp
  scn_v1.cpp
  summarize.cpp
)
target_link_libraries(pg_bench PRIVATE pg)
target_compile_options(pg_bench PRIVATE -Wall -Wextra -Wpedantic)
