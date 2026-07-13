#include "editor/EditorHotkeys.h"
#if WITH_EDITOR

#include <string>

#include "editor/ui/ViewportGizmo.h"
#include "imgui.h"

namespace
{
    enum class ShortcutModifiers
    {
        Plain,
        Ctrl,
        CtrlShift
    };

    enum class ShortcutAction
    {
        SelectMode,
        TranslateMode,
        RotateMode,
        ScaleMode,
        CycleTransformMode,
        DeleteSelection,
        DuplicateSelection,
        CopySelection,
        PasteObject,
        Undo,
        Redo,
        Save,
        FocusSelection,
        FrameScene,
        DropSelectionToGround,
        ClearSelection,
        StoreCameraBookmark,
        RecallCameraBookmark
    };

    struct ShortcutBinding
    {
        const char* label;
        ImGuiKey key;
        ShortcutModifiers modifiers;
        ShortcutAction action;
        bool blockedWhileFlying = false;
        int bookmarkSlot = -1;
    };

    // Editor shortcut bindings live in one place. To retune a key, edit this table
    // and its label; polling and UI hints are generated from the same data.
    constexpr ShortcutBinding kShortcutBindings[] =
    {
        { "Q Select",              ImGuiKey_Q,      ShortcutModifiers::Plain,     ShortcutAction::SelectMode,         true },
        { "W Move",                ImGuiKey_W,      ShortcutModifiers::Plain,     ShortcutAction::TranslateMode,      true },
        { "E Rotate",              ImGuiKey_E,      ShortcutModifiers::Plain,     ShortcutAction::RotateMode,         true },
        { "R Scale",               ImGuiKey_R,      ShortcutModifiers::Plain,     ShortcutAction::ScaleMode,          true },
        { "Space Cycle",           ImGuiKey_Space,  ShortcutModifiers::Plain,     ShortcutAction::CycleTransformMode, true },
        { "Delete Delete",         ImGuiKey_Delete, ShortcutModifiers::Plain,     ShortcutAction::DeleteSelection },
        { "Ctrl+D Duplicate",      ImGuiKey_D,      ShortcutModifiers::Ctrl,      ShortcutAction::DuplicateSelection },
        { "Ctrl+C Copy",           ImGuiKey_C,      ShortcutModifiers::Ctrl,      ShortcutAction::CopySelection },
        { "Ctrl+V Paste",          ImGuiKey_V,      ShortcutModifiers::Ctrl,      ShortcutAction::PasteObject },
        { "Ctrl+Z Undo",           ImGuiKey_Z,      ShortcutModifiers::Ctrl,      ShortcutAction::Undo },
        { "Ctrl+Y Redo",           ImGuiKey_Y,      ShortcutModifiers::Ctrl,      ShortcutAction::Redo },
        { "Ctrl+Shift+Z Redo",     ImGuiKey_Z,      ShortcutModifiers::CtrlShift, ShortcutAction::Redo },
        { "Ctrl+S Save",           ImGuiKey_S,      ShortcutModifiers::Ctrl,      ShortcutAction::Save },
        { "F Frame",               ImGuiKey_F,      ShortcutModifiers::Plain,     ShortcutAction::FocusSelection },
        { "Home Frame Scene",      ImGuiKey_Home,   ShortcutModifiers::Plain,     ShortcutAction::FrameScene,          true },
        { "End Drop",              ImGuiKey_End,    ShortcutModifiers::Plain,     ShortcutAction::DropSelectionToGround },
        { "Esc Clear",             ImGuiKey_Escape, ShortcutModifiers::Plain,     ShortcutAction::ClearSelection },
        { "Ctrl+1 Store Camera 1", ImGuiKey_1,      ShortcutModifiers::Ctrl,      ShortcutAction::StoreCameraBookmark, true, 0 },
        { "Ctrl+2 Store Camera 2", ImGuiKey_2,      ShortcutModifiers::Ctrl,      ShortcutAction::StoreCameraBookmark, true, 1 },
        { "Ctrl+3 Store Camera 3", ImGuiKey_3,      ShortcutModifiers::Ctrl,      ShortcutAction::StoreCameraBookmark, true, 2 },
        { "Ctrl+4 Store Camera 4", ImGuiKey_4,      ShortcutModifiers::Ctrl,      ShortcutAction::StoreCameraBookmark, true, 3 },
        { "Ctrl+5 Store Camera 5", ImGuiKey_5,      ShortcutModifiers::Ctrl,      ShortcutAction::StoreCameraBookmark, true, 4 },
        { "Ctrl+6 Store Camera 6", ImGuiKey_6,      ShortcutModifiers::Ctrl,      ShortcutAction::StoreCameraBookmark, true, 5 },
        { "Ctrl+7 Store Camera 7", ImGuiKey_7,      ShortcutModifiers::Ctrl,      ShortcutAction::StoreCameraBookmark, true, 6 },
        { "Ctrl+8 Store Camera 8", ImGuiKey_8,      ShortcutModifiers::Ctrl,      ShortcutAction::StoreCameraBookmark, true, 7 },
        { "Ctrl+9 Store Camera 9", ImGuiKey_9,      ShortcutModifiers::Ctrl,      ShortcutAction::StoreCameraBookmark, true, 8 },
        { "1 Recall Camera 1",     ImGuiKey_1,      ShortcutModifiers::Plain,     ShortcutAction::RecallCameraBookmark, true, 0 },
        { "2 Recall Camera 2",     ImGuiKey_2,      ShortcutModifiers::Plain,     ShortcutAction::RecallCameraBookmark, true, 1 },
        { "3 Recall Camera 3",     ImGuiKey_3,      ShortcutModifiers::Plain,     ShortcutAction::RecallCameraBookmark, true, 2 },
        { "4 Recall Camera 4",     ImGuiKey_4,      ShortcutModifiers::Plain,     ShortcutAction::RecallCameraBookmark, true, 3 },
        { "5 Recall Camera 5",     ImGuiKey_5,      ShortcutModifiers::Plain,     ShortcutAction::RecallCameraBookmark, true, 4 },
        { "6 Recall Camera 6",     ImGuiKey_6,      ShortcutModifiers::Plain,     ShortcutAction::RecallCameraBookmark, true, 5 },
        { "7 Recall Camera 7",     ImGuiKey_7,      ShortcutModifiers::Plain,     ShortcutAction::RecallCameraBookmark, true, 6 },
        { "8 Recall Camera 8",     ImGuiKey_8,      ShortcutModifiers::Plain,     ShortcutAction::RecallCameraBookmark, true, 7 },
        { "9 Recall Camera 9",     ImGuiKey_9,      ShortcutModifiers::Plain,     ShortcutAction::RecallCameraBookmark, true, 8 }
    };

    bool Pressed(const ShortcutBinding& binding)
    {
        return ImGui::IsKeyPressed(binding.key, false);
    }

    bool ModifiersMatch(const ImGuiIO& io, ShortcutModifiers modifiers)
    {
        switch (modifiers)
        {
        case ShortcutModifiers::Plain:
            return !io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !io.KeySuper;
        case ShortcutModifiers::Ctrl:
            return io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !io.KeySuper;
        case ShortcutModifiers::CtrlShift:
            return io.KeyCtrl && io.KeyShift && !io.KeyAlt && !io.KeySuper;
        default:
            return false;
        }
    }

    void ApplyShortcutAction(
        ShortcutAction action,
        int bookmarkSlot,
        ViewportGizmo& viewportGizmo,
        EditorHotkeyActions& actions)
    {
        switch (action)
        {
        case ShortcutAction::SelectMode:
            viewportGizmo.SetMode(ViewportGizmo::Op::Select);
            break;
        case ShortcutAction::TranslateMode:
            viewportGizmo.SetMode(ViewportGizmo::Op::Translate);
            break;
        case ShortcutAction::RotateMode:
            viewportGizmo.SetMode(ViewportGizmo::Op::Rotate);
            break;
        case ShortcutAction::ScaleMode:
            viewportGizmo.SetMode(ViewportGizmo::Op::Scale);
            break;
        case ShortcutAction::CycleTransformMode:
            viewportGizmo.CycleTransformMode();
            break;
        case ShortcutAction::DeleteSelection:
            actions.deleteSelection = true;
            break;
        case ShortcutAction::DuplicateSelection:
            actions.duplicateSelection = true;
            break;
        case ShortcutAction::CopySelection:
            actions.copySelection = true;
            break;
        case ShortcutAction::PasteObject:
            actions.pasteObject = true;
            break;
        case ShortcutAction::Undo:
            actions.undo = true;
            break;
        case ShortcutAction::Redo:
            actions.redo = true;
            break;
        case ShortcutAction::Save:
            actions.save = true;
            break;
        case ShortcutAction::FocusSelection:
            actions.focusSelection = true;
            break;
        case ShortcutAction::FrameScene:
            actions.frameScene = true;
            break;
        case ShortcutAction::DropSelectionToGround:
            actions.dropSelectionToGround = true;
            break;
        case ShortcutAction::ClearSelection:
            actions.clearSelection = true;
            break;
        case ShortcutAction::StoreCameraBookmark:
            actions.storeCameraBookmark = bookmarkSlot;
            break;
        case ShortcutAction::RecallCameraBookmark:
            actions.recallCameraBookmark = bookmarkSlot;
            break;
        default:
            break;
        }
    }
}

EditorHotkeyActions EditorHotkeys::Poll(ViewportGizmo& viewportGizmo)
{
    EditorHotkeyActions actions;
    ImGuiIO& io = ImGui::GetIO();
    viewportGizmo.SetTemporarySnapInvert(io.KeyCtrl);

    if (io.WantTextInput)
    {
        return actions;
    }

    const bool flying = ImGui::IsMouseDown(ImGuiMouseButton_Right);

    for (const ShortcutBinding& binding : kShortcutBindings)
    {
        if ((flying && binding.blockedWhileFlying) || !ModifiersMatch(io, binding.modifiers))
        {
            continue;
        }
        if (Pressed(binding))
        {
            ApplyShortcutAction(binding.action, binding.bookmarkSlot, viewportGizmo, actions);
        }
    }

    return actions;
}

const char* EditorHotkeys::HintText()
{
    return nullptr;

    static std::string hint;
    if (hint.empty())
    {
        for (const ShortcutBinding& binding : kShortcutBindings)
        {
            if (!hint.empty())
            {
                hint += "  ";
            }
            hint += binding.label;
        }
    }
    return hint.c_str();
}

#endif // WITH_EDITOR
