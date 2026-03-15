#include "color_pipeline.h"
#include "color_math.h"
#include "konaref.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cfloat>

// Test case from problem statement: verify CANTALOUPE detection
// The issue reports that measured Lab (98.5, 65.9, 90.0) should give dE=0.07 to CANTALOUPE
// but device was reporting dE=25.8
void test_cantaloupe_scenario()
{
    std::printf("\n=== CANTALOUPE scenario test (Issue #41) ===\n");
    
    // Reference values from konaref_default.cpp - verified against the Kona table
    // Entry: { 59, 98.500000f, 66.308800f, 90.075200f }  // 59 CANTALOUPE
    
    // Measured Lab from problem statement (device log: L=98.5 a=65.9 b=90.0)
    // Note: GUI measurement was slightly different (a=66.2, b=90.2) giving dE=0.07
    lab_t measured = {98.5f, 65.9f, 90.0f};
    
    // Reference Lab from Kona table (CANTALOUPE, id=59)
    lab_t cantaloupe_ref = {98.500000f, 66.308800f, 90.075200f};
    
    // Compute actual deltaE - varies by exact measurement but should be ~0.1-0.15
    float expected_de = color_math_delta_e_ciede2000(&measured, &cantaloupe_ref);
    std::printf("Direct CIEDE2000: measured (%.1f, %.1f, %.1f) vs CANTALOUPE ref -> dE=%.4f\n",
                measured.l, measured.a, measured.b, expected_de);
    
    // The deltaE should be small (< 1.0) since the values are very close
    assert(expected_de < 1.0f && "CANTALOUPE dE should be < 1.0");
    assert(expected_de > 0.0f && "CANTALOUPE dE should be > 0.0");
    
    // Also test that swapping L and a gives the wrong high value (~25)
    lab_t swapped = {65.9f, 98.5f, 90.0f};  // L and a swapped
    float swapped_de = color_math_delta_e_ciede2000(&swapped, &cantaloupe_ref);
    std::printf("SWAPPED L/a CIEDE2000: measured (%.1f, %.1f, %.1f) vs CANTALOUPE ref -> dE=%.4f\n",
                swapped.l, swapped.a, swapped.b, swapped_de);
    
    // The swapped deltaE should be high (~24-25), matching the bug report
    assert(swapped_de > 20.0f && "Swapped L/a should give dE > 20");
    
    std::printf("CANTALOUPE scenario test PASSED\n");
}

// Verify that scan_lab does NOT apply saturation boost.
// The pre-boost Lab values are what get stored in kona_captures.json so that
// highly-saturated swatches (e.g., CARROT, TORCH, ORANGEADE) remain distinguishable
// from one another — saturation boost + ±110 clamp would otherwise collapse them all
// to identical a*=110, b*=110 values.
void test_scan_lab_no_boost()
{
    std::printf("\n=== scan_lab no-saturation-boost test ===\n");

    color_pipeline_config_t cfg{};
    cfg.min_luminance = 5.0f;
    cfg.max_delta_e = 12.0f;
    cfg.num_samples = 1;
    cfg.sample_delay_ms = 1;
    cfg.gray_threshold = 5.0f;
    cfg.color_threshold = 60.0f;
    cfg.saturation_boost = 1.5f;
    assert(color_pipeline_init(&cfg) == ESP_OK);

    // Two XYZ inputs that differ only in Z (simulating swatches with different redness).
    // swatch_a has more Z (less red) → lower b*; swatch_b has less Z (more red) → higher b*.
    // After saturation boost + clamp both would give a=110, b=110 (indistinguishable).
    // With pre-boost scan_lab they must remain distinct.
    xyz_t swatch_a_xyz{140.0f, 100.0f, 20.0f};  // lower Z → high b* (orange/yellow)
    xyz_t swatch_b_xyz{140.0f, 100.0f, 10.0f};  // even lower Z → higher b* (more red)

    color_result_t a_result{}, b_result{};
    assert(color_pipeline_process_xyz(&swatch_a_xyz, true, &a_result) == ESP_OK);
    assert(color_pipeline_process_xyz(&swatch_b_xyz, true, &b_result) == ESP_OK);

    std::printf("swatch_a scan_lab: L=%.2f a=%.4f b=%.4f\n",
                a_result.scan_lab.l, a_result.scan_lab.a, a_result.scan_lab.b);
    std::printf("swatch_b scan_lab: L=%.2f a=%.4f b=%.4f\n",
                b_result.scan_lab.l, b_result.scan_lab.a, b_result.scan_lab.b);

    // Both swatches should produce a valid color name
    assert(a_result.color_name != nullptr && "swatch_a should identify a color");
    assert(b_result.color_name != nullptr && "swatch_b should identify a color");

    // scan_lab a* should be below 110 (not clamped) — if boost were applied to
    // these XYZ inputs (swatch_a: [140, 100, 20]), the resulting pre-boost a*≈68.9
    // would be boosted to 103.3 and pre-boost b*≈86.3 would be boosted to 129.5 → clamped to
    // 110 (both b* values collapsing). Without boost, a* stays at ≈68.9 (< 110) and
    // the distinct b* values of the two swatches are preserved.
    assert(a_result.scan_lab.a < 110.0f && "scan_lab should NOT have boost applied (a should be < 110)");
    assert(b_result.scan_lab.a < 110.0f && "scan_lab should NOT have boost applied (a should be < 110)");

    // The two swatches must have DISTINCT scan_lab b* values since they differ in Z.
    // If boost were applied, both would clamp to b=110 and be indistinguishable.
    assert(fabsf(a_result.scan_lab.b - b_result.scan_lab.b) > 5.0f
           && "scan_lab b* must be distinct for swatches with different Z (redness)");

    // The display path (result->lab) applies boost and clamping, so it IS allowed
    // to clamp — we just confirm the pipeline completed without crashing.
    std::printf("swatch_a result->lab: L=%.2f a=%.4f b=%.4f\n",
                a_result.lab.l, a_result.lab.a, a_result.lab.b);

    std::printf("scan_lab no-boost test PASSED\n");
}

int main()
{
    color_pipeline_config_t cfg{};
    cfg.min_luminance = 5.0f;
    cfg.max_delta_e = 12.0f;
    cfg.num_samples = 1;
    cfg.sample_delay_ms = 1;
    cfg.gray_threshold = 5.0f;
    cfg.color_threshold = 60.0f;
    cfg.saturation_boost = 1.5f;
    assert(color_pipeline_init(&cfg) == ESP_OK);

    // Basic pipeline sanity: orange-hued XYZ produces a valid color name.
    color_result_t red{};
    xyz_t red_xyz{2460.86f, 1623.25f, 556.42f};
    assert(color_pipeline_process_xyz(&red_xyz, true, &red) == ESP_OK);
    assert(red.color_name != nullptr);
    // scan_lab must not have boost applied: pre-boost a*≈84 for this XYZ.
    // With boost (1.5×) a* would be clamped to 110; without boost a* < 110.
    assert(red.scan_lab.a < 110.0f && "scan_lab should be pre-boost (a < 110)");
    assert(red.scan_lab.a > 50.0f  && "scan_lab a* should be > 50 for orange-hued input");
    std::printf("red test: color=%s scan_lab=(%.2f,%.2f,%.2f) lab=(%.2f,%.2f,%.2f)\n",
                red.color_name, red.scan_lab.l, red.scan_lab.a, red.scan_lab.b,
                red.lab.l, red.lab.a, red.lab.b);

    // Green XYZ: basic pipeline check for a completely different hue.
    color_result_t green{};
    xyz_t green_xyz{1281.60f, 2094.40f, 957.50f};
    assert(color_pipeline_process_xyz(&green_xyz, true, &green) == ESP_OK);
    assert(green.color_name != nullptr);
    assert(!green.kona_matched && "green should not match the yellow/orange/red Kona set");

    // Pipeline from raw sensor reading.
    sensor_reading_t raw{};
    raw.x = 11418369;
    raw.y = 7791616;
    raw.z = 2225664;
    raw.ir = 619520;
    raw.clear = 11418369 + 7791616 + 2225664;
    raw.gain = 5; // TCS3530Gain::X16
    raw.integration_ms = 100;
    raw.saturated = false;
    color_result_t from_raw{};
    assert(color_pipeline_identify_from_reading(&raw, true, &from_raw) == ESP_OK);
    assert(from_raw.color_name != nullptr);
    // Verify the raw-reading path populates scan_lab (non-zero a* for a chromatic input).
    // For highly saturated oranges, pre-boost a* can legitimately exceed 110
    // (this raw input produces a*≈125 pre-boost; with boost+clamp it would be 110) — so
    // we only check that scan_lab.a is non-zero, not that it is < 110.
    assert(from_raw.scan_lab.a != 0.0f && "scan_lab should be populated for raw-reading path");
    std::printf("from_raw: color=%s scan_lab=(%.2f,%.2f,%.2f)\n",
                from_raw.color_name, from_raw.scan_lab.l, from_raw.scan_lab.a, from_raw.scan_lab.b);

    // Run the scan_lab no-boost test (key correctness property).
    test_scan_lab_no_boost();

    // Run the CANTALOUPE scenario test from issue #41.
    test_cantaloupe_scenario();

    std::printf("All Kona matching tests passed!\n");
    return 0;
}
