#include "editor/intent/EditorActionBuilders.h"
#if WITH_EDITOR

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "editor/commands/CompositeCommand.h"

// R10.3: the editor's actions -- the helpers every builder shares -- parameter access, counting, folding into one undo entry.
// Cut out of EditorActionRegistry.cpp; the table that names them is still there (EditorActionBuilders.h).

namespace editoractions
{
    // ----------------------------------------------------------- shared helpers

    const EditorObject* FindEnvironmentObject(const EditorSceneDocument& document, EditorObjectId id)
    {
        for (const EditorObject& environment : document.Environment())
        {
            if (environment.id.value == id.value)
            {
                return &environment;
            }
        }
        return nullptr;
    }

    bool IsBulkObjectSupported(const EditorSceneDocument& document, EditorObjectId id)
    {
        if (const EditorObject* object = document.Find(id))
        {
            return object->type != "ocean";
        }
        const EditorObject* environment = FindEnvironmentObject(document, id);
        return environment &&
            (environment->type == "pointLight" || environment->type == "spotLight");
    }

    // Folds N commands into one history entry. One typed phrase must cost exactly one
    // Ctrl+Z, whether it moved a single palm or two hundred of them (E6).
    //
    // Taken by REFERENCE: callers write `FoldIntoOneEntry(std::move(commands), "... " +
    // std::to_string(commands.size()))`, and with a by-value parameter the order in which the
    // vector is moved out and the label reads its size is unspecified -- "0 Objects" on a
    // left-to-right compiler. Bound to a reference, nothing moves until the body runs.
    std::unique_ptr<EditorCommand> FoldIntoOneEntry(
        std::vector<std::unique_ptr<EditorCommand>>&& commands,
        const std::string& label)
    {
        if (commands.empty())
        {
            return nullptr;
        }
        if (commands.size() == 1)
        {
            return std::move(commands.front());
        }
        auto composite = std::make_unique<CompositeCommand>(label);
        for (std::unique_ptr<EditorCommand>& command : commands)
        {
            composite->Add(std::move(command));
        }
        return composite;
    }

    std::string CountedObjects(std::size_t count)
    {
        return std::to_string(count) + (count == 1 ? " object" : " objects");
    }

    // --------------------------------------------------------- parameter access
    //
    // Everything below assumes ValidateParams has already run, so the shapes are known
    // good and these only have to supply defaults.

    float NumberOr(const nlohmann::json& params, const char* key, float fallback)
    {
        const auto it = params.find(key);
        return (it != params.end() && it->is_number()) ? it->get<float>() : fallback;
    }

    bool BoolOr(const nlohmann::json& params, const char* key, bool fallback)
    {
        const auto it = params.find(key);
        return (it != params.end() && it->is_boolean()) ? it->get<bool>() : fallback;
    }

    std::string StringOr(const nlohmann::json& params, const char* key, const char* fallback)
    {
        const auto it = params.find(key);
        return (it != params.end() && it->is_string()) ? it->get<std::string>() : std::string(fallback);
    }

    void RangeOr(const nlohmann::json& params, const char* key, float& lo, float& hi)
    {
        const auto it = params.find(key);
        if (it != params.end() && it->is_array() && it->size() == 2 &&
            (*it)[0].is_number() && (*it)[1].is_number())
        {
            lo = (*it)[0].get<float>();
            hi = (*it)[1].get<float>();
            if (hi < lo)
            {
                std::swap(lo, hi);
            }
        }
    }

    Math::float3 Vec3Or(const nlohmann::json& params, const char* key, const Math::float3& fallback)
    {
        const auto it = params.find(key);
        if (it != params.end() && it->is_array() && it->size() == 3 &&
            (*it)[0].is_number() && (*it)[1].is_number() && (*it)[2].is_number())
        {
            return Math::float3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
        }
        return fallback;
    }

    // A seed that is DETERMINISTIC but not constant. An explicit `seed` wins; otherwise
    // it comes from how many objects the document holds, so re-running the same phrase
    // after a spawn lays things out differently while a gate that pins the seed gets the
    // same answer every time.
    std::uint32_t SeedFor(const nlohmann::json& params, const EditorSceneDocument& document)
    {
        const auto it = params.find("seed");
        if (it != params.end() && it->is_number())
        {
            return static_cast<std::uint32_t>(std::max(0.0f, it->get<float>()));
        }
        return static_cast<std::uint32_t>(document.Objects().size()) * 2654435761u + 12345u;
    }
}

#endif // WITH_EDITOR
