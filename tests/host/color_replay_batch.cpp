/**
 * @file color_replay_batch.cpp
 * @brief Host tool for batch/regression pipeline replay from saved sensor captures.
 *
 * Reads multiple raw sensor readings from stdin, runs each through the color pipeline,
 * and writes a CSV report to stdout.  When an expected category is supplied for a
 * capture the tool marks it PASS or FAIL, and exits with a non-zero status code if
 * any expectation is not met.  This makes it suitable for CI/regression testing.
 *
 * Input format (stdin):
 *   # Lines starting with '#' are treated as comments and ignored.
 *   # Blank lines are also ignored.
 *   # Fields are space- or tab-separated.
 *   #
 *   # id  x  y  z  ir  clear  gain  integration_ms  led_enabled  [expected_category]
 *   #
 *   # id              — alphanumeric label (no spaces); used in the CSV output
 *   # x y z ir clear  — raw TCS3530 ADC counts (uint32)
 *   # gain            — gain code byte as reported by the sensor driver
 *   # integration_ms  — integration time in milliseconds
 *   # led_enabled     — 1 if the LED was on during capture, 0 otherwise
 *   # expected_category (optional) — e.g. "Green", "Red", "Blue".
 *   #                                When present the PASS/FAIL column in the CSV
 *   #                                reflects whether the pipeline agrees.
 *
 * CSV output columns (written to stdout):
 *   id, expected_category, category, color_name, kona_matched, kona_id,
 *   delta_e, confidence,
 *   xyz_x, xyz_y, xyz_z,
 *   scan_l, scan_a, scan_b,
 *   corrected_l, corrected_a, corrected_b,
 *   display_l, display_a, display_b,
 *   rgb_r, rgb_g, rgb_b,
 *   material, luminance, saturation,
 *   saturated, low_light, pass
 *
 * Exit codes:
 *   0  — all captures with an expected category matched
 *   1  — one or more captures failed the expected-category check
 *   2  — fatal error (pipeline init failed, I/O error, etc.)
 *
 * Usage:
 *   cat tests/host/capture_samples.cfg | ./color_replay_batch
 *   ./color_replay_batch < tests/host/capture_samples.cfg
 *   python3 scripts/color_replay.py --batch tests/host/capture_samples.json | ./color_replay_batch
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

/** @brief Print the CSV header row. */
static void printHeader()
{
    std::printf("id,expected_category,category,color_name,kona_matched,kona_id,"
                "delta_e,confidence,"
                "xyz_x,xyz_y,xyz_z,"
                "scan_l,scan_a,scan_b,"
                "corrected_l,corrected_a,corrected_b,"
                "display_l,display_a,display_b,"
                "rgb_r,rgb_g,rgb_b,"
                "material,luminance,saturation,"
                "saturated,low_light,pass\n");
}

/**
 * @brief Escape a string for safe inclusion in a CSV field.
 *
 * Wraps the value in double-quotes and doubles any internal double-quotes.
 * Writes the result into @p out (at most @p size bytes including the NUL).
 */
static void csvEscape(const char* in, char* out, size_t size)
{
    if (in == nullptr || in[0] == '\0')
    {
        if (size > 0) out[0] = '\0';
        return;
    }

    // Check whether quoting is needed.
    bool needs_quote = false;
    for (const char* p = in; *p; ++p)
    {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') { needs_quote = true; break; }
    }

    if (!needs_quote)
    {
        std::snprintf(out, size, "%s", in);
        return;
    }

    size_t pos = 0;
    if (pos < size - 1) out[pos++] = '"';
    for (const char* p = in; *p && pos < size - 2; ++p)
    {
        if (*p == '"' && pos < size - 2) { out[pos++] = '"'; }
        out[pos++] = *p;
    }
    if (pos < size - 1) out[pos++] = '"';
    out[pos] = '\0';
}

/** @brief Print one CSV result row. */
static void printRow(const char*             id,
                     const char*             expected_category,
                     const color_result_t&   result)
{
    // Determine PASS/FAIL when an expected category was supplied.
    const char* pass_str = "-";
    if (expected_category != nullptr && expected_category[0] != '\0')
    {
        const char* actual = result.category ? result.category : "";
        pass_str = (std::strcmp(actual, expected_category) == 0) ? "PASS" : "FAIL";
    }

    char id_esc[128], cat_esc[64], name_esc[128], expected_esc[64], material_esc[32];
    csvEscape(id,                                      id_esc,       sizeof(id_esc));
    csvEscape(expected_category ? expected_category : "", expected_esc, sizeof(expected_esc));
    csvEscape(result.category   ? result.category   : "", cat_esc,     sizeof(cat_esc));
    csvEscape(result.color_name ? result.color_name : "", name_esc,    sizeof(name_esc));
    csvEscape(color_math_material_name(result.material), material_esc, sizeof(material_esc));

    std::printf("%s,%s,%s,%s,%d,%u,"        // id…kona_id
                "%.4f,%.4f,"                // delta_e, confidence
                "%.4f,%.4f,%.4f,"           // xyz
                "%.4f,%.4f,%.4f,"           // scan_lab
                "%.4f,%.4f,%.4f,"           // corrected_lab
                "%.4f,%.4f,%.4f,"           // display lab
                "%u,%u,%u,"                 // rgb
                "%s,%.4f,%.4f,"             // material, luminance, saturation
                "%d,%d,%s\n",               // saturated, low_light, pass
                id_esc, expected_esc, cat_esc, name_esc,
                (int)result.kona_matched, (unsigned)result.kona_id,
                result.delta_e, result.confidence,
                result.xyz.x, result.xyz.y, result.xyz.z,
                result.scan_lab.l, result.scan_lab.a, result.scan_lab.b,
                result.corrected_lab.l, result.corrected_lab.a, result.corrected_lab.b,
                result.lab.l, result.lab.a, result.lab.b,
                (unsigned)result.rgb[0], (unsigned)result.rgb[1], (unsigned)result.rgb[2],
                material_esc, result.luminance, result.saturation,
                (int)result.saturated, (int)result.low_light,
                pass_str);
}

int main(int /*argc*/, char* /*argv*/[])
{
    if (!initPipeline())
    {
        std::fprintf(stderr, "color_replay_batch: color_pipeline_init() failed\n");
        return 2;
    }

    printHeader();

    int  total    = 0;
    int  failures = 0;
    int  errors   = 0;
    char line[512];

    while (std::fgets(line, sizeof(line), stdin) != nullptr)
    {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
        {
            continue;
        }

        // Tokenise up to 10 space/tab-separated fields.
        char buf[512];
        std::strncpy(buf, line, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        const char* tokens[10] = {};
        int         ntok       = 0;
        char*       p          = buf;

        while (ntok < 10 && *p != '\0')
        {
            while (*p == ' ' || *p == '\t') { ++p; }
            if (*p == '\0' || *p == '\n' || *p == '\r') { break; }
            tokens[ntok++] = p;
            while (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '\0') { ++p; }
            if (*p != '\0') { *p++ = '\0'; }
        }

        // Need at least: id x y z ir clear gain int_ms led (9 fields).
        if (ntok < 9)
        {
            std::fprintf(stderr,
                         "color_replay_batch: malformed input (got %d fields, need 9): %s",
                         ntok, line);
            ++errors;
            continue;
        }

        const char* id                = tokens[0];
        const char* expected_category = (ntok >= 10) ? tokens[9] : nullptr;

        sensor_reading_t reading{};
        reading.x              = static_cast<uint32_t>(std::strtoul(tokens[1], nullptr, 10));
        reading.y              = static_cast<uint32_t>(std::strtoul(tokens[2], nullptr, 10));
        reading.z              = static_cast<uint32_t>(std::strtoul(tokens[3], nullptr, 10));
        reading.ir             = static_cast<uint32_t>(std::strtoul(tokens[4], nullptr, 10));
        reading.clear          = static_cast<uint32_t>(std::strtoul(tokens[5], nullptr, 10));
        reading.gain           = static_cast<uint8_t> (std::strtoul(tokens[6], nullptr, 10));
        reading.integration_ms = static_cast<uint16_t>(std::strtoul(tokens[7], nullptr, 10));
        bool led_enabled       = std::strtol(tokens[8], nullptr, 10) != 0;
        reading.saturated      = false;

        color_result_t result{};
        if (color_pipeline_identify_from_reading(&reading, led_enabled, &result) != ESP_OK)
        {
            std::fprintf(stderr,
                         "color_replay_batch: pipeline failed for id=%s\n", id);
            ++errors;
            continue;
        }

        ++total;
        printRow(id, expected_category, result);
        std::fflush(stdout);

        // Count failures when an expectation was supplied.
        if (expected_category != nullptr && expected_category[0] != '\0')
        {
            const char* actual = result.category ? result.category : "";
            if (std::strcmp(actual, expected_category) != 0)
            {
                ++failures;
                std::fprintf(stderr,
                             "color_replay_batch: FAIL  id=%-30s  expected=%-10s  got=%-10s  "
                             "color='%s'  dE=%.2f\n",
                             id, expected_category, actual,
                             result.color_name ? result.color_name : "?",
                             result.delta_e);
            }
        }
    }

    if (total == 0 && errors == 0)
    {
        std::fprintf(stderr, "color_replay_batch: no input received on stdin.\n");
        return 2;
    }

    std::fprintf(stderr,
                 "color_replay_batch: processed=%d  failures=%d  errors=%d\n",
                 total, failures, errors);

    return (failures > 0 || errors > 0) ? 1 : 0;
}
