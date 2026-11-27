#include "editor/intent/EditorActionBuilders.h"
#if WITH_EDITOR

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "editor/EditorContext.h"
#include "editor/intent/EditorIntentResolver.h"
#include "editor/assets/AssetRegistry.h"
#include "editor/scene/EditorZone.h"
#include "editor/commands/SetCastShadowCommand.h"
#include "editor/commands/SetMaterialCommand.h"
#include "editor/commands/SetMaterialSlotCommand.h"
#include "editor/ui/InspectorMultiEdit.h"

// R10.3: the editor's actions -- how an object looks: colour, surface, material, cast shadow, the live material params.
// Cut out of EditorActionRegistry.cpp; the table that names them is still there (EditorActionBuilders.h).

namespace editoractions
{
    // ------------------------------------------------------------------ setCastShadow

    std::unique_ptr<EditorCommand> BuildSetCastShadow(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        const bool castShadow = BoolOr(intent.params, "enabled", true);
        std::vector<std::unique_ptr<EditorCommand>> commands;
        std::size_t skipped = 0;
        for (const EditorObjectId id : targets)
        {
            const EditorObject* obj = actionCtx.editor.document.Find(id);
            if (!obj || !SetCastShadowCommand::AppliesTo(*obj))
            {
                ++skipped; // lights, zones, water, particles: nothing that casts a map shadow
                continue;
            }
            commands.push_back(std::make_unique<SetCastShadowCommand>(id, castShadow));
        }
        if (commands.empty())
        {
            outStatus = "Nothing that casts a shadow: Cast Shadow is for meshes";
            return nullptr;
        }
        const std::size_t count = commands.size();
        outStatus = (castShadow ? "Casting shadows: " : "No shadows from ") + CountedObjects(count);
        if (skipped > 0) { outStatus += " (" + CountedObjects(skipped) + " skipped: not meshes)"; }
        return FoldIntoOneEntry(std::move(commands),
            std::string(castShadow ? "Cast Shadow On for " : "Cast Shadow Off for ") + std::to_string(count) + " Objects");
    }

    // ---------------------------------------------------------------- live material params

    // A per-object material value -- `baseColor`, `metalRough`, `useMR` -- edited the way the
    // Inspector edits it. EditObjectPropertiesCommand writes the DOCUMENT only, and the runtime
    // reads these keys when the object is CREATED (SceneObjectFactory), so an action built on it
    // changed nothing on screen until the level was reloaded: setColor shipped that way, its
    // comment claiming an "inspector live path" that nothing called. This IS that path:
    // InspectorMultiEdit::ApplySnapshot writes the properties and the runtime MaterialParams
    // together, and Undo applies the snapshot captured before -- runtime values included, so a
    // key the object never authored still comes back as it was.
    class LiveObjectEditCommand final : public EditorCommand
    {
    public:
        LiveObjectEditCommand(InspectorMultiEdit::Snapshot before,
            InspectorMultiEdit::Snapshot after, std::string label)
            : before_(std::move(before)), after_(std::move(after)), label_(std::move(label))
        {
        }
        bool Execute(EditorContext& ctx) override
        {
            InspectorMultiEdit::ApplySnapshot(ctx, after_);
            return true;
        }
        void Undo(EditorContext& ctx) override { InspectorMultiEdit::ApplySnapshot(ctx, before_); }
        std::string_view HistoryLabel() const override { return label_; }

    private:
        InspectorMultiEdit::Snapshot before_;
        InspectorMultiEdit::Snapshot after_;
        std::string label_;
    };

    // Null when `patch` changes nothing -- an entry Ctrl+Z appears to ignore is not an edit.
    std::unique_ptr<EditorCommand> MakeLiveObjectEdit(EditorContext& ctx,
        const EditorObject& object, const nlohmann::json& patch, const char* label)
    {
        InspectorMultiEdit::Snapshot before = InspectorMultiEdit::Capture(ctx, object, false);
        InspectorMultiEdit::Snapshot after = before;
        if (!after.object.properties.is_object())
        {
            after.object.properties = nlohmann::json::object();
        }
        for (auto it = patch.begin(); it != patch.end(); ++it)
        {
            after.object.properties[it.key()] = *it;
            after.authored[it.key()] = *it;
            after.effective[it.key()] = *it;
        }
        if (after.object.properties == before.object.properties)
        {
            return nullptr;
        }
        return std::make_unique<LiveObjectEditCommand>(std::move(before), std::move(after), label);
    }

    // ---------------------------------------------------------------- setColor

    // THE ONE GENUINE GAP IN THE REFUSAL LOG. "покрась пальмы в ярко-красный" was answered
    // needs_api twice, and the model's reasoning was right both times: `replace` swaps the
    // whole mesh and `setMaterial` swaps the whole material, and neither is "the same tree,
    // red". The engine could already do it -- MaterialParams::baseColor multiplies the
    // albedo and reaches the shader every frame -- but nothing could mark it as authored
    // per object, so no level could carry the value and no action could set it.
    //
    // The property is the level's own JSON; MakeLiveObjectEdit puts it on screen in the same
    // frame and Ctrl+Z puts the old colour back.
    std::unique_ptr<EditorCommand> BuildSetColor(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        EditorContext& ctx = actionCtx.editor;
        if (targets.empty())
        {
            outStatus = "Nothing to colour";
            return nullptr;
        }

        const auto colourIt = intent.params.find("color");
        Math::float3 rgb(1.0f, 1.0f, 1.0f);
        if (colourIt == intent.params.end() || !colourIt->is_array() || colourIt->size() < 3 ||
            !(*colourIt)[0].is_number())
        {
            outStatus = "setColor needs a color as [r, g, b], each 0..1";
            return nullptr;
        }
        rgb = Math::float3(std::clamp((*colourIt)[0].get<float>(), 0.0f, 1.0f),
            std::clamp((*colourIt)[1].get<float>(), 0.0f, 1.0f),
            std::clamp((*colourIt)[2].get<float>(), 0.0f, 1.0f));

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(targets.size());
        for (const EditorObjectId id : targets)
        {
            const EditorObject* object = ctx.document.Find(id);
            if (!object)
            {
                continue;
            }
            const nlohmann::json patch = {
                { "baseColor", nlohmann::json::array({ rgb.x, rgb.y, rgb.z }) } };
            if (std::unique_ptr<EditorCommand> edit =
                    MakeLiveObjectEdit(ctx, *object, patch, "Set Colour"))
            {
                commands.push_back(std::move(edit));
            }
        }
        if (commands.empty())
        {
            outStatus = "They are that colour already";
            return nullptr;
        }
        outStatus = "Coloured " + std::to_string(commands.size()) + " objects";
        return FoldIntoOneEntry(std::move(commands), "Set Colour");
    }

    // ---------------------------------------------------------------- setSurface

    // HOW SHINY, per object: metalness and roughness as flat values. The bloom work needed it
    // first (2026-10-15) -- whether a highlight throws the kernel's star depends on how sharp the
    // reflected sun is, and no material in the project was smooth enough to show it. A flat
    // value means the MR texture is switched off for that object (`useMR` false), which is the
    // only way the numbers mean what they say; the status line says so. Either key may be left
    // out and keeps the object's current value.
    std::unique_ptr<EditorCommand> BuildSetSurface(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        EditorContext& ctx = actionCtx.editor;
        if (targets.empty())
        {
            outStatus = "Nothing to change";
            return nullptr;
        }
        const bool hasMetal = intent.params.contains("metal") && intent.params["metal"].is_number();
        const bool hasRough =
            intent.params.contains("roughness") && intent.params["roughness"].is_number();
        // The normal map's strength: a mirror with its material's bumps on it shatters the reflected
        // sun into noise (a chrome sphere's glint fell from ~1500 to 12 that way), so a clean
        // reflector needs 0 here.
        const bool hasNormal =
            intent.params.contains("normalStrength") && intent.params["normalStrength"].is_number();
        // A flat colour in place of the albedo texture: a metal's reflection takes its colour from the
        // albedo, so a "chrome" over a bronze texture reflects the blue sky as near-black orange.
        const auto albedoIt = intent.params.find("albedo");
        const bool hasAlbedo = albedoIt != intent.params.end() && albedoIt->is_array() &&
            albedoIt->size() == 3 && (*albedoIt)[0].is_number() && (*albedoIt)[1].is_number() &&
            (*albedoIt)[2].is_number();
        // A tint: the colour multiplied INTO the albedo texture (the same baseColor, the texture kept on) -- a white-albedo
        // mesh (ocean_buoy_white) takes it with its detail.
        const auto tintIt = intent.params.find("tint");
        const bool hasTint = tintIt != intent.params.end() && tintIt->is_array() && tintIt->size() == 3 && (*tintIt)[0].is_number() &&
            (*tintIt)[1].is_number() && (*tintIt)[2].is_number();
        if (hasTint && hasAlbedo)
        {
            outStatus = "setSurface takes albedo (a flat colour instead of the texture) or tint (a colour times the texture), not both";
            return nullptr;
        }
        if (!hasMetal && !hasRough && !hasNormal && !hasAlbedo && !hasTint)
        {
            outStatus = "setSurface needs metal, roughness, normalStrength, albedo and/or tint";
            return nullptr;
        }

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(targets.size());
        bool droppedTexture = false;
        for (const EditorObjectId id : targets)
        {
            const EditorObject* object = ctx.document.Find(id);
            if (!object || object->type != "staticMesh")
            {
                continue;
            }
            // The current pair, from the runtime when there is one (Capture), else the authored
            // key, else the defaults a placed mesh gets (ContentBrowserPanel: 0 / 0.5).
            const InspectorMultiEdit::Snapshot now = InspectorMultiEdit::Capture(ctx, *object, false);
            float metal = 0.0f;
            float rough = 0.5f;
            const auto current = now.effective.find("metalRough");
            if (current != now.effective.end() && current->is_array() && current->size() == 2)
            {
                metal = (*current)[0].get<float>();
                rough = (*current)[1].get<float>();
            }
            nlohmann::json patch = nlohmann::json::object();
            if (hasMetal || hasRough)
            {
                const auto useMR = now.effective.find("useMR");
                droppedTexture |= useMR == now.effective.end() || !useMR->is_boolean() || useMR->get<bool>();
                if (hasMetal) { metal = std::clamp(intent.params["metal"].get<float>(), 0.0f, 1.0f); }
                if (hasRough) { rough = std::clamp(intent.params["roughness"].get<float>(), 0.0f, 1.0f); }
                patch["metalRough"] = nlohmann::json::array({ metal, rough });
                patch["useMR"] = false;
            }
            if (hasNormal)
            {
                patch["normalStrength"] = std::clamp(intent.params["normalStrength"].get<float>(), 0.0f, 4.0f);
            }
            if (hasAlbedo)
            {
                patch["baseColor"] = nlohmann::json::array({
                    std::clamp((*albedoIt)[0].get<float>(), 0.0f, 1.0f),
                    std::clamp((*albedoIt)[1].get<float>(), 0.0f, 1.0f),
                    std::clamp((*albedoIt)[2].get<float>(), 0.0f, 1.0f) });
                patch["useAlbedo"] = false;
            }
            if (hasTint)
            {
                patch["baseColor"] = nlohmann::json::array({
                    std::clamp((*tintIt)[0].get<float>(), 0.0f, 1.0f),
                    std::clamp((*tintIt)[1].get<float>(), 0.0f, 1.0f),
                    std::clamp((*tintIt)[2].get<float>(), 0.0f, 1.0f) });
                patch["useAlbedo"] = true;
            }
            if (std::unique_ptr<EditorCommand> edit =
                    MakeLiveObjectEdit(ctx, *object, patch, "Set Surface"))
            {
                commands.push_back(std::move(edit));
            }
        }
        if (commands.empty())
        {
            outStatus = "They have that surface already";
            return nullptr;
        }
        outStatus = "Surface of " + std::to_string(commands.size()) + " objects set";
        if (droppedTexture)
        {
            outStatus += " (their metal/rough TEXTURE is off now: flat values)";
        }
        return FoldIntoOneEntry(std::move(commands), "Set Surface");
    }

    // ------------------------------------------------------------- setMaterial

    std::unique_ptr<EditorCommand> BuildSetMaterial(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        const std::string material = StringOr(intent.params, "material", "");
        if (material.empty())
        {
            outStatus = "setMaterial needs a material to apply";
            return nullptr;
        }
        if (targets.empty())
        {
            outStatus = "Nothing to apply a material to";
            return nullptr;
        }
        // Refuse a material the level does not have, by name, rather than writing a
        // dangling reference that only shows up as a missing-asset error later (E5: the
        // resolver answers by name, the model never picks from a closed list).
        if (!actionCtx.assets.FindById({ EditorAssetType::MaterialPreset, material }) &&
            !actionCtx.assets.FindByPath(material))
        {
            outStatus = "Unknown material '" + material + "'";
            return nullptr;
        }

        // A slot is optional: without one the object's whole material is replaced, with one
        // only that slot changes. Two commands exist for exactly this distinction.
        const bool hasSlot = intent.params.contains("slot");
        const int slot = hasSlot
            ? std::max(0, static_cast<int>(NumberOr(intent.params, "slot", 0.0f))) : 0;

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(targets.size());
        for (const EditorObjectId id : targets)
        {
            if (hasSlot)
            {
                commands.push_back(std::make_unique<SetMaterialSlotCommand>(id, slot, material));
            }
            else
            {
                commands.push_back(std::make_unique<SetMaterialCommand>(id, material));
            }
        }

        outStatus = "Set " + CountedObjects(targets.size()) + " to material '" + material + "'" +
            (hasSlot ? " (slot " + std::to_string(slot) + ")" : "");
        return FoldIntoOneEntry(std::move(commands),
            "Set Material on " + std::to_string(commands.size()) + " Objects");
    }
}

#endif // WITH_EDITOR
