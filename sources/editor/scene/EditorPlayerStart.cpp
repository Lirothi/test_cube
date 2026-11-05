#if WITH_EDITOR
#include "editor/scene/EditorPlayerStart.h"

#include <cmath>

#include "app/camera/Camera.h"
#include "app/game/PlayerStartJson.h"
#include "app/scene/Scene.h"
#include "rendering/debug/DebugDraw.h"

namespace editorplayerstart
{
    EditorObject BuildObject(Scene& scene)
    {
        const Camera& camera = scene.CameraRef();
        const physics::PlayerSettings defaults;
        const Math::float3 eye = camera.GetPosition();
        Math::float3 feet(eye.x, eye.y - defaults.eyeHeight, eye.z);

        constexpr float kReach = 200.0f;
        constexpr float kWalkableNormalY = 0.5f;
        physics::RayHit hit;
        if (scene.Physics().RayCast(eye, camera.GetDirection(), kReach, hit) && hit.normal.y >= kWalkableNormalY)
        {
            feet = hit.point;
        }
        else if (scene.Physics().RayCast(eye, Math::float3(0.0f, -1.0f, 0.0f), kReach, hit))
        {
            feet = hit.point;
        }

        EditorObject object;
        object.name = "PlayerStart";
        object.type = game::kPlayerStartType;
        object.enabled = true;
        object.transform.position = feet;
        object.transform.rotationDeg = Math::float3(0.0f, camera.GetYaw() * Math::RAD2DEG, 0.0f);
        object.transform.scale = Math::float3(1.0f, 1.0f, 1.0f);
        object.properties = nlohmann::json::object();
        return object;
    }

    const EditorObject* Find(const EditorSceneDocument& document)
    {
        for (const EditorObject& object : document.Objects())
        {
            if (object.type == game::kPlayerStartType && object.enabled) { return &object; }
        }
        return nullptr;
    }

    void FromObject(const EditorObject& object, game::PlayerStart& out)
    {
        out.feet = object.transform.position;
        out.pitchRad = object.transform.rotationDeg.x * Math::DEG2RAD;
        out.yawRad = object.transform.rotationDeg.y * Math::DEG2RAD;
        game::ReadPlayerKeys(object.properties, out);
    }

    void Draw(DebugDrawSystem& draw, const EditorObject& object, bool selected)
    {
        game::PlayerStart start;
        FromObject(object, start);
        physics::PlayerSettings settings;
        game::ApplyPlayerKeys(start, settings);

        const Math::float4 colour = !object.enabled ? Math::float4(0.45f, 0.45f, 0.45f, 0.5f)
            : selected ? Math::float4(1.0f, 0.78f, 0.2f, 1.0f)
                       : Math::float4(0.35f, 1.0f, 0.45f, 0.85f);
        const Math::float3 feet = start.feet;
        const float radius = settings.radius;
        const Math::float3 low(feet.x, feet.y + radius, feet.z);
        const Math::float3 high(feet.x, feet.y + settings.height - radius, feet.z);
        const Math::float3 sides[] = { { radius, 0.0f, 0.0f }, { -radius, 0.0f, 0.0f }, { 0.0f, 0.0f, radius }, { 0.0f, 0.0f, -radius } };
        for (const Math::float3& side : sides)
        {
            draw.AddLine(low + side, high + side, colour);
        }
        // Rings and arcs: two wire spheres read as balls of wool at this size.
        constexpr int kSegments = 24;
        constexpr float kPi = 3.14159265358979f;
        for (int i = 0; i < kSegments; ++i)
        {
            const float a0 = 2.0f * kPi * static_cast<float>(i) / kSegments;
            const float a1 = 2.0f * kPi * static_cast<float>(i + 1) / kSegments;
            const Math::float3 p0(std::cos(a0) * radius, 0.0f, std::sin(a0) * radius);
            const Math::float3 p1(std::cos(a1) * radius, 0.0f, std::sin(a1) * radius);
            draw.AddLine(low + p0, low + p1, colour);
            draw.AddLine(high + p0, high + p1, colour);
        }
        for (int i = 0; i < kSegments / 2; ++i)
        {
            const float a0 = 2.0f * kPi * static_cast<float>(i) / kSegments;
            const float a1 = 2.0f * kPi * static_cast<float>(i + 1) / kSegments;
            const float c0 = std::cos(a0) * radius;
            const float s0 = std::sin(a0) * radius;
            const float c1 = std::cos(a1) * radius;
            const float s1 = std::sin(a1) * radius;
            draw.AddLine(high + Math::float3(c0, s0, 0.0f), high + Math::float3(c1, s1, 0.0f), colour);
            draw.AddLine(high + Math::float3(0.0f, s0, c0), high + Math::float3(0.0f, s1, c1), colour);
            draw.AddLine(low + Math::float3(c0, -s0, 0.0f), low + Math::float3(c1, -s1, 0.0f), colour);
            draw.AddLine(low + Math::float3(0.0f, -s0, c0), low + Math::float3(0.0f, -s1, c1), colour);
        }

        // The arrow: from the eye, the way the player will look.
        const Math::float3 forward(std::sin(start.yawRad), 0.0f, std::cos(start.yawRad));
        const Math::float3 right(std::cos(start.yawRad), 0.0f, -std::sin(start.yawRad));
        const Math::float3 eye(feet.x, feet.y + settings.eyeHeight, feet.z);
        const Math::float3 tip = eye + forward * 0.8f;
        draw.AddLine(eye, tip, colour);
        draw.AddLine(tip, tip - forward * 0.2f + right * 0.1f, colour);
        draw.AddLine(tip, tip - forward * 0.2f - right * 0.1f, colour);
    }
}

#endif // WITH_EDITOR
