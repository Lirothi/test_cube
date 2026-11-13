#pragma once

#include <string>
#include <vector>

// What the game needs of the graphics card (docs/distribution_build_plan.md §7, §11 item 15): Direct3D 12, shader model
// 6.6 (one set of cooked blobs, bindless resources), resource binding tier 3, DirectX Raytracing 1.1 (inline RayQuery: the
// sky's visibility, GI, reflections -- the path without it is not checked for looks). Mesh shaders are not required (there
// is a fallback). Asked of the adapter the renderer takes (the default one), on a device made and dropped for it, before
// the window exists: a card that cannot run the game gets a sentence, not a crash.
namespace gpu
{
    struct RequirementCheck
    {
        bool ok = false;
        std::string adapter;                 // "NVIDIA GeForce RTX 4090", or "" when no device could be made
        std::string driver;                  // the user-mode driver version, "32.0.16.2101"
        std::vector<std::string> missing;    // in words: "shader model 6.6 (it has 6.5)"
    };

    [[nodiscard]] RequirementCheck CheckRequirements();
    // The message a player reads: what the card lacks and what to do.
    [[nodiscard]] std::wstring RequirementMessage(const RequirementCheck& check, const wchar_t* product);
}
