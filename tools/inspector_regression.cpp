// CPU-only regression: build the engine Debug|x64, then this tool's vcxproj.
// No window/device, level writes, asset writes, or editor preference changes.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <unordered_map>

#include "app/levels/LevelManager.h"
#include "app/scene/Scene.h"
#include "editor/EditorContext.h"
#include "editor/EditorExtensionRegistry.h"
#include "editor/ui/CollisionEditorTab.h"
#include "editor/ui/InspectorMultiEdit.h"
#include "editor/ui/InspectorPanel.h"
#include "editor/ui/InspectorPropertyDelta.h"
#include "editor/ui/SceneOutlinerPanel.h"
#include "editor/commands/TransformObjectCommand.h"
#include "editor/intent/EditorActionRegistry.h"
#include "editor/scene/EnvironmentRuntime.h"
#include "rendering/core/Renderer.h"
#include "rendering/meshes/StaticMesh.h"
#include "vfx/ParticleEmitterObject.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ImGuizmo/ImGuizmo.h"
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

// The outliner's category box (owner, 2026-10-22): the real panel, a real mouse. Meshes 1 and 2 and point light 6 in
// the group "Lamps", mesh 2 off -> one click on the group row's "On" box raises SetGroupEnabled for all three, ON (a
// mix turns everything on); the controller's road (setEnabled over the targets) changes mesh 2 alone, one undo entry;
// a second click turns all three off, again one entry. The selection is never touched.
// The Mesh Editor's Collision tab with real ImGui input: nothing written by looking at it, a click on "+ Box" adds a primitive, a drag of the
// gizmo's axis moves it, and the block comes off the document again when the shape goes back to Auto.
void TestCollisionTab()
{
    Json doc = { { "geometry", "models/coconut/coconut.mesh.bin" }, { "materials", Json::array({ "m" }) } };
    const Json untouched = doc;
    const std::vector<std::string> slots = { "m" };
    CollisionEditorTab tab;
    tab.Reset();
    auto& io = ImGui::GetIO();
    const ImVec2 canvasMin(0.0f, 2000.0f), canvasMax(1000.0f, 3000.0f);
    Math::mat4 view, proj;
    DirectX::XMStoreFloat4x4(&view.m, DirectX::XMMatrixLookAtLH(DirectX::XMVectorSet(0.0f, 2.0f, -5.0f, 1.0f), DirectX::XMVectorZero(), DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)));
    DirectX::XMStoreFloat4x4(&proj.m, DirectX::XMMatrixPerspectiveFovLH(1.0f, 1.0f, 0.1f, 100.0f));
    const auto frame = [&]
    {
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(1000, 1900), ImGuiCond_Always);
        ImGui::Begin("Collision Regression");
        tab.Draw(doc, slots);
        ImGui::End();
        ImGui::SetNextWindowPos(canvasMin, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(canvasMax.x - canvasMin.x, canvasMax.y - canvasMin.y), ImGuiCond_Always);
        ImGui::Begin("Collision Regression Canvas", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
        tab.DrawGizmo(doc, ImGui::GetWindowDrawList(), canvasMin, canvasMax, view, proj);
        ImGui::End();
        ImGui::Render();
    };
    frame(); frame();
    Check(doc == untouched, "the Collision tab writes nothing by being looked at");

    doc["collision"] = { { "shape", "custom" } };
    frame(); frame();
    // The button, by its own id: a sweep of the mouse down the window until ImGui says it is under it.
    ImGuiWindow* window = ImGui::FindWindowByName("Collision Regression");
    Check(window != nullptr, "the tab's window drawn");
    const ImGuiID addBox = window->GetID("+ Box");
    bool found = false;
    ImVec2 at;
    for (float y = 0.0f; y < 400.0f && !found; y += 2.0f)
    {
        for (float x = 5.0f; x < 120.0f && !found; x += 12.0f)
        {
            io.AddMousePosEvent(x, y);
            frame();
            if (GImGui->HoveredId == addBox)
            {
                found = true;
                at = ImVec2(x, y);
            }
        }
    }
    Check(found, "+ Box found under the mouse");
    const std::uint64_t linesBefore = tab.LinesRevision();
    io.AddMousePosEvent(at.x, at.y);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
    Check(doc["collision"].contains("shapes") && doc["collision"]["shapes"].size() == 1 && doc["collision"]["shapes"][0].value("type", std::string()) == "box", "+ Box adds a box");
    Check(tab.SelectedPrimitive() == 0, "the new box is selected");
    // The preview draws these lines and re-renders only when the revision moves: it has to move now, and to stand still while nothing changes, or every frame costs a GPU wait.
    Check(!tab.SelectedLines().empty() && tab.LinesRevision() != linesBefore, "the new box's edges reach the preview");
    const std::uint64_t linesSettled = tab.LinesRevision();
    frame(); frame();
    Check(tab.LinesRevision() == linesSettled, "the shape's revision stands still while nothing changes");

    // The gizmo. Put the box at the origin, then look for the X axis handle: a sweep along the screen line the axis projects on, until the gizmo says it holds the mouse.
    doc["collision"]["shapes"][0]["center"] = Json::array({ 0.0, 0.0, 0.0 });
    frame();
    const auto toScreen = [&](const Math::float3& p)
    {
        const Math::float4 clip = (view * proj).Transform(Math::float4(p, 1.0f));
        return ImVec2(canvasMin.x + (clip.x / clip.w * 0.5f + 0.5f) * (canvasMax.x - canvasMin.x), canvasMin.y + (0.5f - clip.y / clip.w * 0.5f) * (canvasMax.y - canvasMin.y));
    };
    const ImVec2 origin = toScreen(Math::float3(0.0f, 0.0f, 0.0f));
    const ImVec2 along = toScreen(Math::float3(1.0f, 0.0f, 0.0f));
    const float length = std::hypot(along.x - origin.x, along.y - origin.y);
    const ImVec2 axis((along.x - origin.x) / length, (along.y - origin.y) / length);
    float grab = -1.0f;
    for (float d = 12.0f; d < 400.0f && grab < 0.0f; d += 6.0f)
    {
        io.AddMousePosEvent(origin.x + axis.x * d, origin.y + axis.y * d);
        frame();
        frame();
        if (tab.GizmoWantsMouse()) { grab = d; }
    }
    Check(grab > 0.0f, "the gizmo's axis says it holds the mouse");
    const float beforeX = doc["collision"]["shapes"][0]["center"][0].get<float>();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    frame();
    for (int step = 1; step <= 10; ++step)
    {
        io.AddMousePosEvent(origin.x + axis.x * (grab + step * 8.0f), origin.y + axis.y * (grab + step * 8.0f));
        frame();
    }
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
    const float afterX = doc["collision"]["shapes"][0]["center"][0].get<float>();
    const float afterY = doc["collision"]["shapes"][0]["center"][1].get<float>();
    Check(afterX > beforeX + 0.05f && std::fabs(afterY) < 1.0e-3f, "dragging the gizmo's X axis moves the box along x");

    // ImGuizmo's matrix is the engine's: what the gizmo draws is where the primitive stands.
    {
        const float translation[3] = { 1.0f, 2.0f, 3.0f }, rotation[3] = { 10.0f, 20.0f, 30.0f }, scale[3] = { 1.0f, 1.0f, 1.0f };
        float gizmoMatrix[16];
        ImGuizmo::RecomposeMatrixFromComponents(translation, rotation, scale, gizmoMatrix);
        constexpr float kToRad = 3.14159265358979f / 180.0f;
        const Math::mat4 engine = Math::mat4::RotationFromEulerXYZRad(Math::float3(10.0f * kToRad, 20.0f * kToRad, 30.0f * kToRad)) * Math::mat4::Translation(Math::float3(1.0f, 2.0f, 3.0f));
        DirectX::XMFLOAT4X4 engineFloats;
        DirectX::XMStoreFloat4x4(&engineFloats, engine.xm());
        float worst = 0.0f;
        for (int i = 0; i < 16; ++i) { worst = std::max(worst, std::fabs(gizmoMatrix[i] - (&engineFloats.m[0][0])[i])); }
        Check(worst < 1.0e-5f, "ImGuizmo's Euler order is the engine's");
    }

    // Back to no custom shape: the block is written without its defaults, and an emptied one is no key at all.
    doc["collision"] = { { "shape", "none" } };
    frame();
    tab.Reset();
    Check(!tab.GizmoWantsMouse() && tab.SelectedPrimitive() == -1, "Reset forgets the selection");
    io.AddMousePosEvent(-1.0f, -1.0f);
    frame();
    std::puts("PASS: Collision tab: looking writes nothing, click -> + Box, gizmo drag -> the box moves along its axis, ImGuizmo's Euler order = the engine's");
}

void TestOutlinerGroupToggle(EditorContext& ctx)
{
    auto& doc = ctx.document;
    doc.Find({1})->properties["group"] = "Lamps";
    doc.Find({2})->properties["group"] = "Lamps";
    doc.Environment()[0].properties["group"] = "Lamps"; // point light 6
    doc.Find({2})->enabled = false;
    doc.SetDirty(true);
    const std::vector<EditorObjectId> selectedBefore = ctx.selection.Ordered();
    const auto selectionUntouched = [&]
    {
        const auto& now = ctx.selection.Ordered();
        return now.size() == selectedBefore.size() &&
            std::equal(now.begin(), now.end(), selectedBefore.begin(),
                [](EditorObjectId a, EditorObjectId b) { return a.value == b.value; });
    };

    SceneOutlinerPanel panel;
    EditorCommandStack history;
    AssetRegistry registry;
    EditorExtensionRegistry extensions;
    const std::unordered_map<std::uint64_t, std::vector<std::string>> noErrors;
    bool open = true;
    OutlinerAction raised;
    const auto frame = [&]
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        OutlinerAction action = panel.Draw(doc, ctx.selection, noErrors, &open);
        if (action.type != OutlinerAction::Type::None && raised.type == OutlinerAction::Type::None)
        {
            raised = std::move(action);
        }
        ImGui::Render();
    };
    frame(); frame();

    // The box's id, built the way ImGui builds it: the table's id in the window, the group's name, the box.
    ImGuiWindow* window = ImGui::FindWindowByName("Scene Outliner");
    Check(window != nullptr, "outliner window drawn");
    const ImGuiID tableId = ImHashStr("##outliner", 0, window->ID);
    ImGuiTable* table = ImGui::TableFindByID(tableId);
    Check(table != nullptr && table->Columns.size() == 4, "outliner table found");
    const ImGuiID boxId = ImHashStr("##groupEnabled", 0, ImHashStr("Lamps", 0, tableId));
    const float x = (table->Columns[3].MinX + table->Columns[3].MaxX) * 0.5f;
    auto& io = ImGui::GetIO();
    // THIS frame's hovered id: ImGui::GetHoveredID() falls back to the previous frame's when nothing is hovered now,
    // which "found" the box over the column header and clicked the header.
    const auto findBox = [&]
    {
        for (float y = table->OuterRect.Min.y; y < table->OuterRect.Min.y + 160.0f; y += 2.0f)
        {
            io.AddMousePosEvent(x, y);
            frame();
            if (ImGui::GetCurrentContext()->HoveredId == boxId) { return y; }
        }
        return -1.0f;
    };
    const auto click = [&]
    {
        raised = OutlinerAction{};
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMouseButtonEvent(0, false);
        frame();
    };
    const auto runRaised = [&]
    {
        nlohmann::json params;
        params["enabled"] = raised.enabledValue;
        std::string status;
        return RunEditorAction(EditorActionContext{ ctx, registry, extensions }, history,
            MakeActionIntent("setEnabled", params), raised.targets, status);
    };
    const auto lightOn = [&] { return doc.Environment()[0].properties.value("enabled", true); };

    Check(findBox() >= 0.0f, "the Lamps row has an On box under the mouse");
    click();
    Check(raised.type == OutlinerAction::Type::SetGroupEnabled, "a click on the box raises SetGroupEnabled");
    Check(raised.enabledValue, "a mixed category turns ON");
    Check(raised.targets.size() == 3, "the action carries every row of the category");
    Check(runRaised(), "setEnabled over the category runs");
    Check(doc.Find({1})->enabled && doc.Find({2})->enabled && lightOn(), "all rows on");
    Check(history.HistorySize() == 1, "one undo entry for the category");
    Check(selectionUntouched(), "the category box leaves the selection alone");

    frame(); frame(); // the panel re-reads the document (content version)
    Check(findBox() >= 0.0f, "the box is still there");
    click();
    Check(raised.type == OutlinerAction::Type::SetGroupEnabled && !raised.enabledValue, "an all-on category turns OFF");
    Check(runRaised(), "setEnabled off over the category runs");
    Check(!doc.Find({1})->enabled && !doc.Find({2})->enabled && !lightOn(), "all rows off");
    Check(history.HistorySize() == 2, "again one undo entry");
    history.Undo(ctx);
    Check(doc.Find({1})->enabled && doc.Find({2})->enabled && lightOn(), "undo restores the whole category");
    Check(selectionUntouched(), "selection still untouched");
    std::puts("PASS: ImGui click -> outliner category box -> every row of the group, one undo, selection untouched");
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
        TestOutlinerGroupToggle(ctx);
        TestCollisionTab();
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
