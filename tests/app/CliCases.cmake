# These cases never start capture or connect to a receiver.
function(check_case expected pattern)
  execute_process(COMMAND "${APP}" --cli ${ARGN}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
  if(NOT "${result}" STREQUAL "${expected}")
    message(FATAL_ERROR "CLI ${ARGN}: expected ${expected}, got ${result}\n${output}\n${error}")
  endif()
  if(NOT output MATCHES "${pattern}")
    message(FATAL_ERROR "CLI ${ARGN}: missing ${pattern}\n${output}\n${error}")
  endif()
  if(output MATCHES "\"event\":\"(start|streaming)\"")
    message(FATAL_ERROR "A non-transmission case reached the live path: ${output}")
  endif()
endfunction()
set(config "${CMAKE_CURRENT_BINARY_DIR}/cli-config-v4.json")
file(WRITE "${config}" [=[{
  "version": 4, "language": "en", "driverId": "ignored-config-device",
  "left": 50, "right": 51, "networkBinding": null, "receiverSelection": [],
  "timing": {
    "packetSamples": 64, "prebufferSamples": 2048, "backlogSamples": 8192,
    "leadMs": 2000, "settleMs": 2000, "lateMs": 100,
    "inputTimeoutMs": 1000, "connectTimeoutMs": 8000, "requestTimeoutMs": 8000,
    "teardownTimeoutMs": 1000, "ptpSyncMs": 125, "ptpAnnounceMs": 1000,
    "audioSyncMs": 500, "keepAliveMs": 10000
  }
}]=])
check_case(0 "--config" --help)
check_case(0 "AirPlayQt" --version)
check_case(1 "Input device UID not found" --config "${CMAKE_CURRENT_LIST_DIR}/../../doc/config.example.json" --device airplayqt-nonexistent-test-device --receiver 127.0.0.1)
check_case(2 "Unknown option" --unknown)
check_case(2 "--config PATH is required")
check_case(2 "Unexpected positional" extraneous)
check_case(2 "cannot be combined" --list-devices --receiver 127.0.0.1)
check_case(2 "device UID is required" --config "${config}")
check_case(1 "Input device UID not found" --config "${config}" --device airplayqt-nonexistent-test-device --left 2 --right 2 --receiver 127.0.0.1)
check_case(2 "1..256" --config "${config}" --device fake --left 0 --receiver 127.0.0.1)
foreach(seconds 0 3601)
  check_case(2 "1..3600" --config "${config}" --device fake --seconds ${seconds} --receiver 127.0.0.1)
endforeach()
check_case(2 "1..3600" --config "${config}" --device fake --startup-timeout 0 --receiver 127.0.0.1)
check_case(2 "Repeated option" --config "${config}" --device fake --seconds 1 --seconds 2 --receiver 127.0.0.1)
check_case(2 "error" --config "${config}" --device fake --receiver 127.0.0.1 --receiver 127.0.0.1:7000)
check_case(2 "error" --config "${config}" --device fake --receiver 127.0.0.1 --receiver 127.0.0.2 --receiver 127.0.0.3)
check_case(2 "error" --config "${config}" --device fake --receiver 224.0.0.1)
check_case(2 "error" --config "${config}" --device fake --receiver 127.0.0.1:0)
check_case(1 "missing-config.json" --config "${CMAKE_CURRENT_BINARY_DIR}/missing-config.json" --device fake --receiver 127.0.0.1)
file(READ "${config}" original)
file(WRITE "${config}" "{broken")
check_case(1 "Corrupt configuration" --config "${config}" --device fake --receiver 127.0.0.1)
string(REPLACE "\"version\": 4" "\"version\": 3" legacy "${original}")
file(WRITE "${config}" "${legacy}")
check_case(1 "Invalid configuration structure or version" --config "${config}" --device fake --receiver 127.0.0.1)
file(READ "${config}" after)
if(NOT after STREQUAL legacy)
  message(FATAL_ERROR "CLI overwrote incompatible configuration")
endif()
string(REPLACE "\"packetSamples\": 64" "\"packetSamples\": 353" invalid "${original}")
file(WRITE "${config}" "${invalid}")
check_case(1 "out of range" --config "${config}" --device fake --receiver 127.0.0.1)
file(WRITE "${config}" "${original}")
message(STATUS "CLI configuration/argument cases passed without transmission")
