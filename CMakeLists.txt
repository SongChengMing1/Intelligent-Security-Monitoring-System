cmake_minimum_required(VERSION 3.10)

project(rknn_detect)

execute_process(COMMAND git rev-parse --short=12 HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE RKNN_DETECT_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT RKNN_DETECT_GIT_COMMIT)
  set(RKNN_DETECT_GIT_COMMIT "unknown")
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,--allow-shlib-undefined")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wl,--allow-shlib-undefined")
set(RKNPU2_ROOT_DEFAULT "")
if(DEFINED ENV{RKNPU2_ROOT} AND NOT "$ENV{RKNPU2_ROOT}" STREQUAL "")
  set(RKNPU2_ROOT_DEFAULT "$ENV{RKNPU2_ROOT}")
endif()
set(RKNPU2_ROOT "${RKNPU2_ROOT_DEFAULT}" CACHE PATH "RKNN rknpu2 SDK root")
if(NOT RKNPU2_ROOT OR NOT EXISTS "${RKNPU2_ROOT}")
  message(FATAL_ERROR
    "RKNPU2_ROOT is not configured or does not exist: ${RKNPU2_ROOT}. "
    "Set -DRKNPU2_ROOT=/path/to/rknpu2 or export RKNPU2_ROOT before configuring.")
endif()
set(RKNPU2_3RDPARTY_PATH ${RKNPU2_ROOT}/examples/3rdparty)
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE
  ${CMAKE_SOURCE_DIR}/third_party/nlohmann/include
)

# install target and libraries
set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/install/rknn_detect_${CMAKE_SYSTEM_NAME})

set(CMAKE_SKIP_INSTALL_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")

# rknn api
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
  message(FATAL_ERROR "Android builds are not supported by the GStreamer board release")
endif()

if(TARGET_SOC STREQUAL "rk356x")
  set(RKNN_API_PATH ${RKNPU2_ROOT}/runtime/RK356X/${CMAKE_SYSTEM_NAME}/librknn_api)
elseif(TARGET_SOC STREQUAL "rk3588")
  set(RKNN_API_PATH ${RKNPU2_ROOT}/runtime/RK3588/${CMAKE_SYSTEM_NAME}/librknn_api)
else()
  message(FATAL_ERROR "TARGET_SOC is not set, ref value: rk356x or rk3588 or rv110x")
endif()

if (CMAKE_C_COMPILER MATCHES "aarch64")
  set(LIB_ARCH aarch64)
else()
  set(LIB_ARCH armhf)
endif()
set(RKNN_RT_LIB ${RKNN_API_PATH}/${LIB_ARCH}/librknnrt.so)
include_directories(${RKNN_API_PATH}/include)
include_directories(${RKNPU2_3RDPARTY_PATH})

# opencv
if(LIB_ARCH STREQUAL "armhf")
  set(OpenCV_DIR ${RKNPU2_3RDPARTY_PATH}/opencv/opencv-linux-armhf/share/OpenCV)
else()
  set(OpenCV_DIR ${RKNPU2_3RDPARTY_PATH}/opencv/opencv-linux-aarch64/share/OpenCV)
endif()
find_package(OpenCV REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST_RTSP REQUIRED
  gstreamer-1.0
  gstreamer-app-1.0
  gstreamer-video-1.0
  gstreamer-rtp-1.0
  gstreamer-rtsp-server-1.0
)
include_directories(${GST_RTSP_INCLUDE_DIRS})
link_directories(${GST_RTSP_LIBRARY_DIRS})

#rga
if(TARGET_SOC STREQUAL "rk356x")
  set(RGA_PATH ${RKNPU2_3RDPARTY_PATH}/rga/RK356X)
elseif(TARGET_SOC STREQUAL "rk3588")
  set(RGA_PATH ${RKNPU2_3RDPARTY_PATH}/rga/RK3588)
else()
  message(FATAL_ERROR "TARGET_SOC is not set, ref value: rk356x or rk3588")
endif()
if (CMAKE_C_COMPILER MATCHES "aarch64")
  set(LIB_ARCH aarch64)
else()
  set(LIB_ARCH armhf)
endif()
set(RGA_LIB ${RGA_PATH}/lib/Linux/${LIB_ARCH}/librga.so)
include_directories( ${RGA_PATH}/include)

set(CMAKE_INSTALL_RPATH "lib")

# rknn_detect_demo
include_directories( ${CMAKE_SOURCE_DIR}/include)

add_executable(rknn_detect_demo
        src/main.cc
        src/detector.cc
        src/image_preprocess.cc
        src/preprocess_transform.cc
        src/perf_stats.cc
        src/postprocess.cc
        src/result_render.cc
)

target_link_libraries(rknn_detect_demo
  ${RKNN_RT_LIB}
  ${RGA_LIB}
  ${OpenCV_LIBS}
)

add_executable(rknn_detect_stream_server
        src/analysis_record.cc
        src/analysis_record_json.cc
        src/analysis_recorder.cc
        src/main_stream_server.cc
        src/detection_result_views.cc
        src/detector.cc
        src/image_preprocess_opencv.cc
        src/intrusion_evaluator.cc
        src/object_tracker.cc
        src/observation_policy.cc
        src/preprocess_transform.cc
        src/perf_stats.cc
        src/postprocess.cc
        src/runtime_evidence.cc
        src/stream_pipeline.cc
        src/stream_overlay_policy.cc
        src/tracking_profile.cc
        src/sha256.cc
        third_party/bytetrack/src/BYTETracker.cpp
        third_party/bytetrack/src/STrack.cpp
        third_party/bytetrack/src/kalmanFilter.cpp
        third_party/bytetrack/src/lapjv.cpp
        third_party/bytetrack/src/utils.cpp
)

target_compile_definitions(rknn_detect_stream_server PRIVATE EIGEN_MPL2_ONLY RKNN_DETECT_GIT_COMMIT="${RKNN_DETECT_GIT_COMMIT}")
target_include_directories(rknn_detect_stream_server PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/bytetrack/include
  ${CMAKE_SOURCE_DIR}/third_party/eigen3
)

target_link_libraries(rknn_detect_stream_server
  ${RKNN_RT_LIB}
  ${OpenCV_LIBS}
  pthread
  nlohmann_json
  ${GST_RTSP_LIBRARIES}
)

add_executable(rga_resize_probe
        tools/rga_resize_probe.cc
)

target_link_libraries(rga_resize_probe
  ${RGA_LIB}
)

add_executable(rga_rknn_mem_probe
        tools/rga_rknn_mem_probe.cc
)

target_link_libraries(rga_rknn_mem_probe
  ${RKNN_RT_LIB}
  ${RGA_LIB}
)


# install target and libraries
set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/install/rknn_detect_${CMAKE_SYSTEM_NAME})
install(TARGETS rknn_detect_demo DESTINATION ./)
install(TARGETS rknn_detect_stream_server DESTINATION ./)
install(TARGETS rga_resize_probe DESTINATION ./)
install(TARGETS rga_rknn_mem_probe DESTINATION ./)

install(PROGRAMS ${RKNN_RT_LIB} DESTINATION lib)
install(PROGRAMS ${RGA_LIB} DESTINATION lib)
install(DIRECTORY model DESTINATION ./)
