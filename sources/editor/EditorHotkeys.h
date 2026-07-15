#pragma once
#if WITH_EDITOR

class ViewportGizmo;

struct EditorHotkeyActions
{
    bool deleteSelection = false;
    bool duplicateSelection = false;
    bool copySelection = false;
    bool pasteObject = false;
    bool undo = false;
    bool redo = false;
    bool save = false;
    bool focusSelection = false;
    bool frameScene = false;
    bool snapSelectionToSurfaceBelow = false;
    bool clearSelection = false;
    int storeCameraBookmark = -1;
    int recallCameraBookmark = -1;
};

class EditorHotkeys
{
public:
    EditorHotkeyActions Poll(ViewportGizmo& viewportGizmo);

    static const char* HintText();
};

#endif // WITH_EDITOR
