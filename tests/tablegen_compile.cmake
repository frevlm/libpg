# Runs pg_tablegen on SCHEMA and writes OUT. ARGS holds further generator
# arguments separated by "|". With EXPECTED, OUT must equal that file; with
# QUERIES_OUT and EXPECTED_QUERIES, likewise for the query header; with CXX,
# OUT must compile on its own against INCLUDE_DIR.
foreach(var TABLEGEN DSN SCHEMA OUT)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "${var} is not set")
  endif()
endforeach()

string(REPLACE "|" ";" args "${ARGS}")
execute_process(COMMAND ${TABLEGEN} ${DSN} ${SCHEMA} ${OUT} ${args} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "pg_tablegen failed on ${SCHEMA}")
endif()

# The generator's throwaway database must be gone.
if(DEFINED PSQL)
  execute_process(
    COMMAND ${PSQL} -X -At -d ${DSN} -c "SELECT count(*) FROM pg_database WHERE datname LIKE 'tablegen%'"
    OUTPUT_VARIABLE leftover RESULT_VARIABLE rc OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT rc EQUAL 0 OR NOT leftover STREQUAL "0")
    message(FATAL_ERROR "pg_tablegen left ${leftover} database(s) behind")
  endif()
endif()

function(expect_same expected actual)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${expected} ${actual} RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${actual} differs from ${expected}")
  endif()
endfunction()

if(DEFINED EXPECTED)
  expect_same(${EXPECTED} ${OUT})
endif()
if(DEFINED EXPECTED_QUERIES)
  expect_same(${EXPECTED_QUERIES} ${QUERIES_OUT})
endif()

if(DEFINED CXX)
  execute_process(
    COMMAND ${CXX} -std=c++20 -fsyntax-only -I ${INCLUDE_DIR} -include ${OUT} -x c++ /dev/null
    RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${OUT} does not compile")
  endif()
endif()
