/**
 * @file detection_fixes_test.cpp
 * @brief Regression tests for the detection-algorithm fixes
 *
 * Covers:
 * 1. Matcher exactness: color_matcher_find_closest and kona_ref_find_closest
 *    must return the true brute-force CIEDE2000 minimum for every query
 *    (regression for the removed VP-tree, whose pruning assumed a triangle
 *    inequality CIEDE2000 does not satisfy).
 * 2. Optimizer/matcher space parity: auto_cal_apply_calibration must produce
 *    the same Lab as the runtime scan_lab path for identical inputs/params.
 * 3. Material classifier variance branch: a dimensionally-consistent
 *    coefficient of variation must trigger fabric detection (and override the
 *    otherwise-metal heuristics).
 * 4. Measurement-file ordering: out-of-order lines must not be attributed to
 *    the wrong calibration reference.
 */

#include "color_pipeline.h"
#include "color_matcher.h"
#include "color_database.h"
#include "color_math.h"
#include "konaref.h"
#include "auto_calibrate.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cfloat>
#include <cstdlib>

//===========================================================================
// 1. Matcher exactness vs. brute force
//===========================================================================

static void test_xkcd_matcher_exact()
{
    std::printf("\n=== xkcd matcher brute-force parity test ===\n");

    assert(color_matcher_init() == ESP_OK);
    const uint32_t count = color_database_get_count();
    assert(count > 0);

    int queries = 0;
    for (float L = 2.5f; L <= 100.0f; L += 13.0f)
    {
        for (float a = -90.0f; a <= 90.0f; a += 18.0f)
        {
            for (float b = -90.0f; b <= 90.0f; b += 18.0f)
            {
                lab_t q{L, a, b};

                float de_matcher = FLT_MAX;
                const char* name = color_matcher_find_closest(&q, &de_matcher);
                assert(name != nullptr);

                float de_brute = FLT_MAX;
                for (uint32_t i = 0; i < count; i++)
                {
                    lab_t entry;
                    assert(color_database_get_entry(i, nullptr, 0, &entry) == ESP_OK);
                    float d = color_math_delta_e_ciede2000(&q, &entry);
                    if (d < de_brute)
                    {
                        de_brute = d;
                    }
                }

                if (fabsf(de_matcher - de_brute) > 1e-5f)
                {
                    std::printf("MISMATCH at Lab(%.1f,%.1f,%.1f): matcher dE=%.4f brute dE=%.4f\n",
                                L, a, b, de_matcher, de_brute);
                    assert(false && "matcher must return the brute-force CIEDE2000 minimum");
                }
                queries++;
            }
        }
    }
    std::printf("xkcd matcher: %d grid queries, 0 mismatches\n", queries);
    std::printf("xkcd matcher brute-force parity test PASSED\n");
}

static void test_kona_matcher_exact()
{
    std::printf("\n=== kona matcher brute-force parity test ===\n");

    if (!kona_ref_validate() || kona_ref_entry_count() == 0)
    {
        std::printf("Kona table unavailable in this build - SKIPPED\n");
        return;
    }

    const kona_ref_t* entries = kona_ref_entries();
    const size_t count = kona_ref_entry_count();
    int queries = 0;

    for (float L = 2.5f; L <= 100.0f; L += 13.0f)
    {
        for (float a = -90.0f; a <= 90.0f; a += 18.0f)
        {
            for (float b = -90.0f; b <= 90.0f; b += 18.0f)
            {
                float de_matcher = FLT_MAX;
                size_t idx = 0;
                const kona_ref_t* hit = kona_ref_find_closest(L, a, b, &de_matcher, &idx);
                assert(hit != nullptr);

                lab_t q{L, a, b};
                float de_brute = FLT_MAX;
                for (size_t i = 0; i < count; i++)
                {
                    lab_t entry{entries[i].l, entries[i].a, entries[i].b};
                    float d = color_math_delta_e_ciede2000(&q, &entry);
                    if (d < de_brute)
                    {
                        de_brute = d;
                    }
                }

                if (fabsf(de_matcher - de_brute) > 1e-5f)
                {
                    std::printf("MISMATCH at Lab(%.1f,%.1f,%.1f): matcher dE=%.4f brute dE=%.4f\n",
                                L, a, b, de_matcher, de_brute);
                    assert(false && "kona matcher must return the brute-force CIEDE2000 minimum");
                }
                queries++;
            }
        }
    }
    std::printf("kona matcher: %d grid queries over %zu entries, 0 mismatches\n", queries, count);
    std::printf("kona matcher brute-force parity test PASSED\n");
}

//===========================================================================
// 2. Optimizer objective == runtime scan_lab space
//===========================================================================

static void check_calibration_parity(const color_calib_params_t* params,
                                     uint32_t raw_x, uint32_t raw_y, uint32_t raw_z,
                                     const char* label)
{
    // Build a synthetic reading with unity scaling: gain code 1 = 1.0x
    // multiplier, 100 ms integration = 1.0x time scale. IR/clear are zero so
    // the runtime IR-compensation branch is skipped (the calibration flow
    // subtracts IR before submission, so the optimizer input is IR-free).
    sensor_reading_t reading{};
    reading.x = raw_x;
    reading.y = raw_y;
    reading.z = raw_z;
    reading.ir = 0;
    reading.clear = 0;
    reading.gain = 1;             // 1.0x
    reading.integration_ms = 100; // reference integration
    reading.saturated = false;

    color_result_t result{};
    assert(color_pipeline_identify_from_reading(&reading, true, &result) == ESP_OK);

    // The calibration flow submits RESP-normalized XYZ; reproduce it exactly.
    xyz_t resp_xyz;
    resp_xyz.x = (float)raw_x / TCS3530_RESP_X;
    resp_xyz.y = (float)raw_y / TCS3530_RESP_Y;
    resp_xyz.z = (float)raw_z / TCS3530_RESP_Z;

    lab_t cal_lab = auto_cal_apply_calibration(&resp_xyz, params);

    std::printf("%s: scan_lab L=%.4f a=%.4f b=%.4f | optimizer L=%.4f a=%.4f b=%.4f\n",
                label,
                result.scan_lab.l, result.scan_lab.a, result.scan_lab.b,
                cal_lab.l, cal_lab.a, cal_lab.b);

    const float TOLERANCE = 0.05f;  // float rounding through different operation orders
    assert(fabsf(result.scan_lab.l - cal_lab.l) < TOLERANCE);
    assert(fabsf(result.scan_lab.a - cal_lab.a) < TOLERANCE);
    assert(fabsf(result.scan_lab.b - cal_lab.b) < TOLERANCE);
}

static void test_optimizer_matches_scan_lab()
{
    std::printf("\n=== optimizer objective vs runtime scan_lab parity test ===\n");

    color_pipeline_config_t cfg{};
    cfg.min_luminance = 5.0f;
    cfg.max_delta_e = 12.0f;
    cfg.kona_max_delta_e = 5.0f;
    cfg.use_white_balance = false;
    cfg.num_samples = 1;
    cfg.sample_delay_ms = 1;
    cfg.gray_threshold = 5.0f;
    cfg.color_threshold = 60.0f;
    cfg.saturation_boost = 1.5f;
    cfg.use_material_correction = false;
    cfg.assumed_material = MATERIAL_DEFAULT;
    cfg.auto_detect_material = false;
    assert(color_pipeline_init(&cfg) == ESP_OK);

    // Distinctive params: non-trivial PCCM, gamma, offset, blue correction.
    color_calib_params_t params{};
    memset(&params, 0, sizeof(params));
    params.pccm[0][0] = 1.15f; params.pccm[0][1] = -0.05f; params.pccm[0][2] = -0.10f;
    params.pccm[1][0] = -0.20f; params.pccm[1][1] = 1.10f; params.pccm[1][2] = 0.10f;
    params.pccm[2][0] = -0.15f; params.pccm[2][1] = -0.15f; params.pccm[2][2] = 1.30f;
    params.pccm[0][3] = 0.05f;  // one polynomial term to exercise the full path
    params.lightness_scale = 0.85f;
    params.lightness_offset = -4.0f;
    params.lightness_gamma = 1.1f;
    params.lightness_gamma_dark = 1.4f;
    params.lightness_gamma_light = 0.9f;
    params.lightness_transition = 35.0f;  // piecewise gamma active
    params.blue_correction_magnitude = -6.0f;
    params.blue_correction_center = 43.0f;
    params.blue_correction_width = 10.0f;
    params.saturation_boost = 1.3f;
    params.gray_threshold = 4.0f;
    params.color_threshold = 60.0f;
    params.has_black_calibration = false;
    assert(color_pipeline_set_params(&params) == ESP_OK);

    // Mid-tone, chromatic, and a bright case whose post-pipeline Y exceeds
    // D65_Y to exercise the Y-normalization step in both implementations.
    check_calibration_parity(&params, 150000, 140000, 90000, "mid-tone");
    check_calibration_parity(&params, 220000, 130000, 40000, "chromatic (red/orange)");
    check_calibration_parity(&params, 340000, 330000, 300000, "bright (Y > white ref)");

    std::printf("optimizer/scan_lab parity test PASSED\n");
}

//===========================================================================
// 3. Material classifier variance branch
//===========================================================================

static void test_material_variance_branch()
{
    std::printf("\n=== material classifier variance branch test ===\n");

    // Neutral raw reading with a very high clear ratio: without variance data
    // the heuristics classify this as METAL (specular reflection signature).
    sensor_reading_t reading{};
    reading.x = 10000;
    reading.y = 10000;
    reading.z = 10000;
    reading.ir = 100;
    reading.clear = 60000;
    reading.gain = 1;
    reading.integration_ms = 100;

    material_type_t without_variance = color_math_classify_material(&reading, nullptr, nullptr);
    std::printf("without variance: %s\n", color_math_material_name(without_variance));
    assert(without_variance == MATERIAL_METAL &&
           "high clear ratio + low chroma spread must classify as METAL");

    // Same reading, but capture statistics show high texture variance in the
    // corrected-XYZ domain: CoV = sqrt(225)/50 = 0.30 on Y, well above the
    // 0.15 fabric threshold. The variance branch must take priority.
    xyz_t mean_xyz{50.0f, 50.0f, 50.0f};
    xyz_t variance_xyz{225.0f, 225.0f, 225.0f};  // stddev 15 => CoV 0.30

    material_type_t with_variance = color_math_classify_material(&reading, &variance_xyz, &mean_xyz);
    std::printf("with CoV=0.30 variance: %s\n", color_math_material_name(with_variance));
    assert(with_variance == MATERIAL_FABRIC &&
           "high corrected-domain CoV must classify as FABRIC via the variance branch");

    // Low variance (CoV = 0.02) must NOT trigger the fabric branch.
    xyz_t low_variance{1.0f, 1.0f, 1.0f};  // stddev 1 => CoV 0.02
    material_type_t low_var_result = color_math_classify_material(&reading, &low_variance, &mean_xyz);
    std::printf("with CoV=0.02 variance: %s\n", color_math_material_name(low_var_result));
    assert(low_var_result == MATERIAL_METAL &&
           "low CoV must fall through to the clear-ratio heuristics");

    std::printf("material classifier variance branch test PASSED\n");
}

//===========================================================================
// 4. Measurement-file ordering guard
//===========================================================================

static void test_measurement_file_ordering()
{
    std::printf("\n=== measurement-file ordering guard test ===\n");

    auto_cal_ctx_t* ctx = nullptr;
    assert(auto_cal_init(&ctx) == ESP_OK);

    lab_t lab_a{50.0f, 10.0f, 10.0f};
    lab_t lab_b{60.0f, -10.0f, 5.0f};
    lab_t lab_c{70.0f, 5.0f, -10.0f};
    assert(auto_cal_add_reference(ctx, "RefA", &lab_a, 0) == 0);
    assert(auto_cal_add_reference(ctx, "RefB", &lab_b, 0) == 1);
    assert(auto_cal_add_reference(ctx, "RefC", &lab_c, 0) == 2);
    assert(auto_cal_start(ctx) == ESP_OK);
    assert(strcmp(auto_cal_get_current_ref_name(ctx), "RefA") == 0);

    // Shuffled file: RefB first (out of order), then RefA (in order), then
    // RefC (out of order once RefA advanced the cursor to RefB).
    const char* path = "detection_fixes_shuffled.cfg";
    FILE* f = fopen(path, "w");
    assert(f != nullptr);
    fprintf(f, "RefB|1.0|2.0|3.0\n");
    fprintf(f, "RefA|4.0|5.0|6.0\n");
    fprintf(f, "RefC|7.0|8.0|9.0\n");
    fclose(f);

    assert(auto_cal_submit_measurements_from_file(ctx, path) == ESP_OK);
    remove(path);

    const cal_status_t* status = auto_cal_get_status(ctx);
    assert(status != nullptr);
    std::printf("measurements_collected=%d current_ref=%s\n",
                status->measurements_collected,
                auto_cal_get_current_ref_name(ctx) ? auto_cal_get_current_ref_name(ctx) : "<none>");

    // Only the RefA line may be applied: RefB and RefC lines arrived while a
    // different reference was current and must be skipped, not mis-attributed.
    assert(status->measurements_collected == 1 &&
           "out-of-order lines must be skipped, not attributed to the current reference");
    assert(strcmp(auto_cal_get_current_ref_name(ctx), "RefB") == 0 &&
           "cursor must sit at RefB after only RefA was measured");

    auto_cal_deinit(&ctx);
    std::printf("measurement-file ordering guard test PASSED\n");
}

//===========================================================================

int main()
{
    std::printf("=== detection_fixes_test ===\n");

    color_database_init();

    test_xkcd_matcher_exact();
    test_kona_matcher_exact();
    test_optimizer_matches_scan_lab();
    test_material_variance_branch();
    test_measurement_file_ordering();

    std::printf("\nAll detection-fix regression tests PASSED\n");
    return 0;
}
