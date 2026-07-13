#include "editor/commands/PasteObjectCommand.h"
#if WITH_EDITOR

#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/scene/Scene.h"
#include "app/scene/SceneObjectRegistry.h"
#include "editor/EditorContext.h"
#include "editor/scene/EnvironmentRuntime.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/renderables/RenderableObjectBase.h"

namespace
{
    constexpr float kPastePositionOffset = 0.5f;

    bool IsEnvironmentLight(std::string_view type)
    {
        return type == "pointLight" || type == "spotLight";
    }

    bool IsEnvironmentSingleton(std::string_view type)
    {
        return type == "camera" ||
            type == "directionalLight" ||
            type == "skybox" ||
            type == "ocean";
    }

    bool IsDocumentOnlyObject(std::string_view type)
    {
        return type == "freeCameraStart";
    }

    std::string CopyName(const std::string& name, std::string_view type)
    {
        if (!name.empty())
        {
            return name + " Copy";
        }
        return type.empty() ? std::string("Copy") : std::string(type) + " Copy";
    }

    bool IsFloat3(const nlohmann::json& value)
    {
        return value.is_array() && value.size() >= 3 &&
            value[0].is_number() && value[1].is_number() && value[2].is_number();
    }

    bool ValidateOptionalFloat3(const nlohmann::json& objectJson,
        const char* key,
        std::string& outReason)
    {
        const auto it = objectJson.find(key);
        if (it == objectJson.end() || IsFloat3(*it))
        {
            return true;
        }
        outReason = std::string("Field '") + key + "' must be a three-number array";
        return false;
    }

    bool ValidateOptionalNumber(const nlohmann::json& objectJson,
        const char* key,
        std::string& outReason)
    {
        const auto it = objectJson.find(key);
        if (it == objectJson.end() || it->is_number())
        {
            return true;
        }
        outReason = std::string("Field '") + key + "' must be numeric";
        return false;
    }

    bool ValidateOptionalBool(const nlohmann::json& objectJson,
        const char* key,
        std::string& outReason)
    {
        const auto it = objectJson.find(key);
        if (it == objectJson.end() || it->is_boolean())
        {
            return true;
        }
        outReason = std::string("Field '") + key + "' must be true or false";
        return false;
    }

    bool ValidateLight(const nlohmann::json& objectJson,
        std::string_view type,
        std::string& outReason)
    {
        if (!ValidateOptionalBool(objectJson, "enabled", outReason) ||
            !ValidateOptionalFloat3(objectJson, "position", outReason) ||
            !ValidateOptionalFloat3(objectJson, "color", outReason) ||
            !ValidateOptionalNumber(objectJson, "intensity", outReason) ||
            !ValidateOptionalBool(objectJson, "shadowsEnabled", outReason))
        {
            return false;
        }

        if (type == "pointLight")
        {
            return ValidateOptionalNumber(objectJson, "radius", outReason);
        }

        return ValidateOptionalFloat3(objectJson, "direction", outReason) &&
            ValidateOptionalNumber(objectJson, "range", outReason) &&
            ValidateOptionalNumber(objectJson, "innerAngleDeg", outReason) &&
            ValidateOptionalNumber(objectJson, "outerAngleDeg", outReason) &&
            ValidateOptionalNumber(objectJson, "shadowNormalBias", outReason) &&
            ValidateOptionalNumber(objectJson, "shadowDepthBias", outReason);
    }

    void OffsetEnvironmentPosition(EditorObject& object)
    {
        const auto positionIt = object.properties.find("position");
        if (positionIt != object.properties.end() && IsFloat3(*positionIt))
        {
            (*positionIt)[0] = (*positionIt)[0].get<float>() + kPastePositionOffset;
        }
    }
}

PasteObjectCommand::PasteObjectCommand(nlohmann::json objectJson, bool addToSelection)
    : objectJson_(std::move(objectJson))
    , addToSelection_(addToSelection)
{
}

bool PasteObjectCommand::Validate(const nlohmann::json& objectJson, std::string& outReason)
{
    outReason.clear();
    if (!objectJson.is_object())
    {
        outReason = "Clipboard JSON must be an object";
        return false;
    }

    const auto typeIt = objectJson.find("type");
    if (typeIt == objectJson.end() || !typeIt->is_string() || typeIt->get_ref<const std::string&>().empty())
    {
        outReason = "Clipboard object has no valid type";
        return false;
    }

    const std::string& type = typeIt->get_ref<const std::string&>();
    if (IsEnvironmentSingleton(type))
    {
        outReason = "Environment singleton '" + type + "' cannot be pasted";
        return false;
    }

    const auto nameIt = objectJson.find("name");
    if (nameIt != objectJson.end() && !nameIt->is_string())
    {
        outReason = "Field 'name' must be text";
        return false;
    }

    if (IsEnvironmentLight(type))
    {
        return ValidateLight(objectJson, type, outReason);
    }

    if (!ValidateOptionalBool(objectJson, "enabled", outReason) ||
        !ValidateOptionalFloat3(objectJson, "position", outReason) ||
        !ValidateOptionalFloat3(objectJson, "rotationDeg", outReason) ||
        !ValidateOptionalFloat3(objectJson, "scale", outReason))
    {
        return false;
    }

    if (IsDocumentOnlyObject(type))
    {
        return true;
    }

    const SceneObjectRegistry registry = SceneObjectRegistry::CreateWithBuiltins();
    if (!registry.Has(type))
    {
        outReason = "Unknown object type '" + type + "'";
        return false;
    }
    return true;
}

bool PasteObjectCommand::Execute(EditorContext& ctx)
{
    if (!built_)
    {
        std::string reason;
        if (!Validate(objectJson_, reason))
        {
            return false;
        }

        try
        {
            const std::string type = objectJson_["type"].get<std::string>();
            if (IsEnvironmentLight(type))
            {
                object_.type = type;
                object_.name = CopyName(objectJson_.value("name", std::string()), type);
                object_.enabled = objectJson_.value("enabled", true);
                object_.properties = objectJson_;
                object_.properties.erase("id");
                object_.properties.erase("name");
                object_.properties.erase("type");
                OffsetEnvironmentPosition(object_);
                isEnvironment_ = true;
            }
            else
            {
                object_ = EditorSceneDocument::ObjectFromJson(
                    EditorObjectId{}, objectJson_);
                object_.name = CopyName(object_.name, object_.type);
                object_.transform.position.x += kPastePositionOffset;
                isEnvironment_ = false;
            }
        }
        catch (const std::exception&)
        {
            return false;
        }
        built_ = true;
    }

    if (isEnvironment_)
    {
        if (object_.id.value == 0)
        {
            object_.id = ctx.document.AllocateId();
        }
        previousSelection_ = ctx.selection;
        ctx.renderer.WaitForPreviousFrame();
        ctx.document.Environment().push_back(object_);
        EnvironmentRuntime::RebuildLights(ctx);
        if (addToSelection_) { ctx.selection.Add(object_.id); }
        else { ctx.selection.Replace(object_.id); }
        ctx.document.SetDirty(true);
        return true;
    }

    const nlohmann::json objectJson = EditorSceneDocument::ObjectToJson(object_);
    SceneObjectRegistry objectRegistry = SceneObjectRegistry::CreateWithBuiltins();
    if (!objectRegistry.Has(object_.type))
    {
        if (!IsDocumentOnlyObject(object_.type))
        {
            return false;
        }

        previousSelection_ = ctx.selection;
        if (object_.id.value == 0)
        {
            object_.id = ctx.document.AllocateId();
        }
        ctx.document.Add(object_);
        if (addToSelection_) { ctx.selection.Add(object_.id); }
        else { ctx.selection.Replace(object_.id); }
        ctx.document.SetDirty(true);
        return true;
    }

    SceneObjectRegistry::CreationContext creationCtx{ ctx.scene };
    SceneObjectRegistry::ObjectList runtimeObjects;
    try
    {
        runtimeObjects = objectRegistry.Create(object_.type, creationCtx, objectJson);
    }
    catch (const std::exception&)
    {
        return false;
    }
    if (runtimeObjects.empty())
    {
        return false;
    }

    previousSelection_ = ctx.selection;
    if (object_.id.value == 0)
    {
        object_.id = ctx.document.AllocateId();
    }
    ctx.document.Add(object_);

    ctx.renderer.WaitForPreviousFrame();
    UploadBatch uploads;
    if (!uploads.Begin(&ctx.renderer))
    {
        ctx.document.Remove(object_.id);
        return false;
    }

    bool addedAny = false;
    for (std::unique_ptr<RenderableObjectBase>& runtime : runtimeObjects)
    {
        if (!runtime)
        {
            continue;
        }

        runtime->SetVisible(object_.enabled);
        if (!ctx.scene.AddInitializedEditorObject(
                ctx.renderer, uploads, object_.id.value, std::move(runtime)))
        {
            ctx.scene.RemoveEditorObject(object_.id.value);
            ctx.document.Remove(object_.id);
            return false;
        }
        addedAny = true;
    }

    if (!addedAny)
    {
        ctx.document.Remove(object_.id);
        return false;
    }

    uploads.SubmitAndWait(&ctx.renderer);
    if (addToSelection_) { ctx.selection.Add(object_.id); }
    else { ctx.selection.Replace(object_.id); }
    ctx.document.SetDirty(true);
    return true;
}

void PasteObjectCommand::Undo(EditorContext& ctx)
{
    ctx.renderer.WaitForPreviousFrame();
    if (isEnvironment_)
    {
        std::vector<EditorObject>& environment = ctx.document.Environment();
        for (auto it = environment.begin(); it != environment.end(); ++it)
        {
            if (it->id.value == object_.id.value)
            {
                environment.erase(it);
                break;
            }
        }
        EnvironmentRuntime::RebuildLights(ctx);
    }
    else
    {
        ctx.scene.RemoveEditorObject(object_.id.value);
        ctx.document.Remove(object_.id);
    }
    ctx.selection = previousSelection_;
    ctx.document.SetDirty(true);
}

#endif // WITH_EDITOR
