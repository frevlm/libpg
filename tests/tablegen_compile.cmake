# Runs pg_tablegen on SCHEMA and writes OUT. With EXPECTED, OUT must equal that
# file; with CXX, OUT must compile on its own against INCLUDE_DIR.
foreach(var TABLEGEN DSN SCHEMA OUT)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "${var} is not set")
  endif()
endforeach()

execute_process(COMMAND ${TABLEGEN} ${DSN} ${SCHEMA} ${OUT} RESULT_VARIABLE rc)
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

if(DEFINED EXPECTED)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${EXPECTED} ${OUT} RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${OUT} differs from ${EXPECTED}")
  endif()
endif()

if(DEFINED CXX)
  execute_process(
    COMMAND ${CXX} -std=c++20 -fsyntax-only -I ${INCLUDE_DIR} -include ${OUT} -x c++ /dev/null
    RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${OUT} does not compile")
  endif()
endif()
