cmake_minimum_required(VERSION 3.20)
project(test_gstreamer CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(oatpp CONFIG REQUIRED)
find_package(oatpp-postgresql CONFIG REQUIRED)
find_package(oatpp-swagger CONFIG REQUIRED)
find_package(oatpp-websocket CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(GSTREAMER QUIET IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-rtsp-1.0
    gstreamer-rtsp-server-1.0
    gstreamer-app-1.0
    gstreamer-video-1.0
)
if(NOT GSTREAMER_FOUND)
    message(FATAL_ERROR
        "Missing GStreamer development packages. On Debian/Ubuntu install: "
        "sudo apt-get install -y libgstreamer1.0-dev "
        "libgstreamer-plugins-base1.0-dev libgstrtspserver-1.0-dev "
        "gstreamer1.0-plugins-good gstreamer1.0-plugins-bad")
endif()

# --- AI subsystem dependencies (in-process: RKNN NPU + RGA + OpenCV) ----------
pkg_check_modules(OPENCV REQUIRED IMPORTED_TARGET opencv4)

# RKNN runtime headers. librknnrt.so / librga.so live in /usr/lib.
set(RKNN_INCLUDE_DIR
    "/home/orangepi/Documents/rknn-toolkit2/rknpu2/runtime/Linux/librknn_api/include"
    CACHE PATH "Directory containing rknn_api.h")

# Reused detect_ai core (yolov8 / postprocess / bytetrack / face_align / ...).
set(AI_DETECT_SOURCES
    src/ai/detect_ai/face_align.cc
    src/ai/detect_ai/face_recognition.cc
    src/ai/detect_ai/file_utils.c
    src/ai/detect_ai/postprocess.cc
    src/ai/detect_ai/yolov8.cc
    src/ai/detect_ai/yolov8_pose.cc
    src/ai/detect_ai/yolov8_seg.cc)

add_executable(test_gstreamer
    src/App.cpp
    ${AI_DETECT_SOURCES}
)

target_include_directories(test_gstreamer PRIVATE
    src
    src/ai
    src/ai/detect_ai
    ${RKNN_INCLUDE_DIR}
)

target_link_libraries(test_gstreamer PRIVATE
    oatpp::oatpp
    oatpp::oatpp-postgresql
    oatpp::oatpp-swagger
    oatpp::oatpp-websocket
    PkgConfig::GSTREAMER
    PkgConfig::OPENCV
    rknnrt
    rga
)

# Path to swagger UI static resources, shipped with oatpp-swagger.
# vcpkg installs them under include/oatpp-X.Y.Z/bin/oatpp-swagger/res.
# We resolve the path at configure time via find_path so this works
# across oatpp versions / vcpkg layouts.
find_path(OATPP_SWAGGER_RES_DIR
    NAMES favicon-16x16.png
    PATHS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include
    PATH_SUFFIXES
        oatpp-1.3.0/bin/oatpp-swagger/res
        oatpp/bin/oatpp-swagger/res
        share/oatpp-swagger/bin/oatpp-swagger/res
    NO_DEFAULT_PATH
    REQUIRED
)
message(STATUS "Swagger UI resources: ${OATPP_SWAGGER_RES_DIR}")

target_compile_definitions(test_gstreamer PRIVATE
    OATPP_SWAGGER_RES_PATH="${OATPP_SWAGGER_RES_DIR}"
)

# Make config/ and sql/ available next to the binary so the default
# config path "config/config.json" works when running from build dir.
add_custom_command(TARGET test_gstreamer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/config
        $<TARGET_FILE_DIR:test_gstreamer>/config
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/sql
        $<TARGET_FILE_DIR:test_gstreamer>/sql
)

enable_testing()

add_executable(stream_logic_tests
    tests/StreamLogicTests.cpp
)
target_include_directories(stream_logic_tests PRIVATE src)
add_test(NAME stream_logic_tests COMMAND stream_logic_tests)
