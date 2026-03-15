cmake_minimum_required(VERSION 3.16)
project(tcs_host_tests CXX)
set(CMAKE_CXX_STANDARD 17)
add_compile_definitions(TCS_HOST_BUILD=1)
include_directories(${CMAKE_SOURCE_DIR}/../main)

# Generate Kona reference table from kona_captures.json.
# Only swatches with measured=true and valid Lab values are included.
set(KONA_REF_TABLE_SRC "${CMAKE_SOURCE_DIR}/../main/konaref_default.cpp")
set(KONA_CAPTURE_JSON "${CMAKE_SOURCE_DIR}/../kona_captures.json")
set(KONA_GENERATOR "${CMAKE_SOURCE_DIR}/../scripts/generate_kona_table.py")

if(EXISTS "${KONA_CAPTURE_JSON}" AND EXISTS "${KONA_GENERATOR}")
    # Check whether the JSON file contains measured swatches with Lab values
    execute_process(
        COMMAND python3 -c
            "import json,sys; d=json.load(open('${KONA_CAPTURE_JSON}')); sw=[s for s in d.get('swatches',[]) if s.get('measured') and (s.get('lab') or {}).get('l') is not None]; sys.exit(0 if sw else 1)"
        RESULT_VARIABLE _JSON_HAS_DATA
        OUTPUT_QUIET ERROR_QUIET
    )
    if(_JSON_HAS_DATA EQUAL 0)
        set(KONA_ACTIVE_INPUT "${KONA_CAPTURE_JSON}")
    endif()
endif()

if(DEFINED KONA_ACTIVE_INPUT AND EXISTS "${KONA_GENERATOR}")
    set(KONA_REF_GENERATED "${CMAKE_BINARY_DIR}/generated/konaref_generated.cpp")
    add_custom_command(
        OUTPUT "${KONA_REF_GENERATED}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
        COMMAND python3 "${KONA_GENERATOR}"
                --input "${KONA_ACTIVE_INPUT}"
                --output "${KONA_REF_GENERATED}"
        DEPENDS "${KONA_GENERATOR}" "${KONA_ACTIVE_INPUT}"
        COMMENT "Generating Kona reference table from ${KONA_ACTIVE_INPUT}"
        VERBATIM)
    set(KONA_REF_TABLE_SRC "${KONA_REF_GENERATED}")
    add_custom_target(generate_kona_ref DEPENDS "${KONA_REF_GENERATED}")
endif()

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
    ${KONA_REF_TABLE_SRC})

add_executable(autocal_host_test ${CMAKE_SOURCE_DIR}/../tests/host/autocal_host_test.cpp ${CORE_SRCS})
add_executable(color_match_host_test ${CMAKE_SOURCE_DIR}/../tests/host/color_match_host_test.cpp ${CORE_SRCS})

add_executable(color_pipeline_unit_tests ${CMAKE_SOURCE_DIR}/../tests/host/color_pipeline_unit_tests.cpp ${CORE_SRCS})

add_executable(test_ciede2000 ${CMAKE_SOURCE_DIR}/../tests/host/test_ciede2000.cpp ${CORE_SRCS})

add_executable(test_delta_e ${CMAKE_SOURCE_DIR}/../tests/host/test_delta_e.cpp ${CORE_SRCS})

# kona_regenerate: reads raw sensor readings from stdin, runs pipeline, writes Lab to stdout.
# Used by regenerate_kona_lab.py to replay captured raw data after pipeline changes.
add_executable(kona_regenerate ${CMAKE_SOURCE_DIR}/../tests/host/kona_regenerate.cpp ${CORE_SRCS})
