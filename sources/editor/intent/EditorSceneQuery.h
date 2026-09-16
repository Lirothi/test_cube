#pragma once
#if WITH_EDITOR

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/math/Math.h"
#include "editor/intent/EditorActionRegistry.h"
#include "editor/intent/EditorIntent.h"

class Scene;

// What the model may ASK, as opposed to what it may do.
//
// The command bar's model answers in one shot from a prompt built when the level loaded.
// Everything it knows about the scene is in that prompt, and the prompt cannot grow with
// the scene: the prefix is what llama-server's cache keys on, so anything that changes as
// the level is edited would re-prefill on every request. Measured here, prefill runs at
// 450-500 tokens/s -- a 300 KB level dumped into the prompt is minutes, every time.
//
// So the scene does not go to the model. The model asks, and one question gets one short
// answer, appended after the cached prefix as a turn. The round trip is not free either --
// a query turn costs another generation -- so the set is small on purpose and each entry
// has to earn a question that a designer actually asks.
//
// SAME DISCIPLINE AS THE ACTION REGISTRY, for the same reason: this list generates the
// grammar's query names, the prompt's description of them, and the dispatcher below.
// Adding one is an entry here, not three edits that drift apart.
struct EditorQueryDesc
{
    std::string_view id;
    std::string_view description;
    // Filled from target.filter / scope / where, like an action over objects. Queries
    // without it read the level or the editor as a whole.
    bool usesTarget = false;
    // Named parameters it reads, purely so the prompt can say so.
    std::string_view params;
};

namespace editorquery
{
    // Every query, in prompt order.
    const std::vector<EditorQueryDesc>& All();

    // Nullptr when no query has that id.
    const EditorQueryDesc* Find(const std::string& id);

    // Run every ask in the intent and return ONE compact JSON object keyed by query name.
    //
    // JSON RATHER THAN THE PROSE THIS STARTED AS, and the consumer asked for it in those
    // words: prose is fewer tokens, but it has to pull coordinates back out of a sentence,
    // and a number misread from prose is indistinguishable from a number it made up. The
    // saving was real and small -- these answers are a couple of hundred characters -- and
    // it was being paid in the one currency that matters here, which is being right.
    //
    // A failure is an answer too: {"waterLevel":{"error":"this level has no ocean"}} tells
    // the model where the waterline is just as surely as a number would.
    std::string Answer(const EditorActionContext& actionCtx, const EditorIntent& intent);

    // Height of the surface under (x, z), or false when nothing is below. Cast from
    // `startY` downwards through the scene, so the mesh's own transform, the terrain's
    // chunking and anything standing on top are all already accounted for.
    //
    // THE SCATTER USES THIS TOO, and that is the point of it living here rather than in
    // the registry where it was born: the contour of a shoreline and the planting inside
    // that contour have to agree about where the ground is, and they only agree for sure
    // while it is one function.
    bool ProbeGroundHeight(const Scene& scene,
        float x,
        float z,
        float startY,
        const std::vector<std::uint64_t>& ignored,
        float& outHeight);

    // How far above the anchor a ground probe starts.
    constexpr float kGroundProbeUp = 500.0f;
}

#endif // WITH_EDITOR
