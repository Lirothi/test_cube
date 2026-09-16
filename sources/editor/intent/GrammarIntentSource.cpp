#include "editor/intent/GrammarIntentSource.h"
#if WITH_EDITOR

#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/StringMatch.h"
#include "editor/scene/EditorSceneDocument.h"

namespace
{
    // How the words AFTER the verb are read. The shapes are listed instead of invented
    // per verb, because a universal "verb then everything else" grammar is exactly the
    // thing that quietly mis-parses -- `replace palms with rock` and `spawn 10 rock`
    // put the asset in different places, and pretending otherwise loses one of them.
    enum class VerbShape
    {
        Objects,            // <verb> [scope] [needles] [except ...] [within N [of X]]
        Spawn,              // <verb> [count] <asset>
        Replace,            // <verb> [needles] with <asset>
        Transform,          // <verb> [needles] by|to <numbers>
        RandomizeRotation,  // randomize yaw|rotation [needles]
        RandomizeScale,     // randomize scale [needles]
    };

    struct VerbEntry
    {
        const char* word;
        const char* actionId;
        VerbShape shape;
        int enabledParam;   // -1 = no `enabled` param, otherwise the value to pass
    };

    // Only ASCII, and deliberately so. The grammar's whole value is being exact; a
    // Cyrillic verb table would put a second, half-working Russian parser next to the
    // model whose entire purpose is Russian, and the two would disagree. It would not
    // even buy the example the plan is named after: "рассади 10 новых пальм" still
    // needs "пальм" understood as `coconut_palm`, which no substring match can do.
    constexpr VerbEntry kVerbs[] = {
        { "count",     "count",         VerbShape::Objects, -1 },
        { "isolate",   "isolate",       VerbShape::Objects, -1 },
        { "frame",     "frame",         VerbShape::Objects, -1 },
        { "similar",   "selectSimilar", VerbShape::Objects, -1 },
        { "align",     "align",         VerbShape::Objects, -1 },
        { "distribute","distribute",    VerbShape::Objects, -1 },
        { "snap",      "snap",          VerbShape::Objects, -1 },
        { "select",    "select",     VerbShape::Objects,   -1 },
        { "hide",      "setEnabled", VerbShape::Objects,    0 },
        { "disable",   "setEnabled", VerbShape::Objects,    0 },
        { "show",      "setEnabled", VerbShape::Objects,    1 },
        { "enable",    "setEnabled", VerbShape::Objects,    1 },
        { "delete",    "delete",     VerbShape::Objects,   -1 },
        { "remove",    "delete",     VerbShape::Objects,   -1 },
        { "duplicate", "duplicate",  VerbShape::Objects,   -1 },
        { "bury",      "bury",       VerbShape::Objects,   -1 },
        { "spawn",     "spawn",      VerbShape::Spawn,     -1 },
        { "scatter",   "spawn",      VerbShape::Spawn,     -1 },
        { "plant",     "spawn",      VerbShape::Spawn,     -1 },
        { "replace",   "replace",    VerbShape::Replace,   -1 },
        { "move",      "move",       VerbShape::Transform, -1 },
        { "rotate",    "rotate",     VerbShape::Transform, -1 },
        { "scale",     "scale",      VerbShape::Transform, -1 },
    };

    std::vector<std::string> SplitWords(const std::string& phrase)
    {
        std::vector<std::string> words;
        std::string current;
        for (const char ch : phrase)
        {
            if (ch == ' ' || ch == '\t' || ch == ',' || ch == '\n' || ch == '\r')
            {
                if (!current.empty())
                {
                    words.push_back(current);
                    current.clear();
                }
                continue;
            }
            current.push_back(ch);
        }
        if (!current.empty())
        {
            words.push_back(current);
        }
        return words;
    }

    bool EqualsIgnoreCase(const std::string& a, std::string_view b)
    {
        return a.size() == b.size() && textmatch::CompareCaseInsensitive(a, b) == 0;
    }

    const VerbEntry* FindVerb(const std::string& word)
    {
        for (const VerbEntry& verb : kVerbs)
        {
            if (EqualsIgnoreCase(word, verb.word))
            {
                return &verb;
            }
        }
        return nullptr;
    }

    bool ParseNumber(const std::string& text, float& out)
    {
        if (text.empty())
        {
            return false;
        }
        char* end = nullptr;
        const float value = std::strtof(text.c_str(), &end);
        if (end != text.c_str() + text.size())
        {
            return false;
        }
        out = value;
        return true;
    }

    bool ParseCount(const std::string& text, int& out)
    {
        float value = 0.0f;
        if (!ParseNumber(text, value) || value < 1.0f || value != static_cast<float>(static_cast<int>(value)))
        {
            return false;
        }
        out = static_cast<int>(value);
        return true;
    }

    EditorIntent MakeUnclear(std::string_view source, std::string question)
    {
        EditorIntent intent;
        intent.kind = EditorIntentKind::Unclear;
        intent.sourceLabel = std::string(source);
        intent.question = std::move(question);
        return intent;
    }

    // Where the words currently being read are going. Keywords move the sink; everything
    // else lands in whatever it currently points at.
    enum class Sink
    {
        Filter,
        Exclude,
        Asset,
        Numbers,
        Radius,
        Anchor,
    };
}

void GrammarIntentSource::Begin(const std::string& phrase,
    const EditorIntentWorld&,
    const std::vector<IntentTurn>&)
{
    // Nothing to wait for: the grammar settles before Begin returns. The async shape
    // exists for the source that does take seconds, and costs this one a struct field.
    whyNot_.clear();
    result_ = Parse(phrase, whyNot_);
    state_ = result_.kind == EditorIntentKind::None
        ? IntentParseState::Declined : IntentParseState::Ready;
}

IntentParseState GrammarIntentSource::Poll(EditorIntent& outIntent, std::string& outWhyNot)
{
    const IntentParseState state = state_;
    if (state == IntentParseState::Ready)
    {
        outIntent = result_;
    }
    else if (state == IntentParseState::Declined)
    {
        outWhyNot = whyNot_;
    }
    state_ = IntentParseState::Idle;
    return state;
}

void GrammarIntentSource::Cancel()
{
    state_ = IntentParseState::Idle;
    result_ = EditorIntent{};
    whyNot_.clear();
}

EditorIntent GrammarIntentSource::Parse(const std::string& phrase, std::string& outWhyNot)
{
    EditorIntent intent;
    const std::vector<std::string> words = SplitWords(phrase);
    if (words.empty())
    {
        return intent;
    }

    std::size_t next = 1;
    const VerbEntry* verb = FindVerb(words[0]);
    VerbShape shape = verb ? verb->shape : VerbShape::Objects;
    std::string actionId = verb ? verb->actionId : std::string{};

    // `randomize` is the one two-word verb: what it randomizes changes the action, and
    // guessing between rotation and scale is not a guess worth making.
    if (!verb && EqualsIgnoreCase(words[0], "randomize"))
    {
        if (words.size() < 2)
        {
            return MakeUnclear(Name(), "Randomize what -- rotation or scale?");
        }
        if (EqualsIgnoreCase(words[1], "yaw") || EqualsIgnoreCase(words[1], "rotation"))
        {
            shape = VerbShape::RandomizeRotation;
            actionId = "randomizeRotation";
        }
        else if (EqualsIgnoreCase(words[1], "scale") || EqualsIgnoreCase(words[1], "size"))
        {
            shape = VerbShape::RandomizeScale;
            actionId = "randomizeScale";
        }
        else
        {
            return MakeUnclear(Name(), "Randomize what -- rotation or scale?");
        }
        next = 2;
    }
    else if (!verb)
    {
        // Not a phrasing this grammar owns. Say nothing and let the model try; the
        // grammar must never guess, because a wrong guess here is a silent wrong edit.
        outWhyNot = "not an exact command (first word is not a known verb)";
        return intent;
    }

    intent.target.scope = EditorIntentScope::All;
    bool scopeStated = false;
    bool relative = true;
    std::vector<float> numbers;
    std::vector<std::string> loose;   // the words that were not keywords or arguments
    Sink sink = shape == VerbShape::Spawn ? Sink::Asset : Sink::Filter;
    int count = 0;
    bool countSeen = false;

    for (; next < words.size(); ++next)
    {
        const std::string& word = words[next];

        // The anchor sink is read BEFORE the keywords, because the word it expects --
        // "selection" -- is also a scope word. Checking scope first ate it, and
        // "within 50 of selection" silently became "within 50 of the camera".
        if (sink == Sink::Anchor)
        {
            if (EqualsIgnoreCase(word, "camera") || EqualsIgnoreCase(word, "me"))
            {
                intent.target.where.anchor = EditorSpatialAnchor::Camera;
            }
            else if (EqualsIgnoreCase(word, "selected") || EqualsIgnoreCase(word, "selection"))
            {
                intent.target.where.anchor = EditorSpatialAnchor::Selection;
            }
            else
            {
                return MakeUnclear(Name(),
                    "Measure the distance from the camera or from the selection?");
            }
            sink = Sink::Filter;
            continue;
        }

        if (EqualsIgnoreCase(word, "selected") || EqualsIgnoreCase(word, "selection"))
        {
            intent.target.scope = EditorIntentScope::Selected;
            scopeStated = true;
            continue;
        }
        if (EqualsIgnoreCase(word, "all"))
        {
            intent.target.scope = EditorIntentScope::All;
            scopeStated = true;
            continue;
        }
        if (EqualsIgnoreCase(word, "except") || EqualsIgnoreCase(word, "but"))
        {
            sink = Sink::Exclude;
            continue;
        }
        if (EqualsIgnoreCase(word, "with"))
        {
            sink = Sink::Asset;
            continue;
        }
        if (EqualsIgnoreCase(word, "by"))
        {
            sink = Sink::Numbers;
            relative = true;
            continue;
        }
        if (EqualsIgnoreCase(word, "to"))
        {
            sink = Sink::Numbers;
            relative = false;
            continue;
        }
        if (EqualsIgnoreCase(word, "within") || EqualsIgnoreCase(word, "near"))
        {
            sink = Sink::Radius;
            continue;
        }
        if (EqualsIgnoreCase(word, "of") || EqualsIgnoreCase(word, "from"))
        {
            sink = Sink::Anchor;
            continue;
        }

        switch (sink)
        {
        case Sink::Filter:
            loose.push_back(word);
            break;
        case Sink::Exclude:
            intent.target.exclude.push_back(word);
            break;
        case Sink::Asset:
            if (!countSeen && shape == VerbShape::Spawn && ParseCount(word, count))
            {
                countSeen = true;
            }
            else if (intent.target.asset.empty())
            {
                intent.target.asset = word;
            }
            else
            {
                return MakeUnclear(Name(),
                    "Which asset -- '" + intent.target.asset + "' or '" + word + "'?");
            }
            break;
        case Sink::Numbers:
        {
            float value = 0.0f;
            if (!ParseNumber(word, value))
            {
                return MakeUnclear(Name(), "'" + word + "' is not a number.");
            }
            numbers.push_back(value);
            break;
        }
        case Sink::Radius:
        {
            float value = 0.0f;
            if (!ParseNumber(word, value))
            {
                return MakeUnclear(Name(), "'" + word + "' is not a distance in metres.");
            }
            intent.target.where.radius = value;
            // A radius with no stated anchor means "from where I am".
            intent.target.where.anchor = EditorSpatialAnchor::Camera;
            sink = Sink::Filter;
            break;
        }
        case Sink::Anchor:
            break;   // handled above, before the keyword checks
        }
    }

    intent.target.filter = loose;
    intent.action = actionId;
    intent.sourceLabel = std::string(Name());

    switch (shape)
    {
    case VerbShape::Spawn:
        intent.target.kind = EditorTargetKind::Asset;
        if (intent.target.asset.empty())
        {
            return MakeUnclear(Name(), "Which asset should be placed? Name a mesh, e.g. coconut_palm.");
        }
        intent.params["count"] = countSeen ? count : 1;
        break;

    case VerbShape::Replace:
        if (intent.target.asset.empty())
        {
            return MakeUnclear(Name(), "Replace them with which asset? Use: replace <what> with <asset>.");
        }
        break;

    case VerbShape::Transform:
        if (numbers.empty())
        {
            return MakeUnclear(Name(),
                "By how much? Use: " + actionId + " <what> by <x> <y> <z>, or 'to' for an absolute value.");
        }
        if (numbers.size() == 1)
        {
            intent.params["value"] = numbers[0];   // ValidateParams broadens it to all three axes
        }
        else if (numbers.size() == 3)
        {
            intent.params["value"] = nlohmann::json::array({ numbers[0], numbers[1], numbers[2] });
        }
        else
        {
            return MakeUnclear(Name(), "Give one number or three, not " + std::to_string(numbers.size()) + ".");
        }
        intent.params["relative"] = relative;
        break;

    case VerbShape::RandomizeRotation:
    case VerbShape::RandomizeScale:
    case VerbShape::Objects:
        break;
    }

    if (verb && verb->enabledParam >= 0)
    {
        intent.params["enabled"] = verb->enabledParam != 0;
    }

    // A bare verb over objects is the menu item: it acts on what is selected. Anything
    // else would be a guess about the whole level. An `except` list counts as having said
    // something -- "hide except palm" names a universe on purpose.
    const bool saidNothingAboutWhich = intent.target.filter.empty() &&
        intent.target.exclude.empty() && !intent.target.where.Any();
    if (intent.target.kind == EditorTargetKind::Objects && !scopeStated && saidNothingAboutWhich)
    {
        intent.target.scope = EditorIntentScope::Selected;
    }

    // "delete all" is a level, "delete all rocks" is a chore. The grammar refuses to
    // resolve that difference by itself.
    if (intent.target.kind == EditorTargetKind::Objects &&
        intent.target.scope == EditorIntentScope::All &&
        saidNothingAboutWhich)
    {
        return MakeUnclear(Name(), "Which objects? Name an asset or type, or say 'selected'.");
    }

    intent.kind = EditorIntentKind::Command;
    return intent;
}

std::string GrammarIntentSource::HelpText()
{
    // The heading now lives on the fold that hides this, so the text starts at the syntax.
    return
        "  select|hide|show|delete|duplicate|bury  [selected|all] <name> [except <name>] [within <m>]\n"
        "  spawn|scatter|plant  <count> <asset>\n"
        "  replace  <name> with <asset>\n"
        "  move|rotate|scale  <name> by <x> <y> <z>     ('to' instead of 'by' = absolute)\n"
        "  randomize  yaw|scale  <name>\n"
        "  count  <name>                                (answers, changes nothing)\n"
        "  isolate  <name>   |  frame  <name>   |  similar\n"
        "Examples:  spawn 10 coconut_palm  |  bury coconut_palm  |  randomize yaw palm\n"
        "           replace date_palm with coconut_palm  |  hide all except palm";
}

#endif // WITH_EDITOR
