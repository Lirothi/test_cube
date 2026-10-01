#include "editor/commands/SetMaterialCommand.h"
#if WITH_EDITOR

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "app/scene/Scene.h"
#include "app/scene/SceneObjectFactory.h"
#include "editor/EditorContext.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/renderables/GBufferRenderable.h"
#include "rendering/renderables/RenderableObjectBase.h"

SetMaterialCommand::SetMaterialCommand(EditorObjectId id, std::string material)
    : id_(id)
    , material_(std::move(material))
{
}

bool SetMaterialCommand::Apply(EditorContext& ctx, const Key& material, const Key& materials)
{
    EditorObject* obj = ctx.document.Find(id_);
    if (!obj)
    {
        return false;
    }

    const auto write = [obj](const char* name, const Key& key)
    {
        if (key.present)
        {
            obj->properties[name] = key.value;
        }
        else
        {
            obj->properties.erase(name);
        }
    };
    write("material", material);
    write("materials", materials);
    ctx.document.SetDirty(true);

    // Respawn only if this object has a live editor-owned runtime.
    if (ctx.scene.FindEditorObject(id_.value) != nullptr)
    {
        const nlohmann::json json = EditorSceneDocument::ObjectToJson(*obj);
        std::unique_ptr<RenderableObjectBase> runtime = SceneObjectFactory::CreateStaticMeshFromJson(json);
        if (runtime)
        {
            ctx.renderer.WaitForPreviousFrame();
            ctx.scene.RemoveEditorObject(id_.value);
            UploadBatch uploads;
            if (uploads.Begin(&ctx.renderer))
            {
                ctx.scene.AddInitializedEditorObject(ctx.renderer, uploads, id_.value, std::move(runtime),
                    obj->enabled);
                uploads.SubmitAndWait(&ctx.renderer);
            }
        }
    }
    return true;
}

bool SetMaterialCommand::Execute(EditorContext& ctx)
{
    if (!captured_)
    {
        const EditorObject* obj = ctx.document.Find(id_);
        if (!obj)
        {
            return false;
        }
        const auto read = [obj](const char* name)
        {
            auto it = obj->properties.find(name);
            return it != obj->properties.end() ? Key{ true, *it } : Key{};
        };
        oldMaterial_ = read("material");
        oldMaterials_ = read("materials");

        // "The whole object" is every slot it draws with. The scalar alone reached slot 0 only:
        // once the object has a `materials` list -- its own, or one inherited from the mesh asset
        // (the palms, the tent, the boat) -- the factory hands that list to the slots and the
        // scalar is not read at all, so assigning a material changed the document and nothing on
        // screen. A glTF with several submeshes and no list kept "auto" past slot 0 the same way.
        // So the list is written too, one entry per slot: the most the live runtime, the object's
        // own list or the asset's list knows of.
        const nlohmann::json effective =
            SceneObjectFactory::ResolveMeshAsset(EditorSceneDocument::ObjectToJson(*obj));
        const auto listIt = effective.find("materials");
        const bool hasList = listIt != effective.end() && listIt->is_array();
        size_t slots = hasList ? std::max<size_t>(listIt->size(), 1u) : 1u;
        if (RenderableObjectBase* runtime = ctx.scene.FindEditorObject(id_.value))
        {
            if (const GBufferRenderable* gb = runtime->AsGBufferRenderable())
            {
                slots = std::max(slots, gb->SlotCount());
            }
        }

        newMaterial_ = Key{ true, material_ };
        newMaterials_ = (hasList || slots > 1u)
            ? Key{ true, nlohmann::json(std::vector<std::string>(slots, material_)) }
            : oldMaterials_;
        captured_ = true;
    }
    return Apply(ctx, newMaterial_, newMaterials_);
}

void SetMaterialCommand::Undo(EditorContext& ctx)
{
    Apply(ctx, oldMaterial_, oldMaterials_);
}

#endif // WITH_EDITOR
