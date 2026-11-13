# Benchmarks: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLIBPG_BENCH=ON; run with bench/run.sh.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  message(WARNING "libpg bench: CMAKE_BUILD_TYPE is '${CMAKE_BUILD_TYPE}'; numbers are only meaningful with Release")
endif()

add_executable(pg_bench
  pg_bench.cpp
  harness.cpp
  scn_raw.cpp
  scn_pool.cpp
  scn_v2.cpp
  scn_list.cpp
  scn_real.cpp
  summarize.cpp
)
target_link_libraries(pg_bench PRIVATE pg)
# The write-back scenarios exist only when the write-back library is built.
option(LIBPG_BENCH_WRITEBACK "Build the write-back scenarios" ON)
if(LIBPG_BENCH_WRITEBACK AND TARGET pg_writeback)
  target_sources(pg_bench PRIVATE scn_wb.cpp)
  target_link_libraries(pg_bench PRIVATE pg_writeback)
  target_compile_definitions(pg_bench PRIVATE BENCH_WRITEBACK)
endif()
target_compile_options(pg_bench PRIVATE -Wall -Wextra -Wpedantic)
