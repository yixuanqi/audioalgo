cmake_minimum_required(VERSION 3.28)

project(AudioAlgo VERSION 0.1.0 LANGUAGES CXX)

option(AUDIO_BUILD_EDITOR "Build the JUCE desktop editor" ON)
option(AUDIO_ENABLE_PYTHON "Enable the external Python runner integration" ON)
option(AUDIO_ENABLE_SPEEXDSP "Enable SpeexDSP nodes" OFF)
option(AUDIO_BUILD_TESTS "Build and register automated tests" ON)
option(AUDIO_GATE_FAILURE_PROBE "Register the temporary G0 failure propagation probe" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(config Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${config}" config_upper)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/bin")
endforeach()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/versions.json" AUDIO_VERSION_LOCK)
function(audio_lock_value dependency key output)
  string(JSON value GET "${AUDIO_VERSION_LOCK}" "${dependency}" "${key}")
  set("${output}" "${value}" PARENT_SCOPE)
endfunction()

include(FetchContent)

audio_lock_value(juce url juce_url)
audio_lock_value(juce sha512 juce_sha512)
FetchContent_Declare(JUCE URL "${juce_url}" URL_HASH "SHA512=${juce_sha512}" DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(JUCE)

add_library(audio_core STATIC libs/audio_core/AudioBuffer.cpp)
target_include_directories(audio_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs")
target_compile_features(audio_core PUBLIC cxx_std_20)

add_library(audio_algorithms STATIC libs/audio_algorithms/Gain.cpp)
target_include_directories(audio_algorithms PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs")
target_link_libraries(audio_algorithms PUBLIC audio_core)
target_compile_features(audio_algorithms PUBLIC cxx_std_20)

add_library(audio_node_sdk STATIC libs/audio_node_sdk/NodeSDK.cpp)
target_include_directories(audio_node_sdk PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs")
target_link_libraries(audio_node_sdk PUBLIC audio_core)
target_compile_features(audio_node_sdk PUBLIC cxx_std_20)

add_library(audio_project STATIC libs/audio_project/ProjectModel.cpp)
target_include_directories(audio_project PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs")
target_link_libraries(audio_project PUBLIC audio_node_sdk)
target_compile_features(audio_project PUBLIC cxx_std_20)

add_library(audio_io STATIC libs/audio_io/WavFile.cpp)
target_include_directories(audio_io PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs")
target_link_libraries(audio_io PUBLIC audio_core juce::juce_audio_formats)
target_compile_features(audio_io PUBLIC cxx_std_20)

add_library(audio_project_file_io STATIC libs/audio_project/ProjectFileIO.cpp)
target_include_directories(audio_project_file_io PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs")
target_link_libraries(audio_project_file_io PUBLIC audio_project juce::juce_core)
target_compile_features(audio_project_file_io PUBLIC cxx_std_20)

add_library(audio_juce_backend STATIC libs/audio_juce_backend/JuceGraph.cpp)
target_include_directories(audio_juce_backend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs")
target_link_libraries(audio_juce_backend PUBLIC audio_project audio_io juce::juce_audio_processors)
target_compile_features(audio_juce_backend PUBLIC cxx_std_20)

if(AUDIO_BUILD_TESTS)
  set(CATCH_BUILD_TESTING OFF CACHE BOOL "Disable Catch2's own test suite" FORCE)
  set(CATCH_INSTALL_DOCS OFF CACHE BOOL "Do not build or install Catch2 documentation" FORCE)
  audio_lock_value(catch2 url catch2_url)
  audio_lock_value(catch2 sha512 catch2_sha512)
  FetchContent_Declare(Catch2 URL "${catch2_url}" URL_HASH "SHA512=${catch2_sha512}" DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(Catch2)

  include(CTest)
  find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
  add_executable(audioalgo_tests tests/unit/test_environment.cpp tests/unit/test_gain.cpp tests/unit/test_graph.cpp tests/unit/test_io.cpp tests/unit/test_project.cpp tests/unit/test_juce_graph.cpp tests/unit/test_run_service.cpp tests/unit/test_core.cpp tests/unit/test_editor_geometry.cpp tests/unit/test_editor_commands.cpp)
  target_link_libraries(audioalgo_tests PRIVATE Catch2::Catch2WithMain audio_algorithms audio_project audio_project_file_io audio_io audio_juce_backend juce::juce_gui_basics)
  target_compile_features(audioalgo_tests PRIVATE cxx_std_20)
  include(Catch)
  catch_discover_tests(audioalgo_tests TEST_PREFIX "")

  if(MSVC)
    target_compile_options(audioalgo_tests PRIVATE /utf-8)
    target_compile_options(audio_io PRIVATE /utf-8)
    target_compile_options(audio_project_file_io PRIVATE /utf-8)
    target_compile_options(audio_juce_backend PRIVATE /utf-8)
  endif()

  add_test(
    NAME BLD-04_gate_inventory_rejects_unregistered_ids
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_gate_verifier.py"
  )
  set_tests_properties(BLD-04_gate_inventory_rejects_unregistered_ids PROPERTIES LABELS "G0;infrastructure")

  foreach(subcase IN ITEMS a b c d)
    add_test(
      NAME "NED-12.${subcase}"
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_editor_interaction_verifier.py" --group "${subcase}"
    )
    set_tests_properties("NED-12.${subcase}" PROPERTIES LABELS "node_interaction;pure")
  endforeach()

  if(AUDIO_GATE_FAILURE_PROBE)
    add_test(
      NAME G0_temporary_assertion_failure_probe
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_BINARY_DIR}/probe/assertion_failure.py"
    )
    set_tests_properties(G0_temporary_assertion_failure_probe PROPERTIES LABELS "G0;temporary-probe")
  endif()
endif()

add_executable(audio_cli apps/cli/main.cpp)
target_compile_features(audio_cli PRIVATE cxx_std_20)
target_compile_definitions(audio_cli PRIVATE AUDIOALGO_VERSION="${PROJECT_VERSION}")

add_library(audio_run_service STATIC libs/audio_run_service/RunService.cpp)
target_include_directories(audio_run_service PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs")
target_link_libraries(audio_run_service PUBLIC audio_project_file_io audio_juce_backend audio_io juce::juce_core)
target_compile_features(audio_run_service PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(audio_run_service PRIVATE /utf-8)
  target_compile_options(audio_cli PRIVATE /utf-8)
endif()

target_link_libraries(audio_cli PRIVATE audio_run_service juce::juce_gui_basics)

if(AUDIO_BUILD_TESTS)
  target_link_libraries(audioalgo_tests PRIVATE audio_run_service juce::juce_gui_basics)
  add_dependencies(audioalgo_tests audio_cli)
endif()

if(AUDIO_BUILD_EDITOR)
  juce_add_gui_app(AudioAlgoEditor
    PRODUCT_NAME "AudioAlgo Editor"
    COMPANY_NAME "AudioAlgo"
    BUNDLE_ID "org.audioalgo.editor"
  )
  target_sources(AudioAlgoEditor PRIVATE apps/editor/Main.cpp)
  target_compile_features(AudioAlgoEditor PRIVATE cxx_std_20)
  target_compile_definitions(AudioAlgoEditor PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
  if(MSVC)
    target_compile_options(AudioAlgoEditor PRIVATE /utf-8)
  endif()
  target_link_libraries(AudioAlgoEditor PRIVATE audio_run_service audio_io juce::juce_gui_extra juce::juce_audio_utils)
  if(AUDIO_BUILD_TESTS)
    add_test(
      NAME UI-00_editor_binary_launches_and_quits_smoke
      COMMAND "$<TARGET_FILE:AudioAlgoEditor>" --smoke-test
    )
    set_tests_properties(UI-00_editor_binary_launches_and_quits_smoke PROPERTIES TIMEOUT 120)
  endif()
  add_custom_command(TARGET AudioAlgoEditor POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/examples/gain_demo"
      "$<TARGET_FILE_DIR:AudioAlgoEditor>/examples/gain_demo"
    VERBATIM
  )
endif()

if(AUDIO_ENABLE_PYTHON)
  find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
  message(STATUS "AudioAlgo Python 3: ${Python3_EXECUTABLE} (${Python3_VERSION})")
endif()

if(AUDIO_ENABLE_SPEEXDSP)
  message(FATAL_ERROR "SpeexDSP is locked but its adapter target is not implemented yet")
endif()
