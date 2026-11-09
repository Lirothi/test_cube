#include "ocean/WaterMask.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

#include "core/logging/Log.h"
#include "physics/PhysicsWorld.h"
#include "rendering/renderables/RenderableObject.h"

namespace ocean
{
namespace
{
    struct Entry
    {
        RenderableObject* object = nullptr;
        std::shared_ptr<const WaterMaskHull> hull;
        bool looked = false; // the hull was asked for (it may have none)
    };
    struct Registry
    {
        std::mutex mutex;
        std::vector<Entry> entries;
        std::unordered_map<std::string, std::shared_ptr<const WaterMaskHull>> hulls; // by geometry; null = none
    };
    Registry& TheRegistry()
    {
        static Registry registry;
        return registry;
    }

    // Under the registry's lock: the hull of a geometry, built the first time it is asked for.
    std::shared_ptr<const WaterMaskHull> HullOf(Registry& registry, const std::string& path)
    {
        if (path.empty()) { return nullptr; }
        const auto found = registry.hulls.find(path);
        if (found != registry.hulls.end()) { return found->second; }
        auto hull = std::make_shared<WaterMaskHull>();
        std::shared_ptr<const WaterMaskHull> result;
        if (physics::ConvexHullPlanes(path, kWaterMaskMaxPlanes, hull->planes, hull->boxMin, hull->boxMax) && !hull->planes.empty())
        {
            result = hull;
        }
        else
        {
            LOG_WARNING(logging::LogCategory::Ocean, "water mask: no convex hull for {} -- the water is drawn inside it", path);
        }
        registry.hulls.emplace(path, result);
        return result;
    }
}

namespace watermask
{
    void Register(RenderableObject* object)
    {
        Registry& registry = TheRegistry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        const auto it = std::find_if(registry.entries.begin(), registry.entries.end(), [&](const Entry& e) { return e.object == object; });
        if (it == registry.entries.end()) { registry.entries.push_back(Entry{ object, nullptr, false }); }
    }

    void Unregister(RenderableObject* object)
    {
        Registry& registry = TheRegistry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        registry.entries.erase(std::remove_if(registry.entries.begin(), registry.entries.end(),
                                              [&](const Entry& e) { return e.object == object; }),
                               registry.entries.end());
    }

    void Collect(std::vector<WaterMaskInstance>& out)
    {
        out.clear();
        Registry& registry = TheRegistry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        for (Entry& entry : registry.entries)
        {
            if (!entry.looked)
            {
                entry.hull = HullOf(registry, entry.object->GetGeometryPath());
                entry.looked = true;
            }
            if (!entry.hull || !entry.object->IsVisibleGame()) { continue; }
            out.push_back(WaterMaskInstance{ entry.hull, Math::mat4::Inverse(entry.object->GetModelMatrix()) });
        }
    }
}
} // namespace ocean
