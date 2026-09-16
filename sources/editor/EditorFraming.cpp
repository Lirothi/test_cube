#include "editor/EditorFraming.h"
#if WITH_EDITOR

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "app/camera/Camera.h"
#include "app/scene/Scene.h"
#include "rendering/core/Renderer.h"
#include "rendering/renderables/RenderableObjectBase.h"

namespace
{
    // A private copy of the controller's JSON readers. Small, and sharing them would mean
    // dragging EditorController's whole helper namespace into a header for six lines.
    bool TryReadFloat(const nlohmann::json& value, float& out)
    {
        if (!value.is_number())
        {
            return false;
        }
        const float parsed = value.get<float>();
        if (!std::isfinite(parsed))
        {
            return false;
        }
        out = parsed;
        return true;
    }

    bool TryReadFloat3(const nlohmann::json& value, Math::float3& out)
    {
        if (!value.is_array() || value.size() < 3)
        {
            return false;
        }
        Math::float3 parsed;
        if (!TryReadFloat(value[0], parsed.x) ||
            !TryReadFloat(value[1], parsed.y) ||
            !TryReadFloat(value[2], parsed.z))
        {
            return false;
        }
        out = parsed;
        return true;
    }
}

namespace editorframing
{
    bool TryGetWorldBounds(
        const Scene& scene,
        const EditorSceneDocument& document,
        EditorObjectId id,
        Math::float3& outMin,
        Math::float3& outMax)
    {
        if (id.value == 0)
        {
            return false;
        }
        if (const RenderableObjectBase* runtime = scene.FindEditorObject(id.value))
        {
            const AABB& bounds = runtime->GetWorldBounds();
            if (bounds.IsValid())
            {
                outMin = bounds.GetMin();
                outMax = bounds.GetMax();
                return true;
            }
        }
        // No live renderable: fall back to the centre-and-radius answer, which is a sphere
        // and so a cube here. Honest for a light or a zone; it is only the MESHES that
        // deserve better, and those have runtime bounds.
        Math::float3 centre;
        float radius = 0.0f;
        if (!TryGetFrameTarget(scene, document, id, centre, radius))
        {
            return false;
        }
        outMin = centre - Math::float3(radius);
        outMax = centre + Math::float3(radius);
        return true;
    }

    bool TryGetFrameTarget(
        const Scene& scene,
        const EditorSceneDocument& document,
        EditorObjectId id,
        Math::float3& outCenter,
        float& outRadius)
    {
        if (id.value == 0)
        {
            return false;
        }

        if (const RenderableObjectBase* runtime = scene.FindEditorObject(id.value))
        {
            const AABB& bounds = runtime->GetWorldBounds();
            if (bounds.IsValid())
            {
                outCenter = bounds.GetCenter();
                outRadius = std::max(bounds.GetRadius(), 1.0f);
                return true;
            }
        }

        if (const EditorObject* object = document.Find(id))
        {
            outCenter = object->transform.position;
            outRadius = std::max(object->transform.scale.Length(), 1.0f);
            return true;
        }

        for (const EditorObject& env : document.Environment())
        {
            if (env.id.value != id.value)
            {
                continue;
            }

            const auto positionIt = env.properties.find("position");
            if (positionIt == env.properties.end() || !TryReadFloat3(*positionIt, outCenter))
            {
                return false;
            }

            if (env.type == "pointLight")
            {
                outRadius = std::max(env.properties.value("radius", 1.0f), 1.0f);
            }
            else if (env.type == "spotLight")
            {
                outRadius = std::max(env.properties.value("range", 4.0f) * 0.25f, 1.0f);
            }
            else
            {
                outRadius = 1.0f;
            }
            return true;
        }

        return false;
    }

    bool FrameObjects(Renderer& renderer,
        Scene& scene,
        const EditorSceneDocument& document,
        const EditorSelection& objects)
    {
        Math::float3 firstCenter;
        float firstRadius = 1.0f;
        Math::float3 boundsMin;
        Math::float3 boundsMax;
        std::size_t validTargetCount = 0;
        for (const EditorObjectId id : objects.Ordered())
        {
            Math::float3 center;
            float radius = 1.0f;
            if (!TryGetFrameTarget(scene, document, id, center, radius))
            {
                continue;
            }

            if (validTargetCount == 0)
            {
                firstCenter = center;
                firstRadius = radius;
                boundsMin = center - Math::float3(radius);
                boundsMax = center + Math::float3(radius);
            }
            else
            {
                boundsMin.x = std::min(boundsMin.x, center.x - radius);
                boundsMin.y = std::min(boundsMin.y, center.y - radius);
                boundsMin.z = std::min(boundsMin.z, center.z - radius);
                boundsMax.x = std::max(boundsMax.x, center.x + radius);
                boundsMax.y = std::max(boundsMax.y, center.y + radius);
                boundsMax.z = std::max(boundsMax.z, center.z + radius);
            }
            ++validTargetCount;
        }

        if (validTargetCount == 0)
        {
            return false;
        }

        Math::float3 center = firstCenter;
        float radius = firstRadius;
        if (validTargetCount > 1)
        {
            center = (boundsMin + boundsMax) * 0.5f;
            radius = std::max((boundsMax - boundsMin).Length() * 0.5f, 1.0f);
        }

        Camera& camera = scene.CameraRef();
        Math::float3 forward = camera.GetDirection();
        if (forward.Length() <= Math::EPS)
        {
            forward = Math::float3(0.0f, 0.0f, 1.0f);
        }

        const float distance = std::max(radius * 2.5f, 3.0f);
        camera.SetPosition(center - forward.Normalized() * distance);
        camera.CalcMatrices(&renderer);
        camera.ResetHistory();
        return true;
    }

    bool FrameVisibleScene(Renderer& renderer, Scene& scene, const EditorSceneDocument& document)
    {
        EditorSelection visibleObjects;
        for (const EditorObject& object : document.Objects())
        {
            const RenderableObjectBase* runtime = scene.FindEditorObject(object.id.value);
            if (object.enabled && runtime && runtime->IsVisible())
            {
                visibleObjects.Add(object.id, false);
            }
        }
        return FrameObjects(renderer, scene, document, visibleObjects);
    }
}

#endif // WITH_EDITOR
