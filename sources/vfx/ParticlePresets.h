#pragma once

#include "vfx/ParticleTypes.h"

// nlohmann/json — single header
#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)

// E3 authoring: shared parsing of the particle-emitter JSON schema, used by the level loader
// (SceneObjectRegistry), the shared factory (SceneObjectFactory) and the editor inspector's
// live edits — one schema, one parser. Level JSON mirrors the ocean pattern:
//   { "type":"particleEmitter", "preset":"data/particles/fire.json",
//     "overrides":{ ... }, "position":[...] }
// `preset` names a JSON file holding the base EmitterDesc fields; `overrides` tweaks them per
// instance; top-level fields are also honored (inline authoring / back-compat). The `position`
// transform is intentionally NOT read here — the runtime emitter reads its object position.
namespace vfx
{
    // Apply whatever emitter fields are present in `j` onto `d` (absent fields left unchanged).
    void ApplyEmitterJson(const nlohmann::json& j, EmitterDesc& d);

    // Full resolve: defaults < preset file < "overrides" object < top-level inline fields.
    EmitterDesc ResolveEmitterDesc(const nlohmann::json& objectJson);
}
