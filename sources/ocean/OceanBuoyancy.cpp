#include "ocean/OceanBuoyancy.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_set>

#include "core/logging/Log.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "ocean/OceanReadback.h"
#include "ocean/OceanRenderable.h"
#include "ocean/OceanSimulation.h"
#include "rendering/core/Renderer.h"
#include "rendering/renderables/RenderableObject.h"
#include "rendering/renderables/RenderableObjectBase.h"

namespace
{
    constexpr float kGravity = 9.81f;
    constexpr float kPi = 3.14159265358979f;
    // Fixed ceiling on the integration step. The stiffest mode is a small float's heave or a narrow
    // hull's roll, well under 100 rad/s; 1/240 s keeps semi-implicit Euler far inside its limit.
    constexpr float kMaxSubstep = 1.0f / 240.0f;
    constexpr int kMaxSubsteps = 16;
    constexpr float kMaxFrameDelta = 0.1f;   // a hitch integrates as 100 ms, not as a launch
    constexpr float kMaxTilt = 1.2f;         // rad; past this it is not floating, it is sinking
    // How far a pontoon may carry the water it last read (its rate of change) into the future. The
    // copy is 1-3 frames old; a stall longer than this holds the last height rather than shooting
    // it along a stale slope.
    constexpr float kMaxCarrySeconds = 0.25f;

    // A sphere of radius r, submerged to depth s (0..2r): its volume and its waterplane section
    // (dV/ds, the pontoon's stiffness).
    float CapVolume(float s, float r)
    {
        s = std::clamp(s, 0.0f, 2.0f * r);
        return kPi * s * s * (3.0f * r - s) / 3.0f;
    }
    float CapArea(float s, float r)
    {
        s = std::clamp(s, 0.0f, 2.0f * r);
        return kPi * s * (2.0f * r - s);
    }

    bool Finite(const Math::float3& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    // Never destroyed: objects living in statics are torn down after function-local statics made
    // later, and an Unregister from a destructor must not find the registry already gone.
    struct Registry
    {
        std::mutex mutex;
        std::vector<RenderableObject*> objects;
    };
    Registry& TheRegistry()
    {
        static Registry* registry = new Registry();
        return *registry;
    }
}

void OceanBuoyancy::Register(RenderableObject* object)
{
    Registry& registry = TheRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    if (std::find(registry.objects.begin(), registry.objects.end(), object) == registry.objects.end())
    {
        registry.objects.push_back(object);
    }
}

void OceanBuoyancy::Unregister(RenderableObject* object)
{
    Registry& registry = TheRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.objects.erase(std::remove(registry.objects.begin(), registry.objects.end(), object),
        registry.objects.end());
}

Math::mat4 OceanBuoyancy::TiltMatrix(const Math::float3& tilt)
{
    const float angle = tilt.Length();
    if (angle < 1e-7f) { return Math::mat4::Identity(); }
    return Math::mat4::FromQuaternion(Math::quat::FromAxisAngle(tilt / angle, angle));
}

Math::mat4 OceanBuoyancy::RenderOffset(const Body& body)
{
    // Row vectors: p' = ((p - com) * tilt) + com + heave.
    return Math::mat4::Translation(-body.comRest) * TiltMatrix(body.tilt) *
           Math::mat4::Translation(body.comRest + Math::float3(0.0f, body.heave, 0.0f));
}

bool OceanBuoyancy::Setup(Body& body, const RenderableObject& object, const OceanRenderable& ocean) const
{
    body = Body{};
    body.built = true;
    body.geometry = object.GetGeometryPath();
    body.settingsRevision = object.GetBuoyancyRevision();
    body.authored = object.GetAuthoredModelMatrix();
    body.layout = buoyancy::Resolve(object.GetBuoyancySettings(), body.geometry);
    const buoyancy::Layout& layout = body.layout;
    if (!layout.Valid())
    {
        LOG_WARNING_ONCE_PER_MESSAGE(logging::LogCategory::Ocean,
            "buoyancy: '{}' has no pontoons (no geometry to place them on, none authored) -- it will not float",
            body.geometry);
        return false;
    }

    // Rest immersion in MODEL space, where the calm surface is the level plane y = waterline.
    std::vector<float> modelVolume(layout.pontoons.size(), 0.0f);
    float totalModelVolume = 0.0f;
    Math::float3 comModel(0.0f, layout.waterline, 0.0f);
    for (size_t i = 0; i < layout.pontoons.size(); ++i)
    {
        const buoyancy::Pontoon& p = layout.pontoons[i];
        modelVolume[i] = CapVolume(layout.waterline - (p.center.y - p.radius), p.radius);
        totalModelVolume += modelVolume[i];
        comModel.x += modelVolume[i] * p.center.x;
        comModel.z += modelVolume[i] * p.center.z;
    }
    if (totalModelVolume <= 1e-9f)
    {
        LOG_WARNING_ONCE_PER_MESSAGE(logging::LogCategory::Ocean,
            "buoyancy: '{}' -- no pontoon reaches the waterline at draft {:.3f} m; it will not float",
            body.geometry, layout.draft);
        return false;
    }
    // The centre of mass sits over the rest centre of buoyancy (so the calm pose is level) and at
    // the waterline (so it pitches and rolls about the plane it floats in).
    comModel.x /= totalModelVolume;
    comModel.z /= totalModelVolume;

    const Math::float3 scale = object.GetScale();
    const Math::float3 absScale(std::abs(scale.x), std::abs(scale.y), std::abs(scale.z));
    const float uniform = std::cbrt(std::max(absScale.x * absScale.y * absScale.z, 1e-12f));
    const float volumeScale = uniform * uniform * uniform;

    body.authoredRotation = Math::mat4::RotationFromEulerXYZRad(object.GetRotationEulerRad());
    body.comRest = body.authored.TransformPoint(comModel);
    float spanMinX = 1e30f, spanMaxX = -1e30f, spanMinZ = 1e30f, spanMaxZ = -1e30f;
    float stiffnessArea = 0.0f;
    for (size_t i = 0; i < layout.pontoons.size(); ++i)
    {
        const buoyancy::Pontoon& p = layout.pontoons[i];
        body.restPoints.push_back(body.authored.TransformPoint(p.center));
        body.radii.push_back(p.radius * uniform);
        body.restVolume.push_back(modelVolume[i] * volumeScale);
        spanMinX = std::min(spanMinX, p.center.x - p.radius); spanMaxX = std::max(spanMaxX, p.center.x + p.radius);
        spanMinZ = std::min(spanMinZ, p.center.z - p.radius); spanMaxZ = std::max(spanMaxZ, p.center.z + p.radius);
        const float restDepth = (layout.waterline - (p.center.y - p.radius)) * uniform;
        stiffnessArea += CapArea(restDepth, p.radius * uniform);
    }
    body.totalRestVolume = totalModelVolume * volumeScale;
    body.lift = kGravity / body.totalRestVolume;

    // Inertia per unit mass: a box over the pontoon footprint and the hull height, in the body
    // frame, times the per-mesh inertia (added mass). Diagonal: authored hulls are axis-aligned.
    const float spanX = (spanMaxX - spanMinX) * absScale.x;
    const float spanZ = (spanMaxZ - spanMinZ) * absScale.z;
    const float height = layout.hullHeight * absScale.y;
    body.inertiaBody = Math::float3(
        (height * height + spanZ * spanZ) / 12.0f,
        (spanX * spanX + spanZ * spanZ) / 12.0f,
        (spanX * spanX + height * height) / 12.0f) * layout.inertia;

    // Damping: `damping` is the heave's damping RATIO. Stiffness per unit mass is the lift times
    // the submerged waterplane; with inertia m_eff the critical coefficient is 2*sqrt(K*m_eff).
    const float stiffness = body.lift * stiffnessArea;
    body.dampingCoeff = 2.0f * layout.damping * std::sqrt(std::max(stiffness * layout.inertia, 0.0f));

    // Put it on the water at once instead of dropping it from wherever the level placed it.
    const OceanReadback& readback = ocean.GetReadback();
    const float waterY = readback.HasSurface() ? readback.SampleHeight(body.comRest.x, body.comRest.z)
                                               : ocean.GetWaterLevel();
    body.heave = waterY - body.comRest.y;
    body.waterHeight.assign(body.restPoints.size(), waterY);
    body.waterRate.assign(body.restPoints.size(), 0.0f);
    body.valid = true;

    const float heavePeriod = stiffness > 0.0f ? 2.0f * kPi * std::sqrt(layout.inertia / stiffness) : 0.0f;
    LOG_INFO_ONCE_PER_MESSAGE(logging::LogCategory::Ocean,
        "buoyancy: floating '{}' -- {} {} pontoons, draft {:.3f} m, inertia {:.2f}, damping {:.2f}, heave period {:.2f} s",
        body.geometry, layout.pontoons.size(), layout.automatic ? "automatic" : "authored", layout.draft,
        layout.inertia, layout.damping, heavePeriod);
    return true;
}

void OceanBuoyancy::Step(Body& body, const OceanRenderable& ocean, float deltaTime) const
{
    const float dt = std::min(deltaTime, kMaxFrameDelta);
    if (dt <= 0.0f) { return; }
    const size_t count = body.restPoints.size();
    const OceanReadback& readback = ocean.GetReadback();

    // 1. The water under each pontoon. A NEW copy is sampled where the pontoons are now and yields
    //    a rate from the previous copy; every frame then carries height + rate * age forward to the
    //    ocean's current time -- the readback's latency and its steps never reach the body raw.
    if (readback.HasSurface() && readback.AdoptedFrame() != body.waterFrame)
    {
        const Math::mat4 tilt = TiltMatrix(body.tilt);
        const Math::float3 com = body.comRest + Math::float3(0.0f, body.heave, 0.0f);
        const float copyTime = readback.Surface().time;
        const float span = copyTime - body.waterTime;
        for (size_t i = 0; i < count; ++i)
        {
            const Math::float3 p = com + tilt.TransformDirection(body.restPoints[i] - body.comRest);
            const float h = readback.SampleHeight(p.x, p.z);
            const float rate = (body.waterFrame != 0 && span > 1e-4f) ? (h - body.waterHeight[i]) / span : 0.0f;
            // Averaged with the previous rate: one noisy pair of copies must not fling the carry.
            body.waterRate[i] = body.waterFrame != 0 ? 0.5f * (body.waterRate[i] + rate) : 0.0f;
            body.waterHeight[i] = h;
        }
        body.waterFrame = readback.AdoptedFrame();
        body.waterTime = copyTime;
    }
    const float age = readback.HasSurface() ?
        std::clamp(ocean.GetElapsedTime() - body.waterTime, 0.0f, kMaxCarrySeconds) : 0.0f;
    std::vector<float> water(count);
    for (size_t i = 0; i < count; ++i)
    {
        water[i] = readback.HasSurface() ? body.waterHeight[i] + body.waterRate[i] * age
                                         : ocean.GetWaterLevel();
    }

    // 2. Heave + pitch + roll. Forces are vertical, so there is no horizontal or yaw motion to
    //    integrate; `tilt` is a rotation vector about horizontal axes and stays yaw-free.
    const int steps = std::clamp(static_cast<int>(std::ceil(dt / kMaxSubstep)), 1, kMaxSubsteps);
    const float h = dt / static_cast<float>(steps);
    const float inertia = body.layout.inertia;
    for (int step = 0; step < steps; ++step)
    {
        const Math::mat4 tilt = TiltMatrix(body.tilt);
        const Math::float3 com = body.comRest + Math::float3(0.0f, body.heave, 0.0f);
        float force = 0.0f;
        Math::float3 torque(0.0f);
        for (size_t i = 0; i < count; ++i)
        {
            const Math::float3 r = tilt.TransformDirection(body.restPoints[i] - body.comRest);
            const float depth = water[i] - (com.y + r.y - body.radii[i]);
            const float volume = CapVolume(depth, body.radii[i]);
            if (volume <= 0.0f) { continue; }
            const float pointVelocity = body.heaveVelocity + body.angularVelocity.Cross(r).y;
            const float f = body.lift * volume -
                body.dampingCoeff * (volume / body.totalRestVolume) * pointVelocity;
            force += f;
            torque += r.Cross(Math::float3(0.0f, f, 0.0f));
        }
        const float accel = (force - kGravity) / inertia;

        // Torque into the body frame, over its (already inertia-scaled) diagonal, and back.
        const Math::mat4 world = body.authoredRotation * tilt;
        const Math::float3 torqueBody = Math::mat4::Transpose(world).TransformDirection(torque);
        Math::float3 alpha = world.TransformDirection(torqueBody / body.inertiaBody);
        alpha.y = 0.0f;

        body.heaveVelocity += accel * h;
        body.angularVelocity += alpha * h;
        body.angularVelocity.y = 0.0f;
        body.heave += body.heaveVelocity * h;
        body.tilt += body.angularVelocity * h;
        body.tilt.y = 0.0f;
        const float tiltAngle = body.tilt.Length();
        if (tiltAngle > kMaxTilt)
        {
            body.tilt = body.tilt * (kMaxTilt / tiltAngle);
            body.angularVelocity = Math::float3(0.0f);
        }
    }

    if (!std::isfinite(body.heave) || !std::isfinite(body.heaveVelocity) ||
        !Finite(body.tilt) || !Finite(body.angularVelocity))
    {
        LOG_WARNING_ONCE_PER_MESSAGE(logging::LogCategory::Ocean,
            "buoyancy: '{}' diverged -- reset onto the water", body.geometry);
        body.heave = (count > 0 ? water[0] : ocean.GetWaterLevel()) - body.comRest.y;
        body.heaveVelocity = 0.0f;
        body.tilt = Math::float3(0.0f);
        body.angularVelocity = Math::float3(0.0f);
    }
}

void OceanBuoyancy::Tick(OceanRenderable* ocean, Renderer& renderer, float deltaTime)
{
    CPU_SCOPE(ProfilerScopes::kSceneTickBuoyancy);
    if (ocean != lastOcean_)
    {
        bodies_.clear();
        lastOcean_ = ocean;
    }
    std::vector<RenderableObject*> registered;
    {
        Registry& registry = TheRegistry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        if (registry.objects.empty() && bodies_.empty()) { return; } // nothing floats: free
        registered = registry.objects;
    }
    const bool oceanOn = ocean && ocean->IsVisible() && ocean->GetSimulation();
    std::vector<RenderableObject*> floating;
    for (RenderableObject* object : registered)
    {
        if (oceanOn && object->IsVisible())
        {
            floating.push_back(object);
        }
        else
        {
            object->ClearRenderOffset(); // hidden, or no ocean to float on: back where the level put it
        }
    }
    // Bodies of objects that stopped floating or no longer exist. Keys only -- never dereferenced.
    {
        const std::unordered_set<const RenderableObject*> alive(floating.begin(), floating.end());
        for (auto it = bodies_.begin(); it != bodies_.end();)
        {
            it = alive.count(it->first) ? std::next(it) : bodies_.erase(it);
        }
    }
    if (floating.empty()) { return; }

    OceanReadback& readback = ocean->GetReadback();
    readback.Request();
    readback.Poll(&renderer);
    if (ocean->GetSimulation()->GetSettings().GetReadbackMode() ==
        OceanSimulationSettings::ReadbackCascadesMode::None)
    {
        LOG_WARNING_ONCE(logging::LogCategory::Ocean,
            "buoyancy: the ocean preset's readbackCascades is None, so floating objects ride a FLAT "
            "surface -- set it to One or Two (Ocean controls) for waves");
    }

    for (RenderableObject* object : floating)
    {
        Body& body = bodies_[object];
        const Math::mat4 authored = object->GetAuthoredModelMatrix();
        const bool stale = !body.built || body.geometry != object->GetGeometryPath() ||
            body.settingsRevision != object->GetBuoyancyRevision() ||
            std::memcmp(&body.authored.m, &authored.m, sizeof(authored.m)) != 0;
        if (stale) { Setup(body, *object, *ocean); }
        if (!body.valid)
        {
            object->ClearRenderOffset();
            continue;
        }
        Step(body, *ocean, deltaTime);
        object->SetRenderOffset(RenderOffset(body));
    }

    if (!bodies_.empty())
    {
        const Body& first = bodies_.begin()->second;
        LOG_DEBUG_THROTTLED(std::chrono::seconds(2), logging::LogCategory::Ocean,
            "buoyancy: {} floating, surface {} frame(s) old ({} cascade(s)); '{}' heave {:.3f} m (v {:.3f}), tilt x {:.2f} z {:.2f} deg",
            bodies_.size(), readback.LatencyFrames(), readback.Surface().cascades, first.geometry,
            first.heave, first.heaveVelocity, first.tilt.x * 57.2958f, first.tilt.z * 57.2958f);
    }
}

bool OceanBuoyancy::GetWorldPontoons(const RenderableObject* object, std::vector<Math::float4>& out) const
{
    out.clear();
    const auto it = bodies_.find(object);
    if (it == bodies_.end() || !it->second.valid) { return false; }
    const Body& body = it->second;
    const Math::mat4 offset = RenderOffset(body);
    for (size_t i = 0; i < body.restPoints.size(); ++i)
    {
        out.emplace_back(offset.TransformPoint(body.restPoints[i]), body.radii[i]);
    }
    return true;
}
