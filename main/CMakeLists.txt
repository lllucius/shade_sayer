set(KONA_REF_TABLE_SRC "konaref_default.cpp")

# Generate Kona reference table from kona_captures.json.
# Only swatches with measured=true and valid Lab values are included.
set(KONA_CAPTURE_JSON "${CMAKE_SOURCE_DIR}/kona_captures.json")
set(KONA_GENERATOR "${CMAKE_SOURCE_DIR}/scripts/generate_kona_table.py")

if(EXISTS "${KONA_CAPTURE_JSON}" AND EXISTS "${KONA_GENERATOR}")
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
endif()

idf_component_register(
    SRCS
        "main.cpp"
        "tcs3530_driver.cpp"
        "max17048_driver.cpp"
        "color_pipeline.cpp"
        "color_description.cpp"
        "user_interface.cpp"
        "power_manager.cpp"
        "audio_renderer.cpp"
        "tts_manager.cpp"
        "color_math.cpp"
        "color_matcher.cpp"
        "color_database.cpp"
        "color_types.cpp"
        "auto_calibrate.cpp"
        "i2c_bus_manager.cpp"
        "console_logger.cpp"
        "kona_metadata.cpp"
        "konaref.cpp"
        ${KONA_REF_TABLE_SRC}
        "tcs_glue.cpp"
        ${KONA_REF_SRC}
    INCLUDE_DIRS
        "."
    REQUIRES
        driver
        esp_timer
        nvs_flash
        esp_psram
)

# Set C++ standard
set_target_properties(${COMPONENT_LIB} PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
