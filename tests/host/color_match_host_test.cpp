#include "color_pipeline.h"
#include <cstdio>

int main()
{
    color_pipeline_config_t cfg{};
    cfg.min_luminance = 5.0f;
    cfg.max_delta_e = 10.0f;
    cfg.num_samples = 1;
    cfg.sample_delay_ms = 1;
    cfg.gray_threshold = 5.0f;
    cfg.color_threshold = 60.0f;
    cfg.saturation_boost = 1.5f;
    if (color_pipeline_init(&cfg) != ESP_OK) return 1;

    xyz_t xyz{2460.86f, 1623.25f, 556.42f};
    color_result_t result{};
    if (color_pipeline_process_xyz(&xyz, true, &result) != ESP_OK) return 2;
    std::printf("matched=%s confidence=%.3f\n", result.color_name, result.confidence);
    return 0;
}
