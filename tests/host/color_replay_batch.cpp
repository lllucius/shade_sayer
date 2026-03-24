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
 * Pipeline control options (before any stdin content):
 *   --no-auto-cal        Skip auto-calibration load (use firmware defaults)
 *   --no-black-cal       Skip black-level subtraction
 *   --no-d65-scale       Skip D65 white-point pre-scale
 *   --no-ir-comp         Skip IR-channel compensation
 *   --no-material        Skip material-specific Lab correction
 *   --material=NAME      Override material (default|fabric|plastic|metal)
 *   --bypass-pccm        Replace PCCM with identity matrix [DIAGNOSTIC]
 *
 * CSV output columns (written to stdout):
 *   id, expected_category, category, color_name, kona_matched, kona_id,
 *   delta_e, confidence,
 *   resp_norm_x, resp_norm_y, resp_norm_z,
 *   xyz_x, xyz_y, xyz_z,
 *   scan_l, scan_a, scan_b,
 *   corrected_l, corrected_a, corrected_b,
 *   display_l, display_a, display_b,
 *   rgb_r, rgb_g, rgb_b,
 *   material, material_correction_applied, luminance, saturation,
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
 *   ./color_replay_batch --no-auto-cal --no-black-cal < tests/host/capture_samples.cfg
 *   python3 scripts/color_replay.py batch --json tests/host/capture_samples.json \
 *           --no-auto-cal | ./color_replay_batch
 *
 * See docs/replay-harness.md for the full workflow, replay matrix, and developer
 * instructions.
 */

#include "color_pipeline.h"
#include "color_types.h"
#include "color_math.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

// ---------------------------------------------------------------------------
// Replay flag helpers (shared logic with color_replay_inspect)
// ---------------------------------------------------------------------------

struct ReplayOptions
{
    uint32_t          replay_flags    = 0;
    material_type_t   material_override = MATERIAL_FABRIC;
    bool              has_material_override = false;
};

static bool parseMaterialArg(const char* arg, material_type_t& out)
{
    const char* val = std::strchr(arg, '=');
    if (!val) return false;
    ++val;
    if (std::strcmp(val, "default") == 0) { out = MATERIAL_DEFAULT; return true; }
    if (std::strcmp(val, "fabric")  == 0) { out = MATERIAL_FABRIC;  return true; }
    if (std::strcmp(val, "plastic") == 0) { out = MATERIAL_PLASTIC; return true; }
    if (std::strcmp(val, "metal")   == 0) { out = MATERIAL_METAL;   return true; }
    std::fprintf(stderr,
                 "color_replay_batch: unknown material '%s' (valid: default, fabric, plastic, metal)\n",
                 val);
    return false;
}

static bool tryParseFlag(const char* arg, ReplayOptions& opts)
{
    if (std::strcmp(arg, "--no-auto-cal")  == 0) { opts.replay_flags |= REPLAY_NO_AUTO_CAL;     return true; }
    if (std::strcmp(arg, "--no-black-cal") == 0) { opts.replay_flags |= REPLAY_NO_BLACK_CAL;    return true; }
    if (std::strcmp(arg, "--no-d65-scale") == 0) { opts.replay_flags |= REPLAY_NO_D65_SCALE;    return true; }
    if (std::strcmp(arg, "--no-ir-comp")   == 0) { opts.replay_flags |= REPLAY_NO_IR_COMP;      return true; }
    if (std::strcmp(arg, "--no-material")  == 0) { opts.replay_flags |= REPLAY_NO_MATERIAL_COR; return true; }
    if (std::strcmp(arg, "--bypass-pccm")  == 0) { opts.replay_flags |= REPLAY_BYPASS_PCCM;     return true; }
    if (std::strncmp(arg, "--material=", 11) == 0)
    {
        opts.has_material_override = parseMaterialArg(arg, opts.material_override);
        return opts.has_material_override;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Pipeline init
// ---------------------------------------------------------------------------

static bool initPipeline(const ReplayOptions& opts)
{
    color_pipeline_set_replay_flags(opts.replay_flags);

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
    cfg.use_material_correction = !(opts.replay_flags & REPLAY_NO_MATERIAL_COR);
    cfg.assumed_material        = opts.has_material_override ? opts.material_override : MATERIAL_FABRIC;
    cfg.auto_detect_material    = false;
    return color_pipeline_init(&cfg) == ESP_OK;
}

// ---------------------------------------------------------------------------
// CSV output helpers
// ---------------------------------------------------------------------------

/** @brief Print the CSV header row. */
static void printHeader(const ReplayOptions& opts)
{
    // Emit a comment row listing active replay flags so the CSV is self-documenting.
    if (opts.replay_flags || opts.has_material_override)
    {
        std::printf("# replay_flags:%s%s%s%s%s%s%s\n",
                    (opts.replay_flags & REPLAY_NO_AUTO_CAL)     ? " no-auto-cal"     : "",
                    (opts.replay_flags & REPLAY_NO_BLACK_CAL)    ? " no-black-cal"    : "",
                    (opts.replay_flags & REPLAY_NO_D65_SCALE)    ? " no-d65-scale"    : "",
                    (opts.replay_flags & REPLAY_NO_IR_COMP)      ? " no-ir-comp"      : "",
                    (opts.replay_flags & REPLAY_NO_MATERIAL_COR) ? " no-material"     : "",
                    (opts.replay_flags & REPLAY_BYPASS_PCCM)     ? " bypass-pccm"     : "",
                    opts.has_material_override
                        ? (std::string(" material=") + color_math_material_name(opts.material_override)).c_str()
                        : "");
    }
    std::printf("id,expected_category,category,color_name,kona_matched,kona_id,"
                "delta_e,confidence,"
                "resp_norm_x,resp_norm_y,resp_norm_z,"
                "xyz_x,xyz_y,xyz_z,"
                "scan_l,scan_a,scan_b,"
                "corrected_l,corrected_a,corrected_b,"
                "display_l,display_a,display_b,"
                "rgb_r,rgb_g,rgb_b,"
                "material,material_correction_applied,luminance,saturation,"
                "saturated,low_light,pass\n");
}

/**
 * @brief Escape a string for safe inclusion in a CSV field.
 */
static void csvEscape(const char* in, char* out, size_t size)
{
    if (in == nullptr || in[0] == '\0')
    {
        if (size > 0) out[0] = '\0';
        return;
    }

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

/**
 * @brief Compute RESP-normalised X/Y/Z from raw reading (pre-black-sub values).
 *
 * Mirrors the normalization done inside apply_sensor_correction() so the batch
 * CSV can show this intermediate stage without requiring pipeline internals.
 */
static void computeRespNorm(const sensor_reading_t& r,
                             float& rx, float& ry, float& rz)
{
    float gain_mult = tcs3530_gain_code_to_multiplier(r.gain);
    if (gain_mult == 0.0f) gain_mult = 1.0f;
    float time_scale = r.integration_ms / 100.0f;
    float base_scale = 1.0f / (gain_mult * time_scale);
    rx = (r.x * base_scale) / TCS3530_RESP_X;
    ry = (r.y * base_scale) / TCS3530_RESP_Y;
    rz = (r.z * base_scale) / TCS3530_RESP_Z;
}

/** @brief Print one CSV result row. */
static void printRow(const char*             id,
                     const char*             expected_category,
                     const sensor_reading_t& reading,
                     const color_result_t&   result,
                     const ReplayOptions&    opts)
{
    const char* pass_str = "-";
    if (expected_category != nullptr && expected_category[0] != '\0')
    {
        const char* actual = result.category ? result.category : "";
        pass_str = (std::strcmp(actual, expected_category) == 0) ? "PASS" : "FAIL";
    }

    // Material correction was applied when the config allowed it and no override skipped it.
    bool mat_cor_applied = !(opts.replay_flags & REPLAY_NO_MATERIAL_COR)
                           && result.material != MATERIAL_DEFAULT
                           && result.material != MATERIAL_UNKNOWN;

    char id_esc[128], cat_esc[64], name_esc[128], expected_esc[64], material_esc[32];
    csvEscape(id,                                      id_esc,       sizeof(id_esc));
    csvEscape(expected_category ? expected_category : "", expected_esc, sizeof(expected_esc));
    csvEscape(result.category   ? result.category   : "", cat_esc,     sizeof(cat_esc));
    csvEscape(result.color_name ? result.color_name : "", name_esc,    sizeof(name_esc));
    csvEscape(color_math_material_name(result.material), material_esc, sizeof(material_esc));

    float rx, ry, rz;
    computeRespNorm(reading, rx, ry, rz);

    std::printf("%s,%s,%s,%s,%d,%u,"        // id…kona_id
                "%.4f,%.4f,"                // delta_e, confidence
                "%.4f,%.4f,%.4f,"           // resp_norm
                "%.4f,%.4f,%.4f,"           // xyz
                "%.4f,%.4f,%.4f,"           // scan_lab
                "%.4f,%.4f,%.4f,"           // corrected_lab
                "%.4f,%.4f,%.4f,"           // display lab
                "%u,%u,%u,"                 // rgb
                "%s,%d,%.4f,%.4f,"          // material, mat_cor_applied, luminance, saturation
                "%d,%d,%s\n",               // saturated, low_light, pass
                id_esc, expected_esc, cat_esc, name_esc,
                (int)result.kona_matched, (unsigned)result.kona_id,
                result.delta_e, result.confidence,
                rx, ry, rz,
                result.xyz.x, result.xyz.y, result.xyz.z,
                result.scan_lab.l, result.scan_lab.a, result.scan_lab.b,
                result.corrected_lab.l, result.corrected_lab.a, result.corrected_lab.b,
                result.lab.l, result.lab.a, result.lab.b,
                (unsigned)result.rgb[0], (unsigned)result.rgb[1], (unsigned)result.rgb[2],
                material_esc, (int)mat_cor_applied, result.luminance, result.saturation,
                (int)result.saturated, (int)result.low_light,
                pass_str);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    ReplayOptions opts;

    // Parse option flags from CLI args.
    int first_pos = 1;
    while (first_pos < argc && argv[first_pos][0] == '-')
    {
        if (!tryParseFlag(argv[first_pos], opts))
        {
            std::fprintf(stderr,
                         "color_replay_batch: unknown option '%s'\n"
                         "Usage: %s [options] < input.cfg\n"
                         "Options: --no-auto-cal --no-black-cal --no-d65-scale\n"
                         "         --no-ir-comp --no-material --material=NAME --bypass-pccm\n",
                         argv[first_pos], argv[0]);
            return 2;
        }
        ++first_pos;
    }

    if (!initPipeline(opts))
    {
        std::fprintf(stderr, "color_replay_batch: color_pipeline_init() failed\n");
        return 2;
    }

    printHeader(opts);

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
        printRow(id, expected_category, reading, result, opts);
        std::fflush(stdout);

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
