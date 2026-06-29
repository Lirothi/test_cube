#include "editor/EditorController.h"
#if WITH_EDITOR

#include <memory>
#include <string>

#include "editor/EditorContext.h"
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

    ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_FirstUseEver);

    bool open = open_;
    if (ImGui::Begin("Level Editor###LevelEditor", &open))
    {
        EditorContext ctx{ renderer, scene, levelManager, document_, selectedObject_ };

        const std::string& levelPath = document_.LevelPath();
        ImGui::Text("Level: %s | Document objects: %d",
            levelPath.empty() ? "(none)" : levelPath.c_str(),
            static_cast<int>(document_.Objects().size()));

        // Undo/redo are wired to the command stack now; commands that fill it
        // arrive in later steps, so these stay disabled until then.
        ImGui::BeginDisabled(!commandStack_.CanUndo());
        if (ImGui::Button("Undo")) { commandStack_.Undo(ctx); }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!commandStack_.CanRedo());
        if (ImGui::Button("Redo")) { commandStack_.Redo(ctx); }
        ImGui::EndDisabled();

        ImGui::Separator();

        const ContentBrowserAction action = contentBrowser_.Draw(assetRegistry_, selectedAsset_);
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
    ImGui::End();

    open_ = open;
}

#endif // WITH_EDITOR
