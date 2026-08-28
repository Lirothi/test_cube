// CPU-only regression: build the engine Debug|x64, then this tool's vcxproj.
// No window/device, level writes, asset writes, or editor preference changes.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>

#include "app/levels/LevelManager.h"
#include "app/scene/Scene.h"
#include "editor/EditorContext.h"
#include "editor/EditorExtensionRegistry.h"
#include "editor/ui/InspectorMultiEdit.h"
#include "editor/ui/InspectorPanel.h"
#include "editor/ui/InspectorPropertyDelta.h"
#include "editor/commands/TransformObjectCommand.h"
#include "editor/scene/EnvironmentRuntime.h"
#include "rendering/core/Renderer.h"
#include "rendering/meshes/StaticMesh.h"
#include "vfx/ParticleEmitterObject.h"
#include "imgui.h"
#include "imgui_internal.h"
#if INSPECTOR_BASELINE
#include "InspectorPanelBaseline.h"
#endif

using Json = nlohmann::json;
void Check(bool condition, const char* label)
{
    if (!condition) { throw std::runtime_error(label); }
}
bool Near(float a, float b) { return std::abs(a - b) < 0.00001f; }

EditorObject MakeObject(uint64_t id, const char* type, Json properties)
{
    EditorObject object;
    object.id.value = id;
    object.name = "Regression";
    object.type = type;
    object.properties = std::move(properties);
    return object;
}

void TestComponents()
{
    using InspectorPropertyDelta::MergeComponents;
    Check(MergeComponents(Json{ "a", "b" }, Json{ "x", "b" },
        Json{ "c", "d", "e" }) == Json{ "x", "d", "e" }, "extra material slots preserved");
    Check(MergeComponents(Json{ "a", "b" }, Json{ "x", "b" },
        Json{ "c" }) == Json{ "x" }, "smaller slot arrays preserved");
    Json target = { { "flicker", { { "amplitude", 0.7 }, { "seed", 19 } } }, { "unknown", 123 } };
    Json effective = target;
    InspectorPropertyDelta::Apply(Json::object(),
        Json{ { "flicker", { { "frequencyHz", 2.0 } } } },
        Json::object(), Json{ { "flicker", { { "frequencyHz", 2.0 } } } }, target, effective);
    Check(target["flicker"]["seed"] == 19 && target["flicker"]["frequencyHz"] == 2.0 &&
        target["unknown"] == 123, "nested fields preserved");
}

void TestMultiEdit(EditorContext& ctx)
{
    auto& doc = ctx.document;
    EditorCommandStack history;
    InspectorMultiEdit edit;
    doc.Add(MakeObject(1, "staticMesh", { { "unknown", 123 } }));
    doc.Add(MakeObject(2, "staticMesh", { { "metalRough", { 0.8f, 0.7f } }, { "unknown", 456 } }));
    doc.Add(MakeObject(3, "staticMesh", { { "metalRough", { 0.4f, 0.1f } } }));
    for (uint64_t id = 1; id <= 3; ++id)
    {
        auto mesh = std::make_unique<StaticMesh>("", "", "", L"");
        mesh->MaterialParamsRef().metalRough = id == 1 ? Math::float2(0.2f, 0.4f) :
            (id == 2 ? Math::float2(0.8f, 0.7f) : Math::float2(0.4f, 0.1f));
        ctx.scene.AddObjectWithEditorId(std::move(mesh), id);
    }
    ctx.selection.SetOrdered({ EditorObjectId{1}, EditorObjectId{2} }, {1});
    Check(edit.Begin(ctx, history), "same-type selection accepted");
    for (float value : { 0.9f, 0.55f })
    {
        Check(edit.Begin(ctx, history), "gesture continuation");
        doc.Find({1})->properties["metalRough"] = { 0.2f, value };
        ctx.scene.FindEditorObject(1)->AsGBufferRenderable()->MaterialParamsRef().metalRough.y = value;
        doc.SetDirty(true);
        edit.End(ctx, history, true);
        Check(history.HistorySize() == 0, "no undo spam during drag");
        const auto& other = *doc.Find({2});
        Check(Near(other.properties["metalRough"][0].get<float>(), 0.8f), "metallic preserved");
        Check(Near(other.properties["metalRough"][1].get<float>(), value), "roughness broadcast");
        Check(Near(ctx.scene.FindEditorObject(2)->AsGBufferRenderable()->MaterialParamsRef().metalRough.y, value),
            "live material updated");
        Check(Near(doc.Find({3})->properties["metalRough"][1].get<float>(), 0.1f), "unselected object unchanged");
    }
    edit.Begin(ctx, history);
    edit.End(ctx, history, false);
    Check(history.HistorySize() == 1, "one undo entry for entire gesture");
    history.Undo(ctx);
    Check(!doc.Find({1})->properties.contains("metalRough"), "undo restores inherited field absence");
    Check(Near(ctx.scene.FindEditorObject(1)->AsGBufferRenderable()->MaterialParamsRef().metalRough.y, 0.4f),
        "undo restores inherited runtime material");
    Check(Near(doc.Find({2})->properties["metalRough"][1].get<float>(), 0.7f), "undo restores second object");
    history.Redo(ctx);
    Check(Near(doc.Find({2})->properties["metalRough"][1].get<float>(), 0.55f), "redo all");
    Check(doc.Find({1})->properties["unknown"] == 123 && doc.Find({2})->properties["unknown"] == 456,
        "unknown properties preserved");
    std::puts("PASS: live materials and grouped undo/redo");

    doc.Find({2})->transform.position = Math::float3(10, 20, 30);
    doc.SetDirty(true);
    edit.Begin(ctx, history);
    auto transform = doc.Find({1})->transform;
    transform.position.x = 9;
    TransformObjectCommand::ApplyTransform(ctx, {1}, transform);
    edit.End(ctx, history, false);
    Check(doc.Find({2})->transform.position.x == 9 &&
        doc.Find({2})->transform.position.y == 20 && doc.Find({2})->transform.position.z == 30,
        "transform edits preserve other axes");
    history.Undo(ctx);
    Check(doc.Find({2})->transform.position.x == 10, "undo transform");

    edit.Begin(ctx, history);
    const auto version = doc.ContentVersion();
    const auto entries = history.HistorySize();
    for (int i = 0; i < 2000; ++i) { edit.Begin(ctx, history); edit.End(ctx, history, false); }
    Check(doc.ContentVersion() == version && history.HistorySize() == entries, "idle selection does not mutate");
    std::puts("PASS: transforms and idle frames");

    doc.Add(MakeObject(4, "particleEmitter", { { "preset", "unloaded-test-preset" } }));
    doc.Add(MakeObject(5, "particleEmitter", Json::object()));
    vfx::EmitterDesc first, second;
    first.lifetimeMin = 1; first.lifetimeMax = 2;
    second.lifetimeMin = 3; second.lifetimeMax = 4;
    ctx.scene.AddObjectWithEditorId(std::make_unique<ParticleEmitterObject>(first), 4);
    ctx.scene.AddObjectWithEditorId(std::make_unique<ParticleEmitterObject>(second), 5);
    ctx.selection.SetOrdered({ EditorObjectId{4}, EditorObjectId{5} }, {4});
    Check(edit.Begin(ctx, history), "mixed preset/inline emitter selection");
    doc.Find({4})->properties["overrides"]["lifetime"] = { 1.0f, 9.0f };
    ctx.scene.FindEditorObject(4)->AsParticleEmitter()->DescRef().lifetimeMax = 9;
    doc.SetDirty(true);
    edit.End(ctx, history, false);
    Check(!doc.Find({5})->properties.contains("overrides") &&
        doc.Find({5})->properties["lifetime"] == Json{3.0f, 9.0f}, "emitter override paths and components");
    Check(ctx.scene.FindEditorObject(5)->AsParticleEmitter()->Desc().lifetimeMax == 9, "emitter runtime");
    history.Undo(ctx);
    Check(!doc.Find({4})->properties.contains("overrides") &&
        ctx.scene.FindEditorObject(5)->AsParticleEmitter()->Desc().lifetimeMax == 4, "emitter undo");
    std::puts("PASS: preset/inline emitters");

    doc.Environment().push_back(MakeObject(6, "pointLight", { { "radius", 5.0f } }));
    doc.Environment().push_back(MakeObject(7, "pointLight", { { "radius", 9.0f } }));
    ctx.scene.GetLightManager().PointLights().resize(2);
    EnvironmentRuntime::Apply(ctx, doc.Environment()[0]);
    EnvironmentRuntime::Apply(ctx, doc.Environment()[1]);
    ctx.selection.SetOrdered({ EditorObjectId{6}, EditorObjectId{7} }, {6});
    Check(edit.Begin(ctx, history), "same-type environment selection");
    doc.Environment()[0].properties["color"] = { 0.25f, 1.0f, 1.0f };
    EnvironmentRuntime::Apply(ctx, doc.Environment()[0]);
    doc.SetDirty(true);
    edit.End(ctx, history, false);
    Check(doc.Environment()[1].properties["color"] == Json{0.25f, 1.0f, 1.0f} &&
        doc.Environment()[1].properties["radius"] == 9.0f, "light color shared, radius preserved");
    history.Undo(ctx);
    Check(!doc.Environment()[0].properties.contains("color") &&
        !doc.Environment()[1].properties.contains("color"), "environment undo");
    ctx.selection.SetOrdered({ EditorObjectId{1}, EditorObjectId{6} }, {1});
    Check(!edit.Begin(ctx, history), "heterogeneous selection rejected");
    std::puts("PASS: component deltas, runtime materials/emitters, transforms, lights, undo/redo, idle");
}

template<class Panel>
double BenchmarkPostProcess(EditorContext& ctx)
{
    Panel panel;
    EditorCommandStack history;
    AssetRegistry registry;
    EditorExtensionRegistry extensions;
    bool open = true;
    using Clock = std::chrono::steady_clock;
    double elapsed = 0;
    for (int frame = 0; frame < 320; ++frame)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(1300, 6000), ImGuiCond_Always);
        ImGui::Begin("Inspector");
        for (const char* section : { "Camera Exposure", "Color Pipeline", "Ambient Occlusion (GTAO)",
            "Aerial Perspective", "Bloom" })
        {
            ImGui::GetStateStorage()->SetInt(ImGui::GetID(section), 1);
        }
        ImGui::End();
        const auto start = Clock::now();
        panel.Draw(ctx, history, registry, extensions, &open);
        if (frame >= 20) { elapsed += std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
        ImGui::Render();
    }
    Check(history.HistorySize() == 0, "idle Post Process has no undo entries");
    return elapsed / 300.0;
}

class DragProbe final : public IEditorPropertyDrawer
{
public:
    mutable ImVec2 min{}, max{};
    std::string_view Type() const override { return "staticMesh"; }
    void Draw(EditorContext& ctx, EditorCommandStack&, const AssetRegistry&, EditorObject& object) const override
    {
        auto& mp = ctx.scene.FindEditorObject(object.id.value)->AsGBufferRenderable()->MaterialParamsRef();
        float roughness = mp.metalRough.y;
        if (ImGui::DragFloat("Probe Roughness", &roughness, 0.01f, 0.0f, 1.0f))
        {
            mp.metalRough.y = roughness;
            object.properties["metalRough"] = { mp.metalRough.x, roughness };
            ctx.document.SetDirty(true);
        }
        min = ImGui::GetItemRectMin();
        max = ImGui::GetItemRectMax();
    }
};

void TestMouseGesture(EditorContext& ctx)
{
    ctx.selection.SetOrdered({ EditorObjectId{1}, EditorObjectId{2} }, {1});
    EditorCommandStack history;
    InspectorPanel panel;
    AssetRegistry registry;
    EditorExtensionRegistry extensions;
    auto probe = std::make_unique<DragProbe>();
    const auto* rect = probe.get();
    extensions.RegisterPropertyDrawer(std::move(probe));
    bool open = true;
    const auto frame = [&]
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(900, 1200), ImGuiCond_Always);
        panel.Draw(ctx, history, registry, extensions, &open);
        ImGui::Render();
    };
    frame(); frame();
    const float x = rect->min.x + 25, y = (rect->min.y + rect->max.y) * 0.5f;
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    frame();
    io.AddMouseButtonEvent(0, true);
    frame();
    Check(ImGui::IsAnyItemActive(), "mouse activates actual inspector widget");
    const float before = ctx.scene.FindEditorObject(1)->AsGBufferRenderable()->MaterialParamsRef().metalRough.y;
    for (int i = 1; i <= 4; ++i)
    {
        io.AddMousePosEvent(x + i * 5, y);
        frame();
        Check(history.HistorySize() == 0, "UI drag remains a single pending transaction");
    }
    io.AddMouseButtonEvent(0, false);
    frame(); frame();
    const float after = ctx.scene.FindEditorObject(1)->AsGBufferRenderable()->MaterialParamsRef().metalRough.y;
    Check(!Near(before, after), "actual mouse drag changes roughness");
    Check(Near(ctx.scene.FindEditorObject(2)->AsGBufferRenderable()->MaterialParamsRef().metalRough.y, after),
        "actual inspector UI broadcasts changes");
    Check(history.HistorySize() == 1, "mouse release commits one batch undo");
    history.Undo(ctx);
    Check(Near(ctx.scene.FindEditorObject(1)->AsGBufferRenderable()->MaterialParamsRef().metalRough.y, before),
        "actual UI gesture undo");
    std::puts("PASS: ImGui mouse drag -> existing InspectorPanel -> all selected objects -> one undo");
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::puts("Inspector regression: component deltas");
    try
    {
        TestComponents();
        std::puts("Inspector regression: construct CPU context");
        auto renderer = std::make_unique<Renderer>();
        auto scene = std::make_unique<Scene>();
        LevelManager levels;
        EditorSceneDocument doc;
        EditorSelection selection;
        EditorContext ctx{ *renderer, *scene, levels, doc, selection };
        std::puts("Inspector regression: live multi-edit");
        TestMultiEdit(ctx);

        EditorSceneDocument postDocument;
        Check(postDocument.LoadFromLevelFile("data/levels/wind_test.json"), "load benchmark document");
        const EditorObject* post = nullptr;
        for (const auto& env : postDocument.Environment()) { if (env.type == "postProcess") { post = &env; break; } }
        Check(post != nullptr, "Post Process entity found");
        selection.Replace(post->id);
        EditorContext postCtx{ *renderer, *scene, levels, postDocument, selection };
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1600, 6400);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        TestMouseGesture(ctx);
        selection.Replace(post->id);
#if INSPECTOR_BASELINE
        std::printf("Post Process BEFORE: %.3f ms/draw (Debug, 5 expanded categories)\n",
            BenchmarkPostProcess<InspectorPanelBaseline>(postCtx));
#endif
        const auto version = postDocument.ContentVersion();
        const auto properties = post->properties;
        std::printf("Post Process AFTER:  %.3f ms/draw (Debug, 5 expanded categories)\n",
            BenchmarkPostProcess<InspectorPanel>(postCtx));
        Check(postDocument.ContentVersion() == version && post->properties == properties,
            "idle Post Process document unchanged");
        EditorSceneDocument emptyDocument;
        emptyDocument.Environment().push_back(MakeObject(99, "postProcess", Json::object()));
        selection.Replace({99});
        EditorContext emptyCtx{ *renderer, *scene, levels, emptyDocument, selection };
        BenchmarkPostProcess<InspectorPanel>(emptyCtx);
        Check(emptyDocument.Environment()[0].properties.empty() && !emptyDocument.IsDirty(),
            "viewing missing Post Process groups must not author defaults");
        ImGui::DestroyContext();
        std::puts("PASS: actual Post Process draw, no GPU or editor state writes");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
