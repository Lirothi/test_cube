// CPU-only regression for the editor's natural-language command layer
// (docs/editor_llm_plan.md, E1/E3.1/E5/E6/E7). Build the engine Debug|x64, then this
// tool's vcxproj. No window, no device, no level or asset writes.
//
// What it is actually guarding:
//   * the grammar never GUESSES -- an unknown verb must decline, not pick something near;
//   * the registry and the grammar agree on which action ids exist (the plan's whole point
//     is that an action name cannot be invented, and that only holds while these two agree);
//   * PARAMETERS cannot be invented either -- ValidateParams refuses a wrong shape before
//     anything is previewed, and performs exactly the broadenings it documents;
//   * the selector reaches the same objects the outliner's search box would, and the
//     exclude / spatial narrowings actually narrow;
//   * an asset name resolves to ONE asset, or says it is ambiguous rather than picking;
//   * the refusals that protect a level: "all" with no filter, and a filter that matched
//     nothing.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <thread>
#include <string>
#include <vector>

#include "app/levels/LevelManager.h"
#include "app/scene/Scene.h"
#include "editor/EditorContext.h"
#include "editor/EditorExtensionRegistry.h"
#include "editor/assets/AssetRegistry.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/scene/EditorZone.h"
#include "editor/intent/EditorActionRegistry.h"
#include "editor/intent/EditorIntentResolver.h"
#include "editor/intent/EnvironmentSettings.h"
#include "editor/intent/GrammarIntentSource.h"
#include "editor/intent/IntentNotes.h"
#include "editor/intent/IntentPrompt.h"
#include "editor/EditorObjectMatch.h"
#include "editor/intent/EditorSceneQuery.h"
#include "editor/intent/IntentSchema.h"
#include "editor/intent/LlmIntentSource.h"
#include "rendering/core/Renderer.h"

using Json = nlohmann::json;

void Check(bool condition, const char* label)
{
    if (!condition)
    {
        throw std::runtime_error(label);
    }
}

void Check(bool condition, const std::string& label)
{
    if (!condition)
    {
        throw std::runtime_error(label);
    }
}

bool Near(float a, float b)
{
    return std::fabs(a - b) < 0.0001f;
}

// `key` is which property carries the asset. Real levels use BOTH: "mesh" is the modern
// .mesh.json reference and what the atoll's hundred palms are made of, "model" is the older
// raw-file one. The fixture used only "model" and so never noticed that "mesh" was missing
// from the search predicate -- a level full of date palms could not be searched for
// "date_palm" at all, in the outliner as well as here.
EditorObject MakeMeshWithKey(uint64_t id, const char* name, const char* key, const char* asset,
    Math::float3 position)
{
    EditorObject object;
    object.id.value = id;
    object.name = name;
    object.type = "staticMesh";
    object.transform.position = position;
    object.properties = Json{ { key, asset } };
    return object;
}

EditorObject MakeMesh(uint64_t id, const char* name, const char* model, Math::float3 position)
{
    return MakeMeshWithKey(id, name, "model", model, position);
}

// The level the plan's own example talks about: palms whose Russian name shares no
// letter with any asset name, plus something that must NOT be mistaken for one.
EditorSceneDocument MakePalmDocument()
{
    EditorSceneDocument document;
    document.Objects().push_back(MakeMesh(1, "Palm_001", "models/coconut_palm.mesh.json", { 0.0f, 0.0f, 0.0f }));
    document.Objects().push_back(MakeMesh(2, "Palm_002", "models/coconut_palm.mesh.json", { 10.0f, 0.0f, 0.0f }));
    // On the MODERN reference, like every palm in the real atoll level.
    document.Objects().push_back(MakeMeshWithKey(3, "Palm_003", "mesh",
        "models/date_palm.mesh.json", { 100.0f, 0.0f, 0.0f }));
    document.Objects().push_back(MakeMesh(4, "Rock_001", "models/beach_rock.mesh.json", { 0.0f, 20.0f, 0.0f }));
    return document;
}

// ------------------------------------------------------------------ the registry

// The query list, the grammar and the reader are three statements of one contract, and the
// only reason the action registry survives is that a test says so out loud. A query the
// grammar permits but the dispatcher cannot run would be answered with a sentence saying
// it is not implemented -- valid, useless, and invisible until somebody typed the phrase
// that reached it.
void TestQueryListIntegrity()
{
    Check(!editorquery::All().empty(), "there is at least one query");
    for (const EditorQueryDesc& query : editorquery::All())
    {
        Check(!query.id.empty(), "query has an id");
        Check(query.description.size() > 20, "query carries a real description for the model");
        Check(editorquery::Find(std::string(query.id)) == &query, "Find returns the same entry");
    }
    Check(editorquery::Find("no_such_query") == nullptr, "unknown query id is not resolved");

    // Every name the grammar offers must be one the dispatcher answers. Reading it out of
    // the generated grammar rather than the list is the point: this catches the two drifting
    // apart, which comparing the list with itself could not.
    EditorSceneDocument document = MakePalmDocument();
    AssetRegistry assets;
    assets.Refresh();
    const intentschema::Vocabulary vocabulary = intentschema::BuildVocabulary(document, assets);
    const std::string gbnf = intentschema::BuildGbnf(vocabulary);
    Check(gbnf.find("root ::= command | query |") != std::string::npos,
        "the grammar offers the query branch at the root");
    const std::size_t line = gbnf.find("queryname ::=");
    Check(line != std::string::npos, "the grammar has a queryname rule");
    const std::string rule = gbnf.substr(line, gbnf.find('\n', line) - line);
    for (const EditorQueryDesc& query : editorquery::All())
    {
        Check(rule.find("\\\"" + std::string(query.id) + "\\\"") != std::string::npos,
            "grammar lists query '" + std::string(query.id) + "'");
    }

    Check(gbnf.find("ask ::=") != std::string::npos,
        "the grammar spells a query as a LIST of asks");
    // groundHeight takes up to 64 points and its description tells the model to ask for
    // many at once. `pvalue` had no array-of-arrays production, so the sampler could not
    // emit one: a feature described in the prompt, implemented in the editor, and
    // unreachable in between.
    Check(gbnf.find("pointlist ::=") != std::string::npos &&
        gbnf.find("| pointlist") != std::string::npos,
        "a parameter can be a LIST of points, which groundHeight exists to take");

    // And the reader accepts what the grammar can produce, including the target it shares
    // with a command -- "the bounds of the palms" is the same narrowing as "delete the palms".
    EditorIntent intent;
    std::string error;
    Check(intentschema::ParseAnswer(
        R"({"kind":"query","ask":[)"
        R"({"query":"bounds","target":{"filter":["coconut_palm"],"scope":"all"}},)"
        R"({"query":"waterLevel"}]})",
        intent, error), "a batched query answer parses: " + error);
    Check(intent.kind == EditorIntentKind::Query, "kind is Query");
    Check(intent.asks.size() == 2, "both questions survive the read");
    Check(intent.asks[0].query == "bounds" && intent.asks[1].query == "waterLevel",
        "query names survive the read, in order");
    Check(intent.asks[0].target.filter.size() == 1 &&
        intent.asks[0].target.filter[0] == "coconut_palm",
        "a query carries the same target a command would");
    // Each ask owns its own target. Sharing one would make "the bounds of the palms and of
    // the rocks" quietly mean the bounds of whichever was read last.
    Check(intent.asks[1].target.filter.empty(), "a second ask does not inherit the first's target");

    Check(!intentschema::ParseAnswer(R"({"kind":"query","ask":[{"query":"invent_something"}]})",
        intent, error), "a query name that does not exist is refused, not answered");
    Check(!intentschema::ParseAnswer(R"({"kind":"query","ask":[]})", intent, error),
        "an empty ask list is refused rather than answered with nothing");
}

void TestRegistryIntegrity()
{
    const EditorActionRegistry& registry = EditorActionRegistry::Builtin();
    Check(!registry.Actions().empty(), "registry is not empty");

    for (const EditorActionDesc& action : registry.Actions())
    {
        Check(!action.id.empty(), "action has an id");
        // The description is what stops a model from guessing `bury` means "hide".
        Check(action.description.size() > 20, "action carries a real description");
        const bool wantsBuilder = action.effect == EditorActionEffect::DocumentEdit;
        Check(wantsBuilder == (action.build != nullptr),
            "only DocumentEdit actions build a command; selection-only and read-only do not");
        Check(registry.Find(action.id) == &action, "Find returns the same entry");

        for (const EditorActionParam& param : action.params)
        {
            Check(!param.name.empty(), "parameter has a name");
            Check(!param.description.empty(), "parameter carries a description for the model");
            Check((param.kind == EditorParamKind::Enum) == !param.values.empty(),
                "only enum parameters list permitted values");
        }
    }
    Check(registry.Find("no_such_action") == nullptr, "unknown id is not resolved");

    // spawn is the action whose noun is an asset rather than level objects. If that ever
    // silently goes back to Objects, "plant ten palms" starts matching existing palms.
    const EditorActionDesc* spawn = registry.Find("spawn");
    Check(spawn && spawn->target == EditorTargetKind::Asset, "spawn targets an asset");
}

// -------------------------------------------------------------- parameter typing

void TestParamsCannotBeInvented()
{
    const EditorActionDesc& spawn = *EditorActionRegistry::Builtin().Find("spawn");
    const EditorActionDesc& scale = *EditorActionRegistry::Builtin().Find("scale");
    const EditorActionDesc& randomizeRotation =
        *EditorActionRegistry::Builtin().Find("randomizeRotation");
    std::string error;

    Json missing = Json::object();
    Check(!ValidateParams(spawn, missing, error) && !error.empty(),
        "a missing required parameter is refused with a reason");

    Json wrongType = Json{ { "count", "ten" } };
    Check(!ValidateParams(spawn, wrongType, error), "a count of \"ten\" is refused");

    Json good = Json{ { "count", 10 } };
    Check(ValidateParams(spawn, good, error), "a well-formed parameter set passes");

    // The two broadenings the declaration promises, performed in ONE place so neither
    // source has to remember them.
    Json lone = Json{ { "value", 2.0 } };
    Check(ValidateParams(scale, lone, error), "a lone number is accepted where a vec3 belongs");
    Check(lone["value"].is_array() && lone["value"].size() == 3 &&
        Near(lone["value"][1].get<float>(), 2.0f), "and is broadened to all three axes");

    Json range = Json{ { "range", 45.0 } };
    Check(ValidateParams(randomizeRotation, range, error), "a lone number is accepted for a range");
    Check(range["range"].is_array() && range["range"].size() == 2,
        "and is broadened to [n, n]");

    Json badRange = Json{ { "range", Json::array({ 1.0, 2.0, 3.0 }) } };
    Check(!ValidateParams(randomizeRotation, badRange, error), "three numbers are not a range");

    Json badEnum = Json{ { "axis", "sideways" } };
    Check(!ValidateParams(randomizeRotation, badEnum, error), "an unknown enum value is refused");
    Json goodEnum = Json{ { "axis", "all" } };
    Check(ValidateParams(randomizeRotation, goodEnum, error), "a declared enum value passes");
}

// ------------------------------------------------------------------- the grammar

void TestGrammarDeclinesRatherThanGuesses(GrammarIntentSource& grammar,
    const EditorSceneDocument& document)
{
    // The phrase the whole plan is built around. The grammar must NOT answer it: no verb
    // it knows starts the sentence, and a near-miss here is a wrong edit on 247 objects.
    std::string whyNot;
    const EditorIntent intent = grammar.Parse("make all the palm trees red", whyNot);
    Check(intent.kind == EditorIntentKind::None, "unknown phrasing is declined");
    Check(!whyNot.empty(), "declining says why, so the caller can fall through to the model");

    std::string ignored;
    Check(grammar.Parse("", ignored).kind == EditorIntentKind::None,
        "empty phrase is declined");
    Check(grammar.Parse("   ", ignored).kind == EditorIntentKind::None,
        "blank phrase is declined");

    // Russian goes to the model by design; the grammar must not half-answer it.
    Check(grammar.Parse("\xd1\x80\xd0\xb0\xd1\x81\xd1\x81\xd0\xb0\xd0\xb4\xd0\xb8 10", ignored)
        .kind == EditorIntentKind::None,
        "a Russian phrase is declined, not half-parsed");
}

void TestGrammarEmitsOnlyRealActions(GrammarIntentSource& grammar,
    const EditorSceneDocument& document)
{
    // Every verb the help text advertises must round-trip to an action that exists. This
    // is the check that keeps "an action name cannot be invented" true for the fast path.
    const char* const phrases[] = {
        "select coconut_palm",
        "hide coconut_palm",
        "show coconut_palm",
        "enable coconut_palm",
        "disable coconut_palm",
        "delete coconut_palm",
        "remove coconut_palm",
        "duplicate coconut_palm",
        "bury coconut_palm",
        "spawn 10 coconut_palm",
        "scatter 3 coconut_palm",
        "plant 1 coconut_palm",
        "replace date_palm with coconut_palm",
        "move coconut_palm by 0 2 0",
        "rotate coconut_palm by 0 90 0",
        "scale coconut_palm by 1.2",
        "randomize yaw coconut_palm",
        "randomize scale coconut_palm",
    };
    for (const char* phrase : phrases)
    {
        std::string whyNot;
        const EditorIntent intent = grammar.Parse(phrase, whyNot);
        Check(intent.kind == EditorIntentKind::Command, phrase);
        Check(EditorActionRegistry::Builtin().Find(intent.action) != nullptr,
            std::string("grammar emitted an action the registry knows: ") + phrase);
        Check(intent.sourceLabel == "grammar", "intent names its source");
    }

    std::string whyNot;
    const EditorIntent hide = grammar.Parse("hide coconut_palm", whyNot);
    Check(hide.params.contains("enabled") && hide.params["enabled"].get<bool>() == false,
        "hide passes enabled=false");
    const EditorIntent show = grammar.Parse("show coconut_palm", whyNot);
    Check(show.params["enabled"].get<bool>() == true, "show passes enabled=true");
}

void TestGrammarSentenceShapes(GrammarIntentSource& grammar, const EditorSceneDocument& document)
{
    std::string whyNot;

    const EditorIntent bare = grammar.Parse("delete", whyNot);
    Check(bare.kind == EditorIntentKind::Command &&
        bare.target.scope == EditorIntentScope::Selected,
        "a bare verb acts on the selection, like the menu item does");

    const EditorIntent explicitSelected = grammar.Parse("bury selected", whyNot);
    Check(explicitSelected.target.scope == EditorIntentScope::Selected &&
        explicitSelected.target.filter.empty(),
        "'selected' is a scope word, not a filter needle");

    // The asset goes in target.asset, NOT in the filter: spawn's noun is a thing to
    // create, and putting it in the filter would make it a search over existing objects.
    const EditorIntent spawn = grammar.Parse("spawn 10 coconut_palm", whyNot);
    Check(spawn.target.kind == EditorTargetKind::Asset, "spawn targets an asset");
    Check(spawn.target.asset == "coconut_palm", "the asset is the asset, not a filter needle");
    Check(spawn.target.filter.empty(), "and the filter stays empty");
    Check(spawn.params["count"].get<int>() == 10, "the count is a count");

    const EditorIntent spawnOne = grammar.Parse("plant coconut_palm", whyNot);
    Check(spawnOne.params["count"].get<int>() == 1, "an omitted count means one");

    // The verb with TWO nouns.
    const EditorIntent replace = grammar.Parse("replace date_palm with coconut_palm", whyNot);
    Check(replace.target.filter.size() == 1 && replace.target.filter[0] == "date_palm",
        "replace selects by the first noun");
    Check(replace.target.asset == "coconut_palm", "and switches to the second");

    // Negation.
    const EditorIntent except = grammar.Parse("hide all except palm", whyNot);
    Check(except.target.exclude.size() == 1 && except.target.exclude[0] == "palm",
        "'except' fills the exclude list");

    // Spatial narrowing.
    const EditorIntent within = grammar.Parse("select palm within 50", whyNot);
    Check(within.target.where.radius > 49.0f &&
        within.target.where.anchor == EditorSpatialAnchor::Camera,
        "'within N' means N metres from the camera");
    const EditorIntent ofSelection =
        grammar.Parse("select palm within 50 of selection", whyNot);
    Check(ofSelection.target.where.anchor == EditorSpatialAnchor::Selection,
        "'of selection' moves the anchor");

    // Relative vs absolute.
    const EditorIntent by = grammar.Parse("move palm by 0 2 0", whyNot);
    Check(by.params["relative"].get<bool>(), "'by' is relative");
    const EditorIntent to = grammar.Parse("move palm to 0 2 0", whyNot);
    Check(!to.params["relative"].get<bool>(), "'to' is absolute");

    // Things it must refuse rather than guess at.
    const EditorIntent dangerous = grammar.Parse("delete all", whyNot);
    Check(dangerous.kind == EditorIntentKind::Unclear && !dangerous.question.empty(),
        "'delete all' asks which objects instead of deleting the level");
    Check(grammar.Parse("randomize palm", whyNot).kind == EditorIntentKind::Unclear,
        "'randomize' alone asks rotation or scale");
    Check(grammar.Parse("move palm", whyNot).kind == EditorIntentKind::Unclear,
        "a transform with no number asks by how much");
    Check(grammar.Parse("replace palm", whyNot).kind == EditorIntentKind::Unclear,
        "replace with no asset asks for one");
    Check(grammar.Parse("spawn 10", whyNot).kind == EditorIntentKind::Unclear,
        "spawn with no asset asks for one");
    Check(grammar.Parse("move palm by two", whyNot).kind == EditorIntentKind::Unclear,
        "a word where a number belongs is refused, not silently zero");
}

// ------------------------------------------------------------------ the resolver

// Selecting a ZONE and saying "in the selected zone" means the REGION, not the selection.
// Reported from the editor as "Nothing in the selection matches": scope `selected` means
// "among the selected objects", the selection held one zone, and a zone is not a palm.
void TestSelectedZoneMeansItsArea(const EditorActionContext& actionCtx)
{
    EditorContext& ctx = actionCtx.editor;

    // A circle at the origin, radius 20: it covers Palm_001 and Palm_002 (x = 0 and 10) and
    // not Palm_003 (x = 100) or the rock, which is inside it in XZ but is not a palm.
    EditorObject zone = editorzone::BuildObject(editorzone::Shape::Circle,
        Math::float3(0.0f, 0.0f, 0.0f), 20.0f, "TestZone");
    zone.id = EditorObjectId{ 900 };
    ctx.document.Objects().push_back(zone);
    ctx.selection.Replace(zone.id);

    EditorIntent intent;
    intent.kind = EditorIntentKind::Command;
    intent.action = "randomizeRotation";
    intent.sourceLabel = "test";
    intent.target.kind = EditorTargetKind::Objects;
    intent.target.filter.push_back("palm");
    intent.target.scope = EditorIntentScope::Selected;

    const EditorIntentPreview preview = BuildIntentPreview(actionCtx, intent);
    Check(preview.executable, "a selected zone resolves to what is inside it");
    Check(preview.targets.size() == 2,
        "the two palms inside the zone, not the one 100 m away and not the zone itself");

    // And the other reading survives: a selection of real objects still means those objects.
    ctx.selection.Clear();
    ctx.selection.Replace(EditorObjectId{ 3 });     // the distant date palm
    const EditorIntentPreview ordinary = BuildIntentPreview(actionCtx, intent);
    Check(ordinary.targets.size() == 1,
        "selecting objects still means those objects, zone rule not applied");

    ctx.selection.Clear();
    ctx.document.Objects().pop_back();
}

void TestSelectorReachesTheSameObjectsAsSearch(const EditorActionContext& actionCtx,
    GrammarIntentSource& grammar)
{
    EditorContext& ctx = actionCtx.editor;
    std::string whyNot;

    const EditorIntent palms = grammar.Parse("bury all coconut_palm", whyNot);
    const EditorIntentPreview palmPreview = BuildIntentPreview(actionCtx, palms);
    Check(palmPreview.executable, "coconut_palm matches");
    Check(palmPreview.targets.size() == 2, "exactly the two coconut palms, not the date palm");
    Check(palmPreview.groups.size() == 1 && palmPreview.groups[0].count == 2,
        "the preview groups them under one asset name");
    Check(palmPreview.undoable, "bury is one undo entry");

    // A needle that is a substring of several assets takes all of them -- the same thing
    // typing it into the outliner's search box does.
    const EditorIntent anyPalm = grammar.Parse("select all palm", whyNot);
    const EditorIntentPreview anyPalmPreview = BuildIntentPreview(actionCtx, anyPalm);
    Check(anyPalmPreview.targets.size() == 3, "'palm' reaches both palm assets");
    Check(anyPalmPreview.groups.size() == 2, "and says which two they were");
    Check(!anyPalmPreview.undoable, "select is not advertised as undoable");
    Check(anyPalmPreview.groups[0].count >= anyPalmPreview.groups[1].count,
        "groups are ordered by how many objects they carry");

    // Negation actually subtracts.
    const EditorIntent except = grammar.Parse("select all palm except date", whyNot);
    const EditorIntentPreview exceptPreview = BuildIntentPreview(actionCtx, except);
    Check(exceptPreview.targets.size() == 2, "'except date' drops the date palm");

    // Spatial narrowing actually narrows: the camera sits at the origin in this harness,
    // and the date palm is 100 m out.
    // `near` is a windows.h macro, hence the name.
    const EditorIntent nearby = grammar.Parse("select all palm within 50", whyNot);
    const EditorIntentPreview nearPreview = BuildIntentPreview(actionCtx, nearby);
    Check(nearPreview.targets.size() == 2, "the palm 100 m away is outside a 50 m radius");

    const EditorIntent nothing = grammar.Parse("delete all wombat", whyNot);
    const EditorIntentPreview nothingPreview = BuildIntentPreview(actionCtx, nothing);
    Check(!nothingPreview.executable && !nothingPreview.problem.empty(),
        "a filter that matched nothing refuses with a reason");

    // Scope Selected narrows to the selection and the filter both.
    ctx.selection.Clear();
    ctx.selection.Add({ 1 }, false);
    ctx.selection.Add({ 4 }, false);
    const EditorIntent inSelection =
        grammar.Parse("bury selected coconut_palm", whyNot);
    const EditorIntentPreview selectionPreview = BuildIntentPreview(actionCtx, inSelection);
    Check(selectionPreview.targets.size() == 1 && selectionPreview.targets[0].value == 1,
        "selection scope intersects the filter rather than replacing it");
    ctx.selection.Clear();
}

void TestWholeLevelIsRefusedEvenIfAnIntentAsksForIt(const EditorActionContext& actionCtx)
{
    // Neither source is allowed to produce this. The resolver still refuses it, because the
    // cost of being wrong here is the level and the cost of the check is one comparison.
    EditorIntent forged;
    forged.kind = EditorIntentKind::Command;
    forged.action = "delete";
    forged.target.scope = EditorIntentScope::All;
    forged.sourceLabel = "forged";
    Check(!BuildIntentPreview(actionCtx, forged).executable,
        "an unfiltered whole-level action is refused");

    EditorIntent unknown;
    unknown.kind = EditorIntentKind::Command;
    unknown.action = "setBaseColor";
    unknown.target.filter.push_back("coconut_palm");
    Check(!BuildIntentPreview(actionCtx, unknown).executable,
        "an action outside the registry cannot be previewed");

    // The Environment slot exists in the schema but no action fills it yet; the resolver
    // must say so rather than appear to work.
    EditorIntent environment;
    environment.kind = EditorIntentKind::Command;
    environment.action = "delete";
    environment.target.kind = EditorTargetKind::Environment;
    environment.target.setting = "fog.density";
    const EditorIntentPreview environmentPreview = BuildIntentPreview(actionCtx, environment);
    Check(!environmentPreview.executable, "an environment target is refused for now");
}

// THE PREVIEW MUST DESCRIBE WHAT ACTUALLY RUNS. All three of these shipped broken and the
// gate did not notice, for one reason worth remembering: every existing preview test builds
// its intent through the GRAMMAR source, and the grammar cannot produce the shapes that were
// wrong. These go through the model's own reader instead.
void TestPreviewDescribesWhatRuns(const EditorActionContext& actionCtx)
{
    EditorIntent intent;
    std::string error;

    // A spawn naming three kinds -- the shape the prompt teaches by example. The preview
    // resolved only `target.asset` while the builder plants from `target.assets`, so this
    // previewed as "20 x Coconut Palm" and planted seven each of three kinds.
    Check(intentschema::ParseAnswer(
        R"({"kind":"command","action":"spawn","target":{"assets":[)"
        R"("models/coconut_palm.mesh.json","models/date_palm.mesh.json",)"
        R"("models/curly_palm.mesh.json"]},"params":{"count":20}})",
        intent, error), "a multi-asset spawn parses: " + error);
    const EditorIntentPreview many = BuildIntentPreview(actionCtx, intent);
    Check(many.executable, "a multi-asset spawn previews: " + many.problem);
    Check(many.resolved.target.assets.size() == 3,
        "all three assets are resolved, not just the first");
    Check(many.groups.size() == 3, "the preview names every kind it is about to plant");
    std::size_t previewed = 0;
    for (const EditorIntentPreview::Group& group : many.groups)
    {
        previewed += group.count;
    }
    Check(previewed == 20, "the previewed counts add up to the count that was asked for");
    for (const std::string& key : many.resolved.target.assets)
    {
        Check(key.find(".mesh.json") != std::string::npos,
            "the builder is handed a resolved key, never the phrase: " + key);
    }

    // A parameter the action does not declare. `params.zone` on `delete` was dropped in
    // silence, and the command then deleted every palm in the level rather than the twelve
    // in the zone -- with a preview that was honest about the number and wrong about the
    // request.
    Check(intentschema::ParseAnswer(
        R"({"kind":"command","action":"delete","target":{"filter":["coconut_palm"],)"
        R"("scope":"all"},"params":{"zone":"Beach"}})", intent, error),
        "a delete carrying a stray zone parses: " + error);
    const EditorIntentPreview stray = BuildIntentPreview(actionCtx, intent);
    Check(!stray.executable, "an undeclared parameter is refused, not dropped");
    Check(stray.problem.find("zone") != std::string::npos,
        "the refusal names the offending parameter: " + stray.problem);
}

// THE REPORT MUST SAY WHICH FAILURE IT WAS. Each of these used to come back as a sentence
// that was true of a different situation, which is the worst kind of wrong: it sends the
// reader off to fix something that was never broken.
void TestRefusalsNameTheRealReason(const EditorActionContext& actionCtx)
{
    EditorContext& ctx = actionCtx.editor;

    // A zone nobody drew. PassesSpatialFilter correctly excludes everything -- a typo must
    // not widen the command to the whole level -- but the report was "No object in the
    // level matches", indistinguishable from there genuinely being none there.
    EditorIntent intent;
    std::string error;
    Check(intentschema::ParseAnswer(
        R"({"kind":"command","action":"delete","target":{"filter":["coconut_palm"],)"
        R"("scope":"all","where":{"zone":"Nowhere"}}})", intent, error),
        "a delete naming an absent zone parses: " + error);
    const EditorIntentPreview absent = BuildIntentPreview(actionCtx, intent);
    Check(!absent.executable, "an absent zone cannot be acted in");
    Check(absent.problem.find("zone") != std::string::npos,
        "the refusal says the ZONE is the problem, not that nothing matched: " + absent.problem);

    // Two zones whose names share a prefix. The exactly-correct word was ambiguous by
    // substring, so the lookup refused and the user read "nothing matches".
    EditorObject beach = editorzone::BuildObject(editorzone::Shape::Circle,
        Math::float3(0.0f, 0.0f, 0.0f), 40.0f, "Beach");
    // NOT AllocateId(). MakePalmDocument pushes ids 1-4 straight into Objects() without
    // going through the allocator, so nextId_ is still 1 and AllocateId hands back an id a
    // palm is already using -- after which the cleanup below removed the palm instead of
    // the zone, and three tests later something unrelated failed.
    std::uint64_t highest = 0;
    for (const EditorObject& object : ctx.document.Objects())
    {
        highest = std::max(highest, object.id.value);
    }
    beach.id = EditorObjectId{ highest + 1 };
    EditorObject beachNorth = editorzone::BuildObject(editorzone::Shape::Circle,
        Math::float3(200.0f, 0.0f, 0.0f), 40.0f, "Beach North");
    beachNorth.id = EditorObjectId{ highest + 2 };
    ctx.document.Add(beach);
    ctx.document.Add(beachNorth);
    // Taken out again at the end of this function. The document is shared with every test
    // after this one, and leaving two zones in it changed what "hide the palms" reached --
    // which is how this test first announced itself, as a failure three tests later.
    struct ZoneCleanup
    {
        EditorSceneDocument& document;
        EditorObjectId a;
        EditorObjectId b;
        ~ZoneCleanup() { document.Remove(a); document.Remove(b); }
    } cleanup{ ctx.document, beach.id, beachNorth.id };
    editorzone::Zone found;
    std::string whyNot;
    Check(editorzone::Find(ctx.document, "Beach", found, whyNot),
        "an exact zone name wins over a longer one that contains it: " + whyNot);
    Check(found.name == "Beach", "and it is the exactly-named zone: " + found.name);

    // A zone is a PLACE. Acting on everything inside one must not consume the region.
    Check(intentschema::ParseAnswer(
        R"({"kind":"command","action":"delete","target":{"scope":"all",)"
        R"("where":{"zone":"Beach"}}})", intent, error),
        "an unfiltered delete inside a zone parses: " + error);
    const EditorIntentPreview inside = BuildIntentPreview(actionCtx, intent);
    for (const EditorObjectId id : inside.targets)
    {
        const EditorObject* object = ctx.document.Find(id);
        Check(!object || object->type != editorzone::kTypeName,
            "the zone itself is not one of the things inside it");
    }

    // And naming it explicitly still reaches it -- the rule is about unfiltered phrases.
    Check(intentschema::ParseAnswer(
        R"({"kind":"command","action":"delete","target":{"filter":["Beach"],"scope":"all"}})",
        intent, error), "a delete naming the zone parses: " + error);
    const EditorIntentPreview named = BuildIntentPreview(actionCtx, intent);
    Check(!named.targets.empty(), "a zone named in the filter is still a target");
}

// The filter vocabulary is presented to the model as "what is ALREADY in the level -- use
// the exact strings below". A word in it that can never match anything is a trap the prompt
// itself sets.
void TestVocabularyOffersNothingUnreachable(const EditorSceneDocument& document,
    const AssetRegistry& assets)
{
    // WITH AN ENVIRONMENT ENTITY IN IT, which the first version of this test did not have --
    // so it passed against the very bug it was written for. MakePalmDocument has no
    // Environment() entries, the loop that used to feed their types into the filter
    // vocabulary had nothing to feed, and the check proved only that the meshes were fine.
    EditorSceneDocument withEnvironment = document;
    EditorObject sun;
    sun.id = EditorObjectId{ 9001 };
    sun.name = "Sun";
    sun.type = "directionalLight";
    sun.properties = nlohmann::json::object();
    withEnvironment.Environment().push_back(sun);

    const intentschema::Vocabulary vocabulary =
        intentschema::BuildVocabulary(withEnvironment, assets);
    for (const std::string& needle : vocabulary.needles)
    {
        bool reachable = false;
        // Objects() only, which is the point: a filter is resolved against those and nothing
        // else, so a word that matches only an Environment() entry is a word that can never
        // select anything.
        for (const EditorObject& object : withEnvironment.Objects())
        {
            if (editormatch::MatchesSearch(object, needle))
            {
                reachable = true;
                break;
            }
        }
        Check(reachable, "every filter word the prompt offers can match something: '" +
            needle + "'");
    }
}

// A GROUP IS ONLY USEFUL IF ITS NAME IS A FILTER. Grouping that produced a label nothing
// could then select would be a tidy-looking dead end: the point of "сгруппируй эти в
// Северную рощу" is the sentence after it, "а теперь спрячь её".
void TestGroupsAreFilterable(const EditorActionContext& actionCtx)
{
    EditorContext& ctx = actionCtx.editor;
    EditorCommandStack commandStack;
    EditorIntent intent;
    std::string error;

    Check(intentschema::ParseAnswer(
        R"({"kind":"command","action":"group","target":{"filter":["coconut_palm"],)"
        R"("scope":"all"},"params":{"name":"North Grove"}})", intent, error),
        "a group command parses: " + error);
    const EditorIntentPreview grouped = BuildIntentPreview(actionCtx, intent);
    Check(grouped.executable, "grouping the palms previews: " + grouped.problem);
    const std::size_t historyBefore = commandStack.HistorySize();
    std::string status;
    Check(ExecuteIntent(actionCtx, commandStack, grouped, status), "grouping runs: " + status);
    Check(commandStack.HistorySize() == historyBefore + 1,
        "grouping several objects is ONE undo entry");

    // The name must now reach them the same way an asset name does -- through the
    // outliner's own predicate, which is what the command bar resolves through.
    Check(intentschema::ParseAnswer(
        R"({"kind":"command","action":"setEnabled","target":{"filter":["North Grove"],)"
        R"("scope":"all"},"params":{"enabled":false}})", intent, error),
        "a command filtering on the group name parses: " + error);
    const EditorIntentPreview byName = BuildIntentPreview(actionCtx, intent);
    Check(byName.executable, "the group's name selects its members: " + byName.problem);
    Check(byName.targets.size() == grouped.targets.size(),
        "and reaches exactly the objects that were grouped");

    // Undo puts the property back, which is what makes this safe to try.
    commandStack.Undo(ctx);
    const EditorIntentPreview afterUndo = BuildIntentPreview(actionCtx, intent);
    Check(!afterUndo.executable, "undo removes the group, so the name stops matching");
}

// Two ways a narrowing could quietly stop narrowing, and one way a verdict could describe
// a different verb than the one that would run.
void TestNarrowingsCannotEvaporate(const EditorActionContext& actionCtx)
{
    EditorIntent intent;
    std::string error;

    // A negative radius passed Any() on the strength of its anchor and was then dropped by
    // the distance test, so "delete the palms within -50 m" reached every palm in the level.
    Check(intentschema::ParseAnswer(
        R"({"kind":"command","action":"delete","target":{"filter":["coconut_palm"],)"
        R"("scope":"all","where":{"anchor":"camera","radius":-50}}})", intent, error),
        "a negative radius parses: " + error);
    const EditorIntentPreview negative = BuildIntentPreview(actionCtx, intent);
    Check(!negative.executable, "a negative radius is refused, not silently dropped");

    // A stray `asset` on an Objects verb used to enter asset resolution, so the preview read
    // "delete 3 objects -> models/coconut_palm.mesh.json" -- which reads as a replace.
    Check(intentschema::ParseAnswer(
        R"({"kind":"command","action":"delete","target":{"filter":["coconut_palm"],)"
        R"("asset":"models/coconut_palm.mesh.json","scope":"all"}})", intent, error),
        "a delete carrying a stray asset parses: " + error);
    const EditorIntentPreview stray = BuildIntentPreview(actionCtx, intent);
    Check(stray.executable, "the stray asset does not break the delete: " + stray.problem);
    Check(stray.summary.find("->") == std::string::npos,
        "and the verdict does not describe it as becoming something: " + stray.summary);

    // `replace` must still resolve its destination -- it is the verb the flag exists for.
    Check(intentschema::ParseAnswer(
        R"({"kind":"command","action":"replace","target":{"filter":["coconut_palm"],)"
        R"("asset":"date_palm","scope":"all"}})", intent, error),
        "a replace parses: " + error);
    const EditorIntentPreview replace = BuildIntentPreview(actionCtx, intent);
    Check(replace.executable, "replace still resolves its destination: " + replace.problem);
    Check(replace.summary.find("->") != std::string::npos,
        "and says what they become: " + replace.summary);
}

void TestAssetResolution(const EditorActionContext& actionCtx, GrammarIntentSource& grammar)
{
    EditorContext& ctx = actionCtx.editor;
    std::string whyNot;

    const EditorIntent spawn = grammar.Parse("spawn 10 coconut_palm", whyNot);
    const EditorIntentPreview preview = BuildIntentPreview(actionCtx, spawn);
    Check(preview.executable, "the preview resolved coconut_palm: " + preview.problem);
    Check(preview.targets.empty(), "spawn touches no existing object");
    Check(preview.groups.size() == 1 && preview.groups[0].count == 10,
        "the preview says how many are about to appear");
    // The resolved intent carries the registry key, never the word the user typed.
    Check(preview.resolved.target.asset.find("coconut_palm") != std::string::npos &&
        preview.resolved.target.asset != "coconut_palm",
        "the phrase was resolved to a concrete asset key: " + preview.resolved.target.asset);

    const EditorIntent bogus = grammar.Parse("spawn 3 wombat_of_doom", whyNot);
    const EditorIntentPreview bogusPreview = BuildIntentPreview(actionCtx, bogus);
    Check(!bogusPreview.executable, "an asset that does not exist cannot be spawned");

    // A count outside the declared range is clamped, not obeyed.
    const EditorIntent many = grammar.Parse("spawn 10000 coconut_palm", whyNot);
    const EditorIntentPreview manyPreview = BuildIntentPreview(actionCtx, many);
    Check(manyPreview.executable && manyPreview.groups[0].count <= 200,
        "an absurd count is clamped");
}

// ------------------------------------------------------------------- execution

void TestOnePhraseIsOneUndo(const EditorActionContext& actionCtx, GrammarIntentSource& grammar)
{
    EditorContext& ctx = actionCtx.editor;
    EditorCommandStack stack;
    std::string whyNot;
    std::string status;

    const EditorIntent hide = grammar.Parse("hide all palm", whyNot);
    const EditorIntentPreview preview = BuildIntentPreview(actionCtx, hide);
    Check(preview.targets.size() == 3, "three palms to hide");
    Check(ExecuteIntent(actionCtx, stack, preview, status), status);
    Check(stack.HistorySize() == 1, "three objects, ONE history entry");
    for (const EditorObject& object : ctx.document.Objects())
    {
        const bool isPalm = object.id.value <= 3;
        Check(object.enabled != isPalm, "the palms are hidden and the rock is not");
    }

    stack.Undo(ctx);
    Check(stack.AppliedCount() == 0, "one Ctrl+Z is enough");
    for (const EditorObject& object : ctx.document.Objects())
    {
        Check(object.enabled, "undo restored every palm");
    }

    // A selection-only action changes the selection and leaves no history behind.
    const EditorIntent select = grammar.Parse("select all date_palm", whyNot);
    const EditorIntentPreview selectPreview = BuildIntentPreview(actionCtx, select);
    Check(ExecuteIntent(actionCtx, stack, selectPreview, status), status);
    Check(stack.HistorySize() == 1, "select added no history entry");
    Check(ctx.selection.Size() == 1 && ctx.selection.Ordered()[0].value == 3,
        "select changed the selection");
    ctx.selection.Clear();
}

void TestTransforms(const EditorActionContext& actionCtx, GrammarIntentSource& grammar)
{
    EditorContext& ctx = actionCtx.editor;
    EditorCommandStack stack;
    std::string whyNot;
    std::string status;

    const Math::float3 before = ctx.document.Find({ 1 })->transform.position;

    const EditorIntent move = grammar.Parse("move all coconut_palm by 0 2 0", whyNot);
    const EditorIntentPreview movePreview = BuildIntentPreview(actionCtx, move);
    Check(ExecuteIntent(actionCtx, stack, movePreview, status), status);
    Check(Near(ctx.document.Find({ 1 })->transform.position.y, before.y + 2.0f),
        "'by' added to the height");
    Check(Near(ctx.document.Find({ 1 })->transform.position.x, before.x),
        "and left the other axes alone");

    const EditorIntent moveTo = grammar.Parse("move all coconut_palm to 5 5 5", whyNot);
    const EditorIntentPreview moveToPreview = BuildIntentPreview(actionCtx, moveTo);
    Check(ExecuteIntent(actionCtx, stack, moveToPreview, status), status);
    Check(Near(ctx.document.Find({ 1 })->transform.position.y, 5.0f), "'to' set an absolute position");

    // Relative scale MULTIPLIES. If this ever becomes addition, a 0.05 pebble turns into
    // a boulder and nothing in the preview would have warned about it.
    ctx.document.Find({ 1 })->transform.scale = Math::float3(2.0f, 2.0f, 2.0f);
    const EditorIntent scale = grammar.Parse("scale all coconut_palm by 1.5", whyNot);
    const EditorIntentPreview scalePreview = BuildIntentPreview(actionCtx, scale);
    Check(ExecuteIntent(actionCtx, stack, scalePreview, status), status);
    Check(Near(ctx.document.Find({ 1 })->transform.scale.x, 3.0f), "relative scale multiplies");

    const std::size_t historyBefore = stack.HistorySize();
    stack.Undo(ctx);
    Check(stack.AppliedCount() == historyBefore - 1, "each phrase is one entry");
    Check(Near(ctx.document.Find({ 1 })->transform.scale.x, 2.0f), "and undo puts the scale back");
}

void TestRandomizeIsVariedAndDeterministic(const EditorActionContext& actionCtx)
{
    EditorContext& ctx = actionCtx.editor;
    EditorCommandStack stack;
    std::string status;

    EditorIntent randomize = MakeActionIntent("randomizeRotation", Json{ { "seed", 7 } });
    randomize.target.filter.push_back("palm");
    const EditorIntentPreview preview = BuildIntentPreview(actionCtx, randomize);
    Check(preview.targets.size() == 3, "three palms to randomize");
    Check(ExecuteIntent(actionCtx, stack, preview, status), status);

    const float a = ctx.document.Find({ 1 })->transform.rotationDeg.y;
    const float b = ctx.document.Find({ 2 })->transform.rotationDeg.y;
    const float c = ctx.document.Find({ 3 })->transform.rotationDeg.y;
    // The entire point: copies must stop looking like copies.
    Check(!Near(a, b) && !Near(b, c), "each object got its OWN angle");
    Check(Near(ctx.document.Find({ 1 })->transform.rotationDeg.x, 0.0f),
        "yaw only by default -- a randomly pitched tree lies on its side");

    stack.Undo(ctx);

    // Same seed, same answer: a pinned seed is what makes a layout reproducible.
    const EditorIntentPreview again = BuildIntentPreview(actionCtx, randomize);
    Check(ExecuteIntent(actionCtx, stack, again, status), status);
    Check(Near(ctx.document.Find({ 1 })->transform.rotationDeg.y, a),
        "a pinned seed reproduces the layout");
    stack.Undo(ctx);
}

void TestSpawnPlacement(const EditorActionContext& actionCtx)
{
    // BuildSpawn is called directly rather than executed: creating the runtime objects
    // needs a GPU device this harness deliberately does not have, while the placement --
    // the part with the rejection rules in it -- is pure CPU.
    const EditorActionDesc& spawn = *EditorActionRegistry::Builtin().Find("spawn");

    EditorIntent intent = MakeActionIntent("spawn", Json{
        { "count", 8 },
        { "radius", 40.0 },
        { "minSeparation", 5.0 },
        { "alignToGround", false },   // no terrain in a CPU-only scene to land on
        { "seed", 11 },
    });
    intent.target.kind = EditorTargetKind::Asset;

    const EditorIntentPreview preview = BuildIntentPreview(actionCtx, intent);
    Check(!preview.executable, "spawn without an asset is refused");

    intent.target.asset = "models/coconut_palm.mesh.json";
    const EditorIntentPreview resolved = BuildIntentPreview(actionCtx, intent);
    Check(resolved.executable, "spawn with a real asset previews: " + resolved.problem);

    std::string status;
    std::unique_ptr<EditorCommand> command =
        spawn.build(actionCtx, {}, resolved.resolved, status);
    Check(command != nullptr, "spawn built a command: " + status);
    Check(status.find("Spawned") != std::string::npos, "and said what it placed: " + status);

    // Separation is the rule that turns a pile into a scatter, and it has two failure
    // modes that must read differently. Asking for more than fits: place what does, and
    // SAY how many were asked for -- a quiet shortfall is the kind that gets noticed
    // three edits later.
    EditorIntent crowded = intent;
    crowded.params["count"] = 50;
    crowded.params["radius"] = 30.0;
    crowded.params["minSeparation"] = 12.0;
    const EditorIntentPreview crowdedPreview = BuildIntentPreview(actionCtx, crowded);
    std::string crowdedStatus;
    std::unique_ptr<EditorCommand> crowdedCommand =
        spawn.build(actionCtx, {}, crowdedPreview.resolved, crowdedStatus);
    Check(crowdedCommand != nullptr, "a crowded scatter still places what it can");
    Check(crowdedStatus.find("asked for 50") != std::string::npos,
        "and reports the shortfall instead of hiding it: " + crowdedStatus);

    // Asking for something that cannot be placed at all: no command, and a reason that
    // names which rule did the rejecting rather than a bare "failed".
    EditorIntent impossible = intent;
    impossible.params["count"] = 50;
    impossible.params["radius"] = 6.0;
    impossible.params["minSeparation"] = 20.0;
    const EditorIntentPreview impossiblePreview = BuildIntentPreview(actionCtx, impossible);
    std::string impossibleStatus;
    std::unique_ptr<EditorCommand> impossibleCommand =
        spawn.build(actionCtx, {}, impossiblePreview.resolved, impossibleStatus);
    Check(impossibleCommand == nullptr, "a scatter with nowhere to go builds nothing");
    Check(impossibleStatus.find("too crowded") != std::string::npos,
        "and says which rule refused: " + impossibleStatus);
}

void TestReplace(const EditorActionContext& actionCtx, GrammarIntentSource& grammar)
{
    EditorContext& ctx = actionCtx.editor;
    const EditorActionDesc& replace = *EditorActionRegistry::Builtin().Find("replace");
    std::string whyNot;

    const EditorIntent intent =
        grammar.Parse("replace all date_palm with coconut_palm", whyNot);
    const EditorIntentPreview preview = BuildIntentPreview(actionCtx, intent);
    Check(preview.executable, "replace previews: " + preview.problem);
    Check(preview.targets.size() == 1 && preview.targets[0].value == 3,
        "it selected by the first noun");
    Check(preview.resolved.target.asset.find("coconut_palm") != std::string::npos,
        "and resolved the second to an asset key");
    Check(preview.summary.find("->") != std::string::npos,
        "the preview shows what they become: " + preview.summary);

    std::string status;
    std::unique_ptr<EditorCommand> command =
        replace.build(actionCtx, preview.targets, preview.resolved, status);
    Check(command != nullptr, "replace built a command: " + status);
}


// ------------------------------------------------- the contract with the model
//
// The grammar and the reader are two statements of one contract. Neither needs the model
// present to be checked, which is the point: almost everything that can go wrong between
// the editor and llama.cpp is a pure function of the registry and the level.

void TestGeneratedGrammar(const EditorSceneDocument& document, const AssetRegistry& assets)
{
    const intentschema::Vocabulary vocabulary = intentschema::BuildVocabulary(document, assets);
    Check(!vocabulary.needles.empty(), "the level contributes filter names");
    Check(!vocabulary.assets.empty(), "the asset registry contributes spawnable assets");

    const std::string gbnf = intentschema::BuildGbnf(vocabulary);
    Check(!gbnf.empty(), "a grammar was generated");

    // Every action id must appear literally. This is the whole guarantee of E3: an action
    // the registry does not have cannot be sampled, because the grammar never offers it.
    for (const EditorActionDesc& action : EditorActionRegistry::Builtin().Actions())
    {
        Check(gbnf.find(std::string(action.id)) != std::string::npos,
            std::string("grammar lists action ") + std::string(action.id));
    }
    // Names are NOT nailed into the grammar -- see IntentSchema.cpp for why a closed name
    // list recreates E3.1's forced-wrong-choice one level down. They are guidance in the
    // prompt, and the resolver is what refuses an unknown one (covered by TestAssetResolution).
    Check(gbnf.find("needle ::= string") != std::string::npos,
        "names are free text in the grammar, not a closed list");
    Check(gbnf.find("scope ::=") != std::string::npos && gbnf.find("selected") != std::string::npos,
        "the genuinely closed sets are still closed");

    // The three branches, including the two that execute nothing.
    Check(gbnf.find("needs_api") != std::string::npos, "grammar allows an honest refusal");
    Check(gbnf.find("unclear") != std::string::npos, "grammar allows a question");
    Check(gbnf.find("root ::=") != std::string::npos, "grammar has a root rule");

    const std::string prompt = intentprompt::BuildSystemPrompt(document, assets, vocabulary);
    // The instruction that the whole three-branch design rests on. Instruct models reach
    // for a near-miss unless told, in words, that refusing is better.
    Check(prompt.find("PREFER needs_api") != std::string::npos,
        "the prompt tells the model to refuse rather than approximate");
    // The names live here instead: guidance, so the right answer is the cheap one.
    Check(prompt.find("coconut_palm") != std::string::npos,
        "the prompt lists the level's palm asset");
    for (const EditorActionDesc& action : EditorActionRegistry::Builtin().Actions())
    {
        Check(prompt.find(std::string(action.description).substr(0, 24)) != std::string::npos,
            std::string("the prompt carries the description of ") + std::string(action.id));
    }

    // The prefix must be byte-identical between requests or the server's prefill cache
    // never hits and every phrase pays for the whole vocabulary again (E4).
    const std::string a = intentprompt::ApplyChatTemplate(prompt, "one", "chatml");
    const std::string b = intentprompt::ApplyChatTemplate(prompt, "two", "chatml");
    const std::size_t split = a.find("one");
    Check(split != std::string::npos && a.substr(0, split) == b.substr(0, split),
        "two requests share a byte-identical prompt prefix");

    // A follow-up answer to an `unclear` question carries the earlier exchange, and must
    // append it AFTER the system block -- otherwise the cached prefix moves and every
    // follow-up pays for the whole vocabulary again.
    std::vector<IntentTurn> history;
    history.push_back({ "udali derevya", R"({"kind":"unclear","question":"Which trees?"})" });
    const std::string followUp = intentprompt::ApplyChatTemplate(prompt, "coconut", "chatml", history);
    Check(followUp.substr(0, split) == a.substr(0, split),
        "a follow-up keeps the same cached prefix");
    Check(followUp.find("udali derevya") != std::string::npos &&
        followUp.find("Which trees?") != std::string::npos,
        "and carries the earlier exchange so the answer is read as an answer");
}

void TestModelAnswersAreRead()
{
    EditorIntent intent;
    std::string error;

    Check(intentschema::ParseAnswer(
        R"({"kind":"command","action":"spawn","target":{"asset":"models/coconut_palm.mesh.json"},"params":{"count":10}})",
        intent, error), error);
    Check(intent.kind == EditorIntentKind::Command, "a command answer is a command");
    Check(intent.action == "spawn" && intent.target.asset == "models/coconut_palm.mesh.json",
        "the asset survived the trip");
    Check(intent.target.kind == EditorTargetKind::Asset,
        "the target kind comes from the registry, not from the model");
    Check(intent.params["count"].get<int>() == 10, "the count survived");
    Check(intent.sourceLabel == "llm", "the answer knows where it came from");

    Check(intentschema::ParseAnswer(
        R"({"kind":"command","action":"bury","target":{"filter":["models/coconut_palm.mesh.json"],"scope":"all","where":{"anchor":"camera","radius":50}}})",
        intent, error), error);
    Check(intent.target.scope == EditorIntentScope::All, "scope survived");
    Check(intent.target.where.anchor == EditorSpatialAnchor::Camera &&
        intent.target.where.radius > 49.0f, "the spatial filter survived");

    // The branch that executes nothing, by definition.
    Check(intentschema::ParseAnswer(
        // A custom delimiter, because the proposed signature contains `)"` and the
        // default one would end the literal in the middle of the JSON.
        R"json({"kind":"needs_api","requested":"paint red","proposed":"setBaseColor(objects, rgba)","why_existing_dont_fit":"replace swaps the whole mesh"})json",
        intent, error), error);
    Check(intent.kind == EditorIntentKind::NeedsApi && !intent.proposed.empty(),
        "an honest refusal is carried through intact");

    Check(intentschema::ParseAnswer(R"({"kind":"unclear","question":"Which trees?"})",
        intent, error), error);
    Check(intent.kind == EditorIntentKind::Unclear, "a question is a question");

    // The grammar makes these impossible, which is exactly why they are checked: if one
    // ever gets through, the grammar and the reader have drifted apart.
    Check(!intentschema::ParseAnswer(R"({"kind":"command","action":"setBaseColor"})",
        intent, error), "an action outside the registry is refused");
    Check(!intentschema::ParseAnswer("not json at all", intent, error),
        "a non-JSON answer is refused");
    Check(!intentschema::ParseAnswer(R"({"action":"bury"})", intent, error),
        "an answer with no kind is refused");
}

void TestModelSettingsRoundTrip()
{
    LlmIntentSettings settings;
    settings.modelPath = "D:/llm_models/model.gguf";
    settings.serverExe = "D:/llm_models/llama.cpp/llama-server.exe";
    settings.endpoint = "127.0.0.1:9999";
    settings.gpuLayers = 42;

    Json levelEditor;
    levelEditor["intentModel"] = LlmIntentSource::SaveSettings(settings);
    const LlmIntentSettings loaded = LlmIntentSource::LoadSettings(levelEditor);
    Check(loaded.modelPath == settings.modelPath && loaded.endpoint == settings.endpoint &&
        loaded.gpuLayers == settings.gpuLayers, "model settings round-trip through JSON");

    // No model configured is not an error state -- it is the state the editor ships in,
    // and the source must say so in a sentence rather than look broken (E8).
    LlmIntentSource empty{ LlmIntentSettings{} };
    Check(!empty.Available(), "an unconfigured model source is unavailable");
    Check(!empty.UnavailableReason().empty(), "and explains why in one line");

    LlmIntentSettings missing = settings;
    missing.modelPath = "D:/nope/does_not_exist.gguf";
    LlmIntentSource absent{ missing };
    Check(!absent.Available(), "a missing model file is unavailable");
    Check(absent.UnavailableReason().find("not found") != std::string::npos,
        "and names the file it could not find");
}






// ------------------------------------------------- align / distribute / snap

EditorSceneDocument MakeScatteredDocument()
{
    EditorSceneDocument document;
    document.Objects().push_back(MakeMesh(1, "A", "models/coconut_palm.mesh.json", { 0.0f, 1.0f, 0.0f }));
    document.Objects().push_back(MakeMesh(2, "B", "models/coconut_palm.mesh.json", { 4.3f, 5.0f, 0.0f }));
    document.Objects().push_back(MakeMesh(3, "C", "models/coconut_palm.mesh.json", { 10.0f, 3.0f, 0.0f }));
    // D's height is deliberately OFF a 1 m grid: with an integer height, "snap did not
    // touch y" would pass whether or not snap touched y, which is a test that proves
    // nothing and passed a deliberately broken build until this line changed.
    document.Objects().push_back(MakeMesh(4, "D", "models/coconut_palm.mesh.json", { 1.4f, 9.4f, 2.6f }));
    return document;
}

void TestAlignSharesOneAxisOnly(const EditorActionContext& actionCtx)
{
    EditorContext& ctx = actionCtx.editor;
    EditorCommandStack stack;
    std::string status;

    EditorIntent intent = MakeActionIntent("align", Json{ { "axis", "y" }, { "to", "max" } });
    intent.target.filter.push_back("coconut_palm");
    const EditorIntentPreview preview = BuildIntentPreview(actionCtx, intent);
    Check(preview.executable, "align previews: " + preview.problem);
    Check(ExecuteIntent(actionCtx, stack, preview, status), status);

    for (const EditorObject& object : ctx.document.Objects())
    {
        Check(Near(object.transform.position.y, 9.4f), "every object took the highest y");
    }
    // The point of naming an axis is that the other two are LEFT ALONE. An align that
    // quietly moved x as well would look right in a screenshot and be wrong in the level.
    Check(Near(ctx.document.Find({ 3 })->transform.position.x, 10.0f), "x was not touched");
    Check(Near(ctx.document.Find({ 4 })->transform.position.z, 2.6f), "z was not touched");
    Check(stack.HistorySize() == 1, "one history entry");

    stack.Undo(ctx);
    Check(Near(ctx.document.Find({ 1 })->transform.position.y, 1.0f), "undo restores heights");

    // Re-running when they already agree must not stack a no-op entry that a later undo
    // has to walk past.
    const EditorIntentPreview again = BuildIntentPreview(actionCtx, intent);
    ExecuteIntent(actionCtx, stack, again, status);
    const std::size_t before = stack.HistorySize();
    std::string repeatStatus;
    const EditorIntentPreview repeat = BuildIntentPreview(actionCtx, intent);
    ExecuteIntent(actionCtx, stack, repeat, repeatStatus);
    Check(stack.HistorySize() == before, "aligning what is already aligned adds no entry");
    Check(repeatStatus.find("already") != std::string::npos, "and says so: " + repeatStatus);
}

void TestDistributeOrdersBeforeSpacing(const EditorActionContext& actionCtx)
{
    EditorContext& ctx = actionCtx.editor;
    EditorCommandStack stack;
    std::string status;

    EditorIntent intent = MakeActionIntent("distribute", Json{ { "axis", "x" } });
    intent.target.filter.push_back("coconut_palm");
    const EditorIntentPreview preview = BuildIntentPreview(actionCtx, intent);
    Check(preview.executable, "distribute previews: " + preview.problem);
    Check(ExecuteIntent(actionCtx, stack, preview, status), status);

    // Sorted along the axis first: A(0) D(1.4) B(4.3) C(10) -> evenly between 0 and 10.
    // Distributing in SELECTION order would have shuffled them past each other, which is
    // never what "space them evenly" means.
    Check(Near(ctx.document.Find({ 1 })->transform.position.x, 0.0f), "the first end stayed");
    Check(Near(ctx.document.Find({ 3 })->transform.position.x, 10.0f), "the last end stayed");
    Check(Near(ctx.document.Find({ 4 })->transform.position.x, 10.0f / 3.0f),
        "the second-lowest landed on the first third");
    Check(Near(ctx.document.Find({ 2 })->transform.position.x, 20.0f / 3.0f),
        "and the next on the second third");

    stack.Undo(ctx);
    Check(Near(ctx.document.Find({ 2 })->transform.position.x, 4.3f), "undo restores");

    // Two objects are evenly spaced whatever the spacing, so there is nothing to do.
    EditorIntent pair = MakeActionIntent("distribute", Json{ { "axis", "x" } });
    pair.target.scope = EditorIntentScope::Selected;
    ctx.selection.Clear();
    ctx.selection.Add({ 1 }, false);
    ctx.selection.Add({ 2 }, false);
    const EditorIntentPreview pairPreview = BuildIntentPreview(actionCtx, pair);
    std::string pairStatus;
    Check(!ExecuteIntent(actionCtx, stack, pairPreview, pairStatus),
        "distributing two objects is refused");
    ctx.selection.Clear();
}

void TestSnapLeavesHeightAloneByDefault(const EditorActionContext& actionCtx)
{
    EditorContext& ctx = actionCtx.editor;
    EditorCommandStack stack;
    std::string status;

    EditorIntent intent = MakeActionIntent("snap", Json{ { "grid", 1.0 } });
    intent.target.filter.push_back("coconut_palm");
    const EditorIntentPreview preview = BuildIntentPreview(actionCtx, intent);
    Check(preview.executable, "snap previews: " + preview.problem);
    Check(ExecuteIntent(actionCtx, stack, preview, status), status);

    Check(Near(ctx.document.Find({ 2 })->transform.position.x, 4.0f), "x rounded to the grid");
    Check(Near(ctx.document.Find({ 4 })->transform.position.z, 3.0f), "z rounded to the grid");
    // THE DEFAULT LEAVES HEIGHT ALONE, and that is a decision rather than an oversight:
    // rounding y on uneven ground lifts objects off it, undoing exactly what `bury` is for.
    Check(Near(ctx.document.Find({ 4 })->transform.position.y, 9.4f),
        "height was NOT snapped by default");
    stack.Undo(ctx);

    EditorIntent all = MakeActionIntent("snap", Json{ { "grid", 2.0 }, { "axes", "all" } });
    all.target.filter.push_back("coconut_palm");
    const EditorIntentPreview allPreview = BuildIntentPreview(actionCtx, all);
    Check(ExecuteIntent(actionCtx, stack, allPreview, status), status);
    Check(Near(ctx.document.Find({ 4 })->transform.position.y, 10.0f),
        "asking for 'all' does snap height");
    stack.Undo(ctx);

    // The declared enum is the contract: an axis nobody declared must not reach the action.
    const EditorActionDesc& snap = *EditorActionRegistry::Builtin().Find("snap");
    Json bad = Json{ { "axes", "diagonal" } };
    std::string error;
    Check(!ValidateParams(snap, bad, error), "an undeclared axis name is refused");
}

// -------------------------------------------------- isolate, frame, selectSimilar

void TestIsolateShowsAsWellAsHides(const EditorActionContext& actionCtx,
    GrammarIntentSource& grammar)
{
    EditorContext& ctx = actionCtx.editor;
    EditorCommandStack stack;
    std::string whyNot;
    std::string status;

    // The case that makes isolate more than "hide everything except": one of the things to
    // isolate is ALREADY hidden, and hiding the others would leave an empty view.
    ctx.document.Find({ 1 })->enabled = false;

    const EditorIntent intent = grammar.Parse("isolate all coconut_palm", whyNot);
    Check(intent.kind == EditorIntentKind::Command && intent.action == "isolate", "isolate parses");
    const EditorIntentPreview preview = BuildIntentPreview(actionCtx, intent);
    Check(preview.executable, "isolate previews: " + preview.problem);
    Check(preview.undoable, "and it is one undo entry");
    Check(ExecuteIntent(actionCtx, stack, preview, status), status);

    Check(ctx.document.Find({ 1 })->enabled, "the hidden target was SHOWN, not left hidden");
    Check(ctx.document.Find({ 2 })->enabled, "the visible target stayed visible");
    Check(!ctx.document.Find({ 3 })->enabled, "the date palm was hidden");
    Check(!ctx.document.Find({ 4 })->enabled, "and so was the rock");
    Check(stack.HistorySize() == 1, "all of that is ONE history entry");

    stack.Undo(ctx);
    Check(!ctx.document.Find({ 1 })->enabled, "undo restored what was visible before");
    Check(ctx.document.Find({ 4 })->enabled, "including the rock");
    ctx.document.Find({ 1 })->enabled = true;
}

void TestFrameMovesTheCameraAndNothingElse(const EditorActionContext& actionCtx,
    GrammarIntentSource& grammar)
{
    EditorContext& ctx = actionCtx.editor;
    EditorCommandStack stack;
    std::string whyNot;
    std::string status;

    const std::uint64_t versionBefore = ctx.document.ContentVersion();
    ctx.selection.Clear();
    ctx.selection.Add({ 4 }, false);

    const EditorIntent intent = grammar.Parse("frame all coconut_palm", whyNot);
    const EditorIntentPreview preview = BuildIntentPreview(actionCtx, intent);
    Check(preview.executable, "frame previews: " + preview.problem);
    Check(preview.viewOnly, "and is marked as a view change");
    Check(!preview.undoable, "a camera move is not undoable and must not claim to be");
    Check(!preview.answerOnly, "but it is not a question either");

    ExecuteIntent(actionCtx, stack, preview, status);
    Check(stack.HistorySize() == 0, "framing leaves no history entry");
    Check(ctx.document.ContentVersion() == versionBefore, "and does not dirty the document");
    // "Show me the palms" is a request to LOOK, not to select.
    Check(ctx.selection.Size() == 1 && ctx.selection.Ordered()[0].value == 4,
        "and it leaves the selection alone");
    ctx.selection.Clear();
}

void TestSelectSimilarReadsTheSelection(const EditorActionContext& actionCtx,
    GrammarIntentSource& grammar)
{
    EditorContext& ctx = actionCtx.editor;
    EditorCommandStack stack;
    std::string whyNot;
    std::string status;

    // With nothing selected there is nothing to be similar TO, and saying so beats
    // selecting the whole level.
    ctx.selection.Clear();
    const EditorIntent intent = grammar.Parse("similar", whyNot);
    Check(intent.kind == EditorIntentKind::Command && intent.action == "selectSimilar",
        "selectSimilar parses from a bare verb");
    const EditorIntentPreview empty = BuildIntentPreview(actionCtx, intent);
    Check(!empty.executable && empty.problem.find("Select an object first") != std::string::npos,
        "with an empty selection it asks for one: " + empty.problem);

    // One coconut palm selected; the answer is BOTH coconut palms and neither of the others.
    ctx.selection.Clear();
    ctx.selection.Add({ 1 }, false);
    const EditorIntentPreview preview = BuildIntentPreview(actionCtx, intent);
    Check(preview.executable, "with a selection it resolves: " + preview.problem);
    Check(preview.targets.size() == 2, "both coconut palms, and not the date palm");
    Check(!preview.undoable, "selecting is not undoable");
    // The filter came from the EDITOR, not from the phrase -- the model never sees the
    // selection, so this has to be worked out on this side.
    Check(preview.resolved.target.filter.size() == 1 &&
        preview.resolved.target.filter[0].find("coconut_palm") != std::string::npos,
        "the filter was derived from what was selected");

    Check(ExecuteIntent(actionCtx, stack, preview, status), status);
    Check(ctx.selection.Size() == 2, "and the selection grew to both");
    Check(stack.HistorySize() == 0, "with no history entry");
    ctx.selection.Clear();
}

// ------------------------------------------------------------------- the note
//
// A refusal produces two things: the short answer the designer sees, and a note appended
// to a backlog for whoever extends the editor. The note's shape is a pure function, so it
// is checked here without a model in the room.

void TestRefusalWritesANoteForTheImplementer()
{
    EditorIntent refusal;
    refusal.kind = EditorIntentKind::NeedsApi;
    refusal.requested = "paint the palms red";
    refusal.proposed = "setBaseColor(objects, rgba)";
    refusal.whyExistingDontFit = "replace swaps the whole mesh; nothing changes only a colour";

    const std::string note = intentnotes::FormatNote(
        "\xd0\xbf\xd0\xbe\xd0\xba\xd1\x80\xd0\xb0\xd1\x81\xd1\x8c \xd0\xb2 \xd0\xba\xd1\x80\xd0\xb0\xd1\x81\xd0\xbd\xd1\x8b\xd0\xb9",
        refusal, "data/levels/atoll.json", "A colour override on the material instance would do it.",
        "2026-09-15 02:40");

    // Everything the implementer needs to judge it: what was asked, what was proposed, and
    // which existing actions were weighed. A note missing the rejection reasoning is a note
    // that cannot be told apart from a wrong refusal.
    Check(note.find("setBaseColor(objects, rgba)") != std::string::npos, "the note names the API");
    Check(note.find("paint the palms red") != std::string::npos, "and what was read");
    Check(note.find("replace swaps the whole mesh") != std::string::npos,
        "and why the existing actions were rejected");
    Check(note.find("data/levels/atoll.json") != std::string::npos, "and which level");
    Check(note.find("2026-09-15 02:40") != std::string::npos, "and when");
    Check(note.find("colour override on the material instance") != std::string::npos,
        "and the model's own prose");
    Check(note.rfind("\n---\n", 0) == 0, "entries are separated so they can be appended");

    // The REQUEST is the part worth keeping. If the prose never arrives the entry is still
    // written, because three entries asking for the same thing is the signal, not the prose.
    const std::string bare = intentnotes::FormatNote("test", refusal, {}, {}, "2026-01-01 00:00");
    Check(bare.find("setBaseColor") != std::string::npos,
        "a note is written even when the model produced no prose");

    // The prompt for the second request must invite a wrong-refusal admission, which is the
    // most useful thing it can say and the one a proposal-shaped prompt would suppress.
    const std::string prompt = intentnotes::BuildNotePrompt("test", refusal, "lvl", "- bury: ...");
    Check(prompt.find("could actually have covered this") != std::string::npos,
        "the note prompt asks the model to admit a wrong refusal");
    Check(prompt.find("Do not write JSON") != std::string::npos,
        "and to write prose, not another form");
}

// ----------------------------------------------------------------- questions
//
// A question is a preview with nothing to run. What is guarded here is that it stays that
// way: it must answer, it must touch nothing, and it must not grow a Run button.

void TestQuestionsAnswerWithoutChangingAnything(const EditorActionContext& actionCtx,
    GrammarIntentSource& grammar)
{
    EditorContext& ctx = actionCtx.editor;
    EditorCommandStack stack;
    std::string whyNot;

    const std::uint64_t versionBefore = ctx.document.ContentVersion();
    const std::size_t objectsBefore = ctx.document.Objects().size();

    const EditorIntent intent = grammar.Parse("count all palm", whyNot);
    Check(intent.kind == EditorIntentKind::Command && intent.action == "count",
        "'count' is a command with a read-only action, not a new intent kind");

    const EditorIntentPreview preview = BuildIntentPreview(actionCtx, intent);
    Check(preview.executable, "the question was answered: " + preview.problem);
    Check(preview.answerOnly, "and it is marked as an answer, so no Run button appears");
    Check(!preview.undoable, "a question is never advertised as undoable");
    Check(preview.targets.size() == 3, "three palms counted");
    Check(preview.groups.size() == 2, "and the answer breaks them down by asset");
    Check(preview.summary.find("3") != std::string::npos &&
        preview.summary.find("count") == std::string::npos,
        "the answer reads as a sentence, not as a command name: " + preview.summary);

    // "There are none" is an ANSWER, not an error. Refusing it would turn "are there any
    // rocks?" into a failure message.
    const EditorIntent none = grammar.Parse("count all wombat", whyNot);
    const EditorIntentPreview nonePreview = BuildIntentPreview(actionCtx, none);
    Check(nonePreview.executable && nonePreview.targets.empty(),
        "a question with no matches still answers");

    // Asking changes nothing at all -- not the document, not the selection, not history.
    Check(ctx.document.ContentVersion() == versionBefore, "asking did not dirty the document");
    Check(ctx.document.Objects().size() == objectsBefore, "and changed no objects");
    Check(stack.HistorySize() == 0, "and left no history entry");

    // And if something DOES try to run it, it says so instead of quietly doing nothing.
    std::string status;
    Check(!ExecuteIntent(actionCtx, stack, preview, status),
        "running a question is refused");
    Check(status.find("question") != std::string::npos,
        "and says why: " + status);
    Check(stack.HistorySize() == 0, "still no history entry");
}

// --------------------------------------------------------------- environment
//
// The look of the world: fog, sun, wind, water, exposure. What is guarded here is mostly
// that these edits go through the DOCUMENT and the undo stack rather than into runtime
// state -- the engine's `--set` namespace could reach the same knobs in one line, and an
// edit made that way would be untrackable, unsaveable and unrepeatable.

EditorSceneDocument MakeEnvironmentDocument()
{
    EditorSceneDocument document = MakePalmDocument();

    EditorObject wind;
    wind.id.value = 900;
    wind.type = "wind";
    wind.name = "Wind";
    wind.properties = Json{ { "strength", 2.0 }, { "directionDeg", 90.0 }, { "gust", 0.25 } };
    document.Environment().push_back(wind);

    // GTAO carries the nested section for these tests because its runtime apply only
    // touches Scene. Wind and ocean reach the ocean SIMULATION through Systems, which a
    // CPU-only harness does not have -- executing one of those asserts. What is under test
    // here is the document-and-undo path, not any particular knob, so the knob that can be
    // exercised headlessly is the right one to use.
    EditorObject gtao;
    gtao.id.value = 901;
    gtao.type = "gtao";
    gtao.name = "GTAO";
    gtao.properties = Json{ { "enabled", true }, { "intensity", 0.5 },
                            { "filter", Json{ { "radius", 1.5 } } } };
    document.Environment().push_back(gtao);

    EditorObject sun;
    sun.id.value = 902;
    sun.type = "directionalLight";
    sun.name = "Sun";
    sun.properties = Json{ { "direction", Json::array({ 0.3, -0.9, 0.2 }) },
                           { "sunIlluminanceLux", 100000.0 } };
    document.Environment().push_back(sun);

    // A nested section, like the ocean's render block, to prove the walk goes in.
    EditorObject ocean;
    ocean.id.value = 903;
    ocean.type = "ocean";
    ocean.name = "Ocean";
    ocean.properties = Json{ { "windForce", 4.0 },
                             { "render", Json{ { "foamScale", 1.5 } } },
                             { "absorptionColors", Json::array({ Json::array({ 0.0, 1.0 }) }) } };
    document.Environment().push_back(ocean);
    return document;
}

void TestEnvironmentSettingsAreEnumeratedFromTheLevel()
{
    const EditorSceneDocument document = MakeEnvironmentDocument();
    const std::vector<envsettings::Setting> settings = envsettings::Enumerate(document);

    const envsettings::Setting* strength = envsettings::Find(settings, "wind.strength");
    Check(strength != nullptr, "wind.strength was found in the level");
    Check(strength->kind == envsettings::ValueKind::Number, "and it is a number");
    Check(Near(strength->current.get<float>(), 2.0f), "with its CURRENT value");

    Check(envsettings::Find(settings, "gtao.enabled") != nullptr, "booleans are enumerated");
    const envsettings::Setting* direction = envsettings::Find(settings, "directionalLight.direction");
    Check(direction && direction->kind == envsettings::ValueKind::Vec3,
        "a three-number array is a vector, not three settings");

    // Nesting is walked, but the arrays that are not vectors are left alone: there is no
    // single-value edit for a colour ramp and offering one would only invite a bad edit.
    Check(envsettings::Find(settings, "ocean.render.foamScale") != nullptr,
        "nested sections are reached");
    Check(envsettings::Find(settings, "ocean.absorptionColors") == nullptr,
        "an array that is not a vec3 is not offered as a setting");

    // A bare leaf is fine when it names one knob, and must NOT resolve when it names two.
    Check(envsettings::Find(settings, "intensity") != nullptr, "a unique leaf name resolves");
    Check(envsettings::Find(settings, "nope.nothing") == nullptr, "an unknown path does not");

    Check(envsettings::Find(settings, "WIND.STRENGTH") != nullptr, "lookup is case-insensitive");
}

void TestEnvironmentPreviewShowsBeforeAndAfter(const EditorActionContext& actionCtx)
{
    // scale: the whole point of showing the current value is that "thicker" is relative.
    EditorIntent scale = MakeActionIntent("setEnvironment", Json{ { "scale", 1.5 } });
    scale.target.kind = EditorTargetKind::Environment;
    scale.target.setting = "wind.strength";
    const EditorIntentPreview scalePreview = BuildIntentPreview(actionCtx, scale);
    Check(scalePreview.executable, "a scale edit previews: " + scalePreview.problem);
    Check(scalePreview.summary.find("2 ") != std::string::npos &&
        scalePreview.summary.find("3") != std::string::npos,
        "the preview shows before AND after: " + scalePreview.summary);

    EditorIntent absolute = MakeActionIntent("setEnvironment", Json{ { "value", 0.8 } });
    absolute.target.kind = EditorTargetKind::Environment;
    absolute.target.setting = "gtao.intensity";
    Check(BuildIntentPreview(actionCtx, absolute).executable, "an absolute edit previews");

    // The two ways of saying it are different requests, and guessing between them silently
    // is how a look gets wrecked.
    EditorIntent both = MakeActionIntent("setEnvironment", Json{ { "value", 1.0 }, { "scale", 2.0 } });
    both.target.kind = EditorTargetKind::Environment;
    both.target.setting = "wind.strength";
    Check(!BuildIntentPreview(actionCtx, both).executable, "value AND scale together is refused");

    EditorIntent neither = MakeActionIntent("setEnvironment");
    neither.target.kind = EditorTargetKind::Environment;
    neither.target.setting = "wind.strength";
    Check(!BuildIntentPreview(actionCtx, neither).executable, "neither one is refused");

    // A setting's own type is the contract: a boolean handed 1 would become a number in
    // the level file and quietly stop being read.
    EditorIntent wrongType = MakeActionIntent("setEnvironment", Json{ { "value", 1.0 } });
    wrongType.target.kind = EditorTargetKind::Environment;
    wrongType.target.setting = "gtao.enabled";
    const EditorIntentPreview wrongPreview = BuildIntentPreview(actionCtx, wrongType);
    Check(!wrongPreview.executable && wrongPreview.problem.find("true/false") != std::string::npos,
        "a number where a boolean belongs is refused by type: " + wrongPreview.problem);

    EditorIntent scaledVector = MakeActionIntent("setEnvironment", Json{ { "scale", 2.0 } });
    scaledVector.target.kind = EditorTargetKind::Environment;
    scaledVector.target.setting = "directionalLight.direction";
    Check(!BuildIntentPreview(actionCtx, scaledVector).executable,
        "'scale' on a vector is refused rather than guessed at");

    EditorIntent unknown = MakeActionIntent("setEnvironment", Json{ { "value", 1.0 } });
    unknown.target.kind = EditorTargetKind::Environment;
    unknown.target.setting = "fog.thickness";
    const EditorIntentPreview unknownPreview = BuildIntentPreview(actionCtx, unknown);
    Check(!unknownPreview.executable &&
        unknownPreview.problem.find("fog.thickness") != std::string::npos,
        "an unknown setting is refused BY NAME: " + unknownPreview.problem);
}

void TestEnvironmentEditsGoThroughUndo(const EditorActionContext& actionCtx)
{
    EditorContext& ctx = actionCtx.editor;
    EditorCommandStack stack;
    std::string status;

    const auto gtaoValue = [&ctx](const char* key) -> float
    {
        for (const EditorObject& entity : ctx.document.Environment())
        {
            if (entity.type == "gtao")
            {
                return entity.properties[key].get<float>();
            }
        }
        return -1.0f;
    };

    Check(Near(gtaoValue("intensity"), 0.5f), "gtao.intensity starts at 0.5");

    EditorIntent intent = MakeActionIntent("setEnvironment", Json{ { "scale", 1.5 } });
    intent.target.kind = EditorTargetKind::Environment;
    intent.target.setting = "gtao.intensity";
    const EditorIntentPreview preview = BuildIntentPreview(actionCtx, intent);
    Check(ExecuteIntent(actionCtx, stack, preview, status), status);
    Check(Near(gtaoValue("intensity"), 0.75f),
        "the edit landed in the DOCUMENT, not just in runtime state");
    Check(stack.HistorySize() == 1, "and it is one history entry");

    stack.Undo(ctx);
    Check(Near(gtaoValue("intensity"), 0.5f), "one Ctrl+Z puts the look back");
    stack.Redo(ctx);
    Check(Near(gtaoValue("intensity"), 0.75f), "and redo puts it forward again");

    // A nested path writes where it should and leaves its siblings alone.
    EditorIntent nested = MakeActionIntent("setEnvironment", Json{ { "value", 2.5 } });
    nested.target.kind = EditorTargetKind::Environment;
    nested.target.setting = "gtao.filter.radius";
    const EditorIntentPreview nestedPreview = BuildIntentPreview(actionCtx, nested);
    Check(ExecuteIntent(actionCtx, stack, nestedPreview, status), status);
    for (const EditorObject& entity : ctx.document.Environment())
    {
        if (entity.type == "gtao")
        {
            Check(Near(entity.properties["filter"]["radius"].get<float>(), 2.5f),
                "the nested value was written");
            Check(Near(entity.properties["intensity"].get<float>(), 0.75f),
                "and its siblings were left alone");
        }
    }
}

// `intent_regression --dump <level.json> <gbnf-out> <prompt-out>` writes the grammar and
// the system prompt this editor would send for a REAL level. It exists so the contract
// can be exercised against a live llama-server with the exact bytes the editor uses --
// a hand-retyped approximation would test a different contract than the one that ships.
int DumpForLevel(const char* levelPath, const char* gbnfOut, const char* promptOut)
{
    EditorSceneDocument document;
    if (!document.LoadFromLevelFile(levelPath))
    {
        std::fprintf(stderr, "FAIL: cannot load %s\n", levelPath);
        return 1;
    }
    AssetRegistry assets;
    assets.Refresh();

    const intentschema::Vocabulary vocabulary = intentschema::BuildVocabulary(document, assets);
    const std::string gbnf = intentschema::BuildGbnf(vocabulary);
    const std::string prompt = intentprompt::BuildSystemPrompt(document, assets, vocabulary);

    const auto write = [](const char* path, const std::string& text) -> bool
    {
        std::FILE* file = nullptr;
        if (fopen_s(&file, path, "wb") != 0 || !file)
        {
            return false;
        }
        std::fwrite(text.data(), 1, text.size(), file);
        std::fclose(file);
        return true;
    };
    if (!write(gbnfOut, gbnf) || !write(promptOut, prompt))
    {
        std::fprintf(stderr, "FAIL: cannot write dump files\n");
        return 1;
    }

    std::printf("level %s: %zu objects, %zu filter names, %zu spawnable assets\n",
        levelPath, document.Objects().size(), vocabulary.needles.size(), vocabulary.assets.size());
    std::printf("grammar %zu bytes -> %s\n", gbnf.size(), gbnfOut);
    std::printf("prompt  %zu bytes -> %s\n", prompt.size(), promptOut);
    return 0;
}

// `intent_regression --stream <endpoint> <phrase>` sends ONE chat request through the
// editor's own LlmIntentSource and reports how the answer arrived: when the first piece
// landed, and how many pieces there were.
//
// It exists because the chat had no headless path at all, and the thing most worth proving
// about it cannot be proved by reading the code -- whether the answer is delivered as it is
// generated or in one lump at the end. With "stream": false the first byte arrives with the
// last, and every second until then looks like a hang. One arrival for the whole answer is
// a FAILURE here, not a pass.
//
// Needs a llama-server already running; it is not part of the gate for that reason.
int StreamProbe(const char* endpoint, const char* phrase)
{
    LlmIntentSettings settings;
    settings.endpoint = endpoint;
    settings.autoStart = false;          // talk to whatever is already there
    settings.modelPath = "probe";        // Available() only checks these are set
    settings.serverExe = "probe";
    LlmIntentSource model(std::move(settings));

    const std::string prompt =
        std::string("<|im_start|>system\nYou are a helpful assistant embedded in a "
            "DirectX 12 game engine's level editor. Answer concisely, in the language the "
            "user writes in.<|im_end|>\n<|im_start|>user\n") + phrase +
        "<|im_end|>\n<|im_start|>assistant\n";

    std::printf("streaming probe: \"%s\" -> %s\n", phrase, endpoint);
    const auto start = std::chrono::steady_clock::now();
    model.BeginFreeform(prompt, 0.7f, 4096);

    const auto elapsed = [&start]()
    {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
    };

    double firstPieceSec = -1.0;
    std::size_t arrivals = 0;
    std::size_t lastSize = 0;
    std::string text;
    std::string error;
    bool truncated = false;
    for (;;)
    {
        const IntentParseState state = model.PollFreeform(text, error, truncated);
        const std::size_t size = model.FreeformPartial().size();
        if (size != lastSize)
        {
            if (firstPieceSec < 0.0)
            {
                firstPieceSec = elapsed();
            }
            lastSize = size;
            ++arrivals;
        }
        if (state == IntentParseState::Ready || state == IntentParseState::Failed)
        {
            const double total = elapsed();
            if (state == IntentParseState::Failed)
            {
                std::printf("FAILED after %.1fs: %s\n", total, error.c_str());
                return 1;
            }
            std::printf("  first piece at : %.1fs\n", firstPieceSec);
            std::printf("  whole answer at: %.1fs\n", total);
            std::printf("  arrivals       : %zu\n", arrivals);
            std::printf("  answer chars   : %zu\n", text.size());
            if (arrivals <= 1)
            {
                std::puts("FAIL: the answer arrived in ONE piece -- that is not streaming");
                return 1;
            }
            std::printf("PASS: answer arrived in %zu pieces, the first %.0f%% of the way in\n",
                arrivals, firstPieceSec / (total > 0.0 ? total : 1.0) * 100.0);
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));   // a frame
    }
}

// `intent_regression --talk <prompt-file> <answer-file> [max-tokens]` holds ONE turn of a
// conversation with the model editor_state.json is configured for, and times it.
//
// THE PROMPT FILE IS THE WHOLE PROMPT, ChatML markers and all, in UTF-8. The caller owns the
// conversation and appends each answer to it by hand, which is deliberate twice over: a
// Windows command line cannot carry Cyrillic through reliably, and a multi-turn discussion
// needs its history byte-identical under the caller's control -- that is what the server's
// prefix cache keys on, and it is the difference between a 20 s turn and a 3 s one.
//
// Unlike --stream, this prints what the model actually SAID. --stream answers "do tokens
// arrive in pieces"; this one answers "what does this model know", so the text is the point.
int TalkProbe(const char* promptFile, const char* answerFile, int maxTokens)
{
    std::ifstream in(promptFile, std::ios::binary);
    if (!in)
    {
        std::printf("cannot open prompt file: %s\n", promptFile);
        return 2;
    }
    const std::string prompt((std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
    in.close();

    // The editor's own settings, so this talks to the same weights on the same endpoint with
    // the same gpuLayers. Reading them beats retyping them: a probe that quietly differs from
    // the editor measures a configuration nobody runs.
    LlmIntentSettings settings;
    {
        std::ifstream state("editor_state.json", std::ios::binary);
        if (state)
        {
            Json json = Json::parse(state, nullptr, false);
            if (!json.is_discarded())
            {
                const auto it = json.find("levelEditor");
                if (it != json.end() && it->is_object())
                {
                    settings = LlmIntentSource::LoadSettings(*it);
                }
            }
        }
    }
    settings.autoStart = true;
    // Said now rather than discovered by waiting. editor_state.json is read by a RELATIVE
    // path, so a probe started from another directory gets defaults -- no model path, and
    // then three minutes of polling for a server nobody asked to start.
    if (settings.modelPath.empty() || settings.serverExe.empty())
    {
        std::puts("no model configured -- run this from the repository root, where "
            "editor_state.json is");
        return 2;
    }

    LlmIntentSource model(std::move(settings));
    const auto start = std::chrono::steady_clock::now();
    const auto elapsed = [&start]()
    {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
    };

    // EnsureServerReady polls rather than waits -- the editor has frames to draw while a
    // 38 GB model loads, and this tool inherits that. So the waiting happens here.
    std::string status;
    while (!model.EnsureServerReady(status))
    {
        if (elapsed() > 180.0)
        {
            std::printf("server never came up after %.0fs: %s\n", elapsed(), status.c_str());
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    const double serverSec = elapsed();
    std::printf("server ready at : %.1fs\n", serverSec);
    std::printf("prompt chars    : %zu (~%zu tokens at 3 chars/token)\n",
        prompt.size(), prompt.size() / 3);

    model.BeginFreeform(prompt, 0.7f, maxTokens);

    double firstPieceSec = -1.0;
    std::size_t lastSize = 0;
    std::string text;
    std::string error;
    bool truncated = false;
    for (;;)
    {
        const IntentParseState state = model.PollFreeform(text, error, truncated);
        const std::size_t size = model.FreeformPartial().size();
        if (size != lastSize)
        {
            if (firstPieceSec < 0.0)
            {
                firstPieceSec = elapsed();
            }
            lastSize = size;
        }
        if (state == IntentParseState::Ready || state == IntentParseState::Failed)
        {
            const double total = elapsed();
            if (state == IntentParseState::Failed)
            {
                std::printf("FAILED after %.1fs: %s\n", total, error.c_str());
                return 1;
            }
            std::ofstream out(answerFile, std::ios::binary);
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.close();

            // The two halves are worth separating. Time to the first token is PREFILL -- it
            // scales with the prompt and is nearly free on a cache hit; everything after is
            // generation, which scales with the answer and never gets cheaper.
            std::printf("first token at  : %.1fs (prefill %.1fs)\n",
                firstPieceSec, firstPieceSec - serverSec);
            std::printf("answer done at  : %.1fs\n", total);
            std::printf("answer chars    : %zu%s\n", text.size(),
                truncated ? "   TRUNCATED -- raise max-tokens" : "");
            const double genSec = total - firstPieceSec;
            if (genSec > 0.0)
            {
                // Characters, because the server reports no token count on this path. Three
                // chars per token is what the editor itself budgets mixed Russian and
                // English with, so the derived figure is comparable with its numbers.
                std::printf("generation      : %.1fs, %.0f chars/s (~%.1f tok/s)\n",
                    genSec, text.size() / genSec, text.size() / genSec / 3.0);
            }
            std::printf("written to      : %s\n", answerFile);
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 5 && std::string(argv[1]) == "--dump")
    {
        return DumpForLevel(argv[2], argv[3], argv[4]);
    }
    if (argc == 4 && std::string(argv[1]) == "--stream")
    {
        return StreamProbe(argv[2], argv[3]);
    }
    if ((argc == 4 || argc == 5) && std::string(argv[1]) == "--talk")
    {
        return TalkProbe(argv[2], argv[3], argc == 5 ? std::atoi(argv[4]) : 8192);
    }
    std::puts("Intent regression: editor command layer");
    try
    {
        TestRegistryIntegrity();
        TestQueryListIntegrity();
        TestParamsCannotBeInvented();
        std::puts("Intent regression: registry and parameter typing OK");

        GrammarIntentSource grammar;
        EditorSceneDocument document = MakePalmDocument();
        TestGrammarDeclinesRatherThanGuesses(grammar, document);
        TestGrammarEmitsOnlyRealActions(grammar, document);
        TestGrammarSentenceShapes(grammar, document);
        std::puts("Intent regression: grammar OK");

        auto renderer = std::make_unique<Renderer>();
        auto scene = std::make_unique<Scene>();
        LevelManager levels;
        EditorSelection selection;
        EditorContext ctx{ *renderer, *scene, levels, document, selection };

        AssetRegistry assets;
        assets.Refresh();
        EditorExtensionRegistry extensions;
        EditorExtensionRegistry::RegisterBuiltins(extensions);
        const EditorActionContext actionCtx{ ctx, assets, extensions };

        TestSelectorReachesTheSameObjectsAsSearch(actionCtx, grammar);
        TestSelectedZoneMeansItsArea(actionCtx);
        TestWholeLevelIsRefusedEvenIfAnIntentAsksForIt(actionCtx);
        TestAssetResolution(actionCtx, grammar);
        TestPreviewDescribesWhatRuns(actionCtx);
        TestGroupsAreFilterable(actionCtx);
        TestNarrowingsCannotEvaporate(actionCtx);
        TestVocabularyOffersNothingUnreachable(document, assets);
        TestRefusalsNameTheRealReason(actionCtx);
        std::puts("Intent regression: selector, assets and refusals OK");

        TestOnePhraseIsOneUndo(actionCtx, grammar);
        TestTransforms(actionCtx, grammar);
        TestRandomizeIsVariedAndDeterministic(actionCtx);
        TestReplace(actionCtx, grammar);
        TestSpawnPlacement(actionCtx);
        std::puts("Intent regression: actions OK");

        // The environment family runs on its own document: the palm document has no
        // environment entities, and a test that silently finds nothing proves nothing.
        // Placement runs on its own scattered document: the palm document has everything
        // on one line, where "distribute along x" has nothing to prove.
        {
            EditorSceneDocument placementDocument = MakeScatteredDocument();
            EditorSelection placementSelection;
            EditorContext placementCtx{ *renderer, *scene, levels, placementDocument,
                placementSelection };
            const EditorActionContext placementActionCtx{ placementCtx, assets, extensions };
            TestAlignSharesOneAxisOnly(placementActionCtx);
        }
        {
            EditorSceneDocument placementDocument = MakeScatteredDocument();
            EditorSelection placementSelection;
            EditorContext placementCtx{ *renderer, *scene, levels, placementDocument,
                placementSelection };
            const EditorActionContext placementActionCtx{ placementCtx, assets, extensions };
            TestDistributeOrdersBeforeSpacing(placementActionCtx);
        }
        {
            EditorSceneDocument placementDocument = MakeScatteredDocument();
            EditorSelection placementSelection;
            EditorContext placementCtx{ *renderer, *scene, levels, placementDocument,
                placementSelection };
            const EditorActionContext placementActionCtx{ placementCtx, assets, extensions };
            TestSnapLeavesHeightAloneByDefault(placementActionCtx);
        }
        std::puts("Intent regression: align/distribute/snap OK");

        TestIsolateShowsAsWellAsHides(actionCtx, grammar);
        TestFrameMovesTheCameraAndNothingElse(actionCtx, grammar);
        TestSelectSimilarReadsTheSelection(actionCtx, grammar);
        std::puts("Intent regression: isolate/frame/similar OK");

        TestQuestionsAnswerWithoutChangingAnything(actionCtx, grammar);
        std::puts("Intent regression: questions OK");

        TestEnvironmentSettingsAreEnumeratedFromTheLevel();
        {
            EditorSceneDocument envDocument = MakeEnvironmentDocument();
            EditorSelection envSelection;
            EditorContext envCtx{ *renderer, *scene, levels, envDocument, envSelection };
            const EditorActionContext envActionCtx{ envCtx, assets, extensions };
            TestEnvironmentPreviewShowsBeforeAndAfter(envActionCtx);
            TestEnvironmentEditsGoThroughUndo(envActionCtx);
        }
        std::puts("Intent regression: environment OK");

        TestGeneratedGrammar(document, assets);
        TestModelAnswersAreRead();
        TestModelSettingsRoundTrip();
        TestRefusalWritesANoteForTheImplementer();
        std::puts("PASS: intent layer parses, previews, places, undoes, and the model contract holds");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
