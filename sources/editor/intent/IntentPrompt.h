#pragma once
#if WITH_EDITOR

#include <string>

#include <vector>

#include "editor/intent/EditorIntentSource.h"
#include "editor/intent/IntentSchema.h"

class AssetRegistry;
class EditorSceneDocument;

// The system prompt, generated from the same action registry the grammar is generated
// from (docs/editor_llm_plan.md, E4). It is CONSTANT between requests for a given level,
// which is what lets llama.cpp cache the prefill: each phrase then costs a dozen input
// tokens plus a few dozen generated ones, not a re-read of the whole vocabulary.
namespace intentprompt
{
    // The action list with DESCRIPTIONS, the level's assets, and the instruction that
    // matters most: prefer an honest "no such action" over a near-miss.
    std::string BuildSystemPrompt(const EditorSceneDocument& document,
        const AssetRegistry& assets,
        const intentschema::Vocabulary& vocabulary);

    // Wrap system + prior turns + user into the model's chat format. `chatml` covers Qwen
    // and most current instruct models; `plain` is the escape hatch for one that does not.
    // The SYSTEM PREFIX stays byte-identical across calls, which is what the server's
    // prefill cache keys on -- history is appended after it, never woven into it.
    std::string ApplyChatTemplate(const std::string& systemPrompt,
        const std::string& userPhrase,
        const std::string& templateName,
        const std::vector<IntentTurn>& history = {});
}

#endif // WITH_EDITOR
