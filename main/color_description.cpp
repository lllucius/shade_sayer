/**
 * @file color_description.cpp
 * @brief Runtime Color Description Generator Implementation
 *
 * Implements natural language color description generation from LAB values.
 * This is a C++ port of the Python generate_description() function from
 * scripts/import_resene_colors.py.
 */

#include "color_description.h"
#include <cmath>
#include <cstdio>
#include <cstring>

// Chroma threshold for neutral color detection (below this is gray)
static const float NEUTRAL_CHROMA_THRESHOLD = 10.0f;

// Chroma threshold for pure neutral (completely achromatic)
static const float PURE_NEUTRAL_CHROMA_THRESHOLD = 2.0f;

// Lightness thresholds for pure black and white
static const float BLACK_LIGHTNESS_THRESHOLD = 5.0f;
static const float WHITE_LIGHTNESS_THRESHOLD = 95.0f;

// Conversion factor from radians to degrees
static const float RAD_TO_DEG = 180.0f / M_PI;

// Hue name entry
typedef struct
{
    float hue_min;
    float hue_max;
    const char* name;
} hue_entry_t;

// Hue names based on CIELAB hue angle (degrees)
static const hue_entry_t HUE_NAMES[] =
{
    {0, 10, "red"},
    {10, 25, "vermilion"},
    {25, 45, "orange"},
    {45, 60, "amber"},
    {60, 80, "gold"},
    {80, 95, "yellow"},
    {95, 110, "lime"},
    {110, 130, "chartreuse"},
    {130, 155, "green"},
    {155, 175, "teal"},
    {175, 195, "cyan"},
    {195, 215, "azure"},
    {215, 235, "cerulean"},
    {235, 260, "blue"},
    {260, 280, "indigo"},
    {280, 300, "violet"},
    {300, 320, "purple"},
    {320, 340, "magenta"},
    {340, 355, "crimson"},
    {355, 360, "red"}
};
static const int NUM_HUE_ENTRIES = sizeof(HUE_NAMES) / sizeof(hue_entry_t);

// Color associations for descriptions
typedef struct
{
    const char* hue_name;
    const char* association;
} color_association_t;

static const color_association_t COLOR_ASSOCIATIONS[] =
{
    {"red", "roses"},
    {"vermilion", "autumn leaves"},
    {"orange", "oranges"},
    {"amber", "honey"},
    {"gold", "ripe wheat"},
    {"yellow", "sunflowers"},
    {"lime", "spring buds"},
    {"chartreuse", "absinthe"},
    {"green", "emeralds"},
    {"teal", "peacock feathers"},
    {"cyan", "turquoise"},
    {"azure", "clear skies"},
    {"cerulean", "ocean depths"},
    {"blue", "sapphires"},
    {"indigo", "midnight skies"},
    {"violet", "lavender"},
    {"purple", "plums"},
    {"magenta", "orchids"},
    {"crimson", "blood oranges"}
};
static const int NUM_COLOR_ASSOCIATIONS = sizeof(COLOR_ASSOCIATIONS) / sizeof(color_association_t);

// Neutral color associations by lightness range
typedef struct
{
    float l_min;
    float l_max;
    const char* association;
} neutral_association_t;

static const neutral_association_t NEUTRAL_ASSOCIATIONS[] =
{
    {0, 5, "jet"},
    {5, 15, "charcoal"},
    {15, 30, "graphite"},
    {30, 50, "pewter"},
    {50, 70, "dove"},
    {70, 85, "pearl"},
    {85, 95, "snow"},
    {95, 101, "chalk"}
};
static const int NUM_NEUTRAL_ASSOCIATIONS = sizeof(NEUTRAL_ASSOCIATIONS) / sizeof(neutral_association_t);

// Warm hues set
static const char* const WARM_HUES[] = {"red", "vermilion", "orange", "amber", "gold", "yellow", "crimson", "magenta"};
static const int NUM_WARM_HUES = sizeof(WARM_HUES) / sizeof(const char*);

// Cool hues set
static const char* const COOL_HUES[] = {"lime", "chartreuse", "green", "teal", "cyan", "azure", "cerulean", "blue", "indigo", "violet", "purple"};
static const int NUM_COOL_HUES = sizeof(COOL_HUES) / sizeof(const char*);

/**
 * @brief Check if a hue name is in a set
 */
static bool is_in_set(const char* hue_name, const char* const* set, int set_size)
{
    for (int i = 0; i < set_size; i++)
    {
        if (strcmp(hue_name, set[i]) == 0)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Get hue name from hue angle in degrees
 */
static const char* get_hue_name(float hue_degrees)
{
    // Normalize to 0-360 using fmodf
    hue_degrees = fmodf(hue_degrees, 360.0f);
    if (hue_degrees < 0)
    {
        hue_degrees += 360.0f;
    }

    for (int i = 0; i < NUM_HUE_ENTRIES; i++)
    {
        if (hue_degrees >= HUE_NAMES[i].hue_min && hue_degrees < HUE_NAMES[i].hue_max)
        {
            return HUE_NAMES[i].name;
        }
    }

    return "red";  // Default fallback
}

/**
 * @brief Get association for a hue name
 */
static const char* get_color_association(const char* hue_name)
{
    for (int i = 0; i < NUM_COLOR_ASSOCIATIONS; i++)
    {
        if (strcmp(hue_name, COLOR_ASSOCIATIONS[i].hue_name) == 0)
        {
            return COLOR_ASSOCIATIONS[i].association;
        }
    }
    return "objects of this hue";  // Fallback
}

/**
 * @brief Get neutral association for a lightness value
 */
static const char* get_neutral_association(float L)
{
    for (int i = 0; i < NUM_NEUTRAL_ASSOCIATIONS; i++)
    {
        if (L >= NEUTRAL_ASSOCIATIONS[i].l_min && L < NEUTRAL_ASSOCIATIONS[i].l_max)
        {
            return NEUTRAL_ASSOCIATIONS[i].association;
        }
    }
    return "slate";  // Fallback
}

/**
 * @brief Get tone descriptor based on lightness and chroma
 */
static const char* get_tone_descriptor(float L, float chroma)
{
    // High lightness (L >= 70)
    if (L >= 70)
    {
        if (chroma < 20)
        {
            return "pale";
        }
        else if (chroma < 40)
        {
            return "pastel";
        }
        else if (chroma < 65)
        {
            return "bright";
        }
        else
        {
            return "vibrant";
        }
    }
    // Medium-high lightness (L 50-70)
    else if (L >= 50)
    {
        if (chroma < 20)
        {
            return "soft";
        }
        else if (chroma < 40)
        {
            return "moderate";
        }
        else if (chroma < 65)
        {
            return "vivid";
        }
        else
        {
            return "electric";
        }
    }
    // Medium-low lightness (L 30-50)
    else if (L >= 30)
    {
        if (chroma < 20)
        {
            return "muted";
        }
        else if (chroma < 40)
        {
            return "dusty";
        }
        else if (chroma < 65)
        {
            return "rich";
        }
        else
        {
            return "saturated";
        }
    }
    // Low lightness (L < 30)
    else
    {
        if (chroma < 20)
        {
            return "dark";
        }
        else if (chroma < 40)
        {
            return "deep";
        }
        else if (chroma < 65)
        {
            return "intense";
        }
        else
        {
            return "bold";
        }
    }
}

/**
 * @brief Generate description for neutral or near-neutral colors
 */
static int get_neutral_description(float L, float a, float b, float chroma,
                                   char* buffer, size_t buffer_size)
{
    // True neutrals (completely achromatic)
    if (chroma < PURE_NEUTRAL_CHROMA_THRESHOLD)
    {
        if (L < BLACK_LIGHTNESS_THRESHOLD)
        {
            return snprintf(buffer, buffer_size, "Pure black");
        }
        else if (L > WHITE_LIGHTNESS_THRESHOLD)
        {
            return snprintf(buffer, buffer_size, "Pure white");
        }
        else
        {
            const char* assoc = get_neutral_association(L);
            return snprintf(buffer, buffer_size, "A pure gray like %s", assoc);
        }
    }

    // Tinted neutrals (slightly chromatic grays) - calculate hue
    float hue_rad = atan2f(b, a);
    float hue_deg = hue_rad * RAD_TO_DEG;
    if (hue_deg < 0)
    {
        hue_deg += 360.0f;
    }
    const char* hue_name = get_hue_name(hue_deg);

    const char* temp_desc;
    if (is_in_set(hue_name, WARM_HUES, NUM_WARM_HUES))
    {
        temp_desc = "warm";
    }
    else if (is_in_set(hue_name, COOL_HUES, NUM_COOL_HUES))
    {
        temp_desc = "cool";
    }
    else
    {
        temp_desc = "neutral";
    }

    // Get lightness-based association
    const char* assoc = get_neutral_association(L);
    return snprintf(buffer, buffer_size, "A %s gray like %s", temp_desc, assoc);
}

int color_description_generate(const lab_t* lab, char* buffer, size_t buffer_size)
{
    if (!lab || !buffer || buffer_size == 0)
    {
        return 0;
    }

    float L = lab->l;
    float a = lab->a;
    float b = lab->b;
    float chroma = color_math_chroma(lab);

    // Handle neutral and near-neutral colors
    if (chroma < NEUTRAL_CHROMA_THRESHOLD)
    {
        return get_neutral_description(L, a, b, chroma, buffer, buffer_size);
    }

    // Chromatic colors
    float hue_rad = atan2f(b, a);
    float hue_deg = hue_rad * RAD_TO_DEG;
    if (hue_deg < 0)
    {
        hue_deg += 360.0f;
    }

    const char* hue_name = get_hue_name(hue_deg);
    const char* tone_desc = get_tone_descriptor(L, chroma);
    const char* association = get_color_association(hue_name);

    // Determine warm/cool temperature note
    const char* temp;
    if (is_in_set(hue_name, WARM_HUES, NUM_WARM_HUES))
    {
        temp = " - a warm color";
    }
    else if (is_in_set(hue_name, COOL_HUES, NUM_COOL_HUES))
    {
        temp = " - a cool color";
    }
    else
    {
        temp = "";
    }

    // Use "An" for tones starting with a vowel
    const char* article = (tone_desc[0] == 'a' || tone_desc[0] == 'e' ||
                           tone_desc[0] == 'i' || tone_desc[0] == 'o' ||
                           tone_desc[0] == 'u') ? "An" : "A";

    return snprintf(buffer, buffer_size, "%s %s %s like %s%s",
                    article, tone_desc, hue_name, association, temp);
}

const char* color_description_get_hue_name(const lab_t* lab)
{
    if (!lab)
    {
        return "unknown";
    }

    float a = lab->a;
    float b = lab->b;
    float chroma = color_math_chroma(lab);

    // Handle neutral colors
    if (chroma < NEUTRAL_CHROMA_THRESHOLD)
    {
        if (lab->l < BLACK_LIGHTNESS_THRESHOLD)
        {
            return "black";
        }
        else if (lab->l > WHITE_LIGHTNESS_THRESHOLD)
        {
            return "white";
        }
        else
        {
            return "gray";
        }
    }

    // Chromatic colors
    float hue_rad = atan2f(b, a);
    float hue_deg = hue_rad * RAD_TO_DEG;
    if (hue_deg < 0)
    {
        hue_deg += 360.0f;
    }

    return get_hue_name(hue_deg);
}

const char* color_description_get_tone(const lab_t* lab)
{
    if (!lab)
    {
        return "unknown";
    }

    float chroma = color_math_chroma(lab);
    return get_tone_descriptor(lab->l, chroma);
}

bool color_description_is_neutral(const lab_t* lab)
{
    if (!lab)
    {
        return false;
    }

    float chroma = color_math_chroma(lab);
    return chroma < NEUTRAL_CHROMA_THRESHOLD;
}
