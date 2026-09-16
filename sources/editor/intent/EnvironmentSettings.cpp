#include "editor/intent/EnvironmentSettings.h"
#if WITH_EDITOR

#include <cstdio>
#include <string>
#include <vector>

#include "core/StringMatch.h"

namespace
{
    // How deep to walk an entity's properties. Two levels covers everything the level
    // actually nests (`ocean.render.*`); going deeper only drags in the big colour-ramp
    // arrays, which are not single-value knobs and have no business in a command bar.
    constexpr int kMaxDepth = 2;

    bool IsVec3(const nlohmann::json& value)
    {
        return value.is_array() && value.size() == 3 &&
            value[0].is_number() && value[1].is_number() && value[2].is_number();
    }

    void Walk(EditorObjectId owner,
        const std::string& prefix,
        const std::string& subPrefix,
        const nlohmann::json& node,
        int depth,
        std::vector<envsettings::Setting>& out)
    {
        if (!node.is_object())
        {
            return;
        }
        for (auto it = node.begin(); it != node.end(); ++it)
        {
            const std::string key = it.key();
            const nlohmann::json& value = it.value();
            const std::string path = prefix + "." + key;
            const std::string subPath = subPrefix.empty() ? key : subPrefix + "." + key;

            if (value.is_boolean())
            {
                out.push_back({ owner, path, subPath, envsettings::ValueKind::Bool, value });
            }
            else if (value.is_number())
            {
                out.push_back({ owner, path, subPath, envsettings::ValueKind::Number, value });
            }
            else if (value.is_string())
            {
                out.push_back({ owner, path, subPath, envsettings::ValueKind::String, value });
            }
            else if (IsVec3(value))
            {
                out.push_back({ owner, path, subPath, envsettings::ValueKind::Vec3, value });
            }
            else if (value.is_object() && depth < kMaxDepth)
            {
                Walk(owner, path, subPath, value, depth + 1, out);
            }
        }
    }
}

namespace envsettings
{
    const char* KindName(ValueKind kind)
    {
        switch (kind)
        {
        case ValueKind::Number: return "number";
        case ValueKind::Bool:   return "true/false";
        case ValueKind::String: return "text";
        case ValueKind::Vec3:   return "[x, y, z]";
        }
        return "?";
    }

    std::vector<Setting> Enumerate(const EditorSceneDocument& document)
    {
        std::vector<Setting> settings;
        for (const EditorObject& entity : document.Environment())
        {
            if (entity.type.empty() || !entity.properties.is_object())
            {
                continue;
            }
            // Lights come as several entities of the same type; a bare "pointLight.intensity"
            // would be ambiguous between them and there is no good way to say which from a
            // phrase. Those are an Inspector job, not a command-bar one.
            if (entity.type == "pointLight" || entity.type == "spotLight")
            {
                continue;
            }
            Walk(entity.id, entity.type, {}, entity.properties, 0, settings);
        }
        return settings;
    }

    const Setting* Find(const std::vector<Setting>& settings, std::string_view path)
    {
        if (path.empty())
        {
            return nullptr;
        }
        for (const Setting& setting : settings)
        {
            if (textmatch::CompareCaseInsensitive(setting.path, path) == 0)
            {
                return &setting;
            }
        }

        // A bare leaf name ("strength") is a reasonable thing to say, but only when it
        // names one knob. Two matches means the phrase was ambiguous, and picking either
        // would be the silent wrong edit this whole layer is built to avoid.
        const Setting* unique = nullptr;
        for (const Setting& setting : settings)
        {
            const std::size_t dot = setting.path.rfind('.');
            const std::string_view leaf = dot == std::string::npos
                ? std::string_view(setting.path)
                : std::string_view(setting.path).substr(dot + 1);
            if (textmatch::CompareCaseInsensitive(leaf, path) == 0)
            {
                if (unique)
                {
                    return nullptr;
                }
                unique = &setting;
            }
        }
        return unique;
    }

    bool Write(nlohmann::json& properties, const std::string& subPath, const nlohmann::json& value)
    {
        if (subPath.empty())
        {
            return false;
        }
        if (!properties.is_object())
        {
            properties = nlohmann::json::object();
        }

        nlohmann::json* node = &properties;
        std::size_t start = 0;
        for (;;)
        {
            const std::size_t dot = subPath.find('.', start);
            const std::string key = subPath.substr(start, dot == std::string::npos
                ? std::string::npos : dot - start);
            if (key.empty())
            {
                return false;
            }
            if (dot == std::string::npos)
            {
                (*node)[key] = value;
                return true;
            }
            nlohmann::json& child = (*node)[key];
            if (!child.is_object())
            {
                if (!child.is_null())
                {
                    return false;   // the path runs through a value, not a container
                }
                child = nlohmann::json::object();
            }
            node = &child;
            start = dot + 1;
        }
    }

    std::string ToText(const nlohmann::json& value)
    {
        if (value.is_boolean())
        {
            return value.get<bool>() ? "true" : "false";
        }
        if (value.is_number())
        {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%g", value.get<double>());
            return buffer;
        }
        if (value.is_string())
        {
            return value.get<std::string>();
        }
        if (value.is_array())
        {
            std::string text = "[";
            for (std::size_t i = 0; i < value.size(); ++i)
            {
                if (i > 0)
                {
                    text += ", ";
                }
                text += ToText(value[i]);
            }
            return text + "]";
        }
        return value.dump();
    }
}

#endif // WITH_EDITOR
