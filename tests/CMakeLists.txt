cmake_minimum_required(VERSION 3.10)

project(rknn_detect_host_tests CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(v350_object_tracker_test
    v350_object_tracker_test.cc
    ${PROJECT_ROOT}/src/object_tracker.cc
    ${PROJECT_ROOT}/third_party/bytetrack/src/BYTETracker.cpp
    ${PROJECT_ROOT}/third_party/bytetrack/src/STrack.cpp
    ${PROJECT_ROOT}/third_party/bytetrack/src/kalmanFilter.cpp
    ${PROJECT_ROOT}/third_party/bytetrack/src/lapjv.cpp
    ${PROJECT_ROOT}/third_party/bytetrack/src/utils.cpp
)

add_executable(v350_detection_result_views_test
    v350_detection_result_views_test.cc
    ${PROJECT_ROOT}/src/detection_result_views.cc
)

add_executable(v351_contracts_test
    v351_contracts_test.cc
    ${PROJECT_ROOT}/src/analysis_record.cc
    ${PROJECT_ROOT}/src/tracking_profile.cc
    ${PROJECT_ROOT}/src/sha256.cc
)

add_executable(v351_observation_policy_test
    v351_observation_policy_test.cc
    ${PROJECT_ROOT}/src/analysis_record.cc
    ${PROJECT_ROOT}/src/observation_policy.cc
)

add_executable(v351_analysis_recorder_test
    v351_analysis_recorder_test.cc
    ${PROJECT_ROOT}/src/analysis_record.cc
    ${PROJECT_ROOT}/src/analysis_record_json.cc
    ${PROJECT_ROOT}/src/analysis_recorder.cc
    ${PROJECT_ROOT}/src/object_tracker.cc
    ${PROJECT_ROOT}/src/sha256.cc
    ${PROJECT_ROOT}/third_party/bytetrack/src/BYTETracker.cpp
    ${PROJECT_ROOT}/third_party/bytetrack/src/STrack.cpp
    ${PROJECT_ROOT}/third_party/bytetrack/src/kalmanFilter.cpp
    ${PROJECT_ROOT}/third_party/bytetrack/src/lapjv.cpp
    ${PROJECT_ROOT}/third_party/bytetrack/src/utils.cpp
)

add_executable(v351_tracking_replay_test
    v351_tracking_replay_test.cc
    ${PROJECT_ROOT}/tools/tracking_replay/replay_runner.cc
    ${PROJECT_ROOT}/tools/tracking_replay/replay_comparator.cc
    ${PROJECT_ROOT}/tools/tracking_replay/session_reader.cc ${PROJECT_ROOT}/tools/tracking_replay/replay_writer.cc
    ${PROJECT_ROOT}/src/analysis_record.cc ${PROJECT_ROOT}/src/analysis_record_json.cc ${PROJECT_ROOT}/src/analysis_recorder.cc
    ${PROJECT_ROOT}/src/observation_policy.cc ${PROJECT_ROOT}/src/object_tracker.cc ${PROJECT_ROOT}/src/tracking_profile.cc ${PROJECT_ROOT}/src/sha256.cc
    ${PROJECT_ROOT}/third_party/bytetrack/src/BYTETracker.cpp ${PROJECT_ROOT}/third_party/bytetrack/src/STrack.cpp
    ${PROJECT_ROOT}/third_party/bytetrack/src/kalmanFilter.cpp ${PROJECT_ROOT}/third_party/bytetrack/src/lapjv.cpp ${PROJECT_ROOT}/third_party/bytetrack/src/utils.cpp
)

add_executable(v36_intrusion_evaluator_test
    v36_intrusion_evaluator_test.cc
    ${PROJECT_ROOT}/src/intrusion_evaluator.cc
)

add_executable(stream_pipeline_test
    stream_pipeline_test.cc
    ${PROJECT_ROOT}/src/stream_pipeline.cc
)

target_compile_definitions(v350_object_tracker_test PRIVATE EIGEN_MPL2_ONLY)
target_include_directories(v350_object_tracker_test PRIVATE
    ${PROJECT_ROOT}/include
    ${PROJECT_ROOT}/third_party/bytetrack/include
    ${PROJECT_ROOT}/third_party/eigen3
)

target_include_directories(v350_detection_result_views_test PRIVATE
    ${PROJECT_ROOT}/include
)

target_include_directories(v351_contracts_test PRIVATE
    ${PROJECT_ROOT}/include
    ${PROJECT_ROOT}/third_party/nlohmann/include
)

target_include_directories(v351_observation_policy_test PRIVATE
    ${PROJECT_ROOT}/include
)

target_compile_definitions(v351_analysis_recorder_test PRIVATE EIGEN_MPL2_ONLY)
target_include_directories(v351_analysis_recorder_test PRIVATE
    ${PROJECT_ROOT}/include
    ${PROJECT_ROOT}/third_party/nlohmann/include
    ${PROJECT_ROOT}/third_party/bytetrack/include
    ${PROJECT_ROOT}/third_party/eigen3
)
target_link_libraries(v351_analysis_recorder_test pthread)
target_compile_definitions(v351_tracking_replay_test PRIVATE EIGEN_MPL2_ONLY)
target_include_directories(v351_tracking_replay_test PRIVATE ${PROJECT_ROOT}/include ${PROJECT_ROOT}/tools/tracking_replay
    ${PROJECT_ROOT}/third_party/nlohmann/include ${PROJECT_ROOT}/third_party/bytetrack/include ${PROJECT_ROOT}/third_party/eigen3)
target_link_libraries(v351_tracking_replay_test pthread)

target_include_directories(v36_intrusion_evaluator_test PRIVATE
    ${PROJECT_ROOT}/include
)

target_include_directories(stream_pipeline_test PRIVATE
    ${PROJECT_ROOT}/include
)
target_link_libraries(stream_pipeline_test pthread)

enable_testing()
add_test(
    NAME v350_object_tracker
    COMMAND v350_object_tracker_test ${CMAKE_CURRENT_SOURCE_DIR}/data/v350_tracking_sequences.csv
)
add_test(
    NAME v350_detection_result_views
    COMMAND v350_detection_result_views_test
)
add_test(
    NAME v351_contracts
    COMMAND v351_contracts_test ${CMAKE_CURRENT_SOURCE_DIR}/data
)
add_test(
    NAME v351_observation_policy
    COMMAND v351_observation_policy_test
)
add_test(
    NAME v351_analysis_recorder
    COMMAND v351_analysis_recorder_test
)
add_test(NAME v351_tracking_replay COMMAND v351_tracking_replay_test ${CMAKE_CURRENT_SOURCE_DIR}/data)
add_test(NAME v36_intrusion_evaluator COMMAND v36_intrusion_evaluator_test)
add_test(NAME stream_pipeline COMMAND stream_pipeline_test)
