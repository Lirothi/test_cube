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
    // A falling particle under the kill plane (a spray drop back in the sea) is done.
    if (p.age >= p.life || (p.pos.y < killBelowY && p.vel.y < 0.0))
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
    // EmitterDesc::collideHulls: a particle in or over a hull is spent on it (spray thrown at a boat, a boat turning into
    // the drops it threw; InsideHull). Tested where it now is: it is never drawn there. Each one is counted once, its
    // first time in (_pad marks it), into gDeadCount[1] -- the same count whether it dies there or not (the A/B). Not in its
    // first kHullGrace: a spray's root lies on the hull's skin (the waterline's raster, up to a cell inside it), and taken
    // at once a fifth of a straight run's drops died where they were born.
    static const float kHullGrace = 0.08;
    [branch] if (hullCount > 0u && p.age > kHullGrace && InsideHull(p.pos))
    {
        if (asuint(p._pad) == 0u)
        {
            InterlockedAdd(gDeadCount[1], 1u);
            p._pad = asfloat(1u);
        }
        if (hullKill != 0u)
        {
            p.age = -1.0;
            gParticles[slot] = p;
            uint prev;
            InterlockedAdd(gDeadCount[0], 1u, prev);
            gDeadList[prev] = slot;
            return;
        }
    }
    gParticles[slot] = p;
}
