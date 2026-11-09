string(RANDOM LENGTH 12 suffix)
set(root "${CMAKE_CURRENT_BINARY_DIR}/laminar-cli-${suffix}")
file(MAKE_DIRECTORY "${root}")
set(db "${root}/db")

function(check expected_code expected_output)
  execute_process(COMMAND "${CLI}" ${ARGN}
    RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT "${code}" STREQUAL "${expected_code}" OR
     NOT "${output}" STREQUAL "${expected_output}" OR
     (code EQUAL 0 AND NOT "${error}" STREQUAL "") OR
     (NOT code EQUAL 0 AND "${error}" STREQUAL ""))
    message(FATAL_ERROR "Command ${ARGN}: exit=${code}, stdout=[${output}], stderr=[${error}]. Fixture ${root}")
  endif()
endfunction()

check(2 "" inspect "${db}")
check(3 "" put "${db}" 610 00ff)
check(3 "" put "${db}" "61  " 00ff)
check(3 "" put "${db}" 61 XX)
check(3 "" delete "${db}" 61 extra)
if(EXISTS "${db}")
  message(FATAL_ERROR "Inspect or invalid input created ${db}")
endif()

check(0 "" put "${db}" 6100 00FF)
check(0 "00ff\n" get "${db}" 6100)
# ARGN list expansion drops empty elements. Execute this case with literal empty arguments.
execute_process(COMMAND "${CLI}" put "${db}" "" ""
  RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT code EQUAL 0 OR NOT "${output}${error}" STREQUAL "")
  message(FATAL_ERROR "Empty put failed: ${code} [${output}] [${error}]")
endif()
execute_process(COMMAND "${CLI}" get "${db}" ""
  RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT code EQUAL 0 OR NOT "${output}" STREQUAL "\n" OR NOT "${error}" STREQUAL "")
  message(FATAL_ERROR "Empty get failed: ${code} [${output}] [${error}]")
endif()
check(0 "" delete "${db}" 6100)
check(2 "" get "${db}" 6100)

set(log "${db}/data.log")
file(APPEND "${log}" "L")
file(SHA256 "${log}" before)
file(SIZE "${log}" size)
check(0 "{\"format_version\":1,\"log_bytes\":${size},\"record_count\":3,\"live_keys\":1,\"incomplete_tail_bytes\":1}\n"
  inspect "${db}")
file(SHA256 "${log}" after)
if(NOT before STREQUAL after)
  message(FATAL_ERROR "Inspect modified ${log}")
endif()
file(APPEND "${log}" "X")
check(1 "" inspect "${db}")
file(REMOVE_RECURSE "${root}")
