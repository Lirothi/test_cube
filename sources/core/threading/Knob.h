#pragma once
// Render knobs across the two halves of the frame -- the game thread plan (docs/game_thread_plan.md section 3.8, G5).
//
// UE keeps two copies of a render-thread-safe cvar (IConsoleManager.h:92-98, TConsoleVariableData::ShadowedValue[2]) and
// updates the render thread's with a render command. Here a knob the GAME half writes (--set / --sweep / MCP
// set_settings, graphics_settings.json, Developer Controls, hotkeys) has a twin, `g_x_game`, which the game files read
// and write; `g_x` stays the render half's, read by the render code exactly as before. The hand-over (Scene::Sync, and
// the game half parking the render half: Renderer::WaitForPreviousFrame) exchanges them:
//
//     KNOB(float, g_x, 1.0f);   // in a header, inside its namespace: g_x, g_x_game and the registration
//
// A three-way merge against the value last exchanged: a knob the game changed goes to the render copy; one the render
// half changed (a one-shot request it consumed, a mode it fell back from) comes back to the game's; both changed, the
// game's wins. tools/check_render_knobs.py keeps the game files off the render copies.

#include <cstddef>
#include <cstring>
#include <type_traits>
#include <vector>

#include "core/threading/ThreadRole.h"

namespace knob
{
struct Entry
{
    void* game = nullptr;
    void* render = nullptr;
    std::size_t size = 0;
    const char* name = nullptr;
    std::vector<unsigned char> last; // the value both copies held after the last exchange
};

inline std::vector<Entry>& Registry()
{
    static std::vector<Entry> entries;
    return entries;
}

template <class T>
bool Register(T& game, T& render, const char* name)
{
    static_assert(std::is_trivially_copyable_v<T>, "a knob is copied byte for byte");
    Entry e;
    e.game = &game;
    e.render = &render;
    e.size = sizeof(T);
    e.name = name;
    e.last.resize(sizeof(T));
    std::memcpy(e.last.data(), &render, sizeof(T));
    Registry().push_back(std::move(e));
    return true;
}

// The exchange. Only where nothing else runs: the hand-over, boot, or the game half with the render half parked.
inline void Exchange()
{
    TC_CHECK_GAME_PARKED_OR_SYNC("knob::Exchange");
    for (Entry& e : Registry())
    {
        if (std::memcmp(e.game, e.last.data(), e.size) != 0)
        {
            std::memcpy(e.render, e.game, e.size);
            std::memcpy(e.last.data(), e.game, e.size);
        }
        else if (std::memcmp(e.render, e.last.data(), e.size) != 0)
        {
            std::memcpy(e.game, e.render, e.size);
            std::memcpy(e.last.data(), e.render, e.size);
        }
    }
}
} // namespace knob

// The knob, its game twin and their registration -- one line where the knob was declared.
#define KNOB(type, name, init)                                                                     \
    inline type name = init;                                                                       \
    inline type name##_game = init;                                                                \
    inline const bool name##_knob = ::knob::Register(name##_game, name, #name)
