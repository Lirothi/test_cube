// Part E1: per-slot integrate + kill. Runs BEFORE spawn each frame (freshly spawned particles
// render at age 0). Dead slots return their index to the dead-list stack.
#include "particle_common.hlsli"

[numthreads(64, 1, 1)]
[RootSignature(PARTICLE_CS_RS)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    const uint slot = dtid.x;
    if (slot >= maxParticles) { return; }

    Particle p = gParticles[slot];
    if (p.age < 0.0) { return; } // dead slot

    p.age += dt;
    if (p.age >= p.life)
    {
        p.age = -1.0;
        gParticles[slot] = p;
        uint prev;
        InterlockedAdd(gDeadCount[0], 1u, prev);
        gDeadList[prev] = slot;
        return;
    }

    p.vel += float3(0.0, -gravity, 0.0) * dt;   // gravity > 0 falls; < 0 = buoyancy (fire)
    // W8: wind pushes horizontally only. Applied as an ACCELERATION, before drag, so drag still
    // bounds the terminal speed — smoke leans into a steady drift instead of being teleported, and a
    // gust visibly leans it further because the envelope is already folded into windAccelXZ.
    p.vel.xz += windAccelXZ * dt;
    p.vel *= max(0.0, 1.0 - drag * dt);
    p.pos += p.vel * dt;
    p.rot += p.spin * dt;
    gParticles[slot] = p;
}
