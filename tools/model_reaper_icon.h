#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// The watchdog's notification-area glyph, as pixels and nothing else.
//
// Kept apart from the Win32 plumbing in model_reaper_tray.cpp for one reason: the code that
// paints the tray must also be runnable on its own, so the icon can be rendered to a file and
// LOOKED AT. The only other way to see it is to open the taskbar's overflow flyout and click
// through someone's desktop, which is not a verification anyone should need.
//
// A letter rather than the dot it replaced, because a coloured dot says "something is up" and
// nothing about what -- a W next to the clock says which program is holding sixteen gigabytes of
// video memory. The COLOUR still carries the state; that part did its job and stays.
namespace reapericon
{
    // Distance from a point to a line segment. Taking the minimum of these over the strokes of a
    // letter is an outline font in four lines: the ends and the joins come out round without
    // being drawn, which at 16 pixels is the difference between a W and a smudge.
    inline float SegmentDistance(float px, float py, float ax, float ay, float bx, float by)
    {
        const float vx = bx - ax;
        const float vy = by - ay;
        const float wx = px - ax;
        const float wy = py - ay;
        const float lengthSq = vx * vx + vy * vy;
        const float t = lengthSq > 0.0f
            ? std::clamp((wx * vx + wy * vy) / lengthSq, 0.0f, 1.0f) : 0.0f;
        const float dx = wx - vx * t;
        const float dy = wy - vy * t;
        return std::sqrt(dx * dx + dy * dy);
    }

    // Paints a W filling `size` x `size` pixels into `bgra` -- premultiplied, top row first,
    // which is what a 32-bit top-down DIB section wants and what CreateIconIndirect reads.
    //
    // The letter is the colour; round it runs a thin edge in the same hue at a third of the
    // brightness, so it reads on a light taskbar as well as a dark one without looking like a
    // cartoon with a black outline.
    inline void PaintW(int size, std::uint8_t red, std::uint8_t green, std::uint8_t blue,
        std::uint8_t* bgra)
    {
        const float s = static_cast<float>(size);
        // Stroke and edge in proportion to the icon, with floors so the smallest size still
        // gets whole pixels of each. Tuned by eye at 16, 20, 24 and 32 against both taskbars.
        const float halfStroke = std::max(1.05f, s * 0.078f);
        const float edge = std::max(0.6f, s * 0.042f);
        const float pad = halfStroke + edge + 0.2f;

        // A W is wider than it is tall. The middle apex stops short of the top, which is what
        // keeps it reading as one letter rather than as two V's side by side.
        const float left = pad;
        const float right = s - pad;
        // Nearly the full height: a W inset by 7 % looked like a small letter beside tray icons
        // that fill their square, and at 16 pixels every row it gives back is legibility.
        const float top = pad + s * 0.025f;
        const float bottom = s - pad - s * 0.025f;
        const float width = right - left;
        const float height = bottom - top;
        const float xs[5] = { left, left + width * 0.25f, left + width * 0.5f,
                              left + width * 0.75f, right };
        const float ys[5] = { top, bottom, top + height * 0.34f, bottom, top };

        const int samples = 4;   // 4x4 per pixel, as the dot had: enough that no edge is sawn
        const float total = static_cast<float>(samples * samples);
        for (int y = 0; y < size; ++y)
        {
            for (int x = 0; x < size; ++x)
            {
                // Summed over ALL subsamples and divided by their count, so the result is
                // premultiplied by construction.
                float b = 0.0f;
                float g = 0.0f;
                float r = 0.0f;
                float a = 0.0f;
                for (int sy = 0; sy < samples; ++sy)
                {
                    for (int sx = 0; sx < samples; ++sx)
                    {
                        const float px = x + (sx + 0.5f) / samples;
                        const float py = y + (sy + 0.5f) / samples;
                        float distance = 1e9f;
                        for (int i = 0; i < 4; ++i)
                        {
                            distance = std::min(distance,
                                SegmentDistance(px, py, xs[i], ys[i], xs[i + 1], ys[i + 1]));
                        }
                        if (distance > halfStroke + edge)
                        {
                            continue;
                        }
                        const float shade = distance > halfStroke ? 0.34f : 1.0f;
                        b += blue * shade;
                        g += green * shade;
                        r += red * shade;
                        a += 255.0f;
                    }
                }
                std::uint8_t* pixel = bgra + (static_cast<std::size_t>(y) * size + x) * 4;
                pixel[0] = static_cast<std::uint8_t>(b / total);
                pixel[1] = static_cast<std::uint8_t>(g / total);
                pixel[2] = static_cast<std::uint8_t>(r / total);
                pixel[3] = static_cast<std::uint8_t>(a / total);
            }
        }
    }
}
