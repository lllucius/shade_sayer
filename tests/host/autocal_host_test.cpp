#include "auto_calibrate.h"
#include <cstdio>

static void add_refs(auto_cal_ctx_t* ctx)
{
    auto_cal_add_reference_rgb(ctx, "Dark Gray", 35, 31, 32, CAL_REF_FLAG_REQUIRED | CAL_REF_FLAG_IS_BLACK);
    auto_cal_add_reference_rgb(ctx, "White", 241, 241, 242, CAL_REF_FLAG_REQUIRED | CAL_REF_FLAG_IS_WHITE);
    auto_cal_add_reference_rgb(ctx, "Brights Red", 237, 28, 36, CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(ctx, "Brights Green", 0, 161, 75, CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(ctx, "Brights Blue", 33, 63, 153, CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(ctx, "Brights Yellow", 255, 221, 23, CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(ctx, "Brights Orange", 241, 101, 33, CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(ctx, "Cyan", 0, 173, 239, CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(ctx, "Skin", 194, 180, 154, CAL_REF_FLAG_REQUIRED);
    auto_cal_add_reference_rgb(ctx, "Dark Brown", 96, 56, 19, CAL_REF_FLAG_DARK_CHROMATIC);
    auto_cal_add_reference_rgb(ctx, "Dark Taupe", 89, 74, 65, CAL_REF_FLAG_DARK_CHROMATIC);
    auto_cal_add_reference_rgb(ctx, "Dark Green", 0, 103, 56, CAL_REF_FLAG_DARK_CHROMATIC);
    auto_cal_add_reference_rgb(ctx, "Gray 50", 147, 149, 151, CAL_REF_FLAG_GRAY);
    auto_cal_add_reference_rgb(ctx, "Gray 20", 209, 210, 212, CAL_REF_FLAG_GRAY);
    auto_cal_add_reference_rgb(ctx, "Gray 80", 88, 88, 91, CAL_REF_FLAG_GRAY);
    auto_cal_add_reference_rgb(ctx, "Brown 1", 59, 35, 20, 0);
    auto_cal_add_reference_rgb(ctx, "Brown 2", 138, 93, 59, 0);
    auto_cal_add_reference_rgb(ctx, "Brown 3", 195, 165, 107, 0);
    auto_cal_add_reference_rgb(ctx, "Mid Green", 0, 147, 68, CAL_REF_FLAG_REQUIRED);
}

int main()
{
    auto_cal_ctx_t* ctx = nullptr;
    if (auto_cal_init(&ctx) != ESP_OK) return 1;
    add_refs(ctx);
    if (auto_cal_start(ctx) != ESP_OK) return 2;
    if (auto_cal_submit_raw_measurements_from_file(ctx, "host/calibration_measurements_raw.cfg") != ESP_OK) return 3;
    if (auto_cal_run_optimization(ctx) != ESP_OK) return 4;
    if (auto_cal_save_to_nvs(ctx, "auto_cal") != ESP_OK) return 5;
    auto_cal_deinit(&ctx);
    std::puts("autocal host test complete");
    return 0;
}
