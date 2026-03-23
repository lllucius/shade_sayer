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

// Test per-material color correction
// Verifies that material correction is applied correctly and affects matching
void test_material_correction()
{
    std::printf("\n=== Material correction test ===\n");

    // Test 1: Verify material correction factors
    material_correction_t fabric_corr = color_math_get_material_correction(MATERIAL_FABRIC);
    assert(fabsf(fabric_corr.l_scale - 1.10f) < 0.001f && "Fabric L scale should be 1.10");
    assert(fabsf(fabric_corr.l_offset - 2.0f) < 0.001f && "Fabric L offset should be 2.0");
    assert(fabsf(fabric_corr.a_scale - 1.05f) < 0.001f && "Fabric a scale should be 1.05");
    assert(fabsf(fabric_corr.b_scale - 1.05f) < 0.001f && "Fabric b scale should be 1.05");
    std::printf("Fabric correction: L*=%.2f+%.1f, a*=%.2f, b*=%.2f\n",
                fabric_corr.l_scale, fabric_corr.l_offset, fabric_corr.a_scale, fabric_corr.b_scale);

    material_correction_t metal_corr = color_math_get_material_correction(MATERIAL_METAL);
    assert(fabsf(metal_corr.l_scale - 0.90f) < 0.001f && "Metal L scale should be 0.90");
    assert(fabsf(metal_corr.a_scale - 0.95f) < 0.001f && "Metal a scale should be 0.95");
    std::printf("Metal correction: L*=%.2f+%.1f, a*=%.2f, b*=%.2f\n",
                metal_corr.l_scale, metal_corr.l_offset, metal_corr.a_scale, metal_corr.b_scale);

    material_correction_t default_corr = color_math_get_material_correction(MATERIAL_DEFAULT);
    assert(fabsf(default_corr.l_scale - 1.0f) < 0.001f && "Default L scale should be 1.0");
    assert(fabsf(default_corr.l_offset - 0.0f) < 0.001f && "Default L offset should be 0.0");
    std::printf("Default correction: L*=%.2f+%.1f, a*=%.2f, b*=%.2f\n",
                default_corr.l_scale, default_corr.l_offset, default_corr.a_scale, default_corr.b_scale);

    // Test 2: Verify correction application
    lab_t input_lab = {50.0f, 40.0f, 30.0f};
    lab_t corrected = color_math_apply_material_correction(&input_lab, &fabric_corr);
    
    // Expected: L = 50*1.10 + 2 = 57, a = 40*1.05 = 42, b = 30*1.05 = 31.5
    assert(fabsf(corrected.l - 57.0f) < 0.1f && "Fabric-corrected L should be ~57");
    assert(fabsf(corrected.a - 42.0f) < 0.1f && "Fabric-corrected a should be ~42");
    assert(fabsf(corrected.b - 31.5f) < 0.1f && "Fabric-corrected b should be ~31.5");
    std::printf("Input Lab: (%.1f, %.1f, %.1f) -> Fabric-corrected: (%.1f, %.1f, %.1f)\n",
                input_lab.l, input_lab.a, input_lab.b,
                corrected.l, corrected.a, corrected.b);

    // Test 3: Verify that pipeline applies material correction
    color_pipeline_config_t cfg{};
    cfg.min_luminance = 5.0f;
    cfg.max_delta_e = 12.0f;
    cfg.num_samples = 1;
    cfg.sample_delay_ms = 1;
    cfg.gray_threshold = 5.0f;
    cfg.color_threshold = 60.0f;
    cfg.saturation_boost = 1.5f;
    cfg.use_material_correction = true;
    cfg.assumed_material = MATERIAL_FABRIC;
    cfg.auto_detect_material = false;
    assert(color_pipeline_init(&cfg) == ESP_OK);

    // Process a test XYZ value
    xyz_t test_xyz{80.0f, 70.0f, 50.0f};
    color_result_t result{};
    assert(color_pipeline_process_xyz(&test_xyz, true, &result) == ESP_OK);

    // Verify that corrected_lab differs from scan_lab (material correction was applied)
    // For FABRIC: L should be higher, a and b should be slightly boosted
    std::printf("scan_lab: (%.2f, %.2f, %.2f)\n",
                result.scan_lab.l, result.scan_lab.a, result.scan_lab.b);
    std::printf("corrected_lab: (%.2f, %.2f, %.2f)\n",
                result.corrected_lab.l, result.corrected_lab.a, result.corrected_lab.b);
    std::printf("material: %s\n", color_math_material_name(result.material));

    assert(result.material == MATERIAL_FABRIC && "Material should be FABRIC");
    assert(result.corrected_lab.l > result.scan_lab.l && 
           "Fabric-corrected L should be higher than scan_lab L");
    
    // Test 4: Verify that disabling material correction gives identity
    cfg.use_material_correction = false;
    assert(color_pipeline_init(&cfg) == ESP_OK);

    color_result_t result_nocorr{};
    assert(color_pipeline_process_xyz(&test_xyz, true, &result_nocorr) == ESP_OK);

    // With correction disabled, corrected_lab should equal scan_lab
    assert(fabsf(result_nocorr.corrected_lab.l - result_nocorr.scan_lab.l) < 0.001f &&
           "With correction disabled, corrected_lab.l should equal scan_lab.l");
    assert(fabsf(result_nocorr.corrected_lab.a - result_nocorr.scan_lab.a) < 0.001f &&
           "With correction disabled, corrected_lab.a should equal scan_lab.a");
    std::printf("No correction: corrected_lab == scan_lab (%.2f, %.2f, %.2f)\n",
                result_nocorr.corrected_lab.l, result_nocorr.corrected_lab.a, result_nocorr.corrected_lab.b);

    // Test 5: Verify material name function
    assert(strcmp(color_math_material_name(MATERIAL_FABRIC), "Fabric") == 0);
    assert(strcmp(color_math_material_name(MATERIAL_METAL), "Metal") == 0);
    assert(strcmp(color_math_material_name(MATERIAL_PLASTIC), "Plastic") == 0);

    std::printf("Material correction test PASSED\n");
}

// Test auto-detect material feature
void test_auto_detect_material()
{
    std::printf("\n=== Auto-detect material test ===\n");

    // Test that auto_detect_material actually uses classification
    color_pipeline_config_t cfg{};
    cfg.min_luminance = 5.0f;
    cfg.max_delta_e = 12.0f;
    cfg.num_samples = 1;
    cfg.sample_delay_ms = 1;
    cfg.gray_threshold = 5.0f;
    cfg.color_threshold = 60.0f;
    cfg.saturation_boost = 1.5f;
    cfg.use_material_correction = true;
    cfg.assumed_material = MATERIAL_METAL;  // Set assumed to METAL
    cfg.auto_detect_material = true;         // Enable auto-detect
    assert(color_pipeline_init(&cfg) == ESP_OK);

    // Use a raw sensor reading that should classify as FABRIC
    // (low clear ratio, moderate chroma - typical fabric characteristics)
    sensor_reading_t raw{};
    raw.x = 11418369;
    raw.y = 7791616;
    raw.z = 2225664;
    raw.ir = 619520;
    raw.clear = 11418369 + 7791616 + 2225664;  // Total ~21M
    raw.gain = 5;
    raw.integration_ms = 100;
    raw.saturated = false;
    
    color_result_t result{};
    assert(color_pipeline_identify_from_reading(&raw, true, &result) == ESP_OK);
    
    // The material should be auto-detected (likely FABRIC since that's the default fallback)
    // Not METAL which was the assumed_material
    std::printf("auto_detect_material=true: detected material=%s (assumed was METAL)\n",
                color_math_material_name(result.material));
    
    // Verify the classification function works
    material_type_t classified = color_math_classify_material(&raw, nullptr);
    std::printf("Direct classification result: %s\n", color_math_material_name(classified));
    assert(result.material == classified && 
           "Pipeline should use classification result when auto_detect is enabled");

    xyz_t high_variance = {1000000.0f, 1000000.0f, 1000000.0f};
    material_type_t variance_classified = color_math_classify_material(&raw, &high_variance);
    std::printf("Variance-assisted classification result: %s\n",
                color_math_material_name(variance_classified));
    assert(variance_classified == MATERIAL_FABRIC &&
           "High variance input should classify as fabric");
    
    // Test with auto_detect disabled - should use assumed_material
    cfg.auto_detect_material = false;
    assert(color_pipeline_init(&cfg) == ESP_OK);
    
    color_result_t result_no_auto{};
    assert(color_pipeline_identify_from_reading(&raw, true, &result_no_auto) == ESP_OK);
    
    std::printf("auto_detect_material=false: material=%s (assumed=METAL)\n",
                color_math_material_name(result_no_auto.material));
    assert(result_no_auto.material == MATERIAL_METAL && 
           "Pipeline should use assumed_material when auto_detect is disabled");

    std::printf("Auto-detect material test PASSED\n");
}

void test_confidence_scaled_boost_uses_config_saturation()
{
    std::printf("\n=== Confidence-scaled saturation source test ===\n");

    color_pipeline_config_t cfg{};
    cfg.min_luminance = 5.0f;
    cfg.max_delta_e = 12.0f;
    cfg.num_samples = 1;
    cfg.sample_delay_ms = 1;
    cfg.gray_threshold = 5.0f;
    cfg.color_threshold = 60.0f;
    cfg.saturation_boost = 1.10f;
    cfg.use_material_correction = true;
    cfg.assumed_material = MATERIAL_FABRIC;
    cfg.auto_detect_material = false;
    assert(color_pipeline_init(&cfg) == ESP_OK);

    xyz_t xyz{80.0f, 70.0f, 50.0f};
    color_result_t result{};
    assert(color_pipeline_process_xyz(&xyz, true, &result) == ESP_OK);
    assert(result.confidence > 0.0f && result.confidence < 1.0f &&
           "Test input must exercise the partial-confidence saturation path");

    const float effective_boost =
        1.0f + (cfg.saturation_boost - 1.0f) * result.confidence;

    lab_t expected = result.scan_lab;
    color_math_enhance_saturation(&expected,
                                  cfg.gray_threshold,
                                  cfg.color_threshold,
                                  effective_boost);
    expected.a = fminf(fmaxf(expected.a, -110.0f), 110.0f);
    expected.b = fminf(fmaxf(expected.b, -110.0f), 110.0f);

    std::printf("confidence=%.3f effective_boost=%.4f\n",
                result.confidence, effective_boost);
    std::printf("expected lab=(%.4f, %.4f, %.4f)\n",
                expected.l, expected.a, expected.b);
    std::printf("actual   lab=(%.4f, %.4f, %.4f)\n",
                result.lab.l, result.lab.a, result.lab.b);

    assert(fabsf(result.lab.l - expected.l) < 0.001f);
    assert(fabsf(result.lab.a - expected.a) < 0.01f);
    assert(fabsf(result.lab.b - expected.b) < 0.01f);

    std::printf("Confidence-scaled saturation source test PASSED\n");
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
    cfg.use_material_correction = true;
    cfg.assumed_material = MATERIAL_FABRIC;
    cfg.auto_detect_material = false;
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
    // The Kona set now includes green swatches (e.g., CLOVER), so a green XYZ may
    // legitimately match a Kona entry. Just verify the pipeline returns a valid name.
    std::printf("green test: color=%s kona_matched=%d\n", green.color_name, (int)green.kona_matched);

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

    // Run material correction tests
    test_material_correction();

    // Run auto-detect material test
    test_auto_detect_material();

    // Verify confidence-scaled reboost uses the configured saturation source.
    test_confidence_scaled_boost_uses_config_saturation();

    std::printf("All Kona matching tests passed!\n");
    return 0;
}
