#pragma once
#if WITH_EDITOR

#include <string>
#include <vector>

#include "editor/intent/EditorIntent.h"

class AssetRegistry;
class EditorSceneDocument;

// The contract with the model, both halves in one place (docs/editor_llm_plan.md, E3).
//
// A GBNF grammar constrains sampling so that only tokens the grammar permits can be
// emitted -- this is not "usually valid JSON", it is a guarantee. And the GRAMMAR IS
// GENERATED FROM THE LEVEL: the action ids come from the action registry and the asset
// names are listed as literals taken from the level and the asset registry, so inventing
// an asset that does not exist is impossible by construction rather than by agreement.
//
// The reader lives here too, deliberately. A grammar and a parser for the same wire
// format are two statements of one contract, and keeping them in separate files is how
// they end up disagreeing about a field name that nothing catches until a real phrase
// hits the one path that was never exercised.
namespace intentschema
{
    // Distinct asset keys and type names present in the level, plus every mesh asset on
    // disk. The first set is what a filter may name; the second is what `spawn` and
    // `replace` may create from.
    struct Vocabulary
    {
        std::vector<std::string> needles;   // filter/exclude literals
        std::vector<std::string> assets;    // spawnable/replaceable asset keys
        // What setMaterial may name. Separate from `assets` because they are not
        // interchangeable: spawning a material or applying a mesh are both nonsense, and
        // one list for both would invite exactly that.
        std::vector<std::string> materials;
        // Named regions this level has. Same reasoning again: an action that takes a name
        // is unusable until the prompt says which names exist.
        std::vector<std::string> zones;
    };

    Vocabulary BuildVocabulary(const EditorSceneDocument& document, const AssetRegistry& assets);

    // The GBNF text. Rebuilt when the level changes; constant between requests, which is
    // also what lets the server cache the prompt prefix (E4).
    std::string BuildGbnf(const Vocabulary& vocabulary);

    // Read one model answer into an intent. Returns false with a reason when the text is
    // not the shape the grammar promised -- which should be impossible, and is therefore
    // exactly the kind of thing worth checking rather than assuming.
    bool ParseAnswer(const std::string& json, EditorIntent& outIntent, std::string& outError);
}

#endif // WITH_EDITOR
