find_package(Catch2 3 QUIET)
find_program(LIBPG_PG_CTL pg_ctl HINTS /opt/homebrew/opt/postgresql@18/bin)
if(NOT Catch2_FOUND OR NOT LIBPG_PG_CTL)
  message(STATUS "libpg tests skipped: need Catch2 3 and pg_ctl (found Catch2=${Catch2_FOUND}, pg_ctl=${LIBPG_PG_CTL})")
  return()
endif()

# A throwaway cluster for the whole run: started by the setup fixture, removed
# by the cleanup fixture. Tests read LIBPG_TEST_DSN.
set(LIBPG_TEST_PORT 54329 CACHE STRING "Port of the test cluster")
set(LIBPG_TEST_DIR ${CMAKE_CURRENT_BINARY_DIR}/pgdata)
get_filename_component(LIBPG_PG_BINDIR "${LIBPG_PG_CTL}" DIRECTORY)
set(LIBPG_TEST_DSN "postgresql://postgres@127.0.0.1:${LIBPG_TEST_PORT}/libpg_test")

add_test(NAME pg_start COMMAND ${PROJECT_SOURCE_DIR}/scripts/test-pg.sh start ${LIBPG_TEST_DIR} ${LIBPG_TEST_PORT} ${LIBPG_PG_BINDIR})
add_test(NAME pg_stop COMMAND ${PROJECT_SOURCE_DIR}/scripts/test-pg.sh stop ${LIBPG_TEST_DIR} ${LIBPG_TEST_PORT} ${LIBPG_PG_BINDIR})
set_tests_properties(pg_start PROPERTIES FIXTURES_SETUP pg)
set_tests_properties(pg_stop PROPERTIES FIXTURES_CLEANUP pg)

# Each test file is one executable; add it here.
function(libpg_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE pg Catch2::Catch2WithMain)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED pg ENVIRONMENT "LIBPG_TEST_DSN=${LIBPG_TEST_DSN}")
endfunction()

# pg_tablegen: the generator reproduces the checked-in header, and what it
# writes for the real projects' schemas compiles.
set(LIBPG_TABLEGEN_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/tablegen_compile.cmake)
add_test(NAME tablegen_fixture COMMAND ${CMAKE_COMMAND}
  -DTABLEGEN=$<TARGET_FILE:pg_tablegen> -DDSN=${LIBPG_TEST_DSN}
  -DSCHEMA=${CMAKE_CURRENT_SOURCE_DIR}/fixture_schema.sql
  -DOUT=${CMAKE_CURRENT_BINARY_DIR}/tablegen_fixture/fixture_tables.hpp
  -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/generated/fixture_tables.hpp
  -DPSQL=${LIBPG_PG_BINDIR}/psql
  -P ${LIBPG_TABLEGEN_SCRIPT})
function(tablegen_compile_test project schema)
  add_test(NAME tablegen_compile_${project} COMMAND ${CMAKE_COMMAND}
    -DTABLEGEN=$<TARGET_FILE:pg_tablegen> -DDSN=${LIBPG_TEST_DSN}
    -DSCHEMA=${schema}
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/tablegen_${project}/${project}_tables.hpp
    -DCXX=${CMAKE_CXX_COMPILER} -DINCLUDE_DIR=${PROJECT_SOURCE_DIR}/include
    -P ${LIBPG_TABLEGEN_SCRIPT})
endfunction()
set(LIBPG_REVLM_SCHEMA /Users/realm/revlm/backend/schema.sql CACHE FILEPATH "revlm schema for the tablegen compile test")
set(LIBPG_HHHOJ_SCHEMA /Users/realm/HHHOJ/server/schema.sql CACHE FILEPATH "HHHOJ schema for the tablegen compile test")
set(tablegen_tests tablegen_fixture)
foreach(project revlm HHHOJ)
  string(TOUPPER ${project} upper)
  if(EXISTS "${LIBPG_${upper}_SCHEMA}")
    tablegen_compile_test(${project} "${LIBPG_${upper}_SCHEMA}")
    list(APPEND tablegen_tests tablegen_compile_${project})
  endif()
endforeach()
set_tests_properties(${tablegen_tests} PROPERTIES FIXTURES_REQUIRED pg)

libpg_test(codec_test)
libpg_test(query_test)
libpg_test(pool_test)
libpg_test(sql_test)
libpg_test(cached_test)
libpg_test(cached_exit_test)
libpg_test(batch_test)
