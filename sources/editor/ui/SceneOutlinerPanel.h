#pragma once
#if WITH_EDITOR

#include "editor/scene/EditorSceneDocument.h"

// A delete request raised by the outliner; the editor turns it into a command.
struct OutlinerAction
{
    enum class Type { None, DeleteObject, SetEnabled };
    Type type = Type::None;
    EditorObjectId target;
    bool enabledValue = false; // for SetEnabled
};

// Lists the document's objects in a table. Selecting a row writes the editor's
// selected object; the enabled checkbox toggles EditorObject::enabled; the
// "Delete Selected" button raises a delete request.
class SceneOutlinerPanel
{
public:
    // Draws the panel as its own ImGui window. `open` backs the window's close
    // button (the editor owns it).
    OutlinerAction Draw(EditorSceneDocument& document, EditorObjectId& selectedObject, bool* open);
};

#endif // WITH_EDITOR
