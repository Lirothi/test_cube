#pragma once
#if WITH_EDITOR

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h" // EditorObjectId

// UE's Cast Shadow (UPrimitiveComponent::CastShadow) on a mesh: the document's "castShadow" property -- written only
// while OFF, so a level that never switched a shadow off saves byte-identical -- and the runtime object's flag, which
// every shadow path reads (the map passes, the GPU caster set, VSM, the RT shadow rays). A change of a visible object's
// flag changes the caster set, so it asks for the same rebuild a visibility change does. Undoable; Undo restores what
// the object had before, not the opposite of the request.
class SetCastShadowCommand : public EditorCommand
{
public:
    SetCastShadowCommand(EditorObjectId id, bool castShadow);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;
    std::string_view HistoryLabel() const override
    {
        return castShadow_ ? "Enable Cast Shadow" : "Disable Cast Shadow";
    }

    // The object types the flag means something for (meshes: one or a GPU-instanced field).
    static bool AppliesTo(const EditorObject& obj)
    {
        return obj.type == "staticMesh" || obj.type == "instancedModels";
    }
    // What the document says (absent = casts).
    static bool DocumentCastsShadow(const EditorObject& obj);

private:
    static void Apply(EditorContext& ctx, EditorObjectId id, bool castShadow);

    EditorObjectId id_;
    bool castShadow_;
    bool before_ = true;
};

#endif // WITH_EDITOR
