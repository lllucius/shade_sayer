/**
 * @file kona_regenerate.cpp
 * @brief Host utility to re-process raw Kona swatch sensor readings through the color pipeline.
 *
 * Reads sensor readings from stdin in a simple text format, runs each reading through
 * color_pipeline_identify_from_reading(), and writes scan_lab results to stdout.
 *
 * Used by regenerate_kona_lab.py to replay captured raw data through the current pipeline
 * after pipeline changes (PCCM coefficients, responsivity constants, IR compensation, etc.)
 * without requiring a physical rescan of all 365 swatches.
 *
 * Protocol:
 *   stdin:  One reading per line:
 *             <swatch_id> <x> <y> <z> <ir> <clear> <gain> <integration_ms> <led_enabled>
 *           A line starting with '#' is treated as a comment and ignored.
 *   stdout: One result per line:
 *             OK <swatch_id> <l> <a> <b>
 *           On pipeline error:
 *             ERR <swatch_id>
 *
 * The binary exits with 0 on success (all readings processed), non-zero on fatal error.
 *
 * Usage (called by regenerate_kona_lab.py):
 *   echo "449 32200000 33400000 27900000 619520 21435649 5 100 1" | ./kona_regenerate
 */

#include "color_pipeline.h"
#include "color_types.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>

/**
 * @brief Initialize the color pipeline with the same configuration used in main.cpp.
 *
 * These values must stay in sync with the production configuration in main/main.cpp.
 * When pipeline constants change, update both files together.
 */
static bool initPipeline()
{
    color_pipeline_config_t cfg{};
    cfg.min_luminance      = 5.0f;
    cfg.max_delta_e        = 20.0f;
    cfg.use_white_balance  = true;
    cfg.num_samples        = 1;   // No averaging: each reading is a representative mean
    cfg.sample_delay_ms    = 0;

    // Color tuning – must match main/main.cpp
    cfg.gray_threshold    = 2.0f;
    cfg.color_threshold   = 60.0f;
    cfg.saturation_boost  = 1.5f;

    // White reference for cool-white LED – must match main/main.cpp
    cfg.white_reference_led     = {95.0f, 100.0f, 280.0f};
    cfg.white_reference_ambient = {95.047f, 100.0f, 108.883f};

    return color_pipeline_init(&cfg) == ESP_OK;
}

int main()
{
    if (!initPipeline())
    {
        std::fprintf(stderr, "kona_regenerate: color_pipeline_init() failed\n");
        return 1;
    }

    char line[512];
    while (std::fgets(line, sizeof(line), stdin) != nullptr)
    {
        // Skip blank lines and comments
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
        {
            continue;
        }

        // Parse: <swatch_id> <x> <y> <z> <ir> <clear> <gain> <integration_ms> <led_enabled>
        int swatch_id = 0;
        unsigned long x = 0, y = 0, z = 0, ir = 0, clear = 0;
        int gain = 0, integration_ms = 0, led_enabled = 0;

        int parsed = std::sscanf(line, "%d %lu %lu %lu %lu %lu %d %d %d",
                                 &swatch_id, &x, &y, &z, &ir, &clear,
                                 &gain, &integration_ms, &led_enabled);

        if (parsed != 9)
        {
            std::fprintf(stderr, "kona_regenerate: malformed input line (got %d fields): %s",
                         parsed, line);
            continue;
        }

        sensor_reading_t reading{};
        reading.x              = static_cast<uint32_t>(x);
        reading.y              = static_cast<uint32_t>(y);
        reading.z              = static_cast<uint32_t>(z);
        reading.ir             = static_cast<uint32_t>(ir);
        reading.clear          = static_cast<uint32_t>(clear);
        reading.gain           = static_cast<uint8_t>(gain);
        reading.integration_ms = static_cast<uint16_t>(integration_ms);
        reading.saturated      = false;

        color_result_t result{};
        esp_err_t ret = color_pipeline_identify_from_reading(
            &reading, led_enabled != 0, &result);

        if (ret != ESP_OK)
        {
            std::printf("ERR %d\n", swatch_id);
        }
        else
        {
            std::printf("OK %d %.6f %.6f %.6f\n",
                        swatch_id,
                        static_cast<double>(result.scan_lab.l),
                        static_cast<double>(result.scan_lab.a),
                        static_cast<double>(result.scan_lab.b));
        }
        std::fflush(stdout);
    }

    return 0;
}
