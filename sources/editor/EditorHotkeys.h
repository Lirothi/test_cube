#pragma once
#if WITH_EDITOR

class ViewportGizmo;

struct EditorHotkeyActions
{
    bool deleteSelection = false;
    bool duplicateSelection = false;
    bool undo = false;
    bool redo = false;
    bool save = false;
    bool focusSelection = false;
    bool dropSelectionToGround = false;
    bool clearSelection = false;
};

class EditorHotkeys
{
public:
    EditorHotkeyActions Poll(ViewportGizmo& viewportGizmo);

    static const char* HintText();
};

#endif // WITH_EDITOR
