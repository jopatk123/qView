# Optional CAF (content-aware fill) tools — no Qt dependency.
# Can be built as a subdirectory (BUILD_CAF_TOOLS=ON) or standalone:
#   cmake -S src/qvcaf -B build-caf -DCMAKE_BUILD_TYPE=Release && cmake --build build-caf -j

cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(qvcaf_tools LANGUAGES CXX)
  set(QVCAF_STANDALONE ON)
else()
  set(QVCAF_STANDALONE OFF)
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(QVCAF_STANDALONE)
  set(QVCAF_STB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/stb")
  set(QVCAF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
else()
  set(QVCAF_STB_DIR "${CMAKE_SOURCE_DIR}/third_party/stb")
  set(QVCAF_ROOT "${CMAKE_SOURCE_DIR}")
endif()

add_library(qvcaf STATIC qvcaf.cpp)
target_include_directories(qvcaf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
  target_compile_options(qvcaf PRIVATE /W3)
else()
  target_compile_options(qvcaf PRIVATE -Wall -Wextra -O2)
endif()

add_executable(qvcaf-cli qvcaf_cli.cpp)
target_link_libraries(qvcaf-cli PRIVATE qvcaf)
target_include_directories(qvcaf-cli PRIVATE ${QVCAF_STB_DIR})

enable_testing()
add_test(NAME qvcaf_cli_usage COMMAND qvcaf-cli)
set_tests_properties(qvcaf_cli_usage PROPERTIES WILL_FAIL TRUE)

set(CAF_SAMPLE_DIR "/workspace/_archive/2026-10/open-ps-bitmappery-fork/workdir/scratch/caf-feasibility/synthetic_small_rect")
if(EXISTS "${CAF_SAMPLE_DIR}/01_original.png")
  set(CAF_OUT_DIR "${CMAKE_BINARY_DIR}/caf-test-out")
  file(MAKE_DIRECTORY "${CAF_OUT_DIR}")
  add_test(
    NAME qvcaf_synthetic_small_rect
    COMMAND qvcaf-cli
            "${CAF_SAMPLE_DIR}/01_original.png"
            "${CAF_SAMPLE_DIR}/02_mask.png"
            "${CAF_OUT_DIR}/out.png"
  )
endif()
