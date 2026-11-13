# The tests run on a throwaway local cluster, or with LIBPG_TEST_SERVER (a
# URI without the database, e.g. postgresql://libpg@db) on a database
# libpg_test that the setup fixture recreates on that server. Tests read
# LIBPG_TEST_DSN.
set(LIBPG_TEST_SERVER "" CACHE STRING "URI (no database) of an existing server for the tests; empty = a local cluster")
find_package(Catch2 3 QUIET)
if(LIBPG_TEST_SERVER)
  find_program(LIBPG_PG_TOOL psql)
else()
  # LIBPG_PG_CTL names pg_ctl when it is not on PATH (CI passes it).
  find_program(LIBPG_PG_CTL pg_ctl HINTS /opt/homebrew/opt/postgresql@18/bin)
  set(LIBPG_PG_TOOL ${LIBPG_PG_CTL})
endif()
if(NOT Catch2_FOUND OR NOT LIBPG_PG_TOOL)
  message(STATUS "libpg tests skipped: need Catch2 3 and pg_ctl, or psql with LIBPG_TEST_SERVER (found Catch2=${Catch2_FOUND}, tool=${LIBPG_PG_TOOL})")
  return()
endif()
get_filename_component(LIBPG_PG_BINDIR "${LIBPG_PG_TOOL}" DIRECTORY)

if(LIBPG_TEST_SERVER)
  set(LIBPG_TEST_DSN "${LIBPG_TEST_SERVER}/libpg_test")
  add_test(NAME pg_start COMMAND ${PROJECT_SOURCE_DIR}/scripts/test-pg.sh recreate "${LIBPG_TEST_SERVER}" libpg_test ${LIBPG_PG_BINDIR})
  add_test(NAME pg_stop COMMAND ${CMAKE_COMMAND} -E true)
else()
  set(LIBPG_TEST_PORT 54329 CACHE STRING "Port of the test cluster")
  set(LIBPG_TEST_DIR ${CMAKE_CURRENT_BINARY_DIR}/pgdata)
  set(LIBPG_TEST_DSN "postgresql://postgres@127.0.0.1:${LIBPG_TEST_PORT}/libpg_test")
  add_test(NAME pg_start COMMAND ${PROJECT_SOURCE_DIR}/scripts/test-pg.sh start ${LIBPG_TEST_DIR} ${LIBPG_TEST_PORT} ${LIBPG_PG_BINDIR})
  add_test(NAME pg_stop COMMAND ${PROJECT_SOURCE_DIR}/scripts/test-pg.sh stop ${LIBPG_TEST_DIR} ${LIBPG_TEST_PORT} ${LIBPG_PG_BINDIR})
endif()
set_tests_properties(pg_start PROPERTIES FIXTURES_SETUP pg)
set_tests_properties(pg_stop PROPERTIES FIXTURES_CLEANUP pg)

# Each test file is one executable; add it here.
function(libpg_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE pg Catch2::Catch2WithMain)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED pg ENVIRONMENT "LIBPG_TEST_DSN=${LIBPG_TEST_DSN}")
endfunction()

libpg_test(codec_test)
libpg_test(statement_test)
libpg_test(query_test)
libpg_test(pool_test)
libpg_test(transaction_test)
libpg_test(migrate_test)
libpg_test(writeback_test)
target_link_libraries(writeback_test PRIVATE pg_writeback)

# pg_tablegen: the generator reproduces the checked-in headers, and what it
# writes for the real projects' schemas compiles.
set(LIBPG_TABLEGEN_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/tablegen_compile.cmake)
set(LIBPG_FIXTURE_ARGS "--queries|${CMAKE_CURRENT_SOURCE_DIR}/queries/fixture.sql")
add_test(NAME tablegen_fixture COMMAND ${CMAKE_COMMAND}
  -DTABLEGEN=$<TARGET_FILE:pg_tablegen> -DDSN=${LIBPG_TEST_DSN}
  -DSCHEMA=${CMAKE_CURRENT_SOURCE_DIR}/fixture_schema.sql
  -DOUT=${CMAKE_CURRENT_BINARY_DIR}/tablegen_fixture/fixture_tables.hpp
  -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/generated/fixture_tables.hpp
  "-DARGS=${LIBPG_FIXTURE_ARGS}|--queries-out|${CMAKE_CURRENT_BINARY_DIR}/tablegen_fixture/fixture_queries.hpp"
  -DQUERIES_OUT=${CMAKE_CURRENT_BINARY_DIR}/tablegen_fixture/fixture_queries.hpp
  -DEXPECTED_QUERIES=${CMAKE_CURRENT_SOURCE_DIR}/generated/fixture_queries.hpp
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
set(LIBPG_REVLM_SCHEMA "" CACHE FILEPATH "revlm schema for the tablegen compile test")
set(LIBPG_HHHOJ_SCHEMA "" CACHE FILEPATH "HHHOJ schema for the tablegen compile test")
set(tablegen_tests tablegen_fixture)
foreach(project revlm HHHOJ)
  string(TOUPPER ${project} upper)
  if(EXISTS "${LIBPG_${upper}_SCHEMA}")
    tablegen_compile_test(${project} "${LIBPG_${upper}_SCHEMA}")
    list(APPEND tablegen_tests tablegen_compile_${project})
  endif()
endforeach()
set_tests_properties(${tablegen_tests} PROPERTIES FIXTURES_REQUIRED pg RESOURCE_LOCK tablegen)

# pg_tablegen exit codes: --check on the checked-in headers, --check on a stale
# copy, and a type outside the codec table.
function(tablegen_expect_test name rc args)
  add_test(NAME ${name} COMMAND ${CMAKE_COMMAND}
    -DTABLEGEN=$<TARGET_FILE:pg_tablegen> -DDSN=${LIBPG_TEST_DSN} -DPSQL=${LIBPG_PG_BINDIR}/psql
    "-DARGS=${args}" -DEXPECT_RC=${rc} ${ARGN}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tablegen_expect.cmake)
  set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED pg RESOURCE_LOCK tablegen)
endfunction()
set(fixture_schema ${CMAKE_CURRENT_SOURCE_DIR}/fixture_schema.sql)
set(fixture_tables ${CMAKE_CURRENT_SOURCE_DIR}/generated/fixture_tables.hpp)
set(fixture_queries ${CMAKE_CURRENT_SOURCE_DIR}/generated/fixture_queries.hpp)
set(stale_tables ${CMAKE_CURRENT_BINARY_DIR}/tablegen_stale/fixture_tables.hpp)
tablegen_expect_test(tablegen_check 0
  "${fixture_schema}|${fixture_tables}|${LIBPG_FIXTURE_ARGS}|--queries-out|${fixture_queries}|--check")
tablegen_expect_test(tablegen_check_stale 1
  "${fixture_schema}|${stale_tables}|${LIBPG_FIXTURE_ARGS}|--queries-out|${fixture_queries}|--check"
  -DSTALE_FROM=${fixture_tables} -DSTALE_TO=${stale_tables} "-DEXPECT_MESSAGE=fixture_tables.hpp is out of date")
# A CRLF copy of the fixture query file generates the same header.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/queries/fixture.sql fixture_queries_lf)
string(REPLACE "\n" "\r\n" fixture_queries_crlf "${fixture_queries_lf}")
set(crlf_queries ${CMAKE_CURRENT_BINARY_DIR}/crlf/fixture.sql)
file(WRITE ${crlf_queries} "${fixture_queries_crlf}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/queries/fixture.sql)
tablegen_expect_test(tablegen_crlf_queries 0
  "${fixture_schema}|${fixture_tables}|--queries|${crlf_queries}|--queries-out|${fixture_queries}|--check")

# Named placeholders: repeated names, casts after a placeholder, colons in
# literals and comments, the rejected -- params: and bare $1 forms, and a type
# outside the codec table.
set(placeholder_queries ${CMAKE_CURRENT_SOURCE_DIR}/queries)
set(placeholder_out ${CMAKE_CURRENT_BINARY_DIR}/tablegen_placeholders)
tablegen_expect_test(tablegen_unmapped 1
  "${fixture_schema}|${placeholder_out}/unmapped.hpp|--queries|${placeholder_queries}/unmapped_type.sql|--queries-out|${placeholder_out}/unmapped_queries.hpp"
  "-DEXPECT_MESSAGE=unmapped_type\\.sql: unmapped_type\\.addr: PG type inet is not in libpg's codec table")
tablegen_expect_test(tablegen_repeated_placeholder 0
  "${fixture_schema}|${placeholder_out}/repeated.hpp|--queries|${placeholder_queries}/repeated_placeholder.sql|--queries-out|${placeholder_out}/repeated_queries.hpp")
tablegen_expect_test(tablegen_cast_placeholder 0
  "${fixture_schema}|${placeholder_out}/cast.hpp|--queries|${placeholder_queries}/cast_placeholder.sql|--queries-out|${placeholder_out}/cast_queries.hpp")
tablegen_expect_test(tablegen_ignored_placeholders 0
  "${fixture_schema}|${placeholder_out}/ignored.hpp|--queries|${placeholder_queries}/ignored_placeholders.sql|--queries-out|${placeholder_out}/ignored_queries.hpp")
tablegen_expect_test(tablegen_params_line 1
  "${fixture_schema}|${placeholder_out}/params_line.hpp|--queries|${placeholder_queries}/params_line.sql|--queries-out|${placeholder_out}/params_line_queries.hpp"
  "-DEXPECT_MESSAGE=use :name placeholders instead of -- params:")
tablegen_expect_test(tablegen_bare_dollar 1
  "${fixture_schema}|${placeholder_out}/bare_dollar.hpp|--queries|${placeholder_queries}/bare_dollar.sql|--queries-out|${placeholder_out}/bare_dollar_queries.hpp"
  "-DEXPECT_MESSAGE=write :name instead of")

# Generated query functions run against a database built from fixture_schema.sql.
libpg_test(querygen_test)
target_include_directories(querygen_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/generated)
target_compile_definitions(querygen_test PRIVATE "LIBPG_FIXTURE_SCHEMA=\"${fixture_schema}\"")
