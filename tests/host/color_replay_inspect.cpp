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
 *   ./color_replay_inspect [options] <x> <y> <z> <ir> <clear> <gain> <integration_ms> <led_enabled> [label]
 *
 * Usage (stdin, same format as kona_regenerate):
 *   echo "11517952 12179200 7376128 249344 8603648 4 100 1" | ./color_replay_inspect [options]
 *   echo "11517952 12179200 7376128 249344 8603648 4 100 1 green_wall_paint" | ./color_replay_inspect [options]
 *
 * Both forms accept an optional label/id as the 9th token on the same line (stdin) or
 * as the 9th command-line argument.  The label is purely cosmetic.
 *
 * Pipeline control options (can be combined):
 *   --no-auto-cal        Skip loading auto-calibration from NVS / host file; use firmware defaults.
 *   --no-black-cal       Skip black-level subtraction.
 *   --no-d65-scale       Skip the D65 white-point pre-scale step.
 *   --no-ir-comp         Skip IR-channel crosstalk compensation.
 *   --no-material        Skip material-specific Lab correction.
 *   --material=NAME      Override material: default, fabric, plastic, metal.
 *   --bypass-pccm        Replace PCCM with identity matrix (DIAGNOSTIC MODE).
 *
 * The pipeline logs (via ESP_LOGx) are written to stderr and show the detailed
 * internal correction stages (PCCM, IR compensation, D65 scaling, etc.).
 * The structured summary is written to stdout.
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

// ---------------------------------------------------------------------------
// Replay flags / option state
// ---------------------------------------------------------------------------

struct ReplayOptions
{
    uint32_t          replay_flags    = 0;
    material_type_t   material_override = MATERIAL_FABRIC;  // matches production default
    bool              has_material_override = false;
};

/**
 * @brief Parse a --material=NAME argument.
 * @return true on success, false if the name is not recognised.
 */
static bool parseMaterialArg(const char* arg, material_type_t& out)
{
    const char* val = std::strchr(arg, '=');
    if (!val) return false;
    ++val;  // skip '='
    if (std::strcmp(val, "default") == 0) { out = MATERIAL_DEFAULT; return true; }
    if (std::strcmp(val, "fabric")  == 0) { out = MATERIAL_FABRIC;  return true; }
    if (std::strcmp(val, "plastic") == 0) { out = MATERIAL_PLASTIC; return true; }
    if (std::strcmp(val, "metal")   == 0) { out = MATERIAL_METAL;   return true; }
    std::fprintf(stderr,
                 "color_replay_inspect: unknown material '%s' (valid: default, fabric, plastic, metal)\n",
                 val);
    return false;
}

/**
 * @brief Try to parse one argument as a replay flag / option.
 * @return true if the argument was consumed, false if it is not a flag.
 */
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

/**
 * @brief Initialise the pipeline with production-equivalent config plus replay overrides.
 *
 * @param opts Replay options accumulated from CLI flags.
 * @return true on success.
 */
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
// Parsing
// ---------------------------------------------------------------------------

/**
 * @brief Parse eight numeric fields (x y z ir clear gain int_ms led) from a string array.
 *
 * @param tokens  Pointer to array of C-strings starting at the first field.
 * @param n       Number of tokens available (must be ≥ 8).
 * @param reading Output sensor reading (populated on success).
 * @param led_out Set to true when the led token is non-zero.
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

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

/** @brief Print the active replay flags as a human-readable note. */
static void printReplayFlags(uint32_t flags, material_type_t mat, bool has_mat_override)
{
    if (flags == 0 && !has_mat_override)
    {
        return;
    }
    std::printf("--- Replay Overrides Active ---\n");
    if (flags & REPLAY_NO_AUTO_CAL)     std::printf("  --no-auto-cal       NVS auto-calibration skipped (firmware defaults used)\n");
    if (flags & REPLAY_NO_BLACK_CAL)    std::printf("  --no-black-cal      Black-level subtraction skipped\n");
    if (flags & REPLAY_NO_D65_SCALE)    std::printf("  --no-d65-scale      D65 white-point pre-scale skipped\n");
    if (flags & REPLAY_NO_IR_COMP)      std::printf("  --no-ir-comp        IR-channel compensation skipped\n");
    if (flags & REPLAY_NO_MATERIAL_COR) std::printf("  --no-material       Material-specific Lab correction skipped\n");
    if (flags & REPLAY_BYPASS_PCCM)     std::printf("  --bypass-pccm       PCCM replaced with identity matrix [DIAGNOSTIC]\n");
    if (has_mat_override)               std::printf("  --material=%-8s  Material type forced\n",
                                                     color_math_material_name(mat));
    std::printf("\n");
}

/** @brief Print the full inspection report for one result to stdout. */
static void printInspection(const char* label,
                             const sensor_reading_t& reading,
                             bool led_enabled,
                             const color_result_t& result,
                             const ReplayOptions& opts)
{
    std::printf("\n");
    std::printf("=== Color Replay Inspection%s%s ===\n",
                (label[0] != '\0') ? ": " : "",
                label);

    // Print any active replay overrides near the top for easy reading.
    std::printf("\n");
    printReplayFlags(opts.replay_flags, opts.material_override, opts.has_material_override);

    // --- Raw input ---
    std::printf("--- Raw Input ---\n");
    std::printf("  x=%-12u  y=%-12u  z=%-12u\n",
                (unsigned)reading.x, (unsigned)reading.y, (unsigned)reading.z);
    std::printf("  ir=%-11u  clear=%-9u\n",
                (unsigned)reading.ir, (unsigned)reading.clear);
    std::printf("  gain=%-3u  integration_ms=%-5u  led=%s\n",
                (unsigned)reading.gain, (unsigned)reading.integration_ms,
                led_enabled ? "yes" : "no");

    // --- Intermediate pipeline stages ---
    // These are logged verbosely to stderr by the pipeline (ESP_LOGx / TCS_LOGD).
    // The structured summary block below mirrors the key values so that the
    // inspect output can be compared across runs without parsing log lines.
    std::printf("\n--- Pipeline Stages (see stderr for full pipeline logs) ---\n");

    // Derive RESP-normalised values the same way the pipeline does so that a
    // developer can compare them without having to parse stderr.
    {
        float gain_mult = tcs3530_gain_code_to_multiplier(reading.gain);
        if (gain_mult == 0.0f) gain_mult = 1.0f;
        float time_scale = reading.integration_ms / 100.0f;
        float base_scale = 1.0f / (gain_mult * time_scale);

        float rx = (reading.x * base_scale) / TCS3530_RESP_X;
        float ry = (reading.y * base_scale) / TCS3530_RESP_Y;
        float rz = (reading.z * base_scale) / TCS3530_RESP_Z;
        std::printf("  RESP-norm (pre-black-sub):  x=%9.4f  y=%9.4f  z=%9.4f\n", rx, ry, rz);
        std::printf("  (gain_mult=%.1f  base_scale=%.6f)\n", gain_mult, base_scale);
    }

    std::printf("  [see stderr for: post-black-sub, post-D65, post-gain, post-PCCM, IR comp]\n");

    // --- Final pipeline result ---
    std::printf("\n--- Pipeline Result ---\n");
    std::printf("  XYZ:           X=%8.4f  Y=%8.4f  Z=%8.4f\n",
                result.xyz.x, result.xyz.y, result.xyz.z);
    std::printf("  scan_lab:      L=%8.4f  a=%8.4f  b=%8.4f"
                "  (pre-saturation-boost; used for Kona matching)\n",
                result.scan_lab.l, result.scan_lab.a, result.scan_lab.b);
    std::printf("  corrected_lab: L=%8.4f  a=%8.4f  b=%8.4f"
                "  (post-material correction; used for fallback matching)\n",
                result.corrected_lab.l, result.corrected_lab.a, result.corrected_lab.b);
    std::printf("  lab (display): L=%8.4f  a=%8.4f  b=%8.4f"
                "  (post-saturation-boost; used for display/speech)\n",
                result.lab.l, result.lab.a, result.lab.b);
    std::printf("  RGB:           R=%-4u  G=%-4u  B=%-4u\n",
                (unsigned)result.rgb[0], (unsigned)result.rgb[1], (unsigned)result.rgb[2]);

    // --- Classification ---
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
    std::printf("  description: %s\n", result.description ? result.description : "(none)");

    // --- Material ---
    std::printf("\n--- Material ---\n");
    std::printf("  material:               %s\n", color_math_material_name(result.material));
    std::printf("  material_correction:    %s\n",
                (opts.replay_flags & REPLAY_NO_MATERIAL_COR) ? "disabled (--no-material)"
                : "enabled");

    // --- Sensor flags ---
    std::printf("\n--- Sensor Flags ---\n");
    std::printf("  saturated:       %s\n", result.saturated      ? "YES (data may be clipped)" : "no");
    std::printf("  low_light:       %s\n", result.low_light       ? "YES (L* below threshold)" : "no");
    std::printf("  flicker_detected:%s\n", result.flicker_detected ? " YES" : " no");
    std::printf("  luminance:       %.4f\n", result.luminance);
    std::printf("  saturation:      %.4f\n", result.saturation);

    std::printf("\n");
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static void printUsage(const char* prog)
{
    std::fprintf(stderr,
                 "Usage: %s [options] x y z ir clear gain integration_ms led_enabled [label]\n"
                 "       or pipe a line on stdin: x y z ir clear gain integration_ms led_enabled [label]\n"
                 "\n"
                 "Pipeline control options:\n"
                 "  --no-auto-cal        Skip auto-calibration load (use firmware defaults)\n"
                 "  --no-black-cal       Skip black-level subtraction\n"
                 "  --no-d65-scale       Skip D65 white-point pre-scale\n"
                 "  --no-ir-comp         Skip IR-channel compensation\n"
                 "  --no-material        Skip material-specific Lab correction\n"
                 "  --material=NAME      Override material (default|fabric|plastic|metal)\n"
                 "  --bypass-pccm        Replace PCCM with identity matrix [DIAGNOSTIC]\n"
                 "\n"
                 "See docs/replay-harness.md for the replay matrix and developer workflow.\n",
                 prog);
}

int main(int argc, char* argv[])
{
    ReplayOptions opts;

    // Collect flag arguments before the positional ones.
    int first_pos = 1;
    while (first_pos < argc && argv[first_pos][0] == '-')
    {
        if (!tryParseFlag(argv[first_pos], opts))
        {
            std::fprintf(stderr,
                         "color_replay_inspect: unknown option '%s'\n", argv[first_pos]);
            printUsage(argv[0]);
            return 1;
        }
        ++first_pos;
    }

    if (!initPipeline(opts))
    {
        std::fprintf(stderr, "color_replay_inspect: color_pipeline_init() failed\n");
        return 1;
    }

    // --- Command-line positional args ---
    const int remaining = argc - first_pos;
    if (remaining >= 8)
    {
        const char* tokens[8];
        for (int i = 0; i < 8; ++i)
        {
            tokens[i] = argv[first_pos + i];
        }
        const char* label = (remaining >= 9) ? argv[first_pos + 8] : "";

        sensor_reading_t reading{};
        bool led_enabled = false;
        if (!parseReading(tokens, 8, reading, led_enabled))
        {
            std::fprintf(stderr, "color_replay_inspect: failed to parse arguments\n");
            printUsage(argv[0]);
            return 1;
        }

        color_result_t result{};
        if (color_pipeline_identify_from_reading(&reading, led_enabled, &result) != ESP_OK)
        {
            std::fprintf(stderr, "color_replay_inspect: pipeline failed\n");
            return 2;
        }

        printInspection(label, reading, led_enabled, result, opts);
        return 0;
    }

    if (remaining > 0 && remaining < 8)
    {
        std::fprintf(stderr,
                     "color_replay_inspect: too few positional arguments (%d supplied, 8 required)\n",
                     remaining);
        printUsage(argv[0]);
        return 1;
    }

    // --- Stdin fallback (supports the same protocol as kona_regenerate) ---
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

        printInspection(label, reading, led_enabled, result, opts);
        processed_any = true;
    }

    if (!processed_any && exit_code == 0)
    {
        std::fprintf(stderr, "color_replay_inspect: no input received.\n");
        printUsage(argv[0]);
        return 1;
    }

    return exit_code;
}
