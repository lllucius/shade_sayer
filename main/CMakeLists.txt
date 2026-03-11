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
        "tcs_glue.cpp"
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
