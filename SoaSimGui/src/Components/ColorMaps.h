#pragma once
#include "imgui.h"
#include <algorithm>
#include <cstdint>

inline static ImU32 RGBAu8(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return IM_COL32(r, g, b, a);
}

inline static float _lerp(float a, float b, float t) { return a + (b - a) * t; }

inline static ImU32 HSVtoRGBA(float h_deg, float s, float v) {
    h_deg = fmodf(fmodf(h_deg, 360.0f) + 360.0f, 360.0f);
    s = std::clamp(s, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    float c = v * s;
    float h = h_deg / 60.0f;
    float x = c * (1.0f - fabsf(fmodf(h, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    if (0 <= h && h < 1) { r = c; g = x; b = 0; }
    else if (1 <= h && h < 2) { r = x; g = c; b = 0; }
    else if (2 <= h && h < 3) { r = 0; g = c; b = x; }
    else if (3 <= h && h < 4) { r = 0; g = x; b = c; }
    else if (4 <= h && h < 5) { r = x; g = 0; b = c; }
    else { r = c; g = 0; b = x; }
    float m = v - c;
    r += m; g += m; b += m;
    return RGBAu8((uint8_t)roundf(r * 255.0f), (uint8_t)roundf(g * 255.0f), (uint8_t)roundf(b * 255.0f));
}

inline static ImU32 Lerp(ImU32 a, ImU32 b, float t) {
    auto ar = (a >> IM_COL32_R_SHIFT) & 0xFF;
    auto ag = (a >> IM_COL32_G_SHIFT) & 0xFF;
    auto ab = (a >> IM_COL32_B_SHIFT) & 0xFF;
    auto aa = (a >> IM_COL32_A_SHIFT) & 0xFF;
    auto br = (b >> IM_COL32_R_SHIFT) & 0xFF;
    auto bg = (b >> IM_COL32_G_SHIFT) & 0xFF;
    auto bb = (b >> IM_COL32_B_SHIFT) & 0xFF;
    auto ba = (b >> IM_COL32_A_SHIFT) & 0xFF;
    uint8_t r = (uint8_t)(ar + (br - ar) * t);
    uint8_t g = (uint8_t)(ag + (bg - ag) * t);
    uint8_t b_ = (uint8_t)(ab + (bb - ab) * t);
    uint8_t a_ = (uint8_t)(aa + (ba - aa) * t);
    return IM_COL32(r, g, b_, a_);
}

// Negative: purples. Positive: blue -> green -> yellow -> orange -> red.
// Emphasize positive separation. Domain typically [-2, 32]; clamp but adapt to data.
inline static ImU32 SeedDeltaToColor(int delta, int min_neg, int max_pos) {
    if (max_pos < 1) max_pos = 1;
    if (min_neg > -1) min_neg = -1;

    if (delta == 0) {
        return RGBAu8(230, 230, 230);
    }
    if (delta < 0) {
        int dn = std::clamp(delta, min_neg, 0);
        float t = (float)(dn - min_neg) / (float)(0 - min_neg);
        ImU32 deep = RGBAu8(59, 10, 87);    // deep purple
        ImU32 light = RGBAu8(176, 123, 227); // light purple
        return Lerp(deep, light, t);
    }
    else {
        int dp = std::clamp(delta, 0, max_pos);
        float t = (float)dp / (float)max_pos;
        // piecewise gradient: 0..1 across 5 stops
        ImU32 c0 = RGBAu8(44, 123, 182);  // blue
        ImU32 c1 = RGBAu8(26, 152, 80);   // green
        ImU32 c2 = RGBAu8(255, 255, 191); // yellow
        ImU32 c3 = RGBAu8(253, 174, 97);  // orange
        ImU32 c4 = RGBAu8(215, 25, 28);   // red
        if (t < 0.25f) return Lerp(c0, c1, t / 0.25f);
        if (t < 0.50f) return Lerp(c1, c2, (t - 0.25f) / 0.25f);
        if (t < 0.75f) return Lerp(c2, c3, (t - 0.50f) / 0.25f);
        return Lerp(c3, c4, (t - 0.75f) / 0.25f);
    }
}

inline static ImU32 SeedDeltaToColorModB(int delta, int min_neg, int max_pos) {
    if (delta == 0) return RGBAu8(128, 128, 128);

    max_pos = std::max(max_pos, 1);
    min_neg = std::min(min_neg, -1);

    if (delta < 0) {
        int dn = std::clamp(delta, min_neg, 0);
        float t = (float)(dn - min_neg) / (float)(0 - min_neg);
        ImU32 deep = RGBAu8(59, 10, 87);    // deep purple
        ImU32 light = RGBAu8(176, 123, 227); // light purple
        return Lerp(deep, light, t);
    }
    else {
        int d = std::clamp(delta, 0, max_pos);
        float t = powf((float)d / (float)max_pos, 0.6f); // spread small positives
        float h; // piecewise hue ramp
        if (t < 0.25f) h = _lerp(210.0f, 120.0f, t / 0.25f); // blue->green
        else if (t < 0.50f) h = _lerp(120.0f, 60.0f, (t - 0.25f) / 0.25f); // green->yellow
        else if (t < 0.75f) h = _lerp(60.0f, 30.0f, (t - 0.50f) / 0.25f); // yellow->orange
        else                h = _lerp(30.0f, 0.0f, (t - 0.75f) / 0.25f); // orange->red
        static const float VSET[3] = { 0.90f, 0.70f, 0.50f }; // mod-3 cadence
        float v = VSET[d % 3];
        return HSVtoRGBA(h, 0.90f, v);
    }
}
