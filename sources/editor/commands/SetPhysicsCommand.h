#pragma once
#if WITH_EDITOR

#include <memory>
#include <string>
#include <vector>

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h" // EditorObject, EditorObjectId
#include "physics/PhysicsTypes.h"

// A mesh's place in the physics world (docs/physics_plan.md): the document's "physics" key -- written only when it is not Static,
// the default -- and "mass", the kilograms that win over the asset's density (absent = from the density), and the runtime object.
// Undoable; Undo restores what the object had before, not the opposite of the request.
class SetPhysicsCommand : public EditorCommand
{
public:
    SetPhysicsCommand(EditorObjectId id, physics::Motion motion, float mass);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;
    std::string_view HistoryLabel() const override { return "Set Physics"; }

    static bool AppliesTo(const EditorObject& obj) { return obj.type == "staticMesh"; }
    static physics::Motion DocumentMotion(const EditorObject& obj);
    static float DocumentMass(const EditorObject& obj);

private:
    static void Apply(EditorContext& ctx, EditorObjectId id, physics::Motion motion, float mass);

    EditorObjectId id_;
    physics::Motion motion_;
    float mass_;
    physics::Motion beforeMotion_ = physics::Motion::Static;
    float beforeMass_ = 0.0f;
};

// UE's Keep Simulation Changes: the bodies' poses become the objects' authored transforms, one undo entry. `only` null = every body
// that moved; else those of these objects. Null when nothing has moved, and `outStatus` says so.
std::unique_ptr<EditorCommand> BuildKeepSimulationCommand(EditorContext& ctx, const std::vector<EditorObjectId>* only, std::string& outStatus);

#endif // WITH_EDITOR
