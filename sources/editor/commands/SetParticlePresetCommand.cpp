#include "editor/commands/SetParticlePresetCommand.h"
#if WITH_EDITOR

#include <memory>
#include <utility>

#include "app/scene/Scene.h"
#include "app/scene/SceneObjectFactory.h"
#include "editor/EditorContext.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/renderables/RenderableObjectBase.h"

namespace
{
    void ClearParticleSettings(nlohmann::json& properties)
    {
        // A preset supplies all these fields. Removing their old inline and override values makes
        // assigning a preset deterministic rather than silently retaining the previous emitter.
        static constexpr const char* kEmitterSettings[] = {
            "maxParticles", "spawnRate", "lifetime", "coneDir", "coneAngleDeg",
            "speed", "gravity", "drag", "windInfluence", "seed", "rotation", "spin",
            "size", "texture", "additive", "softFade", "sort", "localSpace",
            "flipCols", "flipRows", "flipFps", "flipRandomStart", "frameBlend", "colorKeys"
        };
        for (const char* key : kEmitterSettings)
        {
            properties.erase(key);
        }
        properties.erase("overrides");
    }
}

SetParticlePresetCommand::SetParticlePresetCommand(EditorObjectId id, std::string preset)
    : id_(id)
    , newPreset_(std::move(preset))
{
}

bool SetParticlePresetCommand::Apply(EditorContext& ctx, const nlohmann::json& properties)
{
    EditorObject* obj = ctx.document.Find(id_);
    if (!obj || obj->type != "particleEmitter")
    {
        return false;
    }

    obj->properties = properties;
    ctx.document.SetDirty(true);

    if (ctx.scene.FindEditorObject(id_.value) == nullptr)
    {
        return true;
    }

    const nlohmann::json json = EditorSceneDocument::ObjectToJson(*obj);
    std::unique_ptr<RenderableObjectBase> runtime = SceneObjectFactory::CreateParticleEmitterFromJson(json);
    if (!runtime)
    {
        return false;
    }

    ctx.renderer.WaitForPreviousFrame();
    ctx.scene.RemoveEditorObject(id_.value);
    UploadBatch uploads;
    if (uploads.Begin(&ctx.renderer))
    {
        ctx.scene.AddInitializedEditorObject(ctx.renderer, uploads, id_.value, std::move(runtime));
        uploads.SubmitAndWait(&ctx.renderer);
    }
    return true;
}

bool SetParticlePresetCommand::Execute(EditorContext& ctx)
{
    if (!captured_)
    {
        const EditorObject* obj = ctx.document.Find(id_);
        if (!obj || obj->type != "particleEmitter")
        {
            return false;
        }

        oldProperties_ = obj->properties;
        newProperties_ = oldProperties_;
        ClearParticleSettings(newProperties_);
        newProperties_["preset"] = newPreset_;
        captured_ = true;
    }
    return Apply(ctx, newProperties_);
}

void SetParticlePresetCommand::Undo(EditorContext& ctx)
{
    Apply(ctx, oldProperties_);
}

#endif // WITH_EDITOR
