#include "color_types.h"

static float s_gain_multipliers[TCS3530_MAX_GAIN_CODE + 1] =
{
    0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f,
    128.0f, 256.0f, 512.0f, 1024.0f, 2048.0f, 4096.0f
};

void tcs3530_set_gain_scaling_factors(const float* factors, size_t count)
{
    if (!factors || count == 0)
    {
        return;
    }

    const size_t max_count = TCS3530_MAX_GAIN_CODE + 1;
    const size_t update_count = (count < max_count) ? count : max_count;
    for (size_t i = 0; i < update_count; ++i)
    {
        if (factors[i] > 0.0f)
        {
            s_gain_multipliers[i] = factors[i];
        }
    }
}

const float* tcs3530_get_gain_scaling_factors(size_t* count)
{
    if (count)
    {
        *count = TCS3530_MAX_GAIN_CODE + 1;
    }
    return s_gain_multipliers;
}

float tcs3530_gain_code_to_multiplier(uint8_t gain_code)
{
    if (gain_code <= TCS3530_MAX_GAIN_CODE)
    {
        return s_gain_multipliers[gain_code];
    }
    return s_gain_multipliers[8];
}
