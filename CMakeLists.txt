cmake_minimum_required(VERSION 3.28)

project(AudioAlgo VERSION 0.1.0 LANGUAGES CXX)

option(AUDIO_BUILD_EDITOR "Build the JUCE desktop editor" ON)
option(AUDIO_BUILD_RUNTIME "Build the JUCE-backed runtime, CLI, and application targets" ON)
option(AUDIO_ENABLE_PYTHON "Enable the external Python runner integration" ON)
option(AUDIO_ENABLE_SPEEXDSP "Enable SpeexDSP nodes" OFF)
option(AUDIO_BUILD_TESTS "Build and register automated tests" ON)
option(AUDIO_BUILD_INFRASTRUCTURE_TESTS "Register Python-based infrastructure and architecture tests" ON)
option(AUDIO_GATE_FAILURE_PROBE "Register the temporary G0 failure propagation probe" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(GNUInstallDirs)
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

if(AUDIO_BUILD_RUNTIME)
  include(FetchContent)
  audio_lock_value(juce url juce_url)
  audio_lock_value(juce sha512 juce_sha512)
  FetchContent_Declare(JUCE URL "${juce_url}" URL_HASH "SHA512=${juce_sha512}" DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(JUCE)
elseif(AUDIO_BUILD_EDITOR)
  message(FATAL_ERROR "AUDIO_BUILD_EDITOR requires AUDIO_BUILD_RUNTIME")
endif()

add_library(audio_core STATIC libs/audio_core/AudioBuffer.cpp)
target_sources(audio_core PRIVATE libs/audio_core/AudioTimeline.cpp)
add_library(AudioAlgo::core ALIAS audio_core)
set_target_properties(audio_core PROPERTIES EXPORT_NAME core)
target_include_directories(audio_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(audio_core PUBLIC cxx_std_20)

add_library(audio_algorithms STATIC libs/audio_algorithms/Gain.cpp libs/audio_algorithms/ChannelSwap.cpp libs/audio_algorithms/Mixer.cpp)
add_library(AudioAlgo::algorithms ALIAS audio_algorithms)
set_target_properties(audio_algorithms PROPERTIES EXPORT_NAME algorithms)
target_include_directories(audio_algorithms PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(audio_algorithms PUBLIC audio_core)
target_compile_features(audio_algorithms PUBLIC cxx_std_20)

if(AUDIO_ENABLE_SPEEXDSP)
  enable_language(C)
  include(FetchContent)
  audio_lock_value(speexdsp url speex_url)
  audio_lock_value(speexdsp sha512 speex_hash)
  # Reuse the verified local archive when present; the same mandatory hash applies.
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/build/_downloads/speexdsp-1.2.1.tar.gz")
    set(speex_url "${CMAKE_CURRENT_SOURCE_DIR}/build/_downloads/speexdsp-1.2.1.tar.gz")
  endif()
  FetchContent_Declare(SpeexDSP URL "${speex_url}" URL_HASH "SHA512=${speex_hash}" DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(SpeexDSP)
  set(speex_src "${speexdsp_SOURCE_DIR}/libspeexdsp")
  add_library(audio_speexdsp STATIC
    "${speex_src}/preprocess.c" "${speex_src}/jitter.c" "${speex_src}/mdf.c"
    "${speex_src}/fftwrap.c" "${speex_src}/filterbank.c" "${speex_src}/resample.c"
    "${speex_src}/buffer.c" "${speex_src}/scal.c" "${speex_src}/smallft.c")
  target_include_directories(audio_speexdsp PUBLIC $<BUILD_INTERFACE:${speexdsp_SOURCE_DIR}/include>)
  target_compile_definitions(audio_speexdsp PRIVATE FLOATING_POINT USE_SMALLFT EXPORT=)
  set_target_properties(audio_speexdsp PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES EXPORT_NAME speexdsp_backend)
  target_sources(audio_algorithms PRIVATE libs/audio_algorithms/SpeexDenoise.cpp)
  target_link_libraries(audio_algorithms PRIVATE audio_speexdsp)
  install(TARGETS audio_speexdsp EXPORT AudioAlgoAlgorithmsTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT AudioAlgoAlgorithms)
  install(FILES "${speexdsp_SOURCE_DIR}/COPYING" DESTINATION share/licenses/SpeexDSP COMPONENT AudioAlgoAlgorithms)
endif()

include(CMakePackageConfigHelpers)
set(AUDIOALGO_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/AudioAlgo")
install(TARGETS audio_core audio_algorithms
  EXPORT AudioAlgoAlgorithmsTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT AudioAlgoAlgorithms
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/libs/audio_core/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/audio_core"
  COMPONENT AudioAlgoAlgorithms FILES_MATCHING PATTERN "*.h"
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/libs/audio_algorithms/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/audio_algorithms"
  COMPONENT AudioAlgoAlgorithms FILES_MATCHING PATTERN "*.h"
)
install(EXPORT AudioAlgoAlgorithmsTargets
  FILE AudioAlgoAlgorithmsTargets.cmake
  NAMESPACE AudioAlgo::
  DESTINATION "${AUDIOALGO_INSTALL_CMAKEDIR}"
  COMPONENT AudioAlgoAlgorithms
)
configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AudioAlgoConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/AudioAlgoConfig.cmake"
  INSTALL_DESTINATION "${AUDIOALGO_INSTALL_CMAKEDIR}"
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/AudioAlgoConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/AudioAlgoConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/AudioAlgoConfigVersion.cmake"
  DESTINATION "${AUDIOALGO_INSTALL_CMAKEDIR}"
  COMPONENT AudioAlgoAlgorithms
)

add_library(audio_node_sdk STATIC libs/audio_node_sdk/NodeSDK.cpp)
target_include_directories(audio_node_sdk PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs")
target_link_libraries(audio_node_sdk PUBLIC audio_core)
target_compile_features(audio_node_sdk PUBLIC cxx_std_20)

# The published DLL SDK is header-only C ABI: no host import library, JUCE or STL ABI.
add_library(audio_node_abi INTERFACE)
add_library(AudioAlgo::node_abi ALIAS audio_node_abi)
set_target_properties(audio_node_abi PROPERTIES EXPORT_NAME node_abi)
target_include_directories(audio_node_abi INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
install(TARGETS audio_node_abi EXPORT AudioAlgoNodeSDKTargets COMPONENT AudioAlgoNodeSDK)
install(DIRECTORY sdk/include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT AudioAlgoNodeSDK)
set(NODE_SDK_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/AudioAlgoNodeSDK")
configure_package_config_file(cmake/AudioAlgoNodeSDKConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/AudioAlgoNodeSDKConfig.cmake" INSTALL_DESTINATION "${NODE_SDK_CMAKEDIR}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/AudioAlgoNodeSDKConfigVersion.cmake"
  VERSION 1.0.0 COMPATIBILITY SameMajorVersion)
install(EXPORT AudioAlgoNodeSDKTargets NAMESPACE AudioAlgo:: DESTINATION "${NODE_SDK_CMAKEDIR}" COMPONENT AudioAlgoNodeSDK)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/AudioAlgoNodeSDKConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/AudioAlgoNodeSDKConfigVersion.cmake"
  DESTINATION "${NODE_SDK_CMAKEDIR}" COMPONENT AudioAlgoNodeSDK)
install(DIRECTORY examples/plugins/native_stateful/ DESTINATION share/AudioAlgoNodeSDK/native_stateful COMPONENT AudioAlgoNodeSDK)
install(FILES doc/native_node_sdk.md DESTINATION share/AudioAlgoNodeSDK COMPONENT AudioAlgoNodeSDK)

add_library(audio_project STATIC libs/audio_project/ProjectModel.cpp libs/audio_project/ProjectResourceService.cpp)
target_include_directories(audio_project PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs")
target_link_libraries(audio_project PUBLIC audio_node_sdk)
target_compile_features(audio_project PUBLIC cxx_std_20)

# Headless application layer: owns the edited project, commands/history and presentation state.
# It must not depend on JUCE, the view or file dialogs.
add_library(audio_editor_application STATIC
  libs/audio_editor_application/EditorController.cpp
  libs/audio_editor_application/EditorProjectDocuments.cpp
)
target_include_directories(audio_editor_application PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs")
target_link_libraries(audio_editor_application PUBLIC audio_project)
target_compile_features(audio_editor_application PUBLIC cxx_std_20)

add_library(audio_node_authoring STATIC libs/audio_node_authoring/Draft.cpp libs/audio_node_authoring/Workspace.cpp)
target_include_directories(audio_node_authoring PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs")
target_link_libraries(audio_node_authoring PUBLIC audio_node_sdk)
target_compile_features(audio_node_authoring PUBLIC cxx_std_20)

# Pure execution metadata compiler is available with the JUCE runtime disabled.
add_library(audio_runtime STATIC libs/audio_runtime/NativePlan.cpp libs/audio_runtime/PreparedPlan.cpp)
target_include_directories(audio_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs")
target_link_libraries(audio_runtime PUBLIC audio_project)
target_compile_features(audio_runtime PUBLIC cxx_std_20)

if(AUDIO_BUILD_RUNTIME)
add_library(audio_io STATIC libs/audio_io/WavFile.cpp)
target_include_directories(audio_io PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs")
target_link_libraries(audio_io PUBLIC audio_core juce::juce_audio_formats)
target_compile_features(audio_io PUBLIC cxx_std_20)

add_library(audio_project_file_io STATIC libs/audio_project/ProjectFileIO.cpp)
target_include_directories(audio_project_file_io PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs")
target_link_libraries(audio_project_file_io PUBLIC audio_project juce::juce_core)
target_compile_features(audio_project_file_io PUBLIC cxx_std_20)

add_library(audio_juce_backend STATIC libs/audio_juce_backend/JuceGraph.cpp libs/audio_juce_backend/NativeNodeAudioProcessor.cpp)
target_include_directories(audio_juce_backend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs")
target_link_libraries(audio_juce_backend PUBLIC audio_runtime audio_project audio_io juce::juce_audio_processors)
target_compile_features(audio_juce_backend PUBLIC cxx_std_20)

add_library(audio_nodes_builtin STATIC plugins/builtin/GainNodeAdapter.cpp plugins/builtin/MixerNodeAdapter.cpp plugins/builtin/registration.cpp plugins/examples/ChannelSwapNode.cpp)
target_include_directories(audio_nodes_builtin PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(audio_nodes_builtin PUBLIC audio_node_sdk audio_algorithms)
target_compile_features(audio_nodes_builtin PUBLIC cxx_std_20)

add_library(audio_native_plugins STATIC libs/audio_native_plugins/NativePlugins.cpp)
target_include_directories(audio_native_plugins PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs")
target_link_libraries(audio_native_plugins PUBLIC audio_node_sdk PRIVATE audio_node_abi juce::juce_core juce::juce_cryptography)
add_library(audio_host_nodes STATIC plugins/host/registration.cpp)
target_include_directories(audio_host_nodes PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(audio_host_nodes PUBLIC audio_nodes_builtin audio_native_plugins)
add_library(audio_node_authoring_runtime STATIC libs/audio_node_authoring_runtime/PythonAuthoring.cpp)
target_include_directories(audio_node_authoring_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs")
target_link_libraries(audio_node_authoring_runtime PUBLIC audio_node_authoring PRIVATE audio_native_plugins juce::juce_core juce::juce_cryptography)
target_link_libraries(audio_host_nodes PUBLIC audio_node_authoring_runtime)
if(MSVC)
  target_compile_options(audio_node_authoring_runtime PRIVATE /utf-8)
endif()
if(AUDIO_ENABLE_SPEEXDSP)
  add_library(audio_nodes_speex STATIC plugins/speexdsp/DenoiseNode.cpp)
  target_include_directories(audio_nodes_speex PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(audio_nodes_speex PUBLIC audio_node_sdk audio_algorithms)
  target_link_libraries(audio_host_nodes PUBLIC audio_nodes_speex)
  target_compile_definitions(audio_host_nodes PRIVATE AUDIOALGO_HAS_SPEEXDSP=1)
endif()

endif()

if(AUDIO_BUILD_TESTS)
  include(FetchContent)
  set(CATCH_BUILD_TESTING OFF CACHE BOOL "Disable Catch2's own test suite" FORCE)
  set(CATCH_INSTALL_DOCS OFF CACHE BOOL "Do not build or install Catch2 documentation" FORCE)
  audio_lock_value(catch2 url catch2_url)
  audio_lock_value(catch2 sha512 catch2_sha512)
  FetchContent_Declare(Catch2 URL "${catch2_url}" URL_HASH "SHA512=${catch2_sha512}" DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(Catch2)

  include(CTest)
  add_executable(audio_algorithm_tests tests/unit/test_gain.cpp tests/unit/test_gain_processor.cpp tests/unit/test_channel_swap.cpp tests/unit/test_log_text.cpp)
  target_link_libraries(audio_algorithm_tests PRIVATE Catch2::Catch2WithMain audio_algorithms)
  target_compile_features(audio_algorithm_tests PRIVATE cxx_std_20)
  include(Catch)
  catch_discover_tests(audio_algorithm_tests TEST_PREFIX "")
  add_test(
    NAME AR-04.d
    COMMAND "$<TARGET_FILE:audio_algorithm_tests>" "AR-04.d Gain prepare/process/reset uses borrowed block views"
  )
  set_tests_properties(AR-04.d PROPERTIES LABELS "R1;pure" TIMEOUT 10)
  add_test(
    NAME AR-05.a
    COMMAND "$<TARGET_FILE:audio_algorithm_tests>" "AR-05.a Gain APIs match a float64 oracle across the full block matrix"
  )
  set_tests_properties(AR-05.a PROPERTIES LABELS "R1;pure;DSP-01" TIMEOUT 60)

  add_executable(audio_application_tests tests/unit/test_run_reception.cpp tests/unit/test_native_plan.cpp tests/unit/test_prepared_plan.cpp)
  target_include_directories(audio_application_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libs")
  target_link_libraries(audio_application_tests PRIVATE Catch2::Catch2WithMain audio_runtime)
  catch_discover_tests(audio_application_tests PROPERTIES LABELS "pure;architecture" TIMEOUT 10)

  add_executable(audio_editor_application_tests
    tests/unit/test_editor_controller.cpp
    tests/unit/test_async_completion_slot.cpp
    tests/unit/test_editor_services.cpp
    tests/unit/test_latest_mailbox.cpp
    tests/unit/test_log_mailbox.cpp
  )
  target_include_directories(audio_editor_application_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libs")
  target_link_libraries(audio_editor_application_tests PRIVATE Catch2::Catch2WithMain audio_editor_application)
  target_compile_features(audio_editor_application_tests PRIVATE cxx_std_20)
  catch_discover_tests(audio_editor_application_tests PROPERTIES LABELS "R2;pure;application" TIMEOUT 10)
  add_executable(audio_node_authoring_tests tests/unit/test_authoring_draft_storage.cpp
    tests/unit/test_authoring_workspace_service.cpp)
  target_link_libraries(audio_node_authoring_tests PRIVATE Catch2::Catch2WithMain audio_node_authoring)
  target_compile_definitions(audio_node_authoring_tests PRIVATE AUDIOALGO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  if(MSVC)
    target_compile_options(audio_node_authoring_tests PRIVATE /utf-8)
  endif()
  catch_discover_tests(audio_node_authoring_tests PROPERTIES LABELS "node_authoring;pure" TIMEOUT 10)

  if(AUDIO_BUILD_RUNTIME)
    add_executable(audioalgo_tests tests/unit/test_environment.cpp tests/unit/test_graph.cpp tests/unit/test_io.cpp tests/unit/test_project.cpp tests/unit/test_project_compatibility.cpp tests/unit/test_juce_graph.cpp tests/unit/test_run_service.cpp tests/unit/test_editor_run_job.cpp tests/unit/test_core.cpp tests/unit/test_editor_geometry.cpp tests/unit/test_editor_commands.cpp tests/unit/test_gain_node_adapter.cpp tests/unit/test_node_registry.cpp tests/unit/test_native_node_audio_processor.cpp tests/unit/test_native_node_contracts.cpp tests/unit/test_editor_parameter_panel.cpp tests/unit/test_editor_parameter_binding.cpp apps/editor/EditorParameterPanel.cpp apps/editor/EditorNodeParameterBinding.cpp)
    target_sources(audioalgo_tests PRIVATE tests/unit/test_fixed_frame_fifo.cpp)
    target_sources(audioalgo_tests PRIVATE tests/unit/test_node_authoring.cpp)
    target_sources(audioalgo_tests PRIVATE tests/unit/test_node_workspace_lock.cpp)
    target_compile_definitions(audioalgo_tests PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1)
    target_sources(audioalgo_tests PRIVATE tests/unit/test_native_cancellation.cpp)
    target_sources(audioalgo_tests PRIVATE tests/unit/test_latest_run_queue.cpp)
    target_sources(audioalgo_tests PRIVATE tests/unit/test_stream_progress.cpp)
    target_sources(audioalgo_tests PRIVATE tests/unit/test_native_tail.cpp)
    target_sources(audioalgo_tests PRIVATE tests/unit/test_audio_timeline.cpp)
    target_sources(audioalgo_tests PRIVATE tests/unit/test_native_publication_failure.cpp)
    if(AUDIO_ENABLE_SPEEXDSP)
      target_sources(audioalgo_tests PRIVATE tests/integration/test_speex_denoise.cpp)
      target_link_libraries(audioalgo_tests PRIVATE audio_speexdsp audio_host_nodes)
    endif()
    target_include_directories(audioalgo_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/apps/editor")
    target_link_libraries(audioalgo_tests PRIVATE Catch2::Catch2WithMain audio_algorithms audio_nodes_builtin audio_project audio_editor_application audio_project_file_io audio_io audio_juce_backend juce::juce_gui_basics)
    target_compile_features(audioalgo_tests PRIVATE cxx_std_20)
    target_compile_definitions(audioalgo_tests PRIVATE
      AUDIOALGO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
      AUDIOALGO_CLI_EXECUTABLE="$<TARGET_FILE:audio_cli>"
      AUDIOALGO_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
      AUDIOALGO_TEST_PYTHON_ENABLED=$<BOOL:${AUDIO_ENABLE_PYTHON}>
    )
    catch_discover_tests(audioalgo_tests TEST_PREFIX "")
    add_test(
      NAME AR-05.b
      COMMAND "$<TARGET_FILE:audioalgo_tests>" "AR-05.b Gain node adapter matches an independent block oracle"
    )
    set_tests_properties(AR-05.b PROPERTIES LABELS "R1;native_integration" TIMEOUT 30)
    add_test(
      NAME AR-05.c
      COMMAND "$<TARGET_FILE:audioalgo_tests>" "AR-05.c Gain rejects invalid values consistently across adapter and runtime"
    )
    set_tests_properties(AR-05.c PROPERTIES LABELS "R1;native_integration" TIMEOUT 30)
    add_test(
      NAME AR-05.e
      COMMAND "$<TARGET_FILE:audioalgo_tests>" "AR-05.e*"
    )
    set_tests_properties(AR-05.e PROPERTIES LABELS "R1;native_integration" TIMEOUT 60)
    add_test(
      NAME AR-06.a
      COMMAND "$<TARGET_FILE:audioalgo_tests>" "AR-06.a failed paired registration leaves the descriptor and factory catalog unchanged"
    )
    set_tests_properties(AR-06.a PROPERTIES LABELS "R1;native_integration" TIMEOUT 10)
    add_test(
      NAME AR-06.b
      COMMAND "$<TARGET_FILE:audioalgo_tests>" "AR-06.b descriptor validation rejects invalid schema and names its field"
    )
    set_tests_properties(AR-06.b PROPERTIES LABELS "R1;native_integration" TIMEOUT 10)
    add_test(
      NAME AR-06.c
      COMMAND "$<TARGET_FILE:audioalgo_tests>" "AR-06.c node instances keep state separate and factory failures stay inside the SDK"
    )
    set_tests_properties(AR-06.c PROPERTIES LABELS "R1;native_integration" TIMEOUT 10)
    add_test(
      NAME AR-07.a
      COMMAND "$<TARGET_FILE:audioalgo_tests>" "AR-07.a Gain and a test processor execute through one generic JUCE wrapper"
    )
    set_tests_properties(AR-07.a PROPERTIES LABELS "R1;native_integration" TIMEOUT 30)
    foreach(subcase IN ITEMS b c d e)
      add_test(
        NAME "AR-07.${subcase}"
        COMMAND "$<TARGET_FILE:audioalgo_tests>" "AR-07.${subcase}*"
      )
    endforeach()
    set_tests_properties(AR-07.b PROPERTIES LABELS "R1;native_integration" TIMEOUT 60)
    set_tests_properties(AR-07.c PROPERTIES LABELS "R1;native_integration" TIMEOUT 30)
    set_tests_properties(AR-07.d PROPERTIES LABELS "R1;native_integration" TIMEOUT 60)
    set_tests_properties(AR-07.e PROPERTIES LABELS "R1;native_integration" TIMEOUT 30)
    foreach(subcase IN ITEMS e f)
      add_test(
        NAME "AR-06.${subcase}"
        COMMAND "$<TARGET_FILE:audioalgo_tests>" "AR-06.${subcase}*"
      )
      set_tests_properties("AR-06.${subcase}" PROPERTIES LABELS "R1;native_integration" TIMEOUT 60)
    endforeach()

  if(MSVC)
    target_compile_options(audioalgo_tests PRIVATE /utf-8)
    target_compile_options(audio_algorithm_tests PRIVATE /utf-8)
    target_compile_options(audio_io PRIVATE /utf-8)
    target_compile_options(audio_project_file_io PRIVATE /utf-8)
    target_compile_options(audio_juce_backend PRIVATE /utf-8)
  endif()
  endif()

  if(AUDIO_BUILD_INFRASTRUCTURE_TESTS)
  find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
  add_test(
    NAME BLD-04_gate_inventory_rejects_unregistered_ids
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_gate_verifier.py"
  )
  set_tests_properties(BLD-04_gate_inventory_rejects_unregistered_ids PROPERTIES LABELS "G0;infrastructure")

  add_test(
    NAME LOOP-01_gate_rejects_skipped_required_tests
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_gate_skip_probe.py"
  )
  set_tests_properties(LOOP-01_gate_rejects_skipped_required_tests PROPERTIES LABELS "G0;infrastructure" TIMEOUT 300)

  add_test(
    NAME AR-02.b_review_desktop_and_stage_adversaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_review_adversaries.py"
  )
  set_tests_properties(AR-02.b_review_desktop_and_stage_adversaries PROPERTIES LABELS "R0;infrastructure" TIMEOUT 60)

  add_test(
    NAME AR-05.d_isolation_guard
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_ar05d_isolation.py"
  )
  set_tests_properties(AR-05.d_isolation_guard PROPERTIES LABELS "R1;infrastructure" TIMEOUT 60)

  add_test(
    NAME R2_editor_application_boundary_guard
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_editor_application_boundary.py"
  )
  set_tests_properties(R2_editor_application_boundary_guard PROPERTIES LABELS "R2;infrastructure" TIMEOUT 60)
  add_test(
    NAME R2_production_gain_inspector_route
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_gain_inspector_route.py"
  )
  set_tests_properties(R2_production_gain_inspector_route PROPERTIES LABELS "R2;infrastructure" TIMEOUT 60)
  add_test(
    NAME R2_gain_slider_gesture_route
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_gain_slider_gesture_route.py"
  )
  set_tests_properties(R2_gain_slider_gesture_route PROPERTIES LABELS "R2;infrastructure" TIMEOUT 60)
  add_test(
    NAME R2_project_resource_service_route
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_project_resource_service_route.py"
  )
  set_tests_properties(R2_project_resource_service_route PROPERTIES LABELS "R2;infrastructure" TIMEOUT 60)
  add_test(
    NAME R2_project_format_policy
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_project_format_policy.py"
  )
  set_tests_properties(R2_project_format_policy PROPERTIES LABELS "R2;infrastructure" TIMEOUT 60)
  add_test(
    NAME R2_project_replacement_route
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_project_replacement_route.py"
  )
  set_tests_properties(R2_project_replacement_route PROPERTIES LABELS "R2;infrastructure" TIMEOUT 60)
  add_test(
    NAME R2_editor_service_injection
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_editor_service_injection.py"
  )
  set_tests_properties(R2_editor_service_injection PROPERTIES LABELS "R2;infrastructure" TIMEOUT 60)
  add_test(
    NAME R2_editor_work_routes
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_editor_work_routes.py"
  )
  set_tests_properties(R2_editor_work_routes PROPERTIES LABELS "R2;infrastructure" TIMEOUT 60)
  add_test(
    NAME R2_run_completion_route
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_run_completion_route.py"
  )
  set_tests_properties(R2_run_completion_route PROPERTIES LABELS "R2;infrastructure" TIMEOUT 60)

  foreach(case IN ITEMS a b c d)
    add_test(
      NAME "AR-01.${case}"
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_architecture_baseline.py" --case "${case}"
    )
    set_tests_properties("AR-01.${case}" PROPERTIES LABELS "R0;infrastructure" TIMEOUT 10)
  endforeach()
  # AR-01.a hashes the live source tree and inspects configured CTest inventories;
  # AR-01.d hashes that tree again and verifies every bound build artifact. Allow
  # their measured evidence-collection time under load without changing assertions.
  set_tests_properties(AR-01.a AR-01.d PROPERTIES TIMEOUT 20)

  foreach(case IN ITEMS a b c d e f g)
    add_test(
      NAME "AR-02.${case}"
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_architecture_refactor_verifier.py" --case "${case}"
    )
    set_tests_properties("AR-02.${case}" PROPERTIES LABELS "R0;infrastructure" TIMEOUT 10)
  endforeach()

  add_test(
    NAME AR-04.a
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_ar04_consumer.py"
            --cmake "${CMAKE_COMMAND}" --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --build-dir "${CMAKE_BINARY_DIR}" --config "$<CONFIG>"
  )
  set_tests_properties(AR-04.a PROPERTIES LABELS "R1;build_consumer" TIMEOUT 180)

  add_test(NAME AR-12.b
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_channel_swap_extension.py")
  set_tests_properties(AR-12.b PROPERTIES LABELS "R4;infrastructure;extension" TIMEOUT 60)
  add_test(NAME UPN_mapping_fail_closed
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_upn_mapping.py")
  set_tests_properties(UPN_mapping_fail_closed PROPERTIES LABELS "UPN;infrastructure" TIMEOUT 10)
  add_test(NAME NWS-00.contract
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_node_workspace_manifest.py")
  set_tests_properties(NWS-00.contract PROPERTIES LABELS "NWS;W0;infrastructure;contract" TIMEOUT 60)

  foreach(case IN ITEMS b c)
    add_test(
      NAME "AR-04.${case}"
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_ar04_pure_build.py"
              --case "${case}" --cmake "${CMAKE_COMMAND}" --ctest "${CMAKE_CTEST_COMMAND}"
              --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
              --catch2-source "${catch2_SOURCE_DIR}" --config "$<CONFIG>"
              --generator "${CMAKE_GENERATOR}" --architecture "${CMAKE_GENERATOR_PLATFORM}"
    )
    set_tests_properties("AR-04.${case}" PROPERTIES LABELS "R1;build_consumer" TIMEOUT 600)
  endforeach()

  if(AUDIO_BUILD_RUNTIME)
    add_test(NAME AR-12.e
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_channel_swap_demo.py"
              --build-dir "${CMAKE_BINARY_DIR}")
    set_tests_properties(AR-12.e PROPERTIES LABELS "R4;native_integration;extension" TIMEOUT 60)
    add_test(
      NAME AR-05.d
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_ar05_gain_mutation.py"
              --cmake "${CMAKE_COMMAND}" --build-dir "${CMAKE_BINARY_DIR}" --config "$<CONFIG>"
    )
    set_tests_properties(AR-05.d PROPERTIES LABELS "R1;mutation;native_integration" RUN_SERIAL TRUE TIMEOUT 1200)
    add_test(
      NAME AR-06.d
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/test_ar06d_plugin_boundary.py"
              --source-root "${CMAKE_CURRENT_SOURCE_DIR}" --executable "$<TARGET_FILE:audioalgo_tests>"
    )
    set_tests_properties(AR-06.d PROPERTIES LABELS "R1;architecture;native_integration" TIMEOUT 60)
  endif()

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
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/infrastructure/assertion_failure_probe.py"
    )
    set_tests_properties(G0_temporary_assertion_failure_probe PROPERTIES LABELS "G0;temporary-probe")
  endif()
  endif()
endif()

if(AUDIO_BUILD_RUNTIME)
add_executable(audio_cli apps/cli/main.cpp)
target_compile_features(audio_cli PRIVATE cxx_std_20)
target_compile_definitions(audio_cli PRIVATE AUDIOALGO_VERSION="${PROJECT_VERSION}")

add_library(audio_run_service STATIC libs/audio_run_service/RunService.cpp libs/audio_run_service/MixedExecution.cpp)
target_include_directories(audio_run_service PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs")
target_link_libraries(audio_run_service PUBLIC audio_project_file_io audio_juce_backend audio_io juce::juce_core)
target_link_libraries(audio_run_service PRIVATE juce::juce_cryptography)
target_compile_features(audio_run_service PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(audio_run_service PRIVATE /utf-8)
  target_compile_options(audio_cli PRIVATE /utf-8)
endif()

target_link_libraries(audio_cli PRIVATE audio_run_service audio_host_nodes juce::juce_gui_basics)

if(AUDIO_BUILD_TESTS)
  target_link_libraries(audioalgo_tests PRIVATE audio_run_service audio_node_authoring_runtime juce::juce_gui_extra)
  target_compile_definitions(audioalgo_tests PRIVATE AUDIOALGO_AUTHORING_PYTHON_ENABLED=$<BOOL:${AUDIO_ENABLE_PYTHON}>)
  add_dependencies(audioalgo_tests audio_cli)
  add_executable(audio_python_catalog_tests tests/integration/test_python_catalog.cpp)
  target_link_libraries(audio_python_catalog_tests PRIVATE Catch2::Catch2WithMain audio_host_nodes juce::juce_core)
  add_dependencies(audio_python_catalog_tests audio_cli)
  target_compile_definitions(audio_python_catalog_tests PRIVATE AUDIOALGO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    AUDIOALGO_CLI_EXE="$<TARGET_FILE:audio_cli>" AUDIOALGO_TEST_PYTHON_ENABLED=$<BOOL:${AUDIO_ENABLE_PYTHON}>)
  if(MSVC)
    target_compile_options(audio_python_catalog_tests PRIVATE /utf-8)
  endif()
  catch_discover_tests(audio_python_catalog_tests PROPERTIES LABELS "python;catalog;R6" TIMEOUT 30)
  add_executable(audio_native_plugin_tests tests/integration/test_native_plugin.cpp
    apps/editor/EditorParameterPanel.cpp apps/editor/EditorNodeParameterBinding.cpp)
  target_include_directories(audio_native_plugin_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/apps/editor")
  target_link_libraries(audio_native_plugin_tests PRIVATE Catch2::Catch2WithMain audio_host_nodes
    audio_run_service audio_editor_application juce::juce_gui_basics)
  if(AUDIO_BUILD_INFRASTRUCTURE_TESTS)
    add_test(NAME UPN_external_sdk_pipeline
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/verify_native_plugin.py"
        --build-dir "${CMAKE_BINARY_DIR}" --config "$<CONFIG>" --cmake "${CMAKE_COMMAND}")
    set_tests_properties(UPN_external_sdk_pipeline PROPERTIES LABELS "UPN;build_consumer;native_integration" TIMEOUT 240)
  endif()
endif()

if(AUDIO_BUILD_EDITOR)
  juce_add_gui_app(AudioAlgoEditor
    PRODUCT_NAME "AudioAlgo Editor"
    COMPANY_NAME "AudioAlgo"
    BUNDLE_ID "org.audioalgo.editor"
  )
  target_sources(AudioAlgoEditor PRIVATE apps/editor/Main.cpp apps/editor/EditorParameterPanel.cpp apps/editor/EditorNodeParameterBinding.cpp)
  target_compile_features(AudioAlgoEditor PRIVATE cxx_std_20)
  target_compile_definitions(AudioAlgoEditor PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
  if(MSVC)
    target_compile_options(AudioAlgoEditor PRIVATE /utf-8)
  endif()
  target_link_libraries(AudioAlgoEditor PRIVATE audio_run_service audio_host_nodes audio_io audio_editor_application audio_node_authoring_runtime juce::juce_gui_extra juce::juce_audio_utils)
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
  target_link_libraries(audio_node_authoring_runtime PRIVATE audio_python_backend)
  target_compile_definitions(audio_node_authoring_runtime PRIVATE AUDIOALGO_AUTHORING_PYTHON=1
    AUDIOALGO_AUTHORING_INTERPRETER="${Python3_EXECUTABLE}"
    AUDIOALGO_AUTHORING_RUNNER="${CMAKE_CURRENT_SOURCE_DIR}/python")
  add_library(audio_python_backend STATIC libs/audio_python_backend/PythonTask.cpp)
  target_include_directories(audio_python_backend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs")
  target_link_libraries(audio_python_backend PUBLIC audio_node_sdk PRIVATE juce::juce_core juce::juce_cryptography)
  target_link_libraries(audio_native_plugins PRIVATE audio_python_backend)
  target_compile_definitions(audio_native_plugins PRIVATE AUDIOALGO_HAS_PYTHON=1
    AUDIOALGO_PYTHON_EXE_SOURCE="${Python3_EXECUTABLE}" AUDIOALGO_PYTHON_MODULE_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/python")
  if(MSVC)
    target_compile_options(audio_python_backend PRIVATE /utf-8)
  endif()
  if(WIN32)
    target_link_libraries(audio_python_backend PRIVATE ws2_32)
  endif()
  if(AUDIO_BUILD_TESTS)
    add_executable(audio_python_process_tests tests/integration/test_python_process.cpp tests/integration/test_python_mixed.cpp tests/integration/test_python_snapshot_run.cpp)
    target_sources(audio_python_process_tests PRIVATE tests/integration/test_python_mixed_recovery.cpp)
    target_sources(audio_python_process_tests PRIVATE tests/integration/test_python_mixed_queue.cpp)
    target_sources(audio_python_process_tests PRIVATE tests/integration/test_python_extension.cpp)
    target_link_libraries(audio_python_process_tests PRIVATE audio_python_backend audio_algorithms Catch2::Catch2WithMain juce::juce_core)
    target_link_libraries(audio_python_process_tests PRIVATE audio_host_nodes audio_runtime)
    target_link_libraries(audio_python_process_tests PRIVATE audio_run_service juce::juce_gui_basics)
    target_sources(audio_python_process_tests PRIVATE apps/editor/EditorParameterPanel.cpp apps/editor/EditorNodeParameterBinding.cpp)
    target_include_directories(audio_python_process_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/apps/editor")
    target_link_libraries(audio_python_process_tests PRIVATE audio_editor_application)
    add_dependencies(audio_python_process_tests audio_cli)
    target_compile_definitions(audio_python_process_tests PRIVATE AUDIOALGO_CLI_EXE="$<TARGET_FILE:audio_cli>")
    if(MSVC)
      target_compile_options(audio_python_process_tests PRIVATE /utf-8)
    endif()
    target_compile_definitions(audio_python_process_tests PRIVATE AUDIOALGO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" AUDIOALGO_PYTHON_EXE="${Python3_EXECUTABLE}")
    catch_discover_tests(audio_python_process_tests PROPERTIES LABELS "python;process_integration;R6" TIMEOUT 30)
    set(python_runner_cases
      "PY-01 shared host and runner manifest fixtures|test_python_manifest.py|ManifestTests.test_shared_host_runner_contract"
      "PY-01 manifest discovers metadata without importing user code|test_python_manifest.py|ManifestTests.test_new_directory_discovers_block_clip_and_metadata_without_import"
      "PY-01 malformed and duplicate packages commit atomically|test_python_manifest.py|ManifestTests.test_invalid_metadata_is_atomic_and_duplicates_never_replace_existing_nodes"
      "PY-01 schema entrypoint and parameter metadata reject invalid values|test_python_manifest.py|ManifestTests.test_entrypoints_schema_boolean_numbers_and_path_escape_are_rejected"
      "PY-02 external process Gain and Stateful preserve numeric results and lifecycle|test_python_runner.py|PythonRunnerTests.test_real_process_gain_stateful_two_instances_and_reset"
      "PY-02 invalid payloads and outputs diagnose and recover|test_python_runner.py|PythonRunnerTests.test_wrong_dtype_shape_nan_exception_and_payload_failures_recover"
      "PY-05 namespaced modules restart after code changes and preserve history|test_python_runner.py|PythonRunnerTests.test_namespaced_modules_and_restart_update_preserve_history"
      "PY-05 only frozen hashed source may execute|test_python_runner.py|PythonRunnerTests.test_unhashed_bytecode_cannot_override_the_frozen_source"
      "PY-06 cooperative cancellation and bounded log drainage recover|test_python_runner.py|PythonRunnerTests.test_cooperative_cancel_and_log_flood_use_separate_bounded_pipes"
      "PY-07 clip runs once and byte limits precede allocation|test_python_runner.py|PythonRunnerTests.test_clip_runs_once_and_payload_budget_is_checked_before_plugin_import"
      "PY-07 deep run paths preserve payload code and failure recovery|test_python_runner.py|PythonRunnerTests.test_deep_run_paths_preserve_payload_code_and_failure_recovery"
      "PY-08 protocol length version and unknown messages diagnose then recover|test_python_runner.py|PythonRunnerTests.test_protocol_length_version_and_unknown_message_are_diagnosable")
    foreach(python_case IN LISTS python_runner_cases)
      string(REPLACE "|" ";" parts "${python_case}")
      list(GET parts 0 case_name)
      list(GET parts 1 case_file)
      list(GET parts 2 case_method)
      add_test(NAME "${case_name}" COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/${case_file}" "${case_method}")
      set_tests_properties("${case_name}" PROPERTIES LABELS "python;process_integration;R6" TIMEOUT 30)
    endforeach()
  endif()
endif()
endif()
