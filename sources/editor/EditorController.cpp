#include "editor/EditorController.h"
#if WITH_EDITOR

#include <memory>
#include <string>

#include "editor/EditorContext.h"
#include "editor/commands/DeleteObjectCommand.h"
#include "editor/commands/SpawnMeshCommand.h"
#include "imgui.h"

namespace
{
    // damaged_plaster if present, else the first material preset, else "".
    std::string PickDefaultStaticMaterial(const AssetRegistry& registry)
    {
        const EditorAssetRecord* first = nullptr;
        for (const EditorAssetRecord& rec : registry.Assets())
        {
            if (rec.id.type != EditorAssetType::MaterialPreset)
            {
                continue;
            }
            if (rec.id.key == "damaged_plaster")
            {
                return "damaged_plaster";
            }
            if (!first)
            {
                first = &rec;
            }
        }
        return first ? first->id.key : std::string{};
    }
}

void EditorController::Draw(Renderer& renderer, Scene& scene, LevelManager& levelManager)
{
    if (!open_)
    {
        return;
    }

    // Lazy first-open init: scan assets and load the active level's object
    // metadata once. Neither touches the runtime scene.
    if (!firstOpenInitialized_)
    {
        assetRegistry_.Refresh();
        document_.LoadFromLevelFile("data/levels/demo.json");
        firstOpenInitialized_ = true;
    }

    EditorContext ctx{ renderer, scene, levelManager, document_, selectedObject_ };

    // Main editor window: status, undo/redo, and per-window visibility toggles.
    // Closing it (its X) closes the whole editor interface.
    ImGui::SetNextWindowSize(ImVec2(360.0f, 180.0f), ImGuiCond_FirstUseEver);
    bool open = open_;
    if (ImGui::Begin("Level Editor###LevelEditor", &open))
    {
        const std::string& levelPath = document_.LevelPath();
        ImGui::Text("Level: %s", levelPath.empty() ? "(none)" : levelPath.c_str());
        ImGui::Text("Document objects: %d", static_cast<int>(document_.Objects().size()));

        ImGui::BeginDisabled(!commandStack_.CanUndo());
        if (ImGui::Button("Undo")) { commandStack_.Undo(ctx); }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!commandStack_.CanRedo());
        if (ImGui::Button("Redo")) { commandStack_.Redo(ctx); }
        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::TextUnformatted("Windows");
        ImGui::Checkbox("Content Browser", &showContentBrowser_);
        ImGui::Checkbox("Scene Outliner", &showOutliner_);
    }
    ImGui::End();
    open_ = open;

    // Each panel is its own window, drawn only while the editor is open and its
    // visibility toggle is on. Panels draw their own window and return an action.
    if (showOutliner_)
    {
        const OutlinerAction outlinerAction = outliner_.Draw(document_, selectedObject_, &showOutliner_);
        if (outlinerAction.type == OutlinerAction::Type::DeleteObject)
        {
            commandStack_.Execute(ctx, std::make_unique<DeleteObjectCommand>(outlinerAction.target));
        }
    }

    if (showContentBrowser_)
    {
        const ContentBrowserAction action = contentBrowser_.Draw(assetRegistry_, selectedAsset_, &showContentBrowser_);
        if (action.type == ContentBrowserAction::Type::SpawnStaticMesh)
        {
            commandStack_.Execute(ctx, std::make_unique<SpawnMeshCommand>(
                SpawnMeshCommand::Kind::StaticMesh, action.asset.key, PickDefaultStaticMaterial(assetRegistry_)));
        }
        else if (action.type == ContentBrowserAction::Type::SpawnTransparentMesh)
        {
            commandStack_.Execute(ctx, std::make_unique<SpawnMeshCommand>(
                SpawnMeshCommand::Kind::TransparentMesh, action.asset.key, std::string{}));
        }
    }
}

#endif // WITH_EDITOR
