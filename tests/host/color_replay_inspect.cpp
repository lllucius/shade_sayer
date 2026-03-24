/**
 * @file color_replay_inspect.cpp
 * @brief Host tool for single-capture verbose color pipeline inspection.
 *
 * Runs one raw sensor reading through the full color pipeline and prints every
 * significant intermediate and final value in a human-readable format.  Use this
 * to diagnose misclassifications such as a green surface being identified as yellow.
 *
 * Input: one sensor reading supplied as command-line arguments or via stdin.
 *
 * Usage (CLI args):
 *   ./color_replay_inspect <x> <y> <z> <ir> <clear> <gain> <integration_ms> <led_enabled>
 *   ./color_replay_inspect <x> <y> <z> <ir> <clear> <gain> <integration_ms> <led_enabled> [label]
 *
 * Usage (stdin, same format as kona_regenerate):
 *   echo "11517952 12179200 7376128 249344 8603648 4 100 1" | ./color_replay_inspect
 *   echo "11517952 12179200 7376128 249344 8603648 4 100 1 green_wall_paint" | ./color_replay_inspect
 *
 * Both forms accept an optional label/id as the 9th token on the same line (stdin) or
 * as the 9th command-line argument.  The label is purely cosmetic.
 *
 * The pipeline logs (via ESP_LOGx) are written to stderr and show the detailed
 * internal correction stages (PCCM, IR compensation, D65 scaling, etc.).
 * The structured summary is written to stdout.
 *
 * See docs/replay-harness.md for the full workflow and JSON capture format.
 */

#include "color_pipeline.h"
#include "color_types.h"
#include "color_math.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>

/** Pipeline configuration identical to kona_regenerate / main production init. */
static bool initPipeline()
{
    color_pipeline_config_t cfg{};
    cfg.min_luminance      = 5.0f;
    cfg.max_delta_e        = 20.0f;
    cfg.use_white_balance  = true;
    cfg.num_samples        = 1;
    cfg.sample_delay_ms    = 0;
    cfg.gray_threshold     = 2.0f;
    cfg.color_threshold    = 60.0f;
    cfg.saturation_boost   = 1.5f;
    cfg.white_reference_led     = {95.0f, 100.0f, 280.0f};
    cfg.white_reference_ambient = {95.047f, 100.0f, 108.883f};
    cfg.use_material_correction = true;
    cfg.assumed_material        = MATERIAL_FABRIC;
    cfg.auto_detect_material    = false;
    return color_pipeline_init(&cfg) == ESP_OK;
}

/**
 * @brief Parse eight numeric fields (x y z ir clear gain int_ms led) from a string array.
 *
 * @param tokens  Pointer to array of C-strings starting at the first field.
 * @param n       Number of tokens available.
 * @param reading Output sensor reading (populated on success).
 * @param led_out True when led token is non-zero.
 * @return True on successful parse.
 */
static bool parseReading(const char* const* tokens, int n,
                         sensor_reading_t& reading, bool& led_out)
{
    if (n < 8)
    {
        return false;
    }

    reading.x              = static_cast<uint32_t>(std::strtoul(tokens[0], nullptr, 10));
    reading.y              = static_cast<uint32_t>(std::strtoul(tokens[1], nullptr, 10));
    reading.z              = static_cast<uint32_t>(std::strtoul(tokens[2], nullptr, 10));
    reading.ir             = static_cast<uint32_t>(std::strtoul(tokens[3], nullptr, 10));
    reading.clear          = static_cast<uint32_t>(std::strtoul(tokens[4], nullptr, 10));
    reading.gain           = static_cast<uint8_t> (std::strtoul(tokens[5], nullptr, 10));
    reading.integration_ms = static_cast<uint16_t>(std::strtoul(tokens[6], nullptr, 10));
    led_out                = std::strtol(tokens[7], nullptr, 10) != 0;
    reading.saturated      = false;
    return true;
}

/** @brief Print the full inspection report for one result to stdout. */
static void printInspection(const char* label,
                             const sensor_reading_t& reading,
                             bool led_enabled,
                             const color_result_t& result)
{
    std::printf("\n");
    std::printf("=== Color Replay Inspection%s%s ===\n",
                (label[0] != '\0') ? ": " : "",
                label);

    std::printf("\n--- Raw Input ---\n");
    std::printf("  x=%-12u  y=%-12u  z=%-12u\n",
                (unsigned)reading.x, (unsigned)reading.y, (unsigned)reading.z);
    std::printf("  ir=%-11u  clear=%-9u\n",
                (unsigned)reading.ir, (unsigned)reading.clear);
    std::printf("  gain=%-3u  integration_ms=%-5u  led=%s\n",
                (unsigned)reading.gain, (unsigned)reading.integration_ms,
                led_enabled ? "yes" : "no");

    std::printf("\n--- Pipeline Result ---\n");
    std::printf("  XYZ:           X=%8.4f  Y=%8.4f  Z=%8.4f\n",
                result.xyz.x, result.xyz.y, result.xyz.z);
    std::printf("  scan_lab:      L=%8.4f  a=%8.4f  b=%8.4f"
                "  (pre-saturation-boost; used for Kona matching)\n",
                result.scan_lab.l, result.scan_lab.a, result.scan_lab.b);
    std::printf("  corrected_lab: L=%8.4f  a=%8.4f  b=%8.4f"
                "  (post-material correction)\n",
                result.corrected_lab.l, result.corrected_lab.a, result.corrected_lab.b);
    std::printf("  lab (display): L=%8.4f  a=%8.4f  b=%8.4f"
                "  (post-saturation-boost; used for display/speech)\n",
                result.lab.l, result.lab.a, result.lab.b);
    std::printf("  RGB:           R=%-4u  G=%-4u  B=%-4u\n",
                (unsigned)result.rgb[0], (unsigned)result.rgb[1], (unsigned)result.rgb[2]);

    std::printf("\n--- Classification ---\n");
    std::printf("  category:    %s\n", result.category   ? result.category   : "(none)");
    std::printf("  color_name:  %s\n", result.color_name ? result.color_name : "(none)");
    std::printf("  kona_matched:%s%s\n",
                result.kona_matched ? " yes" : " no",
                result.kona_matched ? "" : "  (fell back to color-database match)");
    if (result.kona_matched)
    {
        std::printf("  kona_id:     %u\n", (unsigned)result.kona_id);
    }
    std::printf("  delta_e:     %.4f\n", result.delta_e);
    std::printf("  confidence:  %.4f  (%.1f%%)\n", result.confidence, result.confidence * 100.0f);

    std::printf("\n--- Sensor Flags ---\n");
    std::printf("  saturated:       %s\n", result.saturated      ? "YES (data may be clipped)" : "no");
    std::printf("  low_light:       %s\n", result.low_light       ? "YES (L* below threshold)" : "no");
    std::printf("  flicker_detected:%s\n", result.flicker_detected ? " YES" : " no");
    std::printf("  luminance:       %.4f\n", result.luminance);
    std::printf("  saturation:      %.4f\n", result.saturation);

    std::printf("\n--- Material ---\n");
    std::printf("  material:    %s\n", color_math_material_name(result.material));

    std::printf("\n");
}

int main(int argc, char* argv[])
{
    if (!initPipeline())
    {
        std::fprintf(stderr, "color_replay_inspect: color_pipeline_init() failed\n");
        return 1;
    }

    // Attempt to parse directly from command-line args first.
    if (argc >= 9)
    {
        const char* tokens[8];
        for (int i = 0; i < 8; ++i)
        {
            tokens[i] = argv[i + 1];
        }
        const char* label = (argc >= 10) ? argv[9] : "";

        sensor_reading_t reading{};
        bool led_enabled = false;
        if (!parseReading(tokens, 8, reading, led_enabled))
        {
            std::fprintf(stderr,
                         "color_replay_inspect: failed to parse arguments\n"
                         "Usage: %s x y z ir clear gain integration_ms led_enabled [label]\n",
                         argv[0]);
            return 1;
        }

        color_result_t result{};
        if (color_pipeline_identify_from_reading(&reading, led_enabled, &result) != ESP_OK)
        {
            std::fprintf(stderr, "color_replay_inspect: pipeline failed\n");
            return 2;
        }

        printInspection(label, reading, led_enabled, result);
        return 0;
    }

    if (argc > 1 && argc < 9)
    {
        std::fprintf(stderr,
                     "color_replay_inspect: too few arguments (%d supplied, 8 required)\n"
                     "Usage: %s x y z ir clear gain integration_ms led_enabled [label]\n"
                     "       or pipe a line on stdin: x y z ir clear gain integration_ms led_enabled [label]\n",
                     argc - 1, argv[0]);
        return 1;
    }

    // Fall back to reading from stdin (supports the same protocol as kona_regenerate).
    bool processed_any = false;
    int  exit_code     = 0;
    char line[512];

    while (std::fgets(line, sizeof(line), stdin) != nullptr)
    {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
        {
            continue;
        }

        // Tokenise up to 9 space/tab-separated fields.
        char  buf[512];
        std::strncpy(buf, line, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        const char* tokens[9] = {};
        int         ntok      = 0;
        char*       p         = buf;

        while (ntok < 9 && *p != '\0')
        {
            while (*p == ' ' || *p == '\t') { ++p; }
            if (*p == '\0' || *p == '\n' || *p == '\r') { break; }
            tokens[ntok++] = p;
            while (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '\0') { ++p; }
            if (*p != '\0') { *p++ = '\0'; }
        }

        if (ntok < 8)
        {
            std::fprintf(stderr,
                         "color_replay_inspect: malformed input (got %d fields): %s",
                         ntok, line);
            exit_code = 1;
            continue;
        }

        sensor_reading_t reading{};
        bool             led_enabled = false;
        if (!parseReading(tokens, ntok, reading, led_enabled))
        {
            std::fprintf(stderr, "color_replay_inspect: parse error: %s", line);
            exit_code = 1;
            continue;
        }
        const char* label = (ntok >= 9 && tokens[8] != nullptr) ? tokens[8] : "";

        color_result_t result{};
        if (color_pipeline_identify_from_reading(&reading, led_enabled, &result) != ESP_OK)
        {
            std::fprintf(stderr, "color_replay_inspect: pipeline failed for: %s", line);
            exit_code = 2;
            continue;
        }

        printInspection(label, reading, led_enabled, result);
        processed_any = true;
    }

    if (!processed_any && exit_code == 0)
    {
        std::fprintf(stderr,
                     "color_replay_inspect: no input received.\n"
                     "Usage: %s x y z ir clear gain integration_ms led_enabled [label]\n"
                     "       or pipe a line on stdin.\n",
                     argv[0]);
        return 1;
    }

    return exit_code;
}
