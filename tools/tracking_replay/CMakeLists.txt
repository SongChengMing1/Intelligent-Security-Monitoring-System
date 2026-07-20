cmake_minimum_required(VERSION 3.10)
project(tracking_replay CXX)
set(CMAKE_CXX_STANDARD 11)
set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
execute_process(COMMAND git rev-parse --short=12 HEAD WORKING_DIRECTORY ${PROJECT_ROOT} OUTPUT_VARIABLE GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE)
add_executable(tracking_replay
  main.cc session_reader.cc replay_runner.cc replay_writer.cc replay_comparator.cc
  ${PROJECT_ROOT}/src/analysis_record.cc ${PROJECT_ROOT}/src/analysis_record_json.cc ${PROJECT_ROOT}/src/analysis_recorder.cc
  ${PROJECT_ROOT}/src/observation_policy.cc ${PROJECT_ROOT}/src/object_tracker.cc ${PROJECT_ROOT}/src/tracking_profile.cc ${PROJECT_ROOT}/src/sha256.cc
  ${PROJECT_ROOT}/third_party/bytetrack/src/BYTETracker.cpp ${PROJECT_ROOT}/third_party/bytetrack/src/STrack.cpp
  ${PROJECT_ROOT}/third_party/bytetrack/src/kalmanFilter.cpp ${PROJECT_ROOT}/third_party/bytetrack/src/lapjv.cpp ${PROJECT_ROOT}/third_party/bytetrack/src/utils.cpp)
target_compile_definitions(tracking_replay PRIVATE EIGEN_MPL2_ONLY RKNN_DETECT_GIT_COMMIT="${GIT_COMMIT}")
target_include_directories(tracking_replay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_ROOT}/include
  ${PROJECT_ROOT}/third_party/nlohmann/include ${PROJECT_ROOT}/third_party/bytetrack/include ${PROJECT_ROOT}/third_party/eigen3)
target_link_libraries(tracking_replay pthread)
