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
    // The most particles an emitter's JSON may ask for (docs/architecture_review_plan.md M6): three buffers are sized by it
    // and 6 x it is counted in 32 bits (ParticleEmitterObject).
    inline constexpr uint32_t kMaxParticlesPerEmitter = 1u << 20;

    // Apply whatever emitter fields are present in `j` onto `d` (absent fields left unchanged). A field of another type, a
    // range or a colour with a non-number in it is left unchanged too -- never an exception out of a level load.
    void ApplyEmitterJson(const nlohmann::json& j, EmitterDesc& d);

    // Full resolve: defaults < preset file < "overrides" object < top-level inline fields.
    EmitterDesc ResolveEmitterDesc(const nlohmann::json& objectJson);
}
