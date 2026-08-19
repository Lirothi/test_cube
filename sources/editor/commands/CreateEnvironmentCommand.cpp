#include "editor/commands/CreateEnvironmentCommand.h"
#if WITH_EDITOR

#include <utility>
#include <vector>

#include "editor/EditorContext.h"
#include "editor/scene/EnvironmentRuntime.h"
#include "rendering/core/Renderer.h"

namespace
{
    bool IsLight(const EditorObject& object)
    {
        return object.type == "pointLight" || object.type == "spotLight";
    }

    bool IsSingletonEnvironment(const EditorObject& object)
    {
        return object.type == "directionalLight" ||
            object.type == "skybox" ||
            object.type == "camera" ||
            object.type == "cameraExposure" || // P1
            object.type == "colorPipeline" ||  // P3C
            object.type == "gtao" ||           // P6B
            object.type == "atmosphere";       // P7
    }

    bool DocumentHasOtherOfType(const EditorSceneDocument& document, const EditorObject& object)
    {
        for (const EditorObject& env : document.Environment())
        {
            if (env.type == object.type && env.id.value != object.id.value)
            {
                return true;
            }
        }
        return false;
    }
}

CreateEnvironmentCommand::CreateEnvironmentCommand(EditorObject object)
    : object_(std::move(object))
{
}

bool CreateEnvironmentCommand::Execute(EditorContext& ctx)
{
    if (object_.type == "ocean" || object_.type.empty())
    {
        return false;
    }

    if (!built_)
    {
        object_.id = ctx.document.AllocateId();
        built_ = true;
    }
    if (IsSingletonEnvironment(object_) && DocumentHasOtherOfType(ctx.document, object_))
    {
        return false;
    }

    previousSelection_ = ctx.selection;

    ctx.renderer.WaitForPreviousFrame();
    ctx.document.Environment().push_back(object_);
    if (IsLight(object_))
    {
        EnvironmentRuntime::RebuildLights(ctx);
    }
    else
    {
        EnvironmentRuntime::Apply(ctx, object_);
    }

    ctx.selection.Replace(object_.id);
    ctx.document.SetDirty(true);
    return true;
}

void CreateEnvironmentCommand::Undo(EditorContext& ctx)
{
    ctx.renderer.WaitForPreviousFrame();

    std::vector<EditorObject>& environment = ctx.document.Environment();
    for (auto it = environment.begin(); it != environment.end(); ++it)
    {
        if (it->id.value == object_.id.value)
        {
            environment.erase(it);
            break;
        }
    }

    EnvironmentRuntime::Remove(ctx, object_);

    ctx.selection = previousSelection_;
    ctx.document.SetDirty(true);
}

#endif // WITH_EDITOR
