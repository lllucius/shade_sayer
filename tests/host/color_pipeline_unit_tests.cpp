#include "color_pipeline.h"
#include <cassert>

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

    color_result_t red{};
    xyz_t red_xyz{2460.86f, 1623.25f, 556.42f};
    assert(color_pipeline_process_xyz(&red_xyz, true, &red) == ESP_OK);
    assert(red.color_name != nullptr);

    color_result_t green{};
    xyz_t green_xyz{1281.60f, 2094.40f, 957.50f};
    assert(color_pipeline_process_xyz(&green_xyz, true, &green) == ESP_OK);
    assert(green.color_name != nullptr);


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

    return 0;
}
