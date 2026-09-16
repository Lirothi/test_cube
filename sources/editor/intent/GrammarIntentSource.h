#pragma once
#if WITH_EDITOR

#include <string>
#include <string_view>

#include "editor/intent/EditorIntentSource.h"

// The fast path (docs/editor_llm_plan.md, E7). An exact formulation -- a verb, an
// optional scope word, and literal asset/type names -- is answered here in
// microseconds, without waking a model.
//
// It is deliberately NOT the main road, and the plan says why: asset names are
// `coconut_palm` / `date_palm` / `curly_palm`, so the Russian word for "palms" shares
// not one letter with any of them. A substring grammar can only ever match what the
// user could have typed into the outliner's search box; the SEMANTICS OF THE NAME is
// the actual problem, and that is the model's job. This class exists so that people
// who already know the asset names do not pay a second of inference to say so.
//
// Grammar:
//     <verb> [ "selected" | "all" ] [ <needle> ... ]
// with the verbs below. A bare verb means "over the current selection", which is what
// the equivalent menu item does. "all" with no needle is refused, not obeyed: the
// difference between "delete all rocks" and "delete all" is a level.
class GrammarIntentSource final : public IEditorIntentSource
{
public:
    std::string_view Name() const override { return "grammar"; }
    bool Available() const override { return true; }

    void Begin(const std::string& phrase,
        const EditorIntentWorld& world,
        const std::vector<IntentTurn>& history) override;
    IntentParseState Poll(EditorIntent& outIntent, std::string& outWhyNot) override;
    void Cancel() override;

    // Synchronous shorthand: the grammar has nothing to wait for, and the gate reads
    // better without a Begin/Poll pair around every single-line assertion.
    EditorIntent Parse(const std::string& phrase, std::string& outWhyNot);

    // The verb list, for the command bar's help text. Newline-separated lines.
    static std::string HelpText();

private:
    IntentParseState state_ = IntentParseState::Idle;
    EditorIntent result_;
    std::string whyNot_;
};

#endif // WITH_EDITOR
