#include "editor/ui/InspectorMultiEdit.h"
#include "core/threading/ThreadRole.h" // R16.10: a park instead of a GPU idle
#include "core/JsonField.h" // R9.6: jsonfield::Value -- value()'s conversions, no throw outside the loader
#if WITH_EDITOR

#include <algorithm>
#include <memory>
#include <utility>

#include "app/scene/Scene.h"
#include "app/scene/SceneObjectFactory.h"
#include "editor/EditorContext.h"
#include "editor/commands/TransformObjectCommand.h"
#include "editor/scene/EnvironmentRuntime.h"
#include "editor/ui/InspectorPropertyDelta.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/RenderLayers.h"
#include "rendering/renderables/GBufferRenderable.h"
#include "rendering/renderables/RenderableObjectBase.h"
#include "rendering/renderables/TransparentStaticMesh.h"
#include "vfx/ParticleEmitterObject.h"

namespace
{
    using Json = nlohmann::json;
    using Snapshot = InspectorMultiEdit::Snapshot;
    using InspectorPropertyDelta::Member;

    EditorObject* Find(EditorContext& ctx, EditorObjectId id, bool& environment)
    {
        environment = false;
        if (auto* object = ctx.document.Find(id)) { return object; }
        environment = true;
        for (auto& object : ctx.document.Environment())
        {
            if (object.id.value == id.value) { return &object; }
        }
        return nullptr;
    }

    Json Vector(const Math::float3& v) { return { v.x, v.y, v.z }; }
    Json Vector(const Math::float4& v) { return { v.x, v.y, v.z, v.w }; }
    Math::float3 Float3(const Json& v)
    {
        return Math::float3(v[0].get<float>(), v[1].get<float>(), v[2].get<float>());
    }
    Math::float4 Float4(const Json& v)
    {
        return Math::float4(v[0].get<float>(), v[1].get<float>(),
            v[2].get<float>(), v[3].get<float>());
    }
    bool SameTransform(const EditorTransform& a, const EditorTransform& b)
    {
        return a.position.x == b.position.x && a.position.y == b.position.y &&
            a.position.z == b.position.z && a.rotationDeg.x == b.rotationDeg.x &&
            a.rotationDeg.y == b.rotationDeg.y && a.rotationDeg.z == b.rotationDeg.z &&
            a.scale.x == b.scale.x && a.scale.y == b.scale.y && a.scale.z == b.scale.z;
    }
    Math::float3 MergeVector(const Math::float3& before, const Math::float3& after,
        const Math::float3& target)
    {
        return Math::float3(before.x != after.x ? after.x : target.x,
            before.y != after.y ? after.y : target.y,
            before.z != after.z ? after.z : target.z);
    }
    bool SameValues(const Snapshot& a, const Snapshot& b)
    {
        return SameTransform(a.object.transform, b.object.transform) &&
            a.effective == b.effective;
    }
    bool SameAuthored(const Snapshot& a, const Snapshot& b)
    {
        return SameTransform(a.object.transform, b.object.transform) &&
            a.object.properties == b.object.properties;
    }

    struct Layer { const char* name; RenderLayer value; };
    constexpr Layer layers[] = {
        { "Default", RenderLayer::Default }, { "Terrain", RenderLayer::Terrain },
        { "Transparent", RenderLayer::Transparent }, { "Sky", RenderLayer::Sky },
        { "Lights", RenderLayer::Lights }, { "Gizmo", RenderLayer::Gizmo },
        { "Debug", RenderLayer::Debug }
    };

    bool EmitterField(const std::string& key)
    {
        static constexpr const char* fields[] = {
            "maxParticles", "spawnRate", "lifetime", "coneDir", "coneAngleDeg",
            "speed", "gravity", "drag", "windInfluence", "seed", "rotation", "spin",
            "size", "texture", "additive", "softFade", "sort", "localSpace",
            "flipCols", "flipRows", "flipFps", "flipRandomStart", "frameBlend", "colorKeys"
        };
        return std::any_of(std::begin(fields), std::end(fields),
            [&](const char* field) { return key == field; });
    }

    void StoreAuthored(Snapshot& target, const Json& previous)
    {
        if (target.object.type != "particleEmitter")
        {
            target.object.properties = target.authored;
            return;
        }
        Json& props = target.object.properties;
        const bool newPreset = Member(previous, "preset") != Member(target.authored, "preset");
        if (newPreset)
        {
            props.erase("overrides");
            for (auto it = props.begin(); it != props.end();)
            {
                if (EmitterField(it.key())) { it = props.erase(it); }
                else { ++it; }
            }
        }
        for (auto it = previous.begin(); it != previous.end(); ++it)
        {
            if (!target.authored.contains(it.key()))
            {
                props.erase(it.key());
                if (props.contains("overrides")) { props["overrides"].erase(it.key()); }
            }
        }
        for (auto it = target.authored.begin(); it != target.authored.end(); ++it)
        {
            if (!newPreset && previous.contains(it.key()) && previous[it.key()] == *it) { continue; }
            if (target.authored.contains("preset") && EmitterField(it.key()))
            {
                props["overrides"][it.key()] = *it;
            }
            else { props[it.key()] = *it; }
        }
    }

    class MultiPropertyCommand final : public EditorCommand
    {
    public:
        MultiPropertyCommand(std::vector<Snapshot> before, std::vector<Snapshot> after)
            : before_(std::move(before)), after_(std::move(after)) {}
        bool Execute(EditorContext& ctx) override
        {
            // The drawer and live broadcast already applied the first execution.
            if (!alreadyApplied_)
            {
                for (const auto& snapshot : after_) { InspectorMultiEdit::ApplySnapshot(ctx, snapshot); }
            }
            alreadyApplied_ = false;
            return true;
        }
        void Undo(EditorContext& ctx) override
        {
            for (const auto& snapshot : before_) { InspectorMultiEdit::ApplySnapshot(ctx, snapshot); }
        }
        std::string_view HistoryLabel() const override { return "Edit Selected Properties"; }
    private:
        std::vector<Snapshot> before_, after_;
        bool alreadyApplied_ = true;
    };
}

InspectorMultiEdit::Snapshot InspectorMultiEdit::Capture(
    EditorContext& ctx, const EditorObject& object, bool environment)
{
    Snapshot result{ object, object.properties, object.properties, environment };
    Json& values = result.effective;
    const auto fallback = [&](const char* key, Json value)
    {
        if (!values.contains(key)) { values[key] = std::move(value); }
    };
    if (environment)
    {
        if (object.type == "pointLight" || object.type == "spotLight")
        {
            fallback("color", { 1.0f, 1.0f, 1.0f });
            fallback("position", { 0.0f, 0.0f, 0.0f });
            if (object.type == "spotLight") { fallback("direction", { 0.0f, -1.0f, 0.0f }); }
        }
        return result;
    }
    auto* runtime = ctx.scene.FindEditorObject(object.id.value);
    if (object.type == "staticMesh" && runtime)
    {
        for (const auto& layer : layers)
        {
            if (runtime->GetRenderLayerMask() == RenderLayerMask(layer.value))
            {
                values["renderLayer"] = layer.name;
                break;
            }
        }
        if (const auto* gb = runtime->AsGBufferRenderable())
        {
            const auto& mp = gb->MaterialParamsRef();
            values["texOffsScale"] = Vector(mp.texOffsScale);
            values["normalStrength"] = mp.texFlags.w;
            values["windStrength"] = gb->GetWindStrength();
            values["useMR"] = mp.texFlags.y > 0.5f;
            values["useAlbedo"] = mp.texFlags.x > 0.5f;
            values["metalRough"] = { mp.metalRough.x, mp.metalRough.y };
            values["baseColor"] = { mp.baseColor.x, mp.baseColor.y, mp.baseColor.z };
            values["materials"] = Json::array();
            for (size_t i = 0; i < gb->SlotCount(); ++i)
            {
                values["materials"].push_back(gb->SlotPreset(i));
            }
        }
    }
    else if (object.type == "transparentMesh")
    {
        fallback("tint", { 0.85f, 0.93f, 1.0f });
        fallback("absorption", { 0.25f, 0.08f, 0.04f });
        fallback("thickness", 0.6f);
        fallback("reflectionStrength", 1.0f);
        fallback("refractionDistortion", 0.015f);
        fallback("roughness", 0.07f);
        fallback("ior", 1.52f);
    }
    else if (object.type == "particleEmitter")
    {
        // Normalize preset overrides to the same editable field space as inline emitters.
        if (object.properties.contains("overrides") && object.properties["overrides"].is_object())
        {
            result.authored.update(object.properties["overrides"]);
        }
        result.authored.erase("overrides");
        values = result.authored;
        if (const auto* emitter = runtime ? runtime->AsParticleEmitter() : nullptr)
        {
            const auto& d = emitter->Desc();
            values["spawnRate"] = d.spawnRate;
            values["lifetime"] = { d.lifetimeMin, d.lifetimeMax };
            values["speed"] = { d.speedMin, d.speedMax };
            values["gravity"] = d.gravity;
            values["drag"] = d.drag;
            values["windInfluence"] = d.windInfluence;
            values["coneAngleDeg"] = d.coneAngleDeg;
            values["coneDir"] = Vector(d.coneDir);
            values["size"] = { d.sizeStart, d.sizeEnd };
            values["softFade"] = d.softFade;
            values["colorKeys"] = Json::array();
            for (const auto& key : d.colorKeys) { values["colorKeys"].push_back(Vector(key)); }
        }
    }
    return result;
}

void InspectorMultiEdit::ApplySnapshot(EditorContext& ctx, const Snapshot& snapshot)
{
    bool environment = false;
    EditorObject* object = Find(ctx, snapshot.object.id, environment);
    if (!object || object->type != snapshot.object.type || environment != snapshot.environment) { return; }
    const Snapshot previous = Capture(ctx, *object, environment);
    object->properties = snapshot.object.properties;
    if (environment)
    {
        EnvironmentRuntime::ApplyChange(ctx, *object, previous.object);
        ctx.document.SetDirty(true);
        return;
    }
    if (!SameTransform(previous.object.transform, snapshot.object.transform))
    {
        TransformObjectCommand::ApplyTransform(ctx, object->id, snapshot.object.transform);
    }
    auto* runtime = ctx.scene.FindEditorObject(object->id.value);
    bool respawn = false;
    for (const char* key : { "mesh", "model", "material", "materials", "inputLayout",
        "shader", "preset", "normalMap" })
    {
        respawn |= Member(previous.object.properties, key) != Member(object->properties, key);
    }
    if (runtime && respawn)
    {
        const Json json = EditorSceneDocument::ObjectToJson(*object);
        std::unique_ptr<RenderableObjectBase> replacement;
        if (object->type == "staticMesh") { replacement = SceneObjectFactory::CreateStaticMeshFromJson(json); }
        else if (object->type == "transparentMesh") { replacement = SceneObjectFactory::CreateTransparentMeshFromJson(ctx.scene, json); }
        else if (object->type == "particleEmitter") { replacement = SceneObjectFactory::CreateParticleEmitterFromJson(json); }
        if (replacement)
        {
            threading::ParkRenderForGame("InspectorMultiEdit"); // R16.10: a park, no GPU idle
            UploadBatch uploads;
            if (uploads.Begin(&ctx.renderer))
            {
                ctx.scene.RemoveEditorObject(object->id.value);
                ctx.scene.AddInitializedEditorObject(ctx.renderer, uploads, object->id.value, std::move(replacement),
                    object->enabled);
                (void)uploads.Submit(&ctx.renderer); // R16.10: with the hand-over (render::deferred)
            }
        }
    }
    else if (runtime)
    {
        const Json& values = snapshot.effective;
        const auto changed = [&](const char* key)
        {
            return values.contains(key) && Member(previous.effective, key) != values[key];
        };
        // R9.6c: the values are the objects' effective JSON (the document over the mesh.json): an undo can bring back a
        // value of another type the file had. Such a key is left alone -- a number, a flag, or an array short of numbers
        // -- instead of throwing out of the frame.
        const auto number = [&](const char* key) -> std::optional<float> { return changed(key) ? jsonfield::Find<float>(values, key) : std::nullopt; };
        const auto flag = [&](const char* key) -> std::optional<bool> { return changed(key) ? jsonfield::Find<bool>(values, key) : std::nullopt; };
        const auto numbers = [&](const char* key, std::size_t count) -> const Json* {
            if (!changed(key)) { return nullptr; }
            const Json& a = values[key];
            if (!a.is_array() || a.size() < count) { return nullptr; }
            for (std::size_t i = 0; i < count; ++i) { if (!a[i].is_number()) { return nullptr; } }
            return &a;
        };
        if (auto* gb = runtime->AsGBufferRenderable())
        {
            // Every surface key reaches all of the object's material slots (GBufferRenderable::SetObject*).
            if (const Json* v = numbers("texOffsScale", 4)) { gb->SetObjectTexOffsScale(Float4(*v)); }
            if (const auto v = number("normalStrength")) { gb->SetObjectNormalStrength(*v); }
            if (const auto v = number("windStrength")) { gb->SetWindStrength(*v); }
            if (const auto v = flag("useMR")) { gb->SetObjectUseMR(*v); }
            // The albedo texture off (a flat colour) -- or back on, but only where the material has one.
            if (const auto v = flag("useAlbedo"))
            {
                gb->SetObjectUseAlbedo(*v);
                gb->MarkMaterialParamOverride(GBufferRenderable::MaterialParamField::UseAlbedo);
            }
            if (const Json* v = numbers("metalRough", 2))
            {
                gb->SetObjectMetalRough(Math::float2((*v)[0].get<float>(), (*v)[1].get<float>()));
            }
            // The tint. Alpha is left as the material set it: painting something red is
            // about its colour, and quietly making it transparent would be a surprise.
            if (const Json* v = numbers("baseColor", 3))
            {
                gb->SetObjectBaseColorRgb(Float3(*v));
                gb->MarkMaterialParamOverride(
                    GBufferRenderable::MaterialParamField::BaseColor);
            }
            if (changed("renderLayer"))
            {
                for (const auto& layer : layers)
                {
                    if (values["renderLayer"] == layer.name) { runtime->SetRenderLayer(layer.value); break; }
                }
                ctx.scene.NotifyEditorShadowCasterVisibilityChanged();
            }
        }
        if (auto* glass = runtime->AsTransparentStaticMesh())
        {
            if (const Json* v = numbers("tint", 3)) { glass->SetTint(Float3(*v)); }
            if (const Json* v = numbers("absorption", 3)) { glass->SetAbsorption(Float3(*v)); }
            if (const auto v = number("thickness")) { glass->SetThickness(*v); }
            if (const auto v = number("reflectionStrength")) { glass->SetReflectionStrength(*v); }
            if (const auto v = number("refractionDistortion")) { glass->SetRefractionDistortion(*v); }
            if (const auto v = number("roughness")) { glass->SetRoughness(*v); }
            if (const auto v = number("ior")) { glass->SetIor(*v); }
        }
        if (auto* emitter = runtime->AsParticleEmitter())
        {
            auto& d = emitter->DescRef();
            if (const auto v = number("spawnRate")) { d.spawnRate = *v; }
            if (const Json* v = numbers("lifetime", 2))
            {
                d.lifetimeMin = (*v)[0].get<float>();
                d.lifetimeMax = (*v)[1].get<float>();
            }
            if (const Json* v = numbers("speed", 2))
            {
                d.speedMin = (*v)[0].get<float>();
                d.speedMax = (*v)[1].get<float>();
            }
            if (const auto v = number("gravity")) { d.gravity = *v; }
            if (const auto v = number("drag")) { d.drag = *v; }
            if (const auto v = number("windInfluence")) { d.windInfluence = *v; }
            if (const auto v = number("coneAngleDeg")) { d.coneAngleDeg = *v; }
            if (const Json* v = numbers("coneDir", 3)) { d.coneDir = Float3(*v); }
            if (const Json* v = numbers("size", 2))
            {
                d.sizeStart = (*v)[0].get<float>();
                d.sizeEnd = (*v)[1].get<float>();
            }
            if (const auto v = number("softFade")) { d.softFade = *v; }
            if (changed("colorKeys") && values["colorKeys"].is_array() && values["colorKeys"].size() >= 4)
            {
                for (size_t i = 0; i < 4; ++i)
                {
                    const Json& key = values["colorKeys"][i];
                    const bool four = key.is_array() && key.size() >= 4 && key[0].is_number() && key[1].is_number() &&
                                      key[2].is_number() && key[3].is_number();
                    if (four) { d.colorKeys[i] = Float4(key); }
                }
            }
        }
    }
    ctx.document.SetDirty(true);
}

void InspectorMultiEdit::Refresh(EditorContext& ctx)
{
    for (auto& snapshot : current_)
    {
        bool environment = false;
        if (const auto* object = Find(ctx, snapshot.object.id, environment))
        {
            snapshot = Capture(ctx, *object, environment);
        }
    }
    mixed_ = std::any_of(current_.begin() + 1, current_.end(),
        [&](const Snapshot& snapshot) { return !SameValues(current_.front(), snapshot); });
    version_ = ctx.document.ContentVersion();
}

bool InspectorMultiEdit::Begin(EditorContext& ctx, EditorCommandStack& history)
{
    drawCommands_.Clear();
    staticVersionBeforeDraw_ = ctx.scene.GetStaticSetVersion();
    if (levelPath_ != ctx.document.LevelPath())
    {
        // IDs can be reused after a level switch; never commit an old level's gesture.
        beforeGesture_.clear();
        current_.clear();
        levelPath_ = ctx.document.LevelPath();
    }
    const auto& selected = ctx.selection.Ordered();
    bool matches = selected.size() > 1 && selected.size() == current_.size() &&
        current_.front().object.id.value == ctx.selection.Primary().value;
    if (matches)
    {
        size_t index = 1;
        for (const auto id : selected)
        {
            if (id.value == ctx.selection.Primary().value) { continue; }
            if (current_[index++].object.id.value != id.value) { matches = false; break; }
        }
    }
    if (!matches)
    {
        Finish(ctx, history);
        current_.clear();
        if (selected.size() < 2) { return false; }
        bool environment = false;
        const auto* primary = Find(ctx, ctx.selection.Primary(), environment);
        if (!primary) { return false; }
        current_.push_back(Capture(ctx, *primary, environment));
        for (const auto id : selected)
        {
            if (id.value == primary->id.value) { continue; }
            bool otherEnvironment = false;
            const auto* object = Find(ctx, id, otherEnvironment);
            if (!object || object->type != primary->type || environment != otherEnvironment)
            {
                current_.clear();
                return false;
            }
            current_.push_back(Capture(ctx, *object, environment));
        }
        Refresh(ctx);
    }
    else if (version_ != ctx.document.ContentVersion())
    {
        // An edit outside this panel (including Undo) invalidated the cached values.
        beforeGesture_.clear();
        Refresh(ctx);
    }
    return !current_.empty();
}

void InspectorMultiEdit::End(EditorContext& ctx, EditorCommandStack& history, bool itemActive)
{
    if (current_.empty()) { return; }
    if (version_ != ctx.document.ContentVersion())
    {
        bool environment = false;
        const auto* object = Find(ctx, current_.front().object.id, environment);
        if (!object) { beforeGesture_.clear(); current_.clear(); return; }
        const Snapshot after = Capture(ctx, *object, environment);
        const Snapshot& before = current_.front();
        if (!SameAuthored(before, after))
        {
            if (beforeGesture_.empty()) { beforeGesture_ = current_; }
            for (size_t i = 1; i < current_.size(); ++i)
            {
                Snapshot target = current_[i];
                const Json oldAuthored = target.authored;
                if (before.object.type == "particleEmitter" &&
                    Member(before.authored, "preset") != Member(after.authored, "preset"))
                {
                    // Selecting a preset resets EVERY recipient's old overrides,
                    // including keys that were never authored on the primary.
                    for (auto it = target.authored.begin(); it != target.authored.end();)
                    {
                        if (EmitterField(it.key())) { it = target.authored.erase(it); }
                        else { ++it; }
                    }
                }
                InspectorPropertyDelta::Apply(before.authored, after.authored,
                    before.effective, after.effective, target.authored, target.effective);
                StoreAuthored(target, oldAuthored);
                auto& transform = target.object.transform;
                transform.position = MergeVector(before.object.transform.position,
                    after.object.transform.position, transform.position);
                transform.rotationDeg = MergeVector(before.object.transform.rotationDeg,
                    after.object.transform.rotationDeg, transform.rotationDeg);
                transform.scale = MergeVector(before.object.transform.scale,
                    after.object.transform.scale, transform.scale);
                if (!SameAuthored(current_[i], target)) { ApplySnapshot(ctx, target); }
            }
            if (staticVersionBeforeDraw_ != ctx.scene.GetStaticSetVersion())
            {
                ctx.scene.RefreshShadowGpuForEditor(ctx.renderer);
            }
        }
        Refresh(ctx);
    }
    if (!itemActive || drawCommands_.HistorySize() != 0) { Finish(ctx, history); }
}

void InspectorMultiEdit::Finish(EditorContext& ctx, EditorCommandStack& history)
{
    if (beforeGesture_.empty()) { return; }
    const bool changed = !std::equal(beforeGesture_.begin(), beforeGesture_.end(),
        current_.begin(), SameAuthored);
    if (changed)
    {
        history.Execute(ctx, std::make_unique<MultiPropertyCommand>(std::move(beforeGesture_), current_));
    }
    beforeGesture_.clear();
}

#endif
