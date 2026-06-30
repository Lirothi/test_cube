#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// nlohmann/json - single header. The registry is runtime code, but its creators
// are JSON-driven so JsonLevel can keep object creation data-driven.
#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)

class RenderableObjectBase;
class Scene;

class SceneObjectRegistry
{
public:
    using ObjectList = std::vector<std::unique_ptr<RenderableObjectBase>>;

    struct CreationContext
    {
        Scene& scene;
    };

    using Creator = std::function<ObjectList(CreationContext& ctx, const nlohmann::json& objectJson)>;

    bool Register(std::string type, Creator creator);
    bool Has(std::string_view type) const;
    ObjectList Create(std::string_view type, CreationContext& ctx, const nlohmann::json& objectJson) const;

    static SceneObjectRegistry CreateWithBuiltins();

private:
    std::unordered_map<std::string, Creator> creators_;
};
