/**
 * @file color_types.cpp
 * @brief TCS3530 gain scaling factor lookup tables and management
 *
 * Provides the mutable gain multiplier table that maps TCS3530 hardware
 * gain codes (0–13) to floating-point multipliers.  The table is
 * initialised to the nominal datasheet values and can be overridden at
 * runtime (e.g. after calibration) via tcs3530_set_gain_scaling_factors().
 */

#include "color_types.h"

/** @brief Default gain multipliers indexed by TCS3530 gain code (0–13). */
static float s_gain_multipliers[TCS3530_MAX_GAIN_CODE + 1] =
{
    0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f,
    128.0f, 256.0f, 512.0f, 1024.0f, 2048.0f, 4096.0f
};

/**
 * @brief Replace gain scaling factors with calibrated values
 *
 * Only entries whose corresponding value in @p factors is positive are
 * updated; zero or negative entries are silently skipped.
 *
 * @param factors  Array of new multiplier values
 * @param count    Number of entries in @p factors (clamped to table size)
 */
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

/**
 * @brief Retrieve the current gain scaling factor table
 *
 * @param[out] count  If non-NULL, receives the number of table entries
 * @return Pointer to the internal multiplier array (read-only use expected)
 */
const float* tcs3530_get_gain_scaling_factors(size_t* count)
{
    if (count)
    {
        *count = TCS3530_MAX_GAIN_CODE + 1;
    }
    return s_gain_multipliers;
}

/**
 * @brief Convert a TCS3530 gain register code to its multiplier value
 *
 * Out-of-range codes fall back to gain code 8 (128×).
 *
 * @param gain_code  Hardware gain code (0–13 valid)
 * @return Floating-point gain multiplier
 */
float tcs3530_gain_code_to_multiplier(uint8_t gain_code)
{
    if (gain_code <= TCS3530_MAX_GAIN_CODE)
    {
        return s_gain_multipliers[gain_code];
    }
    return s_gain_multipliers[8];
}
