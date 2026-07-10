#pragma once
#if WITH_EDITOR

#include <string>

#include "editor/scene/EditorSceneDocument.h"

// A delete request raised by the outliner; the editor turns it into a command.
struct OutlinerAction
{
    enum class Type
    {
        None,
        DeleteObject,
        DuplicateObject,
        FrameSelection,
        RenameObject,
        SetEnabled,
        SetEnvEnabled
    };
    Type type = Type::None;
    EditorObjectId target;
    bool enabledValue = false; // for SetEnabled / SetEnvEnabled
    std::string nameValue; // for RenameObject
};

// Lists document and environment entities in a searchable/filterable table.
// Selecting a row writes the editor's selected object; row controls raise
// actions that the editor turns into commands or live environment updates.
class SceneOutlinerPanel
{
public:
    struct PersistentState
    {
        bool meshesGroupOpen = true;
        bool lightsGroupOpen = true;
        bool camerasGroupOpen = true;
        bool environmentGroupOpen = true;
        bool otherGroupOpen = true;
    };

    // Draws the panel as its own ImGui window. `open` backs the window's close
    // button (the editor owns it).
    OutlinerAction Draw(EditorSceneDocument& document, EditorObjectId& selectedObject, bool* open);

    PersistentState GetPersistentState() const;
    void SetPersistentState(const PersistentState& state);

private:
    char searchBuffer_[256] = {};
    bool showObjects_ = true;
    bool showEnvironment_ = true;
    bool meshesGroupOpen_ = true;
    bool lightsGroupOpen_ = true;
    bool camerasGroupOpen_ = true;
    bool environmentGroupOpen_ = true;
    bool otherGroupOpen_ = true;
    int typeFilterIndex_ = 0;
    EditorObjectId renamingObject_{};
    char renameBuffer_[256] = {};
    bool renameFocusRequested_ = false;
};

#endif // WITH_EDITOR
