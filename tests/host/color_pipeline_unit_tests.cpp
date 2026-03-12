#include "color_pipeline.h"
#include "color_math.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>

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
    assert(!red.kona_matched);
    assert(red.kona_id == 0);

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

    // Test Kona matching with raw Lab values.
    // The reference table entry for SUNNY (id=449) is:
    //   L=86.767400, a=14.785100, b=88.852600
    // We convert this to XYZ and pass it directly to process_xyz.
    // The match should succeed because the raw Lab matches the reference.
    lab_t sunny_lab = {86.767400f, 14.785100f, 88.852600f};
    xyz_t sunny_xyz = color_math_lab_to_xyz(sunny_lab);
    color_result_t sunny_result{};
    assert(color_pipeline_process_xyz(&sunny_xyz, true, &sunny_result) == ESP_OK);
    std::printf("SUNNY test: kona_matched=%d kona_id=%u name=%s delta_e=%.3f\n",
                (int)sunny_result.kona_matched,
                (unsigned int)sunny_result.kona_id,
                sunny_result.color_name ? sunny_result.color_name : "(null)",
                sunny_result.delta_e);
    assert(sunny_result.kona_matched && "SUNNY should match Kona reference");
    assert(sunny_result.kona_id == 449 && "SUNNY id should be 449");
    assert(sunny_result.delta_e < 0.01f && "SUNNY delta_e should be near zero");

    // Test NECTARINE (id=496): L=68.801900, a=48.894800, b=36.220800
    lab_t nectarine_lab = {68.801900f, 48.894800f, 36.220800f};
    xyz_t nectarine_xyz = color_math_lab_to_xyz(nectarine_lab);
    color_result_t nectarine_result{};
    assert(color_pipeline_process_xyz(&nectarine_xyz, true, &nectarine_result) == ESP_OK);
    std::printf("NECTARINE test: kona_matched=%d kona_id=%u name=%s delta_e=%.3f\n",
                (int)nectarine_result.kona_matched,
                (unsigned int)nectarine_result.kona_id,
                nectarine_result.color_name ? nectarine_result.color_name : "(null)",
                nectarine_result.delta_e);
    assert(nectarine_result.kona_matched && "NECTARINE should match Kona reference");
    assert(nectarine_result.kona_id == 496 && "NECTARINE id should be 496");
    assert(nectarine_result.delta_e < 0.01f && "NECTARINE delta_e should be near zero");

    std::printf("All Kona matching tests passed!\n");
    return 0;
}
