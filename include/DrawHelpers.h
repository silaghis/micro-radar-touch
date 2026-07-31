#pragma once

#include "LGFX.h"

// low altitude = red, high altitude (12000m ceiling) = violet
inline uint32_t AltitudeToColor(float baroAltitude)
{
    constexpr float ALTITUDE_CEILING = 12000.0f;
    const float t = constrain(baroAltitude, 0.0f, ALTITUDE_CEILING) / ALTITUDE_CEILING;
    const float hue = t * 270.0f;

    const float c = 1.0f;
    const float x = c * (1.0f - std::abs(fmod(hue / 60.0f, 2.0f) - 1.0f));

    float r, g, b;
    if (hue < 60)       { r = c; g = x; b = 0; }
    else if (hue < 120)  { r = x; g = c; b = 0; }
    else if (hue < 180)  { r = 0; g = c; b = x; }
    else if (hue < 240)  { r = 0; g = x; b = c; }
    else                 { r = x; g = 0; b = c; }

    return lgfx::color888((uint8_t)(r * 255), (uint8_t)(g * 255), (uint8_t)(b * 255));
}

inline void DrawScanLines(LGFX_Sprite& buf, const int x0, const int y0, const int x1, const int y1, const int thickness, const int trailBrightness, const int spacing)
{
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len = sqrt(dx * dx + dy * dy);

    // perpendicular unit vector
    float px = -dy / len;
    float py = dx / len;

    for (int i = 0; i <= thickness; i++) {
        // 1.0 at centre, 0.0 at edges
        float t = i / (float)(thickness);
        uint8_t brightness = (uint8_t)(t * trailBrightness);

        buf.drawLine(
            x0, y0,
            x1 + (px * (i * spacing)), y1 + (py * (i * spacing)),
            lgfx::color888(0, brightness, 0)
        );
    }

    buf.drawLine(
        x0, y0,
        x1 + (px * (thickness * spacing)), y1 + (py * (thickness * spacing)),
        lgfx::color888(0, 200, 0)
    );
}