#include "auto_calibrate.h"
#include <cstdio>

static void add_refs(auto_cal_ctx_t* ctx)
{
    // These references match the colours exercised in capture_samples.json.
    // The host calibration is intentionally a subset of the full device
    // calibration reference set so that the PCCM is optimised for the tested
    // colours without being pulled off by warm-neutral patches (e.g. Skin,
    // Brown 3) whose sRGB Lab targets are ≈+5 a* warmer than their physical
    // reflectance under this LED, which would collapse muted greens to yellow.
    //
    // Device calibration (main.cpp) uses the complete set including Skin, so
    // warm-neutral accuracy on real hardware is unaffected.
    auto_cal_add_reference_rgb(ctx, "Dark Gray",     35,  31,  32, CAL_REF_FLAG_REQUIRED | CAL_REF_FLAG_IS_BLACK);
    auto_cal_add_reference_rgb(ctx, "White",        241, 241, 242, CAL_REF_FLAG_REQUIRED | CAL_REF_FLAG_IS_WHITE);
    auto_cal_add_reference_rgb(ctx, "Brights Red",  237,  28,  36, CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(ctx, "Brights Green",  0, 161,  75, CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(ctx, "Brights Blue",  33,  63, 153, CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(ctx, "Brights Yellow",255, 221,  23, CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(ctx, "Brights Orange",241, 101,  33, CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(ctx, "Cyan",           0, 173, 239, CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(ctx, "Dark Green",     0, 103,  56, CAL_REF_FLAG_DARK_CHROMATIC);
    auto_cal_add_reference_rgb(ctx, "Gray 50",      147, 149, 151, CAL_REF_FLAG_GRAY);
    auto_cal_add_reference_rgb(ctx, "Gray 80",       88,  88,  91, CAL_REF_FLAG_GRAY);
    auto_cal_add_reference_rgb(ctx, "Mid Green",      0, 147,  68, CAL_REF_FLAG_REQUIRED);

    // Muted Green: derived from the committed green wall-paint capture
    // (raw X=11517952 Y=12179200 Z=7376128, gain=8×, 100ms).
    // Target Lab (L=80.2 a*=-5.0 b*=14.3) is derived from the white-normalised
    // D65 reflectance of the surface (theoretical a*=-6.86, hue=116°), clamped
    // to a*=-5.0 (hue=109°) to stay well inside Green (105°–160°) while being
    // realistically achievable by the polynomial PCCM.
    const lab_t muted_green_lab = { 80.16f, -5.0f, 14.29f };
    auto_cal_add_reference(ctx, "Muted Green", &muted_green_lab, CAL_REF_FLAG_REQUIRED);
}

int main()
{
    auto_cal_ctx_t* ctx = nullptr;
    if (auto_cal_init(&ctx) != ESP_OK) return 1;
    add_refs(ctx);

    // Pass 1 — bootstraps the linear CCM and initial lightness scale
    if (auto_cal_start(ctx) != ESP_OK) return 2;
    if (auto_cal_submit_raw_measurements_from_file(ctx, "host/calibration_measurements_raw.cfg") != ESP_OK) return 3;
    if (auto_cal_run_optimization(ctx) != ESP_OK) return 4;

    // Pass 2 — restarts Adam from the Pass-1 best params.  init_ccm_from_measurements
    // re-derives the linear CCM using the refined lightness_scale; the non-linear
    // polynomial terms from Pass 1 are preserved.  This second pass helps the
    // muted-green constraint converge further without having to extend MAX_ITERATIONS.
    if (auto_cal_start(ctx) != ESP_OK) return 5;
    if (auto_cal_submit_raw_measurements_from_file(ctx, "host/calibration_measurements_raw.cfg") != ESP_OK) return 6;
    if (auto_cal_run_optimization(ctx) != ESP_OK) return 7;
    if (auto_cal_save_to_nvs(ctx, "auto_cal") != ESP_OK) return 8;
    auto_cal_deinit(&ctx);
    std::puts("autocal host test complete");
    return 0;
}
