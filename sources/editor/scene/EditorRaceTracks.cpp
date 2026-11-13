#include "editor/scene/EditorRaceTracks.h"
#if WITH_EDITOR

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <utility>

#include "app/game/RaceCourse.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneObjectRegistry.h"
#include "editor/EditorContext.h"
#include "editor/commands/CompositeCommand.h"
#include "editor/commands/DeleteObjectCommand.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/commands/SetRaceKeyCommand.h"
#include "editor/commands/TransformObjectCommand.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/renderables/RenderableObjectBase.h"
#include "imgui.h"

namespace
{
    constexpr float kDegrees = 57.2957795f;

    float Wrap180(float degrees)
    {
        degrees = std::fmod(degrees + 180.0f, 360.0f);
        if (degrees < 0.0f) { degrees += 360.0f; }
        return degrees - 180.0f;
    }

    // A new mark: a copy of `source` (its asset, scale, keys, group) at `transform` with gate `gate` -- the object
    // registry's road, as DuplicateObjectCommand's. Added to the selection when asked; Undo takes it away again.
    class CreateRaceMarkCommand : public EditorCommand
    {
    public:
        CreateRaceMarkCommand(EditorObjectId source, const EditorTransform& transform, int gate, bool select)
            : source_(source), transform_(transform), gate_(gate), select_(select)
        {
        }

        bool Execute(EditorContext& ctx) override
        {
            if (!built_)
            {
                const EditorObject* source = ctx.document.Find(source_);
                if (!source) { return false; }
                object_ = *source;
                object_.id = ctx.document.AllocateId();
                object_.transform = transform_;
                object_.properties[game::kRaceGateKey] = gate_;
                built_ = true;
            }
            const nlohmann::json objectJson = EditorSceneDocument::ObjectToJson(object_);
            SceneObjectRegistry objectRegistry = SceneObjectRegistry::CreateWithBuiltins();
            if (!objectRegistry.Has(object_.type)) { return false; }
            SceneObjectRegistry::CreationContext creationCtx{ ctx.scene };
            SceneObjectRegistry::ObjectList runtimeObjects = objectRegistry.Create(object_.type, creationCtx, objectJson);
            if (runtimeObjects.empty()) { return false; }
            previousSelection_ = ctx.selection;
            ctx.document.Add(object_);
            ctx.renderer.WaitForPreviousFrame();
            UploadBatch uploads;
            if (!uploads.Begin(&ctx.renderer))
            {
                ctx.document.Remove(object_.id);
                return false;
            }
            for (std::unique_ptr<RenderableObjectBase>& runtime : runtimeObjects)
            {
                if (!runtime) { continue; }
                if (!ctx.scene.AddInitializedEditorObject(ctx.renderer, uploads, object_.id.value, std::move(runtime), object_.enabled))
                {
                    ctx.scene.RemoveEditorObject(object_.id.value);
                    ctx.document.Remove(object_.id);
                    return false;
                }
            }
            uploads.SubmitAndWait(&ctx.renderer);
            if (select_) { ctx.selection.Add(object_.id); }
            ctx.document.SetDirty(true);
            return true;
        }

        void Undo(EditorContext& ctx) override
        {
            ctx.renderer.WaitForPreviousFrame();
            ctx.scene.RemoveEditorObject(object_.id.value);
            ctx.document.Remove(object_.id);
            ctx.selection = previousSelection_;
            ctx.document.SetDirty(true);
        }

        std::string_view HistoryLabel() const override { return "Place Race Mark"; }

    private:
        EditorObjectId source_;
        EditorTransform transform_;
        int gate_ = 0;
        bool select_ = false;
        bool built_ = false;
        EditorObject object_;
        EditorSelection previousSelection_;
    };

    // The selection replaced by nothing (the composite's first step when it selects what it makes).
    class ClearSelectionCommand : public EditorCommand
    {
    public:
        bool Execute(EditorContext& ctx) override
        {
            before_ = ctx.selection;
            ctx.selection = EditorSelection{};
            return true;
        }
        void Undo(EditorContext& ctx) override { ctx.selection = before_; }
        std::string_view HistoryLabel() const override { return "Select"; }

    private:
        EditorSelection before_;
    };

    const editorrace::Track* FindTrack(const std::vector<editorrace::Track>& tracks, const std::string& name)
    {
        for (const editorrace::Track& track : tracks)
        {
            if (track.name == name) { return &track; }
        }
        return nullptr;
    }
}

namespace editorrace
{
    std::vector<Track> Collect(const EditorSceneDocument& document)
    {
        std::map<std::string, std::map<int, std::vector<EditorObjectId>>> byTrack;
        for (const EditorObject& object : document.Objects())
        {
            const int gate = SetRaceKeyCommand::DocumentGate(object);
            if (gate <= 0) { continue; }
            byTrack[SetRaceKeyCommand::DocumentTrack(object)][gate].push_back(object.id);
        }
        std::vector<Track> tracks;
        tracks.reserve(byTrack.size());
        for (auto& [name, gates] : byTrack) // std::map: "" (the level's first) sorts first, then by name -- RaceCourse::TrackNames' order
        {
            Track track;
            track.name = name;
            for (auto& [number, marks] : gates) { track.gates.push_back(Gate{ number, std::move(marks) }); }
            tracks.push_back(std::move(track));
        }
        return tracks;
    }

    bool SelectedGate(const EditorContext& ctx, std::string* track, int* gate, EditorObjectId* a, EditorObjectId* b)
    {
        const std::vector<EditorObjectId>& ids = ctx.selection.Ordered();
        if (ids.size() != 2) { return false; }
        const EditorObject* first = ctx.document.Find(ids[0]);
        const EditorObject* second = ctx.document.Find(ids[1]);
        if (!first || !second) { return false; }
        const int number = SetRaceKeyCommand::DocumentGate(*first);
        if (number <= 0 || number != SetRaceKeyCommand::DocumentGate(*second)) { return false; }
        const std::string name = SetRaceKeyCommand::DocumentTrack(*first);
        if (name != SetRaceKeyCommand::DocumentTrack(*second)) { return false; }
        if (track) { *track = name; }
        if (gate) { *gate = number; }
        if (a) { *a = ids[0]; }
        if (b) { *b = ids[1]; }
        return true;
    }

    bool SelectedTrack(const EditorContext& ctx, std::string* track)
    {
        const std::vector<EditorObjectId>& ids = ctx.selection.Ordered();
        if (ids.size() < 4) { return false; }
        const EditorObject* first = ctx.document.Find(ids[0]);
        if (!first || SetRaceKeyCommand::DocumentGate(*first) <= 0) { return false; }
        const std::string name = SetRaceKeyCommand::DocumentTrack(*first);
        std::size_t marks = 0;
        for (const EditorObject& object : ctx.document.Objects())
        {
            if (SetRaceKeyCommand::DocumentGate(object) > 0 && SetRaceKeyCommand::DocumentTrack(object) == name)
            {
                if (!ctx.selection.Contains(object.id)) { return false; }
                ++marks;
            }
        }
        if (marks != ids.size()) { return false; } // something else is selected too
        if (track) { *track = name; }
        return true;
    }

    Frame FrameOf(const EditorTransform& a, const EditorTransform& b)
    {
        Frame frame;
        frame.center = (a.position + b.position) * 0.5f;
        const Math::float2 line(b.position.x - a.position.x, b.position.z - a.position.z);
        frame.width = std::sqrt(line.Dot(line));
        frame.across = frame.width > 1e-4f ? line / frame.width : Math::float2(1.0f, 0.0f);
        frame.bearingDeg = std::atan2(frame.across.x, frame.across.y) * kDegrees;
        return frame;
    }

    Frame FrameWith(const Frame& frame, float bearingDeg)
    {
        Frame turned = frame;
        turned.bearingDeg = Wrap180(bearingDeg);
        const float radians = turned.bearingDeg / kDegrees;
        turned.across = Math::float2(std::sin(radians), std::cos(radians));
        return turned;
    }

    void Place(const Frame& before, const Frame& after, EditorTransform& a, EditorTransform& b)
    {
        const float lift = after.center.y - before.center.y;
        const float turn = Wrap180(after.bearingDeg - before.bearingDeg);
        const float half = 0.5f * after.width;
        a.position = Math::float3(after.center.x - after.across.x * half, a.position.y + lift, after.center.z - after.across.y * half);
        b.position = Math::float3(after.center.x + after.across.x * half, b.position.y + lift, after.center.z + after.across.y * half);
        a.rotationDeg.y = Wrap180(a.rotationDeg.y + turn);
        b.rotationDeg.y = Wrap180(b.rotationDeg.y + turn);
    }

    void MoveGate(EditorContext& ctx, EditorCommandStack& history, EditorObjectId a, EditorObjectId b, const Frame& after, const char* label)
    {
        const EditorObject* markA = ctx.document.Find(a);
        const EditorObject* markB = ctx.document.Find(b);
        if (!markA || !markB) { return; }
        const EditorTransform beforeA = markA->transform;
        const EditorTransform beforeB = markB->transform;
        EditorTransform afterA = beforeA;
        EditorTransform afterB = beforeB;
        Place(FrameOf(beforeA, beforeB), after, afterA, afterB);
        auto composite = std::make_unique<CompositeCommand>(label);
        composite->Add(std::make_unique<TransformObjectCommand>(a, beforeA, afterA));
        composite->Add(std::make_unique<TransformObjectCommand>(b, beforeB, afterB));
        history.Execute(ctx, std::move(composite));
    }

    void InsertGateAfter(EditorContext& ctx, EditorCommandStack& history, const std::string& trackName, int number)
    {
        const std::vector<Track> tracks = Collect(ctx.document);
        const Track* track = FindTrack(tracks, trackName);
        if (!track) { return; }
        const Gate* gate = nullptr;
        const Gate* next = nullptr;
        for (std::size_t i = 0; i < track->gates.size(); ++i)
        {
            if (track->gates[i].number == number)
            {
                gate = &track->gates[i];
                next = &track->gates[(i + 1) % track->gates.size()];
            }
        }
        if (!gate || gate->marks.size() != 2 || !next || next->marks.size() != 2) { return; }
        const EditorObject* a = ctx.document.Find(gate->marks[0]);
        const EditorObject* b = ctx.document.Find(gate->marks[1]);
        const EditorObject* nextA = ctx.document.Find(next->marks[0]);
        const EditorObject* nextB = ctx.document.Find(next->marks[1]);
        if (!a || !b || !nextA || !nextB) { return; }
        // Halfway to the next gate, at this gate's width, its line the mean of the two gates' (each pair's own order:
        // the mean of a line and its reverse would be no line).
        const Frame here = FrameOf(a->transform, b->transform);
        Frame there = FrameOf(nextA->transform, nextB->transform);
        if (here.across.Dot(there.across) < 0.0f) { there = FrameWith(there, there.bearingDeg + 180.0f); }
        Frame middle = here;
        middle.center = (here.center + there.center) * 0.5f;
        const Math::float2 mean = here.across + there.across;
        const float meanLength = std::sqrt(mean.Dot(mean));
        if (meanLength > 1e-3f) { middle = FrameWith(middle, std::atan2(mean.x, mean.y) * kDegrees); }
        EditorTransform newA = a->transform;
        EditorTransform newB = b->transform;
        Place(here, middle, newA, newB);

        auto composite = std::make_unique<CompositeCommand>("Insert Race Gate " + std::to_string(number + 1));
        // The gates after it one on, from the last back (no two gates share a number on the way).
        for (std::size_t i = track->gates.size(); i-- > 0;)
        {
            const Gate& later = track->gates[i];
            if (later.number <= number) { continue; }
            for (const EditorObjectId mark : later.marks) { composite->Add(SetRaceKeyCommand::Gate(mark, later.number + 1)); }
        }
        composite->Add(std::make_unique<ClearSelectionCommand>());
        composite->Add(std::make_unique<CreateRaceMarkCommand>(a->id, newA, number + 1, true));
        composite->Add(std::make_unique<CreateRaceMarkCommand>(b->id, newB, number + 1, true));
        history.Execute(ctx, std::move(composite));
    }

    void DeleteGate(EditorContext& ctx, EditorCommandStack& history, const std::string& trackName, int number)
    {
        const std::vector<Track> tracks = Collect(ctx.document);
        const Track* track = FindTrack(tracks, trackName);
        if (!track) { return; }
        auto composite = std::make_unique<CompositeCommand>("Delete Race Gate " + std::to_string(number));
        composite->Add(std::make_unique<ClearSelectionCommand>());
        for (const Gate& gate : track->gates)
        {
            if (gate.number == number)
            {
                for (const EditorObjectId mark : gate.marks) { composite->Add(std::make_unique<DeleteObjectCommand>(mark)); }
            }
        }
        for (const Gate& gate : track->gates) // the gates after it one back, from the first on
        {
            if (gate.number <= number) { continue; }
            for (const EditorObjectId mark : gate.marks) { composite->Add(SetRaceKeyCommand::Gate(mark, gate.number - 1)); }
        }
        history.Execute(ctx, std::move(composite));
    }

    void RenameTrack(EditorContext& ctx, EditorCommandStack& history, const std::string& trackName, const std::string& name)
    {
        if (name == trackName) { return; }
        const std::vector<Track> tracks = Collect(ctx.document);
        const Track* track = FindTrack(tracks, trackName);
        if (!track) { return; }
        auto composite = std::make_unique<CompositeCommand>("Rename Race Track " + game::TrackDisplayName(name));
        for (const Gate& gate : track->gates)
        {
            for (const EditorObjectId mark : gate.marks) { composite->Add(SetRaceKeyCommand::Track(mark, name)); }
        }
        history.Execute(ctx, std::move(composite));
    }

    void ReverseTrack(EditorContext& ctx, EditorCommandStack& history, const std::string& trackName)
    {
        const std::vector<Track> tracks = Collect(ctx.document);
        const Track* track = FindTrack(tracks, trackName);
        if (!track || track->gates.empty()) { return; }
        // Gate 1 stays the start; the rest run the other way: 2..n -> n..2.
        const int count = track->gates.back().number;
        auto composite = std::make_unique<CompositeCommand>("Reverse Race Track " + game::TrackDisplayName(trackName));
        for (const Gate& gate : track->gates)
        {
            if (gate.number == 1) { continue; }
            const int reversed = count + 2 - gate.number;
            for (const EditorObjectId mark : gate.marks) { composite->Add(SetRaceKeyCommand::Gate(mark, reversed)); }
        }
        history.Execute(ctx, std::move(composite));
    }

    float TrackLength(const EditorSceneDocument& document, const Track& track)
    {
        std::vector<Math::float3> middles;
        for (const Gate& gate : track.gates)
        {
            Math::float3 sum(0.0f, 0.0f, 0.0f);
            int n = 0;
            for (const EditorObjectId mark : gate.marks)
            {
                if (const EditorObject* object = document.Find(mark))
                {
                    sum = sum + object->transform.position;
                    ++n;
                }
            }
            if (n > 0) { middles.push_back(sum * (1.0f / static_cast<float>(n))); }
        }
        float length = 0.0f;
        for (std::size_t i = 0; i < middles.size() && middles.size() > 1; ++i)
        {
            const Math::float3 d = middles[(i + 1) % middles.size()] - middles[i];
            length += std::sqrt(d.x * d.x + d.z * d.z);
        }
        return length;
    }

    std::vector<EditorObjectId> GateMarksOf(const EditorSceneDocument& document, EditorObjectId mark)
    {
        std::vector<EditorObjectId> marks;
        const EditorObject* object = document.Find(mark);
        if (!object) { return marks; }
        const int number = SetRaceKeyCommand::DocumentGate(*object);
        if (number <= 0) { return marks; }
        const std::string name = SetRaceKeyCommand::DocumentTrack(*object);
        marks.push_back(mark);
        for (const EditorObject& other : document.Objects())
        {
            if (other.id.value != mark.value && SetRaceKeyCommand::DocumentGate(other) == number &&
                SetRaceKeyCommand::DocumentTrack(other) == name)
            {
                marks.push_back(other.id);
            }
        }
        return marks;
    }

    std::vector<EditorObjectId> TrackMarks(const Track& track)
    {
        std::vector<EditorObjectId> marks;
        for (const Gate& gate : track.gates) { marks.insert(marks.end(), gate.marks.begin(), gate.marks.end()); }
        return marks;
    }
}

// ---- The Inspector's section ----------------------------------------------------------------------------------------------

namespace
{
    // A drag of one of the gate's numbers moves its marks live; letting go puts the whole drag on the undo stack as one
    // entry (TransformObjectCommand::ApplyTransform while it lasts, the commands with the transforms from before it after).
    struct GateDrag
    {
        bool active = false;
        EditorObjectId a{};
        EditorObjectId b{};
        EditorTransform beforeA{};
        EditorTransform beforeB{};
    };
    GateDrag g_gateDrag;

    void GateDragUpdate(EditorContext& ctx, EditorObjectId a, EditorObjectId b, bool changed, bool released,
                        const editorrace::Frame& after, const char* label, EditorCommandStack& history)
    {
        if (changed)
        {
            if (!g_gateDrag.active || g_gateDrag.a.value != a.value || g_gateDrag.b.value != b.value)
            {
                const EditorObject* markA = ctx.document.Find(a);
                const EditorObject* markB = ctx.document.Find(b);
                if (!markA || !markB) { return; }
                g_gateDrag = GateDrag{ true, a, b, markA->transform, markB->transform };
            }
            EditorTransform newA = g_gateDrag.beforeA;
            EditorTransform newB = g_gateDrag.beforeB;
            editorrace::Place(editorrace::FrameOf(g_gateDrag.beforeA, g_gateDrag.beforeB), after, newA, newB);
            TransformObjectCommand::ApplyTransform(ctx, a, newA);
            TransformObjectCommand::ApplyTransform(ctx, b, newB);
        }
        if (released && g_gateDrag.active)
        {
            const EditorObject* markA = ctx.document.Find(g_gateDrag.a);
            const EditorObject* markB = ctx.document.Find(g_gateDrag.b);
            if (markA && markB)
            {
                auto composite = std::make_unique<CompositeCommand>(label);
                composite->Add(std::make_unique<TransformObjectCommand>(g_gateDrag.a, g_gateDrag.beforeA, markA->transform));
                composite->Add(std::make_unique<TransformObjectCommand>(g_gateDrag.b, g_gateDrag.beforeB, markB->transform));
                history.Execute(ctx, std::move(composite));
            }
            g_gateDrag = GateDrag{};
        }
    }

    // The gate's middle, by its marks (or 0 when it has none).
    Math::float3 GateMiddle(const EditorSceneDocument& document, const editorrace::Gate& gate)
    {
        Math::float3 sum(0.0f, 0.0f, 0.0f);
        int n = 0;
        for (const EditorObjectId mark : gate.marks)
        {
            if (const EditorObject* object = document.Find(mark))
            {
                sum = sum + object->transform.position;
                ++n;
            }
        }
        return n > 0 ? sum * (1.0f / static_cast<float>(n)) : sum;
    }

    float FlatDistance(const Math::float3& p, const Math::float3& q)
    {
        return std::sqrt((p.x - q.x) * (p.x - q.x) + (p.z - q.z) * (p.z - q.z));
    }

    void SelectMarks(EditorContext& ctx, const std::vector<EditorObjectId>& marks)
    {
        if (marks.empty()) { return; }
        ctx.selection.SetOrdered(marks, marks.front());
    }

    void DrawGateList(EditorContext& ctx, const editorrace::Track& track, int current)
    {
        if (!ImGui::BeginTable("##raceGates", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) { return; }
        ImGui::TableSetupColumn("Gate");
        ImGui::TableSetupColumn("Width (m)");
        ImGui::TableSetupColumn("To the next (m)");
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < track.gates.size(); ++i)
        {
            const editorrace::Gate& gate = track.gates[i];
            const editorrace::Gate& next = track.gates[(i + 1) % track.gates.size()];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char label[48];
            std::snprintf(label, sizeof(label), gate.marks.size() == 2 ? "Gate %d" : "Gate %d (!)", gate.number);
            ImGui::PushID(gate.number);
            if (ImGui::Selectable(label, gate.number == current, ImGuiSelectableFlags_SpanAllColumns)) { SelectMarks(ctx, gate.marks); }
            if (gate.marks.size() != 2 && ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%zu marks: a gate is two. The race leaves this number out.", gate.marks.size());
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            if (gate.marks.size() == 2)
            {
                const EditorObject* a = ctx.document.Find(gate.marks[0]);
                const EditorObject* b = ctx.document.Find(gate.marks[1]);
                if (a && b) { ImGui::Text("%.1f", editorrace::FrameOf(a->transform, b->transform).width); }
            }
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", FlatDistance(GateMiddle(ctx.document, gate), GateMiddle(ctx.document, next)));
        }
        ImGui::EndTable();
    }
}

namespace editorrace
{
    bool DrawInspector(EditorContext& ctx, EditorCommandStack& history)
    {
        std::string trackName;
        int number = 0;
        EditorObjectId a{};
        EditorObjectId b{};
        if (SelectedGate(ctx, &trackName, &number, &a, &b))
        {
            const EditorObject* markA = ctx.document.Find(a);
            const EditorObject* markB = ctx.document.Find(b);
            if (!markA || !markB) { return false; }
            const std::vector<Track> tracks = Collect(ctx.document);
            const Track* track = FindTrack(tracks, trackName);
            if (!track) { return false; }
            std::size_t index = 0;
            for (std::size_t i = 0; i < track->gates.size(); ++i)
            {
                if (track->gates[i].number == number) { index = i; }
            }
            const std::string display = game::TrackDisplayName(trackName);
            ImGui::SeparatorText("Race gate");
            ImGui::Text("Gate %d of %zu, course '%s'", number, track->gates.size(), display.c_str());
            if (number == 1) { ImGui::SameLine(); ImGui::TextDisabled("(the start and the finish)"); }

            ImGui::PushID("raceGate");
            const Frame frame = FrameOf(markA->transform, markB->transform);
            // The numbers the drag shows are the gate as it stands now (it moves live), measured from the drag's start.
            float width = frame.width;
            ImGui::SetNextItemWidth(160.0f);
            const bool widthChanged = ImGui::DragFloat("Width (m)", &width, 0.1f, 2.0f, 400.0f, "%.1f");
            Frame wider = g_gateDrag.active ? FrameOf(g_gateDrag.beforeA, g_gateDrag.beforeB) : frame;
            wider.width = std::max(width, 2.0f);
            GateDragUpdate(ctx, a, b, widthChanged, ImGui::IsItemDeactivatedAfterEdit(), wider, "Race Gate Width", history);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Between the two marks' middles. The gate stays centred on its middle.");
            }

            float bearing = frame.bearingDeg;
            ImGui::SetNextItemWidth(160.0f);
            const bool turnChanged = ImGui::DragFloat("Turn (deg)", &bearing, 0.25f, -720.0f, 720.0f, "%.1f");
            const Frame turned = FrameWith(g_gateDrag.active ? FrameOf(g_gateDrag.beforeA, g_gateDrag.beforeB) : frame, bearing);
            GateDragUpdate(ctx, a, b, turnChanged, ImGui::IsItemDeactivatedAfterEdit(), turned, "Race Gate Turn", history);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("The bearing of the line from the first mark to the second (as an object's yaw): the gate turns\n"
                                  "about its middle and each mark turns with it. A boat passes square to this line.");
            }

            float centre[2] = { frame.center.x, frame.center.z };
            ImGui::SetNextItemWidth(220.0f);
            const bool moved = ImGui::DragFloat2("Middle (x, z)", centre, 0.1f, 0.0f, 0.0f, "%.1f");
            Frame placed = g_gateDrag.active ? FrameOf(g_gateDrag.beforeA, g_gateDrag.beforeB) : frame;
            placed.center = Math::float3(centre[0], placed.center.y, centre[1]);
            GateDragUpdate(ctx, a, b, moved, ImGui::IsItemDeactivatedAfterEdit(), placed, "Move Race Gate", history);

            const Track& course = *track;
            const Gate& previous = course.gates[(index + course.gates.size() - 1) % course.gates.size()];
            const Gate& next = course.gates[(index + 1) % course.gates.size()];
            ImGui::TextDisabled("From gate %d: %.0f m, to gate %d: %.0f m", previous.number,
                                FlatDistance(GateMiddle(ctx.document, previous), frame.center), next.number,
                                FlatDistance(frame.center, GateMiddle(ctx.document, next)));

            if (ImGui::Button("< Previous")) { SelectMarks(ctx, previous.marks); }
            ImGui::SameLine();
            if (ImGui::Button("Next >")) { SelectMarks(ctx, next.marks); }
            ImGui::SameLine();
            if (ImGui::Button("Whole course")) { SelectMarks(ctx, TrackMarks(course)); }

            if (ImGui::Button("Insert a gate after")) { InsertGateAfter(ctx, history, trackName, number); }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Copies of these two marks halfway to gate %d, numbered %d; the gates after it move one on.",
                                  next.number, number + 1);
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.14f, 1.0f));
            if (ImGui::Button("Delete gate")) { DeleteGate(ctx, history, trackName, number); }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Its two marks deleted; the gates after it move one back. Ctrl+Z puts it back."); }
            ImGui::PopID();
            ImGui::Separator();
            return true;
        }

        if (SelectedTrack(ctx, &trackName))
        {
            const std::vector<Track> tracks = Collect(ctx.document);
            const Track* track = FindTrack(tracks, trackName);
            if (!track) { return false; }
            ImGui::SeparatorText("Race track");
            ImGui::PushID("raceTrack");
            static std::string s_nameFor;
            static char s_name[64] = {};
            if (s_nameFor != trackName)
            {
                std::snprintf(s_name, sizeof(s_name), "%s", trackName.c_str());
                s_nameFor = trackName;
            }
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::InputTextWithHint("Name", "classic (the level's first)", s_name, sizeof(s_name), ImGuiInputTextFlags_EnterReturnsTrue) ||
                ImGui::IsItemDeactivatedAfterEdit())
            {
                const std::string name = s_name;
                if (name != trackName && !FindTrack(tracks, name)) { RenameTrack(ctx, history, trackName, name); }
                else { std::snprintf(s_name, sizeof(s_name), "%s", trackName.c_str()); }
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Every mark's \"raceTrack\". Empty = the level's first course (shown as classic). A name another\n"
                                  "course already has is refused: two courses would become one.");
            }
            int bad = 0;
            for (const Gate& gate : track->gates) { bad += gate.marks.size() != 2 ? 1 : 0; }
            ImGui::Text("%zu gates, %.0f m through their middles", track->gates.size(), TrackLength(ctx.document, *track));
            const bool numbered = !track->gates.empty() && track->gates.back().number == static_cast<int>(track->gates.size());
            if (bad > 0) { ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f), "%d gate(s) without exactly two marks: left out of the race", bad); }
            if (!numbered) { ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f), "The numbers have gaps (the race runs them in order anyway)"); }
            if (ImGui::Button("Reverse direction")) { ReverseTrack(ctx, history, trackName); }
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Gate 1 stays the start; the rest are run the other way round."); }
            DrawGateList(ctx, *track, 0);
            ImGui::PopID();
            ImGui::Separator();
            return true;
        }

        // One mark: the way to its gate and its course.
        if (ctx.selection.Size() == 1)
        {
            const EditorObject* mark = ctx.document.Find(ctx.selection.Primary());
            if (mark && SetRaceKeyCommand::DocumentGate(*mark) > 0)
            {
                const std::string name = SetRaceKeyCommand::DocumentTrack(*mark);
                ImGui::PushID("raceMark");
                const std::vector<EditorObjectId> gate = GateMarksOf(ctx.document, mark->id);
                char label[64];
                std::snprintf(label, sizeof(label), "Select gate %d", SetRaceKeyCommand::DocumentGate(*mark));
                if (ImGui::Button(label)) { SelectMarks(ctx, gate); }
                ImGui::SameLine();
                if (ImGui::Button("Select its course"))
                {
                    const std::vector<Track> tracks = Collect(ctx.document);
                    if (const Track* track = FindTrack(tracks, name)) { SelectMarks(ctx, TrackMarks(*track)); }
                }
                ImGui::PopID();
                ImGui::Separator();
            }
        }
        return false;
    }
}

#endif // WITH_EDITOR
