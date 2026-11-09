#include "vfx/VesselSpray.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include "app/scene/Scene.h"
#include "core/logging/Log.h"
#include "ocean/BuoyancyLayout.h"
#include "physics/PhysicsWorld.h"
#include "rendering/core/UploadBatch.h"
#include "vfx/ParticleEmitterObject.h"
#include "vfx/ParticlePresets.h"

namespace vfx
{
namespace
{
    constexpr const char* kSheetPreset = "data/particles/boat_spray_sheet.json";
    constexpr const char* kMistPreset = "data/particles/boat_spray_mist.json";
    // Below the first the hull pushes the water aside without throwing it (the bow wave's curl); from the second on the
    // spray is whole -- a planing hull's.
    constexpr float kSpraySpeedOn = 3.0f;
    constexpr float kSpraySpeedFull = 12.0f;
    // Where along the waterline the spray leaves: a share of its length aft of the stem (the forward part, where the
    // spray root of a planing hull meets the chine); and where the side ends, the same share forward of the stern.
    constexpr float kRootFore = 0.05f;
    constexpr float kRootAft = 0.35f;
    constexpr float kSideAft = 0.05f;
    // The spray by the hull's immersion: none under the first (in the air), whole from the second (smoothstep).
    constexpr float kSprayWetFrom = 0.02f;
    constexpr float kSprayWetFull = 0.25f;
    // A slam into a wave (VesselState::impact, a share of the weight) over this throws a burst; drops (clumps) a side and
    // mist puffs a side per unit of it, thrown wider and higher than the running spray.
    constexpr float kSlamOn = 0.15f;
    constexpr float kSlamSheet = 250.0f;
    constexpr float kSlamMist = 30.0f;
    // The white water along a planing hull's sides (the bow wave breaking, the spray falling back beside it): a line a
    // side from the spray's root to the stern, this far outside the waterline (m), moving with the hull -- so it lays its
    // foam where the hull went, on a turn too. A texel the line passes lies under it for length / speed seconds; at
    // `rate` = kSideFoam x sideFoam x speed / length the texel on the line takes kSideFoam x sideFoam of foam (the wake's
    // units: 1 and over = white), a third of that on average across the line's width (OceanWake's kSplashHalfWidth).
    constexpr float kSideFoamOut = 0.3f;
    constexpr float kSideFoam = 2.0f;

    Math::float3 Normalized(const Math::float3& v)
    {
        const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        return len > 1.0e-6f ? v * (1.0f / len) : Math::float3(0.0f, 1.0f, 0.0f);
    }

    Math::float3 CrossOf(const Math::float3& a, const Math::float3& b)
    {
        return Math::float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
    }
}

VesselSpray::Root VesselSpray::FindRoot(const buoyancy::HullFootprint& footprint, int side)
{
    Root root;
    const int along = footprint.along;
    const int across = footprint.across;
    if (!footprint.Valid() || along <= 1 || across <= 1) { return root; }
    auto wet = [&](int column, int row) { return footprint.depth[static_cast<size_t>(row) * along + column] > 0.0f; };
    int stem = -1;
    int stern = along;
    for (int column = 0; column < along; ++column)
    {
        for (int row = 0; row < across; ++row)
        {
            if (wet(column, row))
            {
                stem = std::max(stem, column);
                stern = std::min(stern, column);
                break;
            }
        }
    }
    if (stem < 0 || stem <= stern) { return root; }
    const int length = stem - stern + 1;
    // The outer edge of the wet cells at a column, on this side (port +v, starboard -v); false if the column is dry.
    auto edge = [&](int column, float& v) {
        if (side == 0)
        {
            for (int row = across - 1; row >= 0; --row)
            {
                if (wet(column, row)) { v = footprint.v0 + static_cast<float>(row + 1) * footprint.cellV; return true; }
            }
        }
        else
        {
            for (int row = 0; row < across; ++row)
            {
                if (wet(column, row)) { v = footprint.v0 + static_cast<float>(row) * footprint.cellV; return true; }
            }
        }
        return false;
    };
    const int fore = std::clamp(stem - static_cast<int>(std::lround(kRootFore * length)), stern, stem);
    const int aft = std::clamp(stem - static_cast<int>(std::lround(kRootAft * length)), stern, stem);
    const int end = std::clamp(stern + static_cast<int>(std::lround(kSideAft * length)), stern, aft);
    if (!edge(fore, root.v0) || !edge(aft, root.v1) || !edge(end, root.v2)) { return root; }
    root.u0 = footprint.u0 + (static_cast<float>(fore) + 0.5f) * footprint.cellU;
    root.u1 = footprint.u0 + (static_cast<float>(aft) + 0.5f) * footprint.cellU;
    root.u2 = footprint.u0 + (static_cast<float>(end) + 0.5f) * footprint.cellU;
    root.valid = true;
    return root;
}

bool VesselSpray::Create(Scene& scene, Renderer* renderer, Set& set)
{
    if (!renderer) { return false; }
    UploadBatch uploads;
    if (!uploads.Begin(renderer)) { return false; }
    bool ok = true;
    auto make = [&](const char* preset, uint32_t seed) -> ParticleEmitterObject* {
        nlohmann::json j = nlohmann::json::object();
        j["preset"] = preset;
        EmitterDesc desc = ResolveEmitterDesc(j);
        desc.seed = seed;
        desc.spawnRate = 0.0f;
        auto emitter = std::make_unique<ParticleEmitterObject>(desc);
        ParticleEmitterObject* raw = emitter.get();
        if (!scene.AddInitializedObject(*renderer, uploads, std::move(emitter))) { ok = false; return nullptr; }
        return raw;
    };
    const uint32_t base = static_cast<uint32_t>(sets_.size()) * 16u + 7u;
    for (int side = 0; side < 2; ++side)
    {
        set.sheet[side] = make(kSheetPreset, base + static_cast<uint32_t>(side));
        set.mist[side] = make(kMistPreset, base + 2u + static_cast<uint32_t>(side));
    }
    uploads.SubmitAndWait(renderer);
    for (int k = 0; k < 4; ++k)
    {
        set.sheetAlpha[k] = set.sheet[0] ? set.sheet[0]->Desc().colorKeys[k].w : 0.0f;
        set.mistAlpha[k] = set.mist[0] ? set.mist[0]->Desc().colorKeys[k].w : 0.0f;
    }
    LOG_INFO(logging::LogCategory::Vfx, "vessel spray: emitters for boat {} ({})", sets_.size(), ok ? "made" : "FAILED");
    return ok;
}

bool VesselSpray::SideFoamOf(const void* vessel, Math::float4 (&lines)[2], float& rate) const
{
    for (const Set& set : sets_)
    {
        if (set.vessel != vessel) { continue; }
        lines[0] = set.foamLine[0];
        lines[1] = set.foamLine[1];
        rate = set.foamRate;
        return rate > 0.0f;
    }
    rate = 0.0f;
    return false;
}

void VesselSpray::Sync(Scene& scene, Renderer* renderer, const std::vector<physics::VesselHull>& hulls, const SprayParams& params)
{
    sheetAlive_ = sheetCapacity_ = mistAlive_ = mistCapacity_ = 0u;
    hullEntries_ = 0u;
    for (Set& set : sets_)
    {
        set.seen = false;
        for (int side = 0; side < 2; ++side)
        {
            if (set.sheet[side]) { sheetAlive_ += set.sheet[side]->AliveCount(); sheetCapacity_ += set.sheet[side]->Desc().maxParticles; }
            if (set.mist[side]) { mistAlive_ += set.mist[side]->AliveCount(); mistCapacity_ += set.mist[side]->Desc().maxParticles; }
            if (set.sheet[side]) { hullEntries_ += set.sheet[side]->HullEntries(); }
            if (set.mist[side]) { hullEntries_ += set.mist[side]->HullEntries(); }
        }
    }
    rate_ = 0.0f;
    sheetRate_ = 0.0f;
    for (const physics::VesselHull& hull : hulls)
    {
        if (!hull.object || !hull.settings) { continue; }
        auto it = std::find_if(sets_.begin(), sets_.end(), [&](const Set& s) { return s.vessel == hull.object; });
        if (it == sets_.end())
        {
            Set set;
            set.vessel = hull.object;
            if (!Create(scene, renderer, set)) { continue; }
            sets_.push_back(set);
            it = sets_.end() - 1;
        }
        Set& set = *it;
        set.seen = true;
        const std::shared_ptr<const buoyancy::HullFootprint> footprint = buoyancy::ResolveFootprint(
            hull.object->GetGeometryPath(), Math::float2(hull.settings->forward.x, hull.settings->forward.z), hull.settings->draft);
        if (footprint.get() != set.footprint)
        {
            set.footprint = footprint.get();
            for (int side = 0; side < 2; ++side) { set.side[side] = footprint ? FindRoot(*footprint, side) : Root{}; }
        }

        // The model's (u along its forward, v across, the waterline's height) in the world, as the frame draws the hull.
        const Math::float3 forward = Normalized(hull.forward);
        const Math::float3 up = Normalized(hull.up);
        const Math::float3 across = Normalized(CrossOf(forward, up)); // +v: the model's v is -x for a forward of +z
        const float scale = (hull.scale.x + hull.scale.y + hull.scale.z) / 3.0f;
        auto world = [&](float u, float v) {
            return hull.position + (forward * u + across * v + up * hull.settings->draft) * scale;
        };
        const float speed = std::max(hull.speed, 0.0f);
        // A hull in the air throws nothing: the spray is water the hull meets (VesselState::immersion, 1 = as deep as at
        // rest; a planing hull sits at a fraction of it, a hull off a crest at 0). Its slam on landing is the burst below.
        const float wet = std::clamp((hull.immersion - kSprayWetFrom) / (kSprayWetFull - kSprayWetFrom), 0.0f, 1.0f);
        const float share = params.enabled
            ? std::clamp((speed - kSpraySpeedOn) / (kSpraySpeedFull - kSpraySpeedOn), 0.0f, 1.0f) * wet * wet * (3.0f - 2.0f * wet) : 0.0f;
        const Math::float3 hullVelocity = forward * speed;
        // A slam: its rise past kSlamOn throws a burst this frame (the physics fades it, so a rise is a new one).
        const float impact = std::max(hull.impact, 0.0f);
        const float slam = (params.enabled && impact > kSlamOn && impact > set.lastImpact + 0.05f)
            ? std::min(impact, 2.0f) * std::max(params.slam, 0.0f) : 0.0f;
        set.lastImpact = impact;
        if (slam > 0.0f) { ++bursts_; }
        float sideLength = 0.0f;
        for (int side = 0; side < 2; ++side)
        {
            const Root& root = set.side[side];
            const Math::float3 outward = across * (side == 0 ? 1.0f : -1.0f);
            // On the hull's waterline across, at the water's surface under the hull up: the spray leaves the water there,
            // whatever the hull's heave and trim (the draft line rides 0.5 m over the sea when the hull leaves a crest).
            Math::float3 a = root.valid ? world(root.u0, root.v0) : hull.position;
            Math::float3 b = root.valid ? world(root.u1, root.v1) : hull.position;
            a.y = hull.waterHeight;
            b.y = hull.waterHeight;
            // The root's own motion: the hull's at its middle, v + w x r -- in a turn the bow swings in and the hull slips
            // sideways. The side moving INTO the water pushes it out at least that fast: without it the hull ran into the
            // drops it had just thrown on the turn's inside and they crossed it (owner, 2026-11-09: "particles fly through
            // the boat in turns"); the side moving away leaves them as they are.
            const Math::float3 r = (a + b) * 0.5f - hull.position;
            const Math::float3 w = hull.angularVelocity;
            const Math::float3 rootVelocity = hull.velocity + Math::float3(w.y * r.z - w.z * r.y, w.z * r.x - w.x * r.z, w.x * r.y - w.y * r.x);
            const float sideways = rootVelocity.x * outward.x + rootVelocity.z * outward.z;
            const Math::float3 pushed = Math::float3(outward.x, 0.0f, outward.z) * (vfx::g_spraySidePush_game ? std::max(sideways, 0.0f) : 0.0f);
            // A drop leaves the water the hull pushes aside: outward and up, with a little of the hull's way.
            const float out = std::max(params.out, 0.0f);
            const float rise = std::max(params.up, 0.0f);
            // A slam's frame throws higher and wider: the water a bow drives into is thrown up, not only aside (at least
            // 4 m/s up, the hull at rest in a sea included).
            const float throwSpeed = slam > 0.0f ? std::max(speed * std::sqrt(out * out + rise * rise), 4.0f) * 1.3f
                                                 : speed * std::sqrt(out * out + rise * rise);
            const Math::float3 direction = slam > 0.0f ? Normalized(outward * out + up * std::max(rise, out))
                                                       : Normalized(outward * out + up * rise);
            auto drive = [&](ParticleEmitterObject* emitter, const float (&alpha)[4], float rate, float speedLo, float speedHi,
                             const Math::float3& dir, float burst) {
                if (!emitter) { return; }
                EmitterDesc& d = emitter->DescRef();
                emitter->SetPosition(a);
                d.lineOffset = b - a;
                d.coneDir = dir;
                d.speedMin = throwSpeed * speedLo;
                d.speedMax = throwSpeed * speedHi;
                d.baseVelocity = hullVelocity * params.forward + pushed;
                d.emitterVelocity = rootVelocity; // a birth u into the frame starts where the root was then
                d.killBelowY = hull.waterHeight;
                d.collideHulls = true; // a drop that meets a hull (its own, another's) is spent on it (ocean/WaterMask's columns)
                d.spawnRate = root.valid ? rate * share : 0.0f;
                // The look: the albedo, the presets' alpha keys times the opacity, the sun's forward share.
                const float opacity = std::max(params.opacity, 0.0f);
                for (int k = 0; k < 4; ++k)
                {
                    d.colorKeys[k] = Math::float4(params.color.x, params.color.y, params.color.z, std::min(alpha[k] * opacity, 1.0f));
                }
                d.forwardScatter = std::clamp(params.forwardScatter, 0.0f, 1.0f);
                d.litFlat = std::clamp(params.foamLight, 0.0f, 1.0f);
                if (root.valid && burst > 0.0f) { emitter->AddBurst(burst); }
                rate_ += d.spawnRate;
            };
            const float sheetRate = set.sheet[side] ? set.sheet[side]->Desc().maxParticles / std::max(set.sheet[side]->Desc().lifetimeMax, 0.1f) : 0.0f;
            const float mistRate = set.mist[side] ? set.mist[side]->Desc().maxParticles / std::max(set.mist[side]->Desc().lifetimeMax, 0.1f) : 0.0f;
            // The presets' capacity over their longest life is the most a side can keep alive: the knob scales under it.
            drive(set.sheet[side], set.sheetAlpha, sheetRate * std::clamp(params.rate, 0.0f, 1.0f), 0.6f, 1.15f, direction,
                  slam * kSlamSheet);
            drive(set.mist[side], set.mistAlpha, mistRate * std::clamp(params.rate * params.mist, 0.0f, 1.0f), 0.2f, 0.5f,
                  Normalized(outward * 0.6f + up * 0.8f), slam * kSlamMist * std::max(params.mist, 0.0f));
            sheetRate_ += set.sheet[side] ? set.sheet[side]->Desc().spawnRate : 0.0f;
            // The white water along this side: from the spray's root to the stern, outside the waterline, with the hull.
            const Math::float3 offset = outward * (kSideFoamOut * scale);
            const Math::float3 fore = (root.valid ? world(root.u0, root.v0) : hull.position) + offset;
            const Math::float3 aft = (root.valid ? world(root.u2, root.v2) : hull.position) + offset;
            set.foamLine[side] = Math::float4(fore.x, fore.z, aft.x, aft.z);
            sideLength += 0.5f * std::sqrt((fore.x - aft.x) * (fore.x - aft.x) + (fore.z - aft.z) * (fore.z - aft.z));
        }
        const bool sides = set.side[0].valid && set.side[1].valid && sideLength > 0.5f;
        set.foamRate = sides ? kSideFoam * std::clamp(params.sideFoam, 0.0f, 4.0f) * share * speed / sideLength : 0.0f;
    }
    for (Set& set : sets_)
    {
        if (set.seen) { continue; }
        set.foamRate = 0.0f;
        for (int side = 0; side < 2; ++side)
        {
            if (set.sheet[side]) { set.sheet[side]->DescRef().spawnRate = 0.0f; }
            if (set.mist[side]) { set.mist[side]->DescRef().spawnRate = 0.0f; }
        }
    }
}
} // namespace vfx
