#include "rendering/lighting/NightLamps.h"

#include <algorithm>
#include <cmath>

namespace
{
    std::vector<GBufferRenderable*>& Registry()
    {
        static std::vector<GBufferRenderable*> objects;
        return objects;
    }

    std::uint32_t Hash(std::uint32_t v)
    {
        v ^= v >> 16; v *= 0x7feb352du;
        v ^= v >> 15; v *= 0x846ca68bu;
        v ^= v >> 16;
        return v;
    }

    float Lattice(int x, int y, int z)
    {
        const std::uint32_t h = Hash(static_cast<std::uint32_t>(x) * 0x8da6b343u ^ static_cast<std::uint32_t>(y) * 0xd8163841u ^
                                     static_cast<std::uint32_t>(z) * 0xcb1ab31fu);
        return static_cast<float>(h & 0x00ffffffu) / 16777215.0f;
    }

    float Smooth(float t) { return t * t * (3.0f - 2.0f * t); }

    // Value noise in [0, 1], trilinear over a unit lattice with smoothstep weights: continuous, so two things a
    // few decimetres apart read almost the same value.
    float ValueNoise3(float x, float y, float z)
    {
        const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
        const int ix = static_cast<int>(fx), iy = static_cast<int>(fy), iz = static_cast<int>(fz);
        const float tx = Smooth(x - fx), ty = Smooth(y - fy), tz = Smooth(z - fz);
        float c[2][2];
        for (int dz = 0; dz < 2; ++dz)
        {
            for (int dy = 0; dy < 2; ++dy)
            {
                const float a = Lattice(ix, iy + dy, iz + dz), b = Lattice(ix + 1, iy + dy, iz + dz);
                c[dz][dy] = a + (b - a) * tx;
            }
        }
        const float front = c[0][0] + (c[0][1] - c[0][0]) * ty, back = c[1][0] + (c[1][1] - c[1][0]) * ty;
        return front + (back - front) * tz;
    }

    // 1-D value noise in [0, 1] (UDS's warm-up flicker is a PerlinNoise1D of time).
    float ValueNoise1(float x)
    {
        const float f = std::floor(x);
        const int i = static_cast<int>(f);
        const float a = Lattice(i, 0x51, 0x7c), b = Lattice(i + 1, 0x51, 0x7c);
        return a + (b - a) * Smooth(x - f);
    }

    constexpr float kDelayCellMetres = 10.0f; // a street's lamps 20 m apart differ; a lamp and its bulb do not
    constexpr float kFlickerHz = 12.0f;
}

LampSwitch LampSwitchFromName(std::string_view name)
{
    const auto is = [&name](std::string_view word) {
        if (name.size() != word.size()) { return false; }
        for (std::size_t i = 0; i < word.size(); ++i)
        {
            const char c = name[i] >= 'A' && name[i] <= 'Z' ? static_cast<char>(name[i] - 'A' + 'a') : name[i];
            if (c != word[i]) { return false; }
        }
        return true;
    };
    if (is("night")) { return LampSwitch::Night; }
    if (is("day")) { return LampSwitch::Day; }
    return LampSwitch::Always;
}

const char* LampSwitchName(LampSwitch lampSwitch)
{
    switch (lampSwitch)
    {
    case LampSwitch::Night: return "night";
    case LampSwitch::Day: return "day";
    case LampSwitch::Always: break;
    }
    return "always";
}

bool NightLamps::Tick(double deltaSeconds, float sunElevationDeg, bool clockSetByHand, const Params& params)
{
    params_ = params;
    clock_ += std::max(deltaSeconds, 0.0);
    const bool night = sunElevationDeg < params.sunElevationDeg;
    if (started_ && night == night_) { return false; }
    // The first frame, and any clock set by hand, settle at once: a level opens with its lamps already as the time
    // says, and dragging the time slider does not start a warm-up at every frame.
    switchedAt_ = (!started_ || clockSetByHand) ? -1.0e30 : clock_;
    night_ = night;
    started_ = true;
    return true;
}

float NightLamps::Level(LampSwitch lampSwitch, const Math::float3& position) const
{
    if (lampSwitch == LampSwitch::Always) { return 1.0f; }
    const bool on = (lampSwitch == LampSwitch::Night) == night_;
    const float delay = std::max(params_.randomDelaySec, 0.0f) *
                        ValueNoise3(position.x / kDelayCellMetres, position.y / kDelayCellMetres, position.z / kDelayCellMetres);
    const double t = clock_ - switchedAt_ - delay;
    if (!on) { return t < 0.0 ? 1.0f : 0.0f; }
    if (t < 0.0) { return 0.0f; }
    if (params_.turnOnSec <= 0.0f || t >= params_.turnOnSec) { return 1.0f; }
    const float a = static_cast<float>(t / params_.turnOnSec);
    float level = std::pow(a, std::max(params_.turnOnExponent, 0.01f));
    if (params_.turnOnFlicker > 0.0f)
    {
        // A function of the time since THIS item came on: a lamp and its bulb, starting together, flicker together.
        level *= 1.0f - std::min(params_.turnOnFlicker, 1.0f) * ValueNoise1(static_cast<float>(t) * kFlickerHz) * (1.0f - a);
    }
    return std::clamp(level, 0.0f, 1.0f);
}

void NightLamps::Register(GBufferRenderable* object)
{
    std::vector<GBufferRenderable*>& objects = Registry();
    if (object && std::find(objects.begin(), objects.end(), object) == objects.end()) { objects.push_back(object); }
}

void NightLamps::Unregister(GBufferRenderable* object)
{
    std::vector<GBufferRenderable*>& objects = Registry();
    objects.erase(std::remove(objects.begin(), objects.end(), object), objects.end());
}

const std::vector<GBufferRenderable*>& NightLamps::Registered()
{
    return Registry();
}
