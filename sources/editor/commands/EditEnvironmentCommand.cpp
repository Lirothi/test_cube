#include "editor/commands/EditEnvironmentCommand.h"
#if WITH_EDITOR

#include <utility>

#include "editor/EditorContext.h"
#include "editor/scene/EnvironmentRuntime.h"

EditEnvironmentCommand::EditEnvironmentCommand(
    EditorObjectId id,
    nlohmann::json beforeProperties,
    nlohmann::json afterProperties,
    std::string historyLabel)
    : id_(id)
    , beforeProperties_(std::move(beforeProperties))
    , afterProperties_(std::move(afterProperties))
    , historyLabel_(std::move(historyLabel))
{
}

bool EditEnvironmentCommand::Apply(
    EditorContext& ctx,
    const nlohmann::json& properties)
{
    for (EditorObject& environment : ctx.document.Environment())
    {
        if (environment.id.value != id_.value)
        {
            continue;
        }

        const EditorObject previous = environment;
        environment.properties = properties;
        EnvironmentRuntime::ApplyChange(ctx, environment, previous);
        ctx.document.SetDirty(true);
        return true;
    }
    return false;
}

bool EditEnvironmentCommand::Execute(EditorContext& ctx)
{
    return beforeProperties_ != afterProperties_ && Apply(ctx, afterProperties_);
}

void EditEnvironmentCommand::Undo(EditorContext& ctx)
{
    Apply(ctx, beforeProperties_);
}

#endif // WITH_EDITOR
