#include "editor/intent/IntentSchema.h"
#if WITH_EDITOR

#include <algorithm>
#include <string>
#include <vector>

#include "editor/assets/AssetRegistry.h"
#include "editor/scene/EditorZone.h"
#include "editor/intent/EditorActionRegistry.h"
#include "editor/scene/EditorSceneDocument.h"

namespace
{
    // GBNF string literals are double-quoted and escape with a backslash, and the JSON
    // inside them is itself quoted -- so every literal here goes through two levels of
    // escaping and doing it by hand is how a grammar ends up silently unparseable.
    std::string GbnfLiteral(const std::string& text)
    {
        std::string out = "\"";
        for (const char ch : text)
        {
            if (ch == '"' || ch == '\\')
            {
                out += '\\';
            }
            out += ch;
        }
        out += '"';
        return out;
    }

    // A JSON string literal, as it must appear inside a GBNF literal: \"value\".
    std::string GbnfJsonString(const std::string& value)
    {
        std::string escaped = "\\\"";
        for (const char ch : value)
        {
            if (ch == '"')
            {
                escaped += "\\\\\\\"";
            }
            else if (ch == '\\')
            {
                escaped += "\\\\\\\\";
            }
            else
            {
                escaped += ch;
            }
        }
        escaped += "\\\"";
        return "\"" + escaped + "\"";
    }

    void PushUnique(std::vector<std::string>& values, const std::string& value)
    {
        if (value.empty())
        {
            return;
        }
        if (std::find(values.begin(), values.end(), value) == values.end())
        {
            values.push_back(value);
        }
    }

    std::string Alternation(const std::vector<std::string>& values)
    {
        std::string rule;
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            if (i > 0)
            {
                rule += " | ";
            }
            rule += GbnfJsonString(values[i]);
        }
        return rule;
    }

    bool ReadStringMember(const nlohmann::json& object, const char* key, std::string& out)
    {
        const auto it = object.find(key);
        if (it == object.end() || !it->is_string())
        {
            return false;
        }
        out = it->get<std::string>();
        return true;
    }

    void ReadNeedleList(const nlohmann::json& object, const char* key, std::vector<std::string>& out)
    {
        const auto it = object.find(key);
        if (it == object.end() || !it->is_array())
        {
            return;
        }
        for (const nlohmann::json& entry : *it)
        {
            if (entry.is_string())
            {
                out.push_back(entry.get<std::string>());
            }
        }
    }
}

namespace intentschema
{
    Vocabulary BuildVocabulary(const EditorSceneDocument& document, const AssetRegistry& assets)
    {
        Vocabulary vocabulary;

        // What a filter may say. The level's OWN assets and types, because a filter that
        // names something absent from this level cannot select anything anyway -- and
        // leaving it out of the grammar means the model cannot waste a turn on it.
        for (const EditorObject& object : document.Objects())
        {
            PushUnique(vocabulary.needles, object.type);
            if (object.properties.is_object())
            {
                for (const char* key : { "mesh", "model", "preset", "material" })
                {
                    const auto it = object.properties.find(key);
                    if (it != object.properties.end() && it->is_string())
                    {
                        PushUnique(vocabulary.needles, it->get<std::string>());
                    }
                }
            }
        }
        for (const EditorObject& object : document.Environment())
        {
            PushUnique(vocabulary.needles, object.type);
        }
        for (const editorzone::Zone& zone : editorzone::Collect(document))
        {
            PushUnique(vocabulary.zones, zone.name);
        }

        // What may be CREATED. This is the asset registry, not the level: planting a palm
        // the level has never seen is the whole point of the request that made `spawn`
        // exist, so restricting it to what is already placed would defeat it.
        for (const EditorAssetRecord& record : assets.Assets())
        {
            if (record.id.type == EditorAssetType::Mesh)
            {
                PushUnique(vocabulary.assets, record.id.key);
            }
            // setMaterial's vocabulary. Without it the action exists and cannot be used:
            // the model has no way to learn what this project's materials are called, so it
            // invents a plausible one and the resolver refuses it -- which reads as the
            // feature being broken rather than as the prompt never having said.
            else if (record.id.type == EditorAssetType::MaterialPreset)
            {
                PushUnique(vocabulary.materials, record.id.key);
            }
        }

        std::sort(vocabulary.needles.begin(), vocabulary.needles.end());
        std::sort(vocabulary.assets.begin(), vocabulary.assets.end());
        std::sort(vocabulary.materials.begin(), vocabulary.materials.end());
        // Zones deliberately NOT sorted: document order is creation order, and a list that
        // reads "Beach, North Shore, Lagoon" in the order they were drawn is easier to
        // recognise than the same three alphabetised.
        return vocabulary;
    }

    std::string BuildGbnf(const Vocabulary& vocabulary)
    {
        const EditorActionRegistry& registry = EditorActionRegistry::Builtin();

        std::vector<std::string> actionIds;
        for (const EditorActionDesc& action : registry.Actions())
        {
            actionIds.push_back(std::string(action.id));
        }

        std::string g;
        g += "# Generated from the action registry and this level. Do not hand-edit.\n";
        g += "root ::= command | needsapi | unclear\n\n";

        // --- command ------------------------------------------------------------
        g += "command ::= \"{\\\"kind\\\":\\\"command\\\",\\\"action\\\":\" action "
             "\",\\\"target\\\":\" target ( \",\\\"params\\\":\" params )? \"}\"\n";
        g += "action ::= " + Alternation(actionIds) + "\n\n";

        // EVERY RULE ON ONE LINE. llama.cpp's GBNF parser ends a rule at the newline, so a
        // continuation line beginning with `|` is not a prettier alternation -- it is a
        // parse error, and the server rejects the entire grammar with one unhelpful
        // sentence ("failed to parse grammar") naming neither the rule nor the line.
        g += "target ::= \"{\" ( tmember ( \",\" tmember )* )? \"}\"\n";
        g += "tmember ::= \"\\\"filter\\\":\" needlelist"
             " | \"\\\"exclude\\\":\" needlelist"
             " | \"\\\"scope\\\":\" scope"
             " | \"\\\"asset\\\":\" asset"
             " | \"\\\"assets\\\":\" assetlist"
             " | \"\\\"where\\\":\" where"
             " | \"\\\"setting\\\":\" string\n";
        g += "needlelist ::= \"[\" ( needle ( \",\" needle )* )? \"]\"\n";
        g += "scope ::= \"\\\"all\\\"\" | \"\\\"selected\\\"\"\n";
        g += "where ::= \"{\" ( wmember ( \",\" wmember )* )? \"}\"\n";
        g += "wmember ::= \"\\\"anchor\\\":\" anchor | \"\\\"radius\\\":\" number"
             " | \"\\\"minY\\\":\" number | \"\\\"maxY\\\":\" number\n";
        g += "anchor ::= \"\\\"camera\\\"\" | \"\\\"selection\\\"\"\n\n";

        // NAMES ARE FREE TEXT, AND THAT IS A DELIBERATE REVERSAL OF THE PLAN.
        //
        // E3 said the level's asset names would be listed as literals so that inventing one
        // is impossible by construction. Applied to NAMES that turns out to recreate the
        // very failure E3.1 was written to prevent, one level down: a grammar permitting
        // only the fifteen strings this level happens to use leaves a model asked about
        // something else no way to say so -- it MUST emit one of the fifteen, and the
        // nearest wrong one is indistinguishable from the right one downstream. "Hide the
        // rocks" in a level whose rock asset is spelled unexpectedly would hide the palms.
        //
        // The CLOSED sets stay closed: action ids come from the registry and `needs_api` is
        // the escape hatch, so no wrong choice is ever forced there; scope and anchor are
        // two-valued and complete. Names are an OPEN set, so the grammar guides instead of
        // imprisoning -- the prompt lists them, which is what makes the right answer the
        // cheap one -- and the RESOLVER refuses an unknown one by name before anything runs
        // ("No mesh asset matches 'X'"). Construction where the set is closed, guidance
        // where it is open, and a preview in front of both.
        (void)vocabulary;
        g += "needle ::= string\n";
        g += "asset ::= string\n";
        // `assets` exists because ONE asset was a schema limit pretending to be a design.
        // Asked to plant palms "разного типа", the model looked at a target that has room
        // for exactly one and correctly answered needs_api: there was nowhere to put the
        // second kind. `asset` stays for the single case and for `replace`, whose target is
        // a set of objects and whose destination is one mesh.
        g += "assetlist ::= \"[\" ( asset ( \",\" asset )* )? \"]\"\n\n";

        g += "params ::= \"{\" ( pmember ( \",\" pmember )* )? \"}\"\n";
        g += "pmember ::= pname \":\" pvalue\n";
        // Same reasoning for parameter names, and ValidateParams is what actually refuses a
        // shape that does not fit -- with a message naming the permitted values, which a
        // sampler refusal cannot do. A string parameter may also be genuinely free text
        // (EditorParamKind::String), which an enum-only rule would have made unwritable.
        g += "pname ::= string\n";
        g += "pvalue ::= number | boolean | pstring | range | vec3\n";
        g += "range ::= \"[\" number \",\" number \"]\"\n";
        g += "vec3 ::= \"[\" number \",\" number \",\" number \"]\"\n";
        g += "pstring ::= string\n\n";

        // --- the two branches that execute nothing --------------------------------
        // `needs_api` is the only place free text is allowed, and by definition it runs
        // nothing: its maximum effect is a log line and a sentence on screen (E3.1).
        g += "needsapi ::= \"{\\\"kind\\\":\\\"needs_api\\\",\\\"requested\\\":\" string "
             "\",\\\"proposed\\\":\" string \",\\\"why_existing_dont_fit\\\":\" string \"}\"\n";
        g += "unclear ::= \"{\\\"kind\\\":\\\"unclear\\\",\\\"question\\\":\" string \"}\"\n\n";

        g += "string ::= \"\\\"\" char* \"\\\"\"\n";
        g += "char ::= [^\"\\\\] | \"\\\\\" [\"\\\\/bfnrt]\n";
        g += "boolean ::= \"true\" | \"false\"\n";
        g += "number ::= \"-\"? [0-9]+ ( \".\" [0-9]+ )?\n";
        return g;
    }

    bool ParseAnswer(const std::string& json, EditorIntent& outIntent, std::string& outError)
    {
        nlohmann::json parsed = nlohmann::json::parse(json, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object())
        {
            outError = "model answer was not a JSON object";
            return false;
        }

        std::string kind;
        if (!ReadStringMember(parsed, "kind", kind))
        {
            outError = "model answer has no 'kind'";
            return false;
        }

        outIntent = EditorIntent{};
        outIntent.sourceLabel = "llm";

        if (kind == "needs_api")
        {
            outIntent.kind = EditorIntentKind::NeedsApi;
            ReadStringMember(parsed, "requested", outIntent.requested);
            ReadStringMember(parsed, "proposed", outIntent.proposed);
            ReadStringMember(parsed, "why_existing_dont_fit", outIntent.whyExistingDontFit);
            return true;
        }
        if (kind == "unclear")
        {
            outIntent.kind = EditorIntentKind::Unclear;
            ReadStringMember(parsed, "question", outIntent.question);
            if (outIntent.question.empty())
            {
                outIntent.question = "Could you say which objects you mean?";
            }
            return true;
        }
        if (kind != "command")
        {
            outError = "unknown answer kind '" + kind + "'";
            return false;
        }

        outIntent.kind = EditorIntentKind::Command;
        if (!ReadStringMember(parsed, "action", outIntent.action))
        {
            outError = "command answer has no 'action'";
            return false;
        }
        // The grammar lists the ids literally, so this cannot fire -- which is precisely
        // why it is worth asserting rather than trusting: if it ever does, the grammar and
        // the registry have drifted and everything downstream is built on a false premise.
        const EditorActionDesc* action = EditorActionRegistry::Builtin().Find(outIntent.action);
        if (!action)
        {
            outError = "model named an action that does not exist: '" + outIntent.action + "'";
            return false;
        }
        outIntent.target.kind = action->target;

        const auto targetIt = parsed.find("target");
        if (targetIt != parsed.end() && targetIt->is_object())
        {
            ReadNeedleList(*targetIt, "filter", outIntent.target.filter);
            ReadNeedleList(*targetIt, "exclude", outIntent.target.exclude);

            std::string scope;
            if (ReadStringMember(*targetIt, "scope", scope))
            {
                outIntent.target.scope = scope == "selected"
                    ? EditorIntentScope::Selected : EditorIntentScope::All;
            }
            ReadStringMember(*targetIt, "asset", outIntent.target.asset);
            ReadNeedleList(*targetIt, "assets", outIntent.target.assets);
            // One spelling downstream: a single `asset` is just a list of one, so nothing
            // that consumes this has to ask which of the two fields was used.
            if (outIntent.target.assets.empty() && !outIntent.target.asset.empty())
            {
                outIntent.target.assets.push_back(outIntent.target.asset);
            }
            else if (!outIntent.target.assets.empty() && outIntent.target.asset.empty())
            {
                outIntent.target.asset = outIntent.target.assets.front();
            }
            ReadStringMember(*targetIt, "setting", outIntent.target.setting);

            const auto whereIt = targetIt->find("where");
            if (whereIt != targetIt->end() && whereIt->is_object())
            {
                std::string anchor;
                if (ReadStringMember(*whereIt, "anchor", anchor))
                {
                    outIntent.target.where.anchor = anchor == "selection"
                        ? EditorSpatialAnchor::Selection : EditorSpatialAnchor::Camera;
                }
                const auto radiusIt = whereIt->find("radius");
                if (radiusIt != whereIt->end() && radiusIt->is_number())
                {
                    outIntent.target.where.radius = radiusIt->get<float>();
                    if (outIntent.target.where.anchor == EditorSpatialAnchor::None)
                    {
                        outIntent.target.where.anchor = EditorSpatialAnchor::Camera;
                    }
                }
                const auto minYIt = whereIt->find("minY");
                if (minYIt != whereIt->end() && minYIt->is_number())
                {
                    outIntent.target.where.hasMinY = true;
                    outIntent.target.where.minY = minYIt->get<float>();
                }
                const auto maxYIt = whereIt->find("maxY");
                if (maxYIt != whereIt->end() && maxYIt->is_number())
                {
                    outIntent.target.where.hasMaxY = true;
                    outIntent.target.where.maxY = maxYIt->get<float>();
                }
            }
        }

        const auto paramsIt = parsed.find("params");
        if (paramsIt != parsed.end() && paramsIt->is_object())
        {
            outIntent.params = *paramsIt;
        }
        return true;
    }
}

#endif // WITH_EDITOR
