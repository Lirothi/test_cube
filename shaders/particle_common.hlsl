// Part E (GPU particles): shared declarations for the spawn/update compute shaders and the
// E2 billboard VS/PS. Mirrors vfx::GpuParticle / vfx::GpuEmitterParams (ParticleTypes.h).
#ifndef PARTICLE_COMMON_HLSL
#define PARTICLE_COMMON_HLSL

// Slot-array + dead-list scheme: particles[] never compacts; age < 0 marks a dead slot (the
// render VS emits degenerate quads for them, so no indirect draw / alive-list is needed).
struct Particle
{
    float3 pos; float age;
    float3 vel; float life;
    float rot; float spin; uint seed; float _pad;
};

cbuffer EmitterParams : register(b1)
{
    float3 emitterPos;  float dt;
    float3 coneDir;     float coneHalfAngleRad;
    float lifeMin;      float lifeMax;  float speedMin; float speedMax;
    float gravity;      float drag;     float rotMin;   float rotMax;
    float spinMin;      float spinMax;  uint maxParticles; uint frameSeed;
};

// Both CS share one root signature: b0 = spawn-batch constants (unused by update), b1 = emitter
// CB, u0..u2 = particles / dead list / dead counter.
#define PARTICLE_CS_RS \
    "RootConstants(num32BitConstants=4, b0)," \
    "CBV(b1)," \
    "DescriptorTable(UAV(u0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

RWStructuredBuffer<Particle> gParticles : register(u0);
RWStructuredBuffer<uint>     gDeadList  : register(u1); // stack of dead slot indices
RWStructuredBuffer<uint>     gDeadCount : register(u2); // [0] = stack size (alive = max - this)

// PCG-style integer hash; float01 maps to [0,1).
inline uint Pcg(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
inline float Rand01(inout uint h) { h = Pcg(h); return float(h) * (1.0 / 4294967296.0); }
inline float RandRange(inout uint h, float lo, float hi) { return lerp(lo, hi, Rand01(h)); }

// Uniform direction inside a cone around `axis` (half-angle in radians).
inline float3 RandCone(inout uint h, float3 axis, float halfAngle)
{
    float cosMax = cos(halfAngle);
    float cosT = lerp(cosMax, 1.0, Rand01(h)); // uniform in solid angle
    float sinT = sqrt(saturate(1.0 - cosT * cosT));
    float phi = Rand01(h) * 6.2831853;

    // Orthonormal basis around the axis.
    float3 up = abs(axis.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 t = normalize(cross(up, axis));
    float3 b = cross(axis, t);
    return normalize(t * (sinT * cos(phi)) + b * (sinT * sin(phi)) + axis * cosT);
}

#endif // PARTICLE_COMMON_HLSL
