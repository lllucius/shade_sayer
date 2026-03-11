/**
 * @file color_description.h
 * @brief Runtime Color Description Generator
 *
 * Generates human-readable descriptions of colors based on their LAB values.
 * This module provides natural language descriptions suitable for TTS output.
 *
 * The descriptions include:
 * - Tone descriptors (pale, vibrant, muted, dark, etc.)
 * - Hue names (red, orange, yellow, green, blue, etc.)
 * - Color associations (like roses, oranges, sapphires, etc.)
 * - Temperature notes (warm/cool color)
 *
 * For neutral/achromatic colors:
 * - Lightness-based descriptions (pure black, charcoal, slate, snow, etc.)
 * - Warm/cool gray detection for tinted neutrals
 */

#ifndef COLOR_DESCRIPTION_H
#define COLOR_DESCRIPTION_H

#include <stddef.h>
#include <stdbool.h>
#include "color_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Generate a human-readable description of a color
 *
 * Creates a natural language description based on the color's LAB values.
 * The description includes tone, hue name, associations, and temperature.
 *
 * @param lab Pointer to LAB color values
 * @param buffer Output buffer for the description string
 * @param buffer_size Size of the output buffer
 * @return Number of characters written (excluding null terminator), or 0 on error
 *
 * @note The output is null-terminated and safe to use with TTS
 * @note Typical descriptions are 50-100 characters
 *
 * @par Example output
 * - "A vibrant red like roses - a warm color"
 * - "A pale blue like clear skies - a cool color"
 * - "A pure gray like slate"
 * - "Pure black"
 */
int color_description_generate(const lab_t* lab, char* buffer, size_t buffer_size);

/**
 * @brief Get just the hue name for a color
 *
 * Returns a simple hue name without additional description.
 * Useful for building custom descriptions.
 *
 * @param lab Pointer to LAB color values
 * @return Static string with hue name (e.g., "red", "blue", "gray")
 */
const char* color_description_get_hue_name(const lab_t* lab);

/**
 * @brief Get the tone descriptor for a color
 *
 * Returns a tone word based on lightness and chroma.
 *
 * @param lab Pointer to LAB color values
 * @return Static string with tone descriptor (e.g., "pale", "vibrant", "dark")
 */
const char* color_description_get_tone(const lab_t* lab);

/**
 * @brief Check if a color is neutral (gray/black/white)
 *
 * @param lab Pointer to LAB color values
 * @return true if the color is achromatic or near-achromatic
 */
bool color_description_is_neutral(const lab_t* lab);

#ifdef __cplusplus
}
#endif

#endif /* COLOR_DESCRIPTION_H */
