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
    //
    // `modelName` is what the model should say when asked what it is. It is passed in
    // rather than written here because it is a fact about the FILE that was loaded, and a
    // string compiled into the prompt would go on claiming a model the person had already
    // replaced. Empty when nothing is loaded, and then the prompt claims nothing.
    // `grammar` is the GBNF this level's answers will be constrained by. It is put in the
    // prompt verbatim: the model is being sampled against it either way, and a constraint
    // you can read is one you can satisfy deliberately instead of bumping into.
    std::string BuildSystemPrompt(const EditorSceneDocument& document,
        const AssetRegistry& assets,
        const intentschema::Vocabulary& vocabulary,
        const std::string& modelName = std::string{},
        const std::string& grammar = std::string{});

    // "D:/llm_models/Qwen3.6-35B-A3B-UD-Q8_K_XL.gguf" -> "Qwen3.6-35B-A3B-UD-Q8_K_XL".
    // The only identity the editor actually has: llama.cpp is handed a file, and the file's
    // name is what the person chose to run.
    std::string ModelNameFromPath(const std::string& modelPath);

    // Wrap system + prior turns + user into the model's chat format. `chatml` covers Qwen
    // and most current instruct models; `plain` is the escape hatch for one that does not.
    // The SYSTEM PREFIX stays byte-identical across calls, which is what the server's
    // prefill cache keys on -- history is appended after it, never woven into it.
    // `reasoning` false pre-fills an already-closed think block, which is how a model that
    // reasons by default is told it has finished. True leaves it free to think -- only safe
    // when the grammar is applied lazily, or the very first token is rejected.
    std::string ApplyChatTemplate(const std::string& systemPrompt,
        const std::string& userPhrase,
        const std::string& templateName,
        const std::vector<IntentTurn>& history = {},
        bool reasoning = false);
}

#endif // WITH_EDITOR
