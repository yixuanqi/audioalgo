cmake_minimum_required(VERSION 3.28)
project(AudioAlgoAlgorithmsConsumer LANGUAGES CXX)

find_package(AudioAlgo 0.1 CONFIG REQUIRED)

add_executable(algorithms_consumer main.cpp)
target_link_libraries(algorithms_consumer PRIVATE AudioAlgo::algorithms)
target_compile_features(algorithms_consumer PRIVATE cxx_std_20)
set_target_properties(algorithms_consumer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(config Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${config}" config_upper)
  set_target_properties(algorithms_consumer PROPERTIES "RUNTIME_OUTPUT_DIRECTORY_${config_upper}" "${CMAKE_BINARY_DIR}/bin")
endforeach()
