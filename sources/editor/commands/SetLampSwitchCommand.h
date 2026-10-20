#pragma once
#if WITH_EDITOR

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h" // EditorObjectId
#include "rendering/lighting/NightLamps.h"

// Plan L: when a mesh's emissive glows -- the document's "onAt" ("night" / "day"; absent = always, so a level that
// never switched anything saves byte-identical) and the runtime object's switch, which NightLamps drives from the
// next frame. Lights take the same key through EditEnvironmentCommand (their runtime is rebuilt from the
// document). Undoable; Undo restores what the object had before.
class SetLampSwitchCommand : public EditorCommand
{
public:
    SetLampSwitchCommand(EditorObjectId id, LampSwitch lampSwitch);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;
    std::string_view HistoryLabel() const override { return "Set Glow On At"; }

    static void Apply(EditorContext& ctx, EditorObjectId id, LampSwitch lampSwitch);

private:
    EditorObjectId id_;
    LampSwitch lampSwitch_;
    LampSwitch before_ = LampSwitch::Always;
};

#endif // WITH_EDITOR
