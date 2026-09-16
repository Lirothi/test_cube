#include "editor/intent/IntentSchema.h"
#if WITH_EDITOR

#include <algorithm>
#include <string>
#include <vector>

#include "editor/EditorObjectMatch.h"
#include "editor/assets/AssetRegistry.h"
#include "editor/scene/EditorZone.h"
#include "editor/intent/EditorActionRegistry.h"
#include "editor/intent/EditorSceneQuery.h"
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

    // Defined below, next to the rest of the reader. Declared here because ParseAnswer
    // sits between the two and both of its branches need it.
    void ReadTargetAndParams(const nlohmann::json& parsed,
        EditorIntentTarget& target,
        nlohmann::json& params);
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
                for (const char* key : editormatch::kAssetNameKeys)
                {
                    const auto it = object.properties.find(key);
                    if (it != object.properties.end() && it->is_string())
                    {
                        PushUnique(vocabulary.needles, it->get<std::string>());
                    }
                }
            }
        }
        // ENVIRONMENT TYPES ARE DELIBERATELY NOT HERE, and they used to be. `needles` is what
        // the prompt presents as "what is ALREADY in the level -- use the exact strings
        // below", and a filter is resolved against `document.Objects()` only. So listing
        // `directionalLight`, `ocean` and `wind` beside `models/coconut_palm.mesh.json`
        // offered the model a word that can never match anything: "выключи солнце" came back
        // as setEnabled with filter ["directionalLight"] -- correct-looking, and empty every
        // time. The right answer for those is `setEnvironment`, and the vocabulary was
        // steering away from it.
        //
        // Environment entities stay reachable the one way they ever were: selected, with a
        // phrase that says nothing about WHICH (EditorIntentResolver's saysNothingAboutWhich).
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
        g += "root ::= command | query | needsapi | unclear\n\n";

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
             " | \"\\\"minY\\\":\" number | \"\\\"maxY\\\":\" number"
             " | \"\\\"zone\\\":\" string\n";
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
        g += "pvalue ::= number | boolean | pstring | range | vec3 | pointlist\n";
        g += "range ::= \"[\" number \",\" number \"]\"\n";
        g += "vec3 ::= \"[\" number \",\" number \",\" number \"]\"\n";
        // A LIST OF POINTS, without which `groundHeight`'s whole reason for existing is
        // unreachable. The query takes up to 64 of them and its own description tells the
        // model to ask for many at once -- tracing a shoreline is that question along a
        // line -- but `pvalue` had no array-of-arrays production, so the sampler blocked
        // the second `[` and forced a single point. The feature was advertised in the
        // prompt and could not be sampled: described, implemented, and unreachable.
        g += "pointlist ::= \"[\" ( range | vec3 ) ( \",\" ( range | vec3 ) )* \"]\"\n";
        g += "pstring ::= string\n\n";

        // --- query ----------------------------------------------------------------
        // A question to the editor rather than an instruction. It reuses `target` and
        // `params` verbatim, which is not laziness: "the bounds of the palms in zone Beach"
        // is the same narrowing as "delete the palms in zone Beach", and giving the query
        // branch its own way to say WHICH would be a second dialect of the same sentence.
        //
        // The names are a CLOSED set for the same reason action ids are: a question the
        // editor cannot answer must be unaskable, not answered with the nearest thing. And
        // the escape hatch is the same one -- needs_api covers what no query can reach.
        {
            std::vector<std::string> queryIds;
            for (const EditorQueryDesc& query : editorquery::All())
            {
                queryIds.push_back(std::string(query.id));
            }
            // A LIST, always, even for one question. A round trip is what costs seconds;
            // a second question inside it costs microseconds. Wanting the waterline AND
            // the island's extent is one thought, and charging two turns for it was the
            // thing that made the three-round cap bite.
            g += "query ::= \"{\\\"kind\\\":\\\"query\\\",\\\"ask\\\":[\" ask "
                 "( \",\" ask )* \"]}\"\n";
            g += "ask ::= \"{\\\"query\\\":\" queryname ( \",\\\"target\\\":\" target )? "
                 "( \",\\\"params\\\":\" params )? \"}\"\n";
            g += "queryname ::= " + Alternation(queryIds) + "\n\n";
        }

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
        if (kind == "query")
        {
            outIntent.kind = EditorIntentKind::Query;
            const auto askIt = parsed.find("ask");
            if (askIt == parsed.end() || !askIt->is_array() || askIt->empty())
            {
                outError = "query answer has no 'ask' list";
                return false;
            }
            for (const nlohmann::json& entry : *askIt)
            {
                EditorIntentQuery ask;
                if (!entry.is_object() || !ReadStringMember(entry, "query", ask.query))
                {
                    outError = "an entry in 'ask' has no query name";
                    return false;
                }
                // Same assertion as the one the command branch makes, for the same reason:
                // the grammar lists the names literally, so an unknown one means the grammar
                // and the query list have drifted, and everything downstream would then rest
                // on a false premise.
                if (!editorquery::Find(ask.query))
                {
                    outError = "model named a query that does not exist: '" + ask.query + "'";
                    return false;
                }
                ReadTargetAndParams(entry, ask.target, ask.params);
                outIntent.asks.push_back(std::move(ask));
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

        ReadTargetAndParams(parsed, outIntent.target, outIntent.params);
        return true;
    }
}

namespace
{
    // The `target` and `params` blocks, which a command and a query spell identically.
    // Kept in one function because they are one wire format: the moment the query branch
    // reads `where` its own way, "in zone Beach" starts meaning two things.
    void ReadTargetAndParams(const nlohmann::json& parsed,
        EditorIntentTarget& target,
        nlohmann::json& params)
    {
        const auto targetIt = parsed.find("target");
        if (targetIt != parsed.end() && targetIt->is_object())
        {
            ReadNeedleList(*targetIt, "filter", target.filter);
            ReadNeedleList(*targetIt, "exclude", target.exclude);

            std::string scope;
            if (ReadStringMember(*targetIt, "scope", scope))
            {
                target.scope = scope == "selected"
                    ? EditorIntentScope::Selected : EditorIntentScope::All;
            }
            ReadStringMember(*targetIt, "asset", target.asset);
            ReadNeedleList(*targetIt, "assets", target.assets);
            // One spelling downstream: a single `asset` is just a list of one, so nothing
            // that consumes this has to ask which of the two fields was used.
            if (target.assets.empty() && !target.asset.empty())
            {
                target.assets.push_back(target.asset);
            }
            else if (!target.assets.empty() && target.asset.empty())
            {
                target.asset = target.assets.front();
            }
            ReadStringMember(*targetIt, "setting", target.setting);

            const auto whereIt = targetIt->find("where");
            if (whereIt != targetIt->end() && whereIt->is_object())
            {
                std::string anchor;
                if (ReadStringMember(*whereIt, "anchor", anchor))
                {
                    target.where.anchor = anchor == "selection"
                        ? EditorSpatialAnchor::Selection : EditorSpatialAnchor::Camera;
                }
                ReadStringMember(*whereIt, "zone", target.where.zone);
                const auto radiusIt = whereIt->find("radius");
                if (radiusIt != whereIt->end() && radiusIt->is_number())
                {
                    target.where.radius = radiusIt->get<float>();
                    if (target.where.anchor == EditorSpatialAnchor::None)
                    {
                        target.where.anchor = EditorSpatialAnchor::Camera;
                    }
                }
                const auto minYIt = whereIt->find("minY");
                if (minYIt != whereIt->end() && minYIt->is_number())
                {
                    target.where.hasMinY = true;
                    target.where.minY = minYIt->get<float>();
                }
                const auto maxYIt = whereIt->find("maxY");
                if (maxYIt != whereIt->end() && maxYIt->is_number())
                {
                    target.where.hasMaxY = true;
                    target.where.maxY = maxYIt->get<float>();
                }
            }
        }

        const auto paramsIt = parsed.find("params");
        if (paramsIt != parsed.end() && paramsIt->is_object())
        {
            params = *paramsIt;
        }
    }
}

#endif // WITH_EDITOR
