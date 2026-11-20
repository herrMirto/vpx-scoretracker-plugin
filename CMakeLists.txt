cmake_minimum_required(VERSION 3.24)

set(PLATFORM "" CACHE STRING "ios, ios-simulator or android for static builds; empty for the host")
if(PLATFORM STREQUAL "ios" OR PLATFORM STREQUAL "ios-simulator")
   set(CMAKE_SYSTEM_NAME iOS)
   set(CMAKE_OSX_DEPLOYMENT_TARGET 18.0)
   set(CMAKE_OSX_ARCHITECTURES arm64)
   if(PLATFORM STREQUAL "ios-simulator")
      set(CMAKE_OSX_SYSROOT iphonesimulator)
   endif()
elseif(PLATFORM STREQUAL "android")
   set(CMAKE_SYSTEM_NAME Android)
   set(CMAKE_SYSTEM_VERSION 33)
   set(CMAKE_ANDROID_ARCH_ABI arm64-v8a)
elseif(NOT PLATFORM STREQUAL "")
   message(FATAL_ERROR "Unsupported PLATFORM=${PLATFORM}; leave empty for desktop builds")
elseif(CMAKE_HOST_APPLE)
   set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "")
endif()
file(STRINGS VERSION SCORETRACKER_VERSION LIMIT_COUNT 1)
project(scoretracker_vpx_plugin VERSION "${SCORETRACKER_VERSION}" LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
   add_compile_options(/utf-8)
endif()
set(CMAKE_C_STANDARD 99)
if(PLATFORM STREQUAL "")
   option(BUILD_STATIC "Build a library for linking into libvpinball" OFF)
else()
   set(BUILD_STATIC ON)
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/include/plugins/VPXPlugin.h")
   message(FATAL_ERROR "VPX SDK missing; run platforms/<platform>/external.sh first")
endif()
set(SCORETRACKER_SOURCES
   src/ScoreTrackerPlugin.cpp src/GameRecord.cpp src/ScriptRecorder.cpp src/ScoresFileWriter.cpp
   src/common.h src/GameRecord.h src/ScriptRecorder.h src/ScoresFileWriter.h
   src/PinMameCapture.cpp src/PinMameCapture.h)
if(BUILD_STATIC)
   add_library(ScoreTrackerPlugin STATIC ${SCORETRACKER_SOURCES})
   set_target_properties(ScoreTrackerPlugin PROPERTIES OUTPUT_NAME plugin-scoretracker POSITION_INDEPENDENT_CODE ON)
else()
   add_library(ScoreTrackerPlugin MODULE ${SCORETRACKER_SOURCES})
   set_target_properties(ScoreTrackerPlugin PROPERTIES PREFIX "" OUTPUT_NAME plugin-scoretracker SKIP_BUILD_RPATH ON)
   if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
      set_target_properties(ScoreTrackerPlugin PROPERTIES OUTPUT_NAME plugin-scoretracker64)
   elseif(APPLE)
      set_target_properties(ScoreTrackerPlugin PROPERTIES SUFFIX .dylib)
   endif()
endif()
target_include_directories(ScoreTrackerPlugin PRIVATE src third-party/include)
if(WIN32)
   if(MSVC)
      set_target_properties(ScoreTrackerPlugin PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
   elseif(MINGW)
      target_link_options(ScoreTrackerPlugin PRIVATE -static-libgcc -static-libstdc++)
   endif()
endif()
set(STAGE_DIR "${CMAKE_BINARY_DIR}/scoretracker")
set_property(TARGET ScoreTrackerPlugin APPEND PROPERTY LINK_DEPENDS
   "${CMAKE_CURRENT_SOURCE_DIR}/INSTALL.txt"
   "${CMAKE_CURRENT_SOURCE_DIR}/schema/scores.schema.json"
   "${CMAKE_CURRENT_SOURCE_DIR}/docs/CAPTURE-CONTRACT.md"
   "${CMAKE_CURRENT_SOURCE_DIR}/examples/table-recorder.vbs")
add_custom_command(TARGET ScoreTrackerPlugin POST_BUILD
   COMMAND "${CMAKE_COMMAND}" -E rm -rf "${STAGE_DIR}"
   COMMAND "${CMAKE_COMMAND}" -E make_directory "${STAGE_DIR}/licenses"
   COMMAND "${CMAKE_COMMAND}" -E make_directory "${STAGE_DIR}/schema"
   COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/schema/scores.schema.json" "${STAGE_DIR}/schema/scores.schema.json"
   COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/examples" "${STAGE_DIR}/examples"
   COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/docs/CAPTURE-CONTRACT.md" "${STAGE_DIR}/CAPTURE-CONTRACT.md"
   COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:ScoreTrackerPlugin>" "${STAGE_DIR}/"
   COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${STAGE_DIR}/licenses/ScoreTracker.txt"
   COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/third-party/include/nlohmann/json.hpp" "${STAGE_DIR}/licenses/nlohmann-json.hpp")
if(NOT BUILD_STATIC)
   add_custom_command(TARGET ScoreTrackerPlugin POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin.cfg" "${STAGE_DIR}/plugin.cfg"
      COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/INSTALL.txt" "${STAGE_DIR}/README.txt")
endif()

if(PLATFORM STREQUAL "")
   include(CTest)
else()
   option(BUILD_TESTING "Build replay tests with a cross toolchain" OFF)
endif()
if(BUILD_TESTING)
   find_package(Threads REQUIRED)
   add_executable(capture-tests tests/capture.cpp src/GameRecord.cpp src/ScoresFileWriter.cpp src/ScriptRecorder.cpp src/PinMameCapture.cpp)
   target_include_directories(capture-tests PRIVATE src third-party/include)
   target_link_libraries(capture-tests PRIVATE Threads::Threads)
   add_test(NAME capture-and-storage COMMAND capture-tests "${CMAKE_CURRENT_BINARY_DIR}/test-history")
   if(UNIX AND NOT BUILD_STATIC)
      add_executable(host-compatibility tests/compatibility.cpp)
      target_include_directories(host-compatibility PRIVATE third-party/include)
      target_link_libraries(host-compatibility PRIVATE ${CMAKE_DL_LIBS})
      add_test(NAME plugin-host COMMAND host-compatibility "$<TARGET_FILE:ScoreTrackerPlugin>" "${CMAKE_CURRENT_BINARY_DIR}/host-history")
   endif()
endif()
