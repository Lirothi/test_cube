#include "editor/intent/IntentPrompt.h"
#if WITH_EDITOR

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/logging/Log.h"

#include "editor/assets/AssetRegistry.h"
#include "editor/intent/EditorActionRegistry.h"
#include "editor/intent/EditorSceneQuery.h"
#include "editor/scene/EditorSceneDocument.h"

namespace
{
    const char* ParamKindWord(EditorParamKind kind)
    {
        switch (kind)
        {
        case EditorParamKind::Number: return "number";
        case EditorParamKind::Bool:   return "true/false";
        case EditorParamKind::String: return "string";
        case EditorParamKind::Range:  return "[low, high]";
        case EditorParamKind::Vec3:   return "[x, y, z]";
        case EditorParamKind::Enum:   return "enum";
        case EditorParamKind::Any:    return "value";
        }
        return "value";
    }

    void AppendList(std::string& text, const std::vector<std::string>& values, std::size_t limit)
    {
        for (std::size_t i = 0; i < values.size() && i < limit; ++i)
        {
            text += "  " + values[i] + "\n";
        }
        if (values.size() > limit)
        {
            text += "  ... and " + std::to_string(values.size() - limit) + " more\n";
        }
    }
}

namespace
{
    // (continued) The orientation text, or nothing. Read every time the prompt is built, which is once
    // per level: editing the file and reloading the level is the whole edit-test loop.
    std::string ReadIntroFile()
    {
        std::ifstream file("docs/editor_model_intro.md", std::ios::binary);
        if (!file)
        {
            LOG_WARNING(logging::LogCategory::Editor,
                "intent model: docs/editor_model_intro.md not found -- the model gets no "
                "orientation, only the rules");
            return {};
        }
        std::string text((std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());
        if (text.empty())
        {
            return {};
        }
        if (text.back() != '\n')
        {
            text += '\n';
        }
        return text + "\n";
    }
}

namespace intentprompt
{
    std::string ModelNameFromPath(const std::string& modelPath)
    {
        if (modelPath.empty())
        {
            return std::string{};
        }
        const std::string::size_type slash = modelPath.find_last_of("/\\");
        std::string name = slash == std::string::npos
            ? modelPath
            : modelPath.substr(slash + 1);
        const std::string::size_type dot = name.rfind('.');
        if (dot != std::string::npos && name.compare(dot, std::string::npos, ".gguf") == 0)
        {
            name.erase(dot);
        }
        return name;
    }

    std::string BuildSystemPrompt(const EditorSceneDocument& document,
        const AssetRegistry& assets,
        const intentschema::Vocabulary& vocabulary,
        const std::string& modelName,
        const std::string& grammar)
    {
        (void)assets;

        std::string p;
        // WHERE YOU ARE, FIRST, AND FROM A FILE. Everything after this is rules and lists,
        // and rules land differently when the reader knows what they are looking at.
        //
        // It lives in docs/editor_model_intro.md rather than in this function because it is
        // PROSE ABOUT THE SITUATION, not generated from the registry like everything below
        // it: nobody should rebuild the editor to reword a paragraph, and the person can
        // read exactly what their model was told. Missing file is not an error -- the rest
        // of the prompt is what makes the thing work.
        p += ReadIntroFile();
        // THINK SHORT HERE. This turn is allowed to reason -- the grammar is held back
        // until the answer's opening brace -- and left unsaid, a plan-shaped request gets a
        // plan-shaped deliberation: two minutes and sixteen seconds of weighing which zone
        // to trace, the whole token budget gone, and no command at all. The budget is not
        // a thinking allowance, it is somebody sitting in front of the editor.
        p += "THINK BRIEFLY BEFORE YOU ANSWER, and then answer. A few sentences is the\n";
        p += "right amount: which action, what it applies to, whether you need to look\n";
        p += "something up first. That is a decision, not a plan -- do not weigh options\n";
        p += "you have already rejected, do not rehearse the whole task, and do not write\n";
        p += "out what you will do after this command. If the request is too big for one\n";
        p += "command, say so with `unclear` or do the first part; do not think your way\n";
        p += "through all of it. Every second spent thinking is a second somebody is\n";
        p += "watching a box that says \"thinking\", and a reply that never arrives because\n";
        p += "the budget ran out is worse than a plain one that does.\n\n";
        p += "You translate a level designer's instruction into ONE editor command.\n";
        p += "The designer usually writes Russian; the JSON you emit is always English.\n";
        p += "Answer with a single JSON object and nothing else.\n\n";

        p += "FIVE ANSWERS ARE ALLOWED, and choosing between them is your actual job:\n";
        p += "  {\"kind\":\"command\", ...}    -- the editor can do this\n";
        p += "  {\"kind\":\"query\", ...}      -- you need to SEE something first; ask, then act\n";
        p += "  {\"kind\":\"chat\"}            -- not about editing this level at all\n";
        p += "  {\"kind\":\"needs_api\", ...}  -- the editor has NO action for this\n";
        p += "  {\"kind\":\"unclear\", ...}    -- you need one more detail to be safe\n\n";

        // ONE BOX, and the model decides which kind of thing was typed. There used to be
        // two windows -- a command bar and a chat -- and the person had to know which to
        // type into. The fork belongs to whatever understands the sentence, not to the
        // person writing it.
        p += "CHAT IS FOR EVERYTHING THAT IS NOT AN EDIT. Greetings, how you are, what a\n";
        p += "word means, why the water looks wrong, graphics, maths, how this engine is\n";
        p += "built, what you can do. Answer {\"kind\":\"chat\"} and NOTHING else -- the\n";
        p += "editor will immediately ask you again, in prose, with the conversation so far,\n";
        p += "and THAT is where you say your piece. Do not try to answer here: this reply is\n";
        p += "twenty tokens and a decision, not a sentence.\n";
        p += "BUT AN INSTRUCTION IS NOT CHAT, however casually it is put. \"убери эти\",\n";
        p += "\"давай посадим пальм\", \"can you hide the rocks\" are commands. The test is\n";
        p += "whether doing something to the level would answer them; if it would, it is not\n";
        p += "chat. A question ABOUT the level that an action can answer -- \"сколько тут\n";
        p += "пальм\" -- is `count`, not chat.\n";
        // A QUESTION ABOUT YOURSELF IS NOT A TASK. Right after grouping 614 objects, asked
        // "и что ты сделал?", the model answered with another `group` command -- it read a
        // question about the work as an instruction to carry on with it. The answer to that
        // question is in the conversation, which only the prose turn can see; no action in
        // the registry can produce it.
        p += "BUT A QUESTION ABOUT WHAT *YOU* DID IS CHAT, always. \"что ты сделал\", \"и\n";
        p += "что дальше\", \"почему ты так решил\", \"ты уверен\" -- these are about the\n";
        p += "conversation, not about the level, and no action can answer them. Do not read\n";
        p += "them as permission to carry on with the last task.\n\n";

        // WHAT IT IS, stated rather than recalled. A model asked its own version answers
        // from training data -- it knows the family it belongs to and nothing about the
        // file somebody actually loaded, so it names a release, rounds the parameter count
        // or invents a quantisation. The file name is the one authoritative answer in the
        // building, and it costs about thirty tokens of prefix to hand it over.
        if (!modelName.empty())
        {
            p += "WHAT YOU ARE. You are `" + modelName + "` -- the exact file this editor\n";
            p += "loaded -- running on this machine through llama.cpp. Nothing you are told\n";
            p += "leaves the computer: there is no cloud and no API behind you.\n";
            p += "WHEN ASKED WHICH MODEL OR WHICH VERSION YOU ARE, give that name exactly as\n";
            p += "written above, quantisation suffix included. Do not work it out from what\n";
            p += "you remember about yourself and do not round it off -- your own guess at\n";
            p += "your version is the one fact you are reliably wrong about, and the name\n";
            p += "above is the file on disk.\n\n";
        }

        // This is the paragraph the whole three-branch design exists for. Instruct models
        // are trained to be helpful and will reach for a near-miss unless told, in these
        // words, that refusing is the better answer.
        p += "PREFER needs_api OVER A SIMILAR ACTION. A command that does not run costs\n";
        p += "nothing. A command that runs and was the wrong one damages the level. If the\n";
        p += "designer asks to paint something red and there is no colour action, say\n";
        p += "needs_api -- do NOT pick the nearest action that happens to exist.\n";
        p += "In why_existing_dont_fit, name the actions you considered and why each fails.\n";
        p += "That field is also your protection against refusing something that IS possible:\n";
        p += "if you cannot name a reason, the action probably exists and you should use it.\n\n";

        p += "ACTIONS (use the descriptions, not the names -- 'bury' means push into the\n";
        p += "ground, it does NOT mean hide):\n";
        for (const EditorActionDesc& action : EditorActionRegistry::Builtin().Actions())
        {
            p += "- " + std::string(action.id) + ": " + std::string(action.description) + "\n";
            switch (action.target)
            {
            case EditorTargetKind::Objects:
                p += "    target: objects already in the level (use target.filter)\n";
                break;
            case EditorTargetKind::Asset:
                p += "    target: an asset to create from (use target.asset)\n";
                break;
            case EditorTargetKind::Environment:
                p += "    target: one of the settings listed below (use target.setting)\n";
                break;
            case EditorTargetKind::None:
                p += "    target: none\n";
                break;
            }
            for (const EditorActionParam& param : action.params)
            {
                p += "    params." + std::string(param.name) + " (" + ParamKindWord(param.kind);
                if (param.required)
                {
                    p += ", required";
                }
                p += "): " + std::string(param.description);
                if (!param.values.empty())
                {
                    p += " [";
                    for (std::size_t i = 0; i < param.values.size(); ++i)
                    {
                        if (i > 0)
                        {
                            p += " | ";
                        }
                        p += std::string(param.values[i]);
                    }
                    p += "]";
                }
                p += "\n";
            }
        }

        // WHAT THIS PROMPT CANNOT TELL YOU is the point of the query branch. Everything
        // above is built when the level loads and must stay byte-identical afterwards --
        // the server's prefix cache is what makes a request 3 s instead of 20 -- so
        // nothing that changes while somebody edits can live up here. Asking is the way
        // to see something current, and the cost is one whole extra answer.
        p += "\nQUERIES ask the EDITOR a question and change nothing. The editor answers with\n";
        p += "JSON, and you get another turn to act on what it said. Ask when a number you\n";
        p += "need is not above -- where something is, how big it is, what is selected right\n";
        p += "now. Do NOT ask when the answer is already in this prompt.\n";
        // The round trip is the expense, not the question, and a model that does not know
        // that asks one thing per turn and spends a minute on what one turn could answer.
        p += "ASK FOR EVERYTHING YOU NEED IN ONE GO -- `ask` is a list, and three questions\n";
        p += "in one turn cost what one costs. You get at most three turns of asking before\n";
        p += "the editor stops waiting, so a turn spent on a single question is wasted.\n";
        for (const EditorQueryDesc& query : editorquery::All())
        {
            p += "- " + std::string(query.id) + ": " + std::string(query.description) + "\n";
            if (query.usesTarget)
            {
                p += "    target: the same filter/scope/where a command uses\n";
            }
            if (!query.params.empty())
            {
                p += "    params." + std::string(query.params) + "\n";
            }
        }

        p += "\nTARGET.FILTER names what is ALREADY in the level. Use the exact strings below;\n";
        p += "a Russian word like \"palms\" must become the asset string that means it.\n";
        AppendList(p, vocabulary.needles, 80);

        p += "\nTARGET.ASSET names something to CREATE, and may be any of these, whether or not\n";
        p += "the level already uses it. TARGET.ASSETS is the same list when a phrase asks for\n";
        p += "several KINDS at once (\"palms of different types\") -- one spawn, not three:\n";
        AppendList(p, vocabulary.assets, 80);

        if (!vocabulary.materials.empty())
        {
            p += "\nsetMaterial's `material` parameter must be one of these:\n";
            AppendList(p, vocabulary.materials, 80);
        }

        // ALWAYS SAID, even when the answer is "none", and that is the whole point. The
        // section used to be printed only when the level had zones, so a level with none
        // said nothing about them -- and silence reads as "not mentioned", not as "there
        // are zero". Asked to plant on the south shore of a zoneless atoll, the model
        // passed zone:"South Shore": not a guess about the world, a guess about what the
        // editor had, filling in a section that was missing rather than empty.
        if (vocabulary.zones.empty())
        {
            p += "\nZONES: this level has NONE. The `zone` parameter therefore cannot be\n";
            p += "used at all, and target.where.zone matches nothing. To say WHERE without a\n";
            p += "zone, ask for bounds and pass the point as spawn's `at`.\n";
        }
        else
        {
            p += "\nZONES are named regions someone drew on this level. Two ways to use one,\n";
            p += "and which one depends on whether objects are being MADE or FOUND:\n";
            p += "  spawn        -> params.zone: \"<name>\"   fills the region with new things\n";
            p += "  anything else-> target.where.zone: \"<name>\"  narrows to what is ALREADY\n";
            p += "                 inside it -- \"удали пальмы в зоне Beach\", \"сколько камней\n";
            p += "                 в зоне Meadow\". Leave radius and anchor alone when using it.\n";
            // A NAME IN THIS LIST IS VOCABULARY, NOT AN INSTRUCTION. Asked to tidy up the
            // outliner -- a sentence with no place in it at all -- the model read
            // `zones:["test"]` out of sceneSummary and quietly added where.zone:"test" to
            // the group command. 51 objects of 610 were grouped and the answer looked like
            // a success. A zone existing is not a reason to use it, and that has to be said
            // here: the empty case above already says what to do when there are none, and
            // the case that bit was the opposite one.
            p += "ONLY WHEN THEY NAME THE PLACE. A zone existing is not a reason to narrow\n";
            p += "to it. If the phrase does not mention that region -- \"наведи порядок\",\n";
            p += "\"сгруппируй пальмы\", \"сколько тут камней\" -- leave where.zone OUT and\n";
            p += "act on the whole level. Adding it silently turns the answer into a\n";
            p += "different, smaller question than the one you were asked.\n";
            p += "This level has:\n";
            AppendList(p, vocabulary.zones, 80);
        }

        // The CURRENT values are the useful half of this list. "Make the fog thicker" is
        // only answerable against what it is now, and a model that can see 0.012 can pick
        // a scale instead of inventing an absolute number out of nothing.
        {
            std::vector<envsettings::Setting> settings = envsettings::Enumerate(document);

            // SHALLOW FIRST, and the reason is not tidiness. A section like `ocean.render`
            // carries dozens of tuning knobs that nobody addresses in a sentence, and in
            // document order they crowded out `wind.strength` and `cameraExposure.*` --
            // the ones a phrase actually names -- while doubling the prompt and with it the
            // prefill every request pays. The resolver still knows the deep ones by name;
            // they simply do not need to be recited here.
            std::stable_sort(settings.begin(), settings.end(),
                [](const envsettings::Setting& a, const envsettings::Setting& b)
                {
                    const auto depth = [](const std::string& path)
                    {
                        return std::count(path.begin(), path.end(), '.');
                    };
                    const auto da = depth(a.path);
                    const auto db = depth(b.path);
                    return da != db ? da < db : a.path < b.path;
                });

            p += "\nTARGET.SETTING names one environment knob, for the setEnvironment action.\n";
            p += "These are the ones this level has, with their CURRENT values:\n";
            constexpr std::size_t kListed = 55;
            std::size_t shown = 0;
            for (const envsettings::Setting& setting : settings)
            {
                if (shown >= kListed)
                {
                    break;
                }
                p += "  " + setting.path + " = " + envsettings::ToText(setting.current) +
                    " (" + envsettings::KindName(setting.kind) + ")\n";
                ++shown;
            }
            if (settings.size() > shown)
            {
                p += "  (plus " + std::to_string(settings.size() - shown) +
                    " deeper tuning settings such as ocean.render.* -- name one exactly if\n";
                p += "   the designer asks for it by name, otherwise stay with the list above)\n";
            }
        }

        p += "\nSCOPE: \"all\" means everything in the level that matches the filter;\n";
        p += "\"selected\" means only what the designer has selected right now.\n";
        p += "The editor currently has " + std::to_string(document.Objects().size()) + " objects.\n\n";

        // The fields below are the ones a model skips unless it has SEEN them used. Left
        // undemonstrated, "hide everything except the palms" came back as a filter on the
        // palms with enabled=true -- the exact opposite of the request, and valid JSON.
        p += "TARGET FIELDS -- use the right one, they are not interchangeable:\n";
        p += "  filter  : act ON these. \"bury the palms\" -> filter lists the palm assets.\n";
        p += "  exclude : act on everything EXCEPT these. A phrase with \"except\", \"apart from\",\n";
        p += "            \"everything but\" or \"krome\" needs exclude, and usually an EMPTY filter.\n";
        p += "  asset   : the thing to create (spawn) or to switch to (replace). `replace` needs\n";
        p += "            BOTH -- filter for what changes, asset for what it becomes.\n";
        p += "  where   : a distance limit. \"within 50 m of the camera\" -> "
             "{\"anchor\":\"camera\",\"radius\":50}.\n";
        p += "            Or a named region: \"in zone Beach\" -> {\"zone\":\"Beach\"}.\n";
        // HEIGHT HAS BEEN IN THE GRAMMAR AND THE READER ALL ALONG and was described
        // nowhere, so "удали всё под водой" had to ask for the waterline and then had
        // nothing to do with the answer. The comment two paragraphs down says a field left
        // undemonstrated goes unused; these two were not even mentioned.
        p += "            Or a height band: minY / maxY, in world metres. \"everything below\n";
        p += "            the waterline\" -> ask waterLevel, then {\"maxY\": <that y>}.\n";
        p += "  scope   : \"all\" is the whole level; \"selected\" is what the designer has\n";
        p += "            picked right now. A phrase that says \"these\", \"this one\",\n";
        p += "            \"выделенные\" or names nothing at all means SELECTED -- and then\n";
        p += "            the filter is usually empty, because the selection already says\n";
        p += "            which. Omitting scope means \"all\", which is rarely what\n";
        p += "            \"подвинь их повыше\" meant.\n";
        p += "            Leaving where out silently widens the command to the whole level.\n\n";

        p += "EXAMPLES\n";
        p += "  \"zaroy vse palmy\" (bury all palms)\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"bury\","
             "\"target\":{\"filter\":[\"models/coconut_palm.mesh.json\"],\"scope\":\"all\"}}\n";
        p += "  \"rassadi 10 novyh palm\" (plant 10 new palms)\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"spawn\","
             "\"target\":{\"asset\":\"models/coconut_palm.mesh.json\"},\"params\":{\"count\":10}}\n";
        // The multi-asset example earns its place the hard way: `assets` shipped working and
        // went UNUSED -- across a whole battery the model wrote `asset` every time, because
        // every example it could see wrote `asset`. Described but never demonstrated is the
        // same as absent. Note the count too: it is the TOTAL, shared between the kinds, not
        // a count per kind, and a run without this said 10 when asked for 30.
        p += "  \"posadi 20 palm raznogo tipa\" (plant 20 palms of different types)\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"spawn\","
             "\"target\":{\"assets\":[\"models/coconut_palm.mesh.json\","
             "\"models/curly_palm.mesh.json\",\"models/date_palm.mesh.json\"]},"
             "\"params\":{\"count\":20}}\n";
        p += "  \"spryach' vsyo krome palm\" (hide everything except the palms)\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"setEnabled\","
             "\"target\":{\"exclude\":[\"models/coconut_palm.mesh.json\"],\"scope\":\"all\"},"
             "\"params\":{\"enabled\":false}}\n";
        p += "  \"zameni finikovye palmy na kokosovye\" (replace the date palms with coconut ones)\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"replace\","
             "\"target\":{\"filter\":[\"models/date_palm.mesh.json\"],"
             "\"asset\":\"models/coconut_palm.mesh.json\",\"scope\":\"all\"}}\n";
        p += "  \"uberi kamni v radiuse 50 metrov ot kamery\" (remove the rocks within 50 m of the camera)\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"delete\","
             "\"target\":{\"filter\":[\"models/beach_rock.mesh.json\"],\"scope\":\"all\","
             "\"where\":{\"anchor\":\"camera\",\"radius\":50}}}\n";
        // The two shapes that were described and never shown. `assets` taught this lesson
        // once already: a field the examples do not use is a field the model does not use.
        // Groups are a label, so the second half of the story is the important half: the
        // name becomes an ordinary filter, and nothing else had to learn about groups.
        p += "  \"sgruppiruy eti palmy v Severnuyu roshchu\" (group these palms into "
             "\"Северная роща\")\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"group\","
             "\"target\":{\"scope\":\"selected\"},"
             "\"params\":{\"name\":\"Северная роща\"}}\n";
        p += "     ...and afterwards the group's NAME is a filter like any other:\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"setEnabled\","
             "\"target\":{\"filter\":[\"Северная роща\"],\"scope\":\"all\"},"
             "\"params\":{\"enabled\":false}}\n";
        p += "  \"podnimi eti na dva metra\" (raise THESE by two metres) -- what is selected\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"move\","
             "\"target\":{\"scope\":\"selected\"},"
             "\"params\":{\"value\":[0,2,0],\"relative\":true}}\n";
        p += "  \"udali vsyo pod vodoy\" (delete everything below the waterline)\n";
        p += "  -> {\"kind\":\"query\",\"ask\":[{\"query\":\"waterLevel\"}]}\n";
        p += "     ...the editor answers {\"waterLevel\":{\"y\":0}}, and then the height band\n";
        p += "     is what carries it:\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"delete\","
             "\"target\":{\"scope\":\"all\",\"where\":{\"maxY\":0}}}\n";
        p += "  \"skolko palm na urovne?\" (how many palms are in the level?)\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"count\","
             "\"target\":{\"filter\":[\"models/coconut_palm.mesh.json\"],\"scope\":\"all\"}}\n";
        p += "  \"sdelay veter silnee\" (make the wind stronger)\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"setEnvironment\","
             "\"target\":{\"setting\":\"wind.strength\"},\"params\":{\"scale\":1.5}}\n";
        p += "  \"postav' gtao intensivnost 0.8\" (set the GTAO intensity to 0.8)\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"setEnvironment\","
             "\"target\":{\"setting\":\"gtao.intensity\"},\"params\":{\"value\":0.8}}\n";
        // The query example earns its place the way the multi-asset one did: a branch that
        // is described but never demonstrated goes unused. Note that it asks about
        // something the prompt genuinely cannot say -- the island's extent -- and note the
        // SECOND line, which is what the model does with the answer when it arrives.
        p += "  \"posadi palmy po krayu ostrova, tolko nad vodoy\" (plant palms around the "
             "island's edge, only above water) -- ask for BOTH facts in one turn\n";
        p += "  -> {\"kind\":\"query\",\"ask\":["
             "{\"query\":\"bounds\",\"target\":{\"filter\":"
             "[\"models/atoll_island.mesh.json\"],\"scope\":\"all\"}},"
             "{\"query\":\"waterLevel\"}]}\n";
        p += "     ...the editor answers {\"bounds\":{\"centre\":[-2,-4.7,-4],"
             "\"size\":[361,15,388],...},\"waterLevel\":{\"y\":0}}, and THEN you act on the\n";
        p += "     NUMBERS THAT CAME BACK -- not on the ones in this example:\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"spawn\","
             "\"target\":{\"asset\":\"models/coconut_palm.mesh.json\"},"
             "\"params\":{\"count\":10,\"at\":[-2,0,-4],\"radius\":180,\"minHeight\":0.3}}\n";

        // THIS ENGINE HAS NO COMPASS, and the first version of this example invented one --
        // "the north edge is the largest z" -- which is a convention that exists nowhere in
        // the code. The model then used it, correctly, for a sentence that said SOUTH. A
        // fabricated fact in the prompt is worse than a missing one: it is obeyed.
        p += "  THERE IS NO NORTH. This engine has no compass and no world orientation, so\n";
        p += "  \"the north shore\" names nothing you can compute. If a zone is called that,\n";
        p += "  use the zone. Otherwise ask which side is meant:\n";
        p += "  \"postav' kamen' na severnom beregu\" (put a rock on the north shore), no "
             "such zone\n";
        p += "  -> {\"kind\":\"unclear\",\"question\":\"There is no compass in this level -- "
             "which side do you mean, +X, -X, +Z or -Z?\"}\n";
        p += "  \"pokras' palmy v krasnyy\" (paint the palms red)\n";
        p += "  -> {\"kind\":\"needs_api\",\"requested\":\"paint objects red\","
             "\"proposed\":\"setBaseColor(objects, rgba)\","
             "\"why_existing_dont_fit\":\"replace swaps the whole mesh and there is no "
             "action that changes only a colour\"}\n";
        p += "  \"privet, kak dela\" / \"pochemu voda temnaya?\" / \"chto takoe SDSM?\"\n";
        p += "  -> {\"kind\":\"chat\"}\n";
        p += "  \"udali derevya\" (delete the trees) when several tree assets exist\n";
        p += "  -> {\"kind\":\"unclear\",\"question\":\"Which trees -- coconut, date or curly palms?\"}\n";

        // THE GRAMMAR ITSELF, LAST. The sampler is constrained by this whether the model
        // has read it or not: a token outside it simply cannot be emitted. Describing the
        // shapes in prose and hiding the actual rule was leaving it to discover the walls
        // by walking into them -- and a model that knows only the prose spends its choice
        // on forms that were never reachable. It is the authority here, so it is quoted
        // rather than paraphrased, and it says so.
        if (!grammar.empty())
        {
            p += "\nTHE GRAMMAR YOUR ANSWER IS SAMPLED AGAINST. This is not advice and not a\n";
            p += "summary -- it is the actual GBNF the server enforces on every token you\n";
            p += "emit for a command. Anything it does not allow you literally cannot say,\n";
            p += "and every name in it is a name that exists in THIS level:\n";
            p += "```gbnf\n";
            p += grammar;
            if (grammar.back() != '\n')
            {
                p += '\n';
            }
            p += "```\n";
            p += "Read it when you are unsure whether a form is legal. It is also the honest\n";
            p += "answer to \"can you do X\": if no rule spells X, the answer is needs_api.\n";
        }
        return p;
    }

    std::string ApplyChatTemplate(const std::string& systemPrompt,
        const std::string& userPhrase,
        const std::string& templateName,
        const std::vector<IntentTurn>& history,
        bool reasoning)
    {
        if (templateName == "plain")
        {
            std::string text = systemPrompt;
            for (const IntentTurn& turn : history)
            {
                text += "\n\nInstruction: " + turn.user + "\nJSON: " + turn.assistant;
            }
            return text + "\n\nInstruction: " + userPhrase + "\nJSON: ";
        }
        // ChatML, which Qwen and most current instruct models speak. Applying it here
        // rather than going through the server's chat endpoint keeps the prefix BYTE
        // IDENTICAL between requests, which is what the prefill cache keys on. History is
        // appended AFTER the system block for the same reason -- the cached prefix must
        // not move when a follow-up answer arrives.
        std::string text = "<|im_start|>system\n" + systemPrompt + "<|im_end|>\n";
        for (const IntentTurn& turn : history)
        {
            text += "<|im_start|>user\n" + turn.user + "<|im_end|>\n";
            text += "<|im_start|>assistant\n" + turn.assistant + "<|im_end|>\n";
        }
        text += "<|im_start|>user\n" + userPhrase + "<|im_end|>\n";
        text += "<|im_start|>assistant\n";

        // QWEN3 AND ITS RELATIVES THINK BEFORE THEY ANSWER, and a grammar that demands JSON
        // from the first token does not let them: the reasoning block is the first thing
        // they want to emit, the grammar forbids it, and the two fight over every token.
        // Pre-filling an EMPTY think block is the documented way to say "you have finished
        // thinking" -- it is part of the prompt, so the grammar never sees it, and
        // generation starts where the answer belongs.
        //
        // Costs nothing on a model that does not think: it reads as a stray tag it ignores.
        // `chatml-think` is here for the day someone wants the reasoning back.
        if (!reasoning)
        {
            text += "<think>\n\n</think>\n\n";
        }
        return text;
    }
}

#endif // WITH_EDITOR
