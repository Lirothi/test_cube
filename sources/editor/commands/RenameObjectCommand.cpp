#include "editor/commands/RenameObjectCommand.h"
#if WITH_EDITOR

#include <algorithm>
#include <cctype>
#include <utility>

#include "editor/EditorContext.h"

namespace
{
    std::string TrimName(std::string name)
    {
        const auto isNotSpace = [](unsigned char ch)
        {
            return std::isspace(ch) == 0;
        };

        const auto first = std::find_if(name.begin(), name.end(), isNotSpace);
        if (first == name.end())
        {
            return {};
        }

        const auto last = std::find_if(name.rbegin(), name.rend(), isNotSpace).base();
        return std::string(first, last);
    }
}

RenameObjectCommand::RenameObjectCommand(
    EditorObjectId id,
    std::string beforeName,
    std::string afterName)
    : id_(id)
    , beforeName_(std::move(beforeName))
    , afterName_(TrimName(std::move(afterName)))
{
}

void RenameObjectCommand::Apply(EditorContext& ctx, const std::string& name)
{
    if (EditorObject* object = ctx.document.Find(id_))
    {
        object->name = name;
        ctx.document.SetDirty(true);
    }
}

bool RenameObjectCommand::Execute(EditorContext& ctx)
{
    if (afterName_.empty() || afterName_ == beforeName_ || !ctx.document.Find(id_))
    {
        return false;
    }

    Apply(ctx, afterName_);
    return true;
}

void RenameObjectCommand::Undo(EditorContext& ctx)
{
    Apply(ctx, beforeName_);
}

#endif // WITH_EDITOR
