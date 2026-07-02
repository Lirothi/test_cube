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
        Undo,
        Redo,
        Save,
        FocusSelection,
        ClearSelection
    };

    struct ShortcutBinding
    {
        const char* label;
        ImGuiKey key;
        ShortcutModifiers modifiers;
        ShortcutAction action;
        bool blockedWhileFlying = false;
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
        { "Ctrl+Z Undo",           ImGuiKey_Z,      ShortcutModifiers::Ctrl,      ShortcutAction::Undo },
        { "Ctrl+Y Redo",           ImGuiKey_Y,      ShortcutModifiers::Ctrl,      ShortcutAction::Redo },
        { "Ctrl+Shift+Z Redo",     ImGuiKey_Z,      ShortcutModifiers::CtrlShift, ShortcutAction::Redo },
        { "Ctrl+S Save",           ImGuiKey_S,      ShortcutModifiers::Ctrl,      ShortcutAction::Save },
        { "F Frame",               ImGuiKey_F,      ShortcutModifiers::Plain,     ShortcutAction::FocusSelection },
        { "Esc Clear",             ImGuiKey_Escape, ShortcutModifiers::Plain,     ShortcutAction::ClearSelection }
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
        case ShortcutAction::ClearSelection:
            actions.clearSelection = true;
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
            ApplyShortcutAction(binding.action, viewportGizmo, actions);
        }
    }

    return actions;
}

const char* EditorHotkeys::HintText()
{
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
