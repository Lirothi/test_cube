// Part E1: spawn N particles by popping slots off the dead-list stack. N comes from the CPU's
// fractional spawnRate*dt accumulator (root constants b0). All randomness is a GPU hash of
// (slot, frameSeed, thread) — no CPU-side RNG state.
#include "particle_common.hlsli"

cbuffer SpawnBatch : register(b0)
{
    uint spawnCount;
    uint spawnSeed; // frame-varying (mixed with the emitter seed on the CPU)
    uint2 _pad;
};

[numthreads(64, 1, 1)]
[RootSignature(PARTICLE_CS_RS)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= spawnCount) { return; }

    // Pop a dead slot; when the pool is exhausted, undo and drop the spawn.
    uint prev;
    InterlockedAdd(gDeadCount[0], uint(-1), prev);
    if (prev == 0u || prev > maxParticles) // unsigned underflow guard
    {
        InterlockedAdd(gDeadCount[0], 1u);
        return;
    }
    const uint slot = gDeadList[prev - 1u];

    uint h = Pcg(slot * 0x9E3779B9u ^ spawnSeed ^ (dtid.x * 0x85EBCA6Bu));

    Particle p;
    p.vel = RandCone(h, normalize(coneDir), coneHalfAngleRad) * RandRange(h, speedMin, speedMax) + baseVel;
    p.life = max(RandRange(h, lifeMin, lifeMax), 1e-3);
    // Anywhere on the segment, and born a share of the frame ago: where the emitter was then, flown since -- a fast
    // emitter lays its particles along its path instead of one clump a frame.
    const float along = Rand01(h);
    const float before = Rand01(h) * dt;
    p.pos = emitterPos + lineVec * along - emitterVel * before + p.vel * before;
    p.age = min(before, 0.999 * p.life);
    p.rot = RandRange(h, rotMin, rotMax);
    p.spin = RandRange(h, spinMin, spinMax);
    p.seed = h;
    p._pad = 0.0;
    gParticles[slot] = p;
}
