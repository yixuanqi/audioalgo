cmake_minimum_required(VERSION 3.24)
project(AudioAlgoGateFailureProbeFixture NONE)

option(AUDIO_GATE_FAILURE_PROBE "Register the intentional assertion failure" OFF)
enable_testing()

foreach(test_id IN ITEMS BLD-01 BLD-02 BLD-03 BLD-04 LOOP-01)
  add_test(NAME "${test_id}-fixture" COMMAND "${CMAKE_COMMAND}" -E true)
endforeach()

if(AUDIO_GATE_FAILURE_PROBE)
  find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
  get_filename_component(REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
  add_test(
    NAME G0_temporary_assertion_failure_probe
    COMMAND "${Python3_EXECUTABLE}" "${REPOSITORY_ROOT}/tests/infrastructure/assertion_failure_probe.py"
  )
endif()
