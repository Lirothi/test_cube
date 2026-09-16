#pragma once
#if WITH_EDITOR

#include <string>

#include "imgui.h"

namespace editorui
{
    // ImGui's InputText writes into a caller-owned char buffer, so that buffer's capacity
    // is a hard ceiling on what can be typed -- and a SILENT one: at the limit the widget
    // simply stops accepting keystrokes, with no message, no colour change and nothing to
    // notice except that the keyboard appears to have died.
    //
    // That is a control that lies. The chat box was 2048 bytes, which is about a thousand
    // Cyrillic characters because UTF-8 spends two bytes on each, and the model on the
    // other end of it has a 262144-token context. Pasting a shader to ask about would hit
    // the wall long before the model would.
    //
    // ImGuiInputTextFlags_CallbackResize exists for exactly this: instead of truncating,
    // ImGui hands the resize back to the owner of the string, who is the only one who can
    // do it without invalidating the pointer ImGui is holding. The ImGui drop vendored in
    // third_party/ has no misc/cpp, so these are the few lines imgui_stdlib.cpp would
    // otherwise have provided, and they behave the same way.
    namespace detail
    {
        inline int GrowStringCallback(ImGuiInputTextCallbackData* data)
        {
            if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
            {
                std::string* str = static_cast<std::string*>(data->UserData);
                IM_ASSERT(data->Buf == str->c_str());
                str->resize(static_cast<std::size_t>(data->BufTextLen));
                data->Buf = str->data();
            }
            return 0;
        }
    }

    // Capacity, not size, is handed to ImGui: the string carries its own terminator and
    // the callback grows it whenever the text outruns what was reserved.
    inline bool InputText(const char* label, std::string& text, ImGuiInputTextFlags flags = 0)
    {
        IM_ASSERT((flags & ImGuiInputTextFlags_CallbackResize) == 0);
        return ImGui::InputText(label, text.data(), text.capacity() + 1,
            flags | ImGuiInputTextFlags_CallbackResize, detail::GrowStringCallback, &text);
    }

    inline bool InputTextMultiline(const char* label, std::string& text,
        const ImVec2& size = ImVec2(0.0f, 0.0f), ImGuiInputTextFlags flags = 0)
    {
        IM_ASSERT((flags & ImGuiInputTextFlags_CallbackResize) == 0);
        return ImGui::InputTextMultiline(label, text.data(), text.capacity() + 1, size,
            flags | ImGuiInputTextFlags_CallbackResize, detail::GrowStringCallback, &text);
    }
}

#endif // WITH_EDITOR
