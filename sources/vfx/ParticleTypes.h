#pragma once
#include <cstdint>
#include <string>

#include "core/math/Math.h"

namespace vfx
{
// Debug: freeze every particle sim (Tick accumulation and GPU dispatches stop; state persists).
inline bool g_freeze = false;

// Debug: throttled alive-count readback log to DBWIN ("[vfx] ..."), ~1 line/sec per emitter.
// Cheap (4-byte copy per frame into a readback ring); the E1 verification instrument.
inline bool g_debugAliveLog = true;

// GPU particle state — mirrors `Particle` in shaders/particle_common.hlsl (structured buffer,
// 48 bytes). age < 0 marks a dead slot (the render VS emits degenerate quads for those — E2).
struct alignas(16) GpuParticle
{
    float pos[3]; float age;
    float vel[3]; float life;
    float rot; float spin; uint32_t seed; float _pad;
};
static_assert(sizeof(GpuParticle) == 48, "GpuParticle must match the HLSL struct (48 bytes)");

// Per-emitter constants — mirrors `EmitterParams` (b1) in shaders/particle_common.hlsl.
// Uploaded per frame via AllocDynamic (editor tweaks are a CB refill, no buffer rebuilds).
struct alignas(16) GpuEmitterParams
{
    float emitterPos[3]; float dt;          // 0
    float coneDir[3];    float coneHalfAngleRad; // 16
    float lifeMin, lifeMax, speedMin, speedMax;  // 32
    float gravity, drag, rotMin, rotMax;         // 48
    float spinMin, spinMax; uint32_t maxParticles; uint32_t frameSeed; // 64
};
static_assert(sizeof(GpuEmitterParams) == 80, "GpuEmitterParams must match the HLSL cbuffer");

// Per-emitter draw constants — mirrors `DrawParams` (b2) in shaders/particles.hlsl (E2).
struct alignas(16) GpuEmitterDrawParams
{
    float sizeStart; float sizeEnd; uint32_t flipCols; uint32_t flipRows;         // 0
    float flipFps; uint32_t flipRandomStart; uint32_t frameBlend; uint32_t hasTexture; // 16
    float colorKeys[4][4];                                                        // 32
    // depthOcclude: 1 = occlude/soft-fade against the opaque depth copy in the PS; transparent
    // surfaces are handled by the hardware depth test. softFadeDist = fade width.
    uint32_t maxParticles; float softFadeDist; float depthOcclude; float _pad;    // 96
};
static_assert(sizeof(GpuEmitterDrawParams) == 112, "GpuEmitterDrawParams must match HLSL");

// CPU-side emitter description. E1 consumes the sim fields; size/color/flipbook/blend/texture
// are stored now (one JSON schema) and consumed by the E2 renderer / E3 presets.
struct EmitterDesc
{
    // --- sim (E1) ---
    uint32_t maxParticles = 1024;      // slot-array capacity (also the sort ceiling, E2c)
    float spawnRate = 100.0f;          // particles / second
    float lifetimeMin = 0.6f;
    float lifetimeMax = 1.2f;
    Math::float3 coneDir = Math::float3(0.0f, 1.0f, 0.0f); // local emission axis
    float coneAngleDeg = 25.0f;        // full-angle of the emission cone
    float speedMin = 1.0f;
    float speedMax = 2.0f;
    float gravity = -2.5f;             // downward accel; NEGATIVE = buoyancy (fire rises)
    float drag = 0.0f;                 // 1/s velocity damping
    float rotMin = 0.0f;               // initial billboard roll, radians
    float rotMax = 6.2831853f;
    float spinMin = -1.0f;             // roll speed, rad/s
    float spinMax = 1.0f;
    uint32_t seed = 1u;                // emitter RNG stream

    // --- rendering (consumed by E2/E3; carried here so the JSON schema is stable) ---
    float sizeStart = 0.5f;
    float sizeEnd = 0.15f;
    Math::float4 colorKeys[4] = {      // colorOverLife gradient (RGBA; A drives fade)
        Math::float4(1.0f, 1.0f, 1.0f, 1.0f),
        Math::float4(1.0f, 1.0f, 1.0f, 1.0f),
        Math::float4(1.0f, 1.0f, 1.0f, 1.0f),
        Math::float4(1.0f, 1.0f, 1.0f, 0.0f),
    };
    uint32_t flipbookCols = 1;
    uint32_t flipbookRows = 1;
    float flipbookFps = 0.0f;
    bool flipbookRandomStart = false;
    bool frameBlend = false;
    bool additive = true;              // blendMode: additive | (premultiplied) alpha
    float softFade = 0.3f;             // E2b depth-fade distance in world units (0 disables)
    bool sortParticles = false;        // back-to-front within the emitter (alpha smoke, E2c)
    bool localSpace = false;           // particles follow the emitter transform
    std::string texture;               // flipbook/sprite atlas path
};
} // namespace vfx
