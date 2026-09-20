#pragma once

#include "editor/intent/LlmReaper.h"

// The watchdog's face in the notification area.
//
// Until now the watchdog was invisible: a process with no window, holding 13 GB of video
// memory hostage on a promise, and the only way to see it was Task Manager and the only way
// to end it early was to kill it there -- which leaves the server running, exactly the
// orphan the watchdog exists to prevent.
//
// So it gets an icon. The icon's COLOUR is its state, so a glance is enough: green while an
// editor is open and the server is held, amber while the clock runs, grey before the server
// has answered at all. Clicking it opens a menu that says the same thing in words, with the
// seconds left, and offers the two endings a person might want.
namespace reapertray
{
    // Shows the icon and runs the watch underneath it, returning the watch's exit code.
    // Falls back to running the watch bare if the window cannot be created: a watchdog
    // without an icon is still a watchdog, and a server nobody retires is the worse outcome.
    int RunWithTray(const llmreaper::Options& options);
}
