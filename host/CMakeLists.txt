cmake_minimum_required(VERSION 3.16)
project(tcs_host_tests CXX)
set(CMAKE_CXX_STANDARD 17)
add_compile_definitions(TCS_HOST_BUILD=1)
include_directories(${CMAKE_SOURCE_DIR}/../main)

set(CORE_SRCS
    ${CMAKE_SOURCE_DIR}/../main/tcs_glue.cpp
    ${CMAKE_SOURCE_DIR}/../main/color_math.cpp
    ${CMAKE_SOURCE_DIR}/../main/color_types.cpp
    ${CMAKE_SOURCE_DIR}/../main/color_database.cpp
    ${CMAKE_SOURCE_DIR}/../main/color_matcher.cpp
    ${CMAKE_SOURCE_DIR}/../main/color_description.cpp
    ${CMAKE_SOURCE_DIR}/../main/color_pipeline.cpp
    ${CMAKE_SOURCE_DIR}/../main/auto_calibrate.cpp
    ${CMAKE_SOURCE_DIR}/../main/kona_metadata.cpp
    ${CMAKE_SOURCE_DIR}/../main/konaref.cpp
    ${CMAKE_SOURCE_DIR}/../main/konaref_default.cpp)

add_executable(autocal_host_test ${CMAKE_SOURCE_DIR}/../tests/host/autocal_host_test.cpp ${CORE_SRCS})
add_executable(color_match_host_test ${CMAKE_SOURCE_DIR}/../tests/host/color_match_host_test.cpp ${CORE_SRCS})

add_executable(color_pipeline_unit_tests ${CMAKE_SOURCE_DIR}/../tests/host/color_pipeline_unit_tests.cpp ${CORE_SRCS})

add_executable(test_ciede2000 ${CMAKE_SOURCE_DIR}/../tests/host/test_ciede2000.cpp ${CORE_SRCS})

add_executable(test_delta_e ${CMAKE_SOURCE_DIR}/../tests/host/test_delta_e.cpp ${CORE_SRCS})
