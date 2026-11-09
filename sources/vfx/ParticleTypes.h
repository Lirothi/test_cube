#pragma once
#include <cstdint>
#include <string>

#include "core/math/Math.h"
#include "core/threading/Knob.h"

namespace vfx
{
// Debug: freeze every particle sim (Tick accumulation and GPU dispatches stop; state persists).
inline bool g_freeze = false;

// A/B, transient (`--set particles.airFromFog:1`): a lit particle takes the air's light from the height fog's colour, as
// before 2026-11-09, instead of the sky's irradiance cube (DrawParams.skyIrradianceScale).
inline bool g_litAirFromFog = false;
inline bool g_litAirFromFog_game = false;
inline const bool g_litAirFromFog_knob = ::knob::Register(g_litAirFromFog_game, g_litAirFromFog, "vfx::g_litAirFromFog");

// Debug: throttled alive-count readback log to DBWIN ("[vfx] ..."), ~1 line/sec per emitter.
// Cheap (4-byte copy per frame into a readback ring); the E1 verification instrument.
inline bool g_debugAliveLog = true;

// GPU particle state — mirrors `Particle` in shaders/particle_common.hlsli (structured buffer,
// 48 bytes). age < 0 marks a dead slot (the render VS emits degenerate quads for those — E2).
struct alignas(16) GpuParticle
{
    float pos[3]; float age;
    float vel[3]; float life;
    float rot; float spin; uint32_t seed; float _pad;
};
static_assert(sizeof(GpuParticle) == 48, "GpuParticle must match the HLSL struct (48 bytes)");

// Per-emitter constants — mirrors `EmitterParams` (b1) in shaders/particle_common.hlsli.
// Uploaded per frame via AllocDynamic (editor tweaks are a CB refill, no buffer rebuilds).
struct alignas(16) GpuEmitterParams
{
    float emitterPos[3]; float dt;          // 0
    float coneDir[3];    float coneHalfAngleRad; // 16
    float lifeMin, lifeMax, speedMin, speedMax;  // 32
    float gravity, drag, rotMin, rotMax;         // 48
    float spinMin, spinMax; uint32_t maxParticles; uint32_t frameSeed; // 64
    // W8 wind drift. windAccelXZ is ALREADY the finished acceleration in m/s^2 (the emitter folds
    // its own windInfluence into the scene's gust-modulated wind push), so the CS just integrates it
    // — no wind parameters or per-emitter authoring leak into the shader.
    // killBelowY: a FALLING particle under it dies (a spray drop back in the sea); -1e30 = never.
    float windAccelXZ[2]; float killBelowY; float _padW;   // 80
    // A segment emitter: particles are born anywhere on emitterPos .. emitterPos + lineVec (0 = a point).
    float lineVec[3]; float _padL;                         // 96
    // The emitter's own motion (m/s): a particle born a fraction u into the frame starts where the emitter was then
    // and has already flown the rest of it -- a fast emitter lays a sheet, not one clump a frame.
    float emitterVel[3]; float _padE;                      // 112
    // Added to every particle's birth velocity (m/s, world): what the carrier gives it (a hull's way).
    float baseVel[3]; float _padB;                         // 128
};
static_assert(sizeof(GpuEmitterParams) == 144, "GpuEmitterParams must match the HLSL cbuffer");

// Per-emitter draw constants — mirrors `DrawParams` (b2) in shaders/particles.hlsl (E2).
struct alignas(16) GpuEmitterDrawParams
{
    float sizeStart; float sizeEnd; uint32_t flipCols; uint32_t flipRows;         // 0
    float flipFps; uint32_t flipRandomStart; uint32_t frameBlend; uint32_t hasTexture; // 16
    float colorKeys[4][4];                                                        // 32
    // depthOcclude: 1 = occlude/soft-fade against the opaque depth copy in the PS; transparent
    // surfaces are handled by the hardware depth test. softFadeDist = fade width.
    // P16.1: preExposure -- particles draw in the transparent pass, after compose, so they
    // apply the factor themselves. It took the trailing pad slot, so the layout is unchanged.
    uint32_t maxParticles; float softFadeDist; float depthOcclude; float preExposure; // 96
    // P16.7: the luminance the authored colour MEANS, in cd/m2. See EmitterDesc.
    // lit: 1 = the colour is an ALBEDO lit by the frame's light and the air (EmitterDesc::lit), 0 = luminanceCdM2.
    float luminanceCdM2; float lit; float forwardScatter; float phaseG;           // 112
    // skyIrradianceScale: lit, the scale of the sky's irradiance cube (t5) -- the air's light on a drop, as lighting_cs
    // fills an opaque surface; 0 = no cube (or the A/B): the height fog's colour then.
    // litFlat: lit, the shading normal from the sprite's sphere (0) to the world's up (1, the water foam's light).
    float velocityStretch; float skyIrradianceScale; float litFlat; float _pad;   // 128
};
static_assert(sizeof(GpuEmitterDrawParams) == 144, "GpuEmitterDrawParams must match HLSL");

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
    // W8: horizontal acceleration from the global wind, in m/s^2 at wind strength 1. Per emitter
    // because the engine has no particle mass: smoke and embers should be carried, sparks and debris
    // should not, and only the author knows which this is. Default 0 keeps every existing emitter
    // byte-identical, so enabling wind on a level cannot silently disturb its effects.
    float windInfluence = 0.0f;
    // A segment emitter (world metres from the emitter's position to the other end; 0 = a point).
    Math::float3 lineOffset = Math::float3(0.0f, 0.0f, 0.0f);
    // A falling particle under this height dies (spray back in the sea); the default never.
    float killBelowY = -1.0e30f;
    // Runtime only (a carrier sets them every frame, VesselSpray): the emitter's own motion and what it adds to a
    // particle's birth velocity, m/s world.
    Math::float3 emitterVelocity = Math::float3(0.0f, 0.0f, 0.0f);
    Math::float3 baseVelocity = Math::float3(0.0f, 0.0f, 0.0f);
    float rotMin = 0.0f;               // initial billboard roll, radians
    float rotMax = 6.2831853f;
    float spinMin = -1.0f;             // roll speed, rad/s
    float spinMax = 1.0f;
    uint32_t seed = 1u;                // emitter RNG stream

    // --- rendering (consumed by E2/E3; carried here so the JSON schema is stable) ---
    float sizeStart = 0.5f;
    float sizeEnd = 0.15f;
    // P16.7 -- WHAT THE COLOUR MEANS, in cd/m2. `colorKeys` is a HUE and an alpha; this is the
    // brightness, and it has to be a real number for the same reason the sun does: a particle
    // writes straight into scene colour, and scene colour is in cd/m2 since P16. An authored 1.0
    // used to be "about as bright as the scene"; against a physical sky it is a thousandth of it,
    // which is why the campfire's smoke went BLACK -- an alpha-blended particle darker than what
    // is behind it subtracts from the frame.
    //
    // 3000 = smoke lit by a bright overcast sky, which is what the existing presets were drawing.
    // A wood fire is 20000-50000, a candle flame about 10000, a glowing ember a few hundred. The
    // reference is the same one the sky uses, so a flame set to 20000 reads as a flame against a
    // 16000 lx sky and still reads as one at night.
    float luminanceCdM2 = 3000.0f;
    // LIT particles (water spray, dust): the colour keys are an ALBEDO lit by the frame's directional light and the sky's
    // fill (the irradiance cube lighting_cs reads), so they follow the hour and go dark at night; luminanceCdM2 is not
    // used. The sun's part is the water foam's wrapped Lambert (0.2 + 0.8 N.L, LitFoamColor), (1 - forwardScatter) of it,
    // plus forwardScatter x Henyey-Greenstein(phaseG): water drops scatter forward, spray against the sun glows.
    // litFlat: the shading normal, from the sphere the sprite stands for (0) to the world's up (1) -- lit as a level
    // sheet of foam on the water, the wake's own light, so spray and the white water it falls into are one white.
    bool lit = false;
    float forwardScatter = 0.5f;
    float phaseG = 0.7f;
    float litFlat = 0.0f;
    // Stretched along the velocity seen across the view: the quad is size + |v| x this (seconds) long, no roll
    // (0 = a round billboard, as before). A fast drop reads as a streak.
    float velocityStretch = 0.0f;
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
