#include "editor/commands/EditorCommandStack.h"
#if WITH_EDITOR

#include <utility>

#include "app/scene/Scene.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "editor/EditorContext.h"

namespace
{
    // After a command applies, rebuild the shadow-caster GPU data + mega VB/IB if (and only if) the
    // caster set actually changed (spawn/delete or a shadow-caster visibility toggle bump the version;
    // transform/rename/select do not). Skipping this leaves VSM's per-page draw in the ~10ms
    // per-group fallback (see Scene::RefreshShadowGpuForEditor). One choke point coalesces a whole
    // composite command and covers Execute/Undo/Redo.
    void RefreshShadowsIfCasterSetChanged(EditorContext& ctx, std::uint32_t versionBefore)
    {
        if (ctx.scene.GetStaticSetVersion() != versionBefore)
        {
            ctx.scene.RefreshShadowGpuForEditor(ctx.renderer);
        }
    }
}

bool EditorCommandStack::Execute(EditorContext& ctx, std::unique_ptr<EditorCommand> command)
{
    CPU_SCOPE(ProfilerScopes::kEditorCommandExecute);
    if (!command)
    {
        return false;
    }
    const std::uint32_t versionBefore = ctx.scene.GetStaticSetVersion();
    if (!command->Execute(ctx))
    {
        return false; // a command that fails to apply is not recorded
    }
    RefreshShadowsIfCasterSetChanged(ctx, versionBefore);

    history_.erase(
        history_.begin() + static_cast<std::ptrdiff_t>(appliedCount_),
        history_.end());
    history_.push_back(std::move(command));
    appliedCount_ = history_.size();
    return true;
}

void EditorCommandStack::Undo(EditorContext& ctx)
{
    CPU_SCOPE(ProfilerScopes::kEditorCommandUndo);
    if (!CanUndo())
    {
        return;
    }

    const std::uint32_t versionBefore = ctx.scene.GetStaticSetVersion();
    history_[appliedCount_ - 1]->Undo(ctx);
    --appliedCount_;
    RefreshShadowsIfCasterSetChanged(ctx, versionBefore);
}

void EditorCommandStack::Redo(EditorContext& ctx)
{
    CPU_SCOPE(ProfilerScopes::kEditorCommandRedo);
    if (!CanRedo())
    {
        return;
    }

    const std::uint32_t versionBefore = ctx.scene.GetStaticSetVersion();
    if (history_[appliedCount_]->Execute(ctx))
    {
        ++appliedCount_;
        RefreshShadowsIfCasterSetChanged(ctx, versionBefore);
    }
}

bool EditorCommandStack::MoveTo(EditorContext& ctx, std::size_t appliedCommandCount)
{
    CPU_SCOPE(ProfilerScopes::kEditorCommandMoveTo);
    if (appliedCommandCount > history_.size())
    {
        return false;
    }

    const std::size_t target = appliedCommandCount;
    while (appliedCount_ > target)
    {
        Undo(ctx);
    }
    while (appliedCount_ < target)
    {
        const std::size_t before = appliedCount_;
        Redo(ctx);
        if (appliedCount_ == before)
        {
            return false;
        }
    }
    return appliedCount_ == target;
}

const EditorCommand* EditorCommandStack::HistoryEntry(std::size_t index) const
{
    return index < history_.size() ? history_[index].get() : nullptr;
}

void EditorCommandStack::Clear()
{
    history_.clear();
    appliedCount_ = 0;
}

#endif // WITH_EDITOR
