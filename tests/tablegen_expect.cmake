# Runs `pg_tablegen DSN <ARGS>` (ARGS separated by "|") and requires exit code
# EXPECT_RC and, with EXPECT_MESSAGE, that regex in its stderr. With
# STALE_FROM and STALE_TO, first copies STALE_FROM to STALE_TO and appends a
# line to the copy. No scratch database may be left behind.
foreach(var TABLEGEN DSN ARGS EXPECT_RC PSQL)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "${var} is not set")
  endif()
endforeach()

if(DEFINED STALE_FROM)
  configure_file(${STALE_FROM} ${STALE_TO} COPYONLY)
  file(APPEND ${STALE_TO} "// edited by hand\n")
endif()

string(REPLACE "|" ";" args "${ARGS}")
execute_process(COMMAND ${TABLEGEN} ${DSN} ${args} RESULT_VARIABLE rc ERROR_VARIABLE err)
message(STATUS "pg_tablegen exit ${rc}: ${err}")
if(NOT rc EQUAL EXPECT_RC)
  message(FATAL_ERROR "pg_tablegen exited with ${rc}, expected ${EXPECT_RC}")
endif()
if(DEFINED EXPECT_MESSAGE AND NOT err MATCHES "${EXPECT_MESSAGE}")
  message(FATAL_ERROR "pg_tablegen stderr does not match \"${EXPECT_MESSAGE}\"")
endif()

execute_process(
  COMMAND ${PSQL} -X -At -d ${DSN} -c "SELECT count(*) FROM pg_database WHERE datname LIKE 'tablegen%'"
  OUTPUT_VARIABLE leftover RESULT_VARIABLE rc OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 0 OR NOT leftover STREQUAL "0")
  message(FATAL_ERROR "pg_tablegen left ${leftover} database(s) behind")
endif()
