#include "editor/ui/CollisionEditorTab.h"
#if WITH_EDITOR

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ImGuizmo/ImGuizmo.h"
#include "physics/CollisionJson.h"

namespace
{
    constexpr float kMinSize = 0.001f; // 1 mm: what the world clamps a primitive's size to as well

    Math::float3 Size(const physics::CollisionPrimitive& p)
    {
        switch (p.kind)
        {
        case physics::PrimitiveKind::Sphere: return Math::float3(p.radius, p.radius, p.radius);
        case physics::PrimitiveKind::Capsule:
        case physics::PrimitiveKind::Cylinder: return Math::float3(p.radius, p.halfHeight, p.radius);
        case physics::PrimitiveKind::Box: break;
        }
        return p.halfExtents;
    }

    void SetSize(physics::CollisionPrimitive& p, const Math::float3& size)
    {
        const Math::float3 s(std::max(size.x, kMinSize), std::max(size.y, kMinSize), std::max(size.z, kMinSize));
        switch (p.kind)
        {
        case physics::PrimitiveKind::Sphere:
        {
            // A gizmo moves one axis: the one that left the old radius furthest is the new one.
            const float old = p.radius;
            float best = s.x;
            if (std::fabs(s.y - old) > std::fabs(best - old)) { best = s.y; }
            if (std::fabs(s.z - old) > std::fabs(best - old)) { best = s.z; }
            p.radius = best;
            break;
        }
        case physics::PrimitiveKind::Capsule:
        case physics::PrimitiveKind::Cylinder:
            p.radius = std::fabs(s.x - p.radius) >= std::fabs(s.z - p.radius) ? s.x : s.z;
            p.halfHeight = s.y;
            break;
        case physics::PrimitiveKind::Box:
            p.halfExtents = s;
            break;
        }
    }

    Math::mat4 PrimitiveMatrix(const physics::CollisionPrimitive& p)
    {
        constexpr float kToRad = 3.14159265358979f / 180.0f;
        return Math::mat4::RotationFromEulerXYZRad(Math::float3(p.rotationDeg.x * kToRad, p.rotationDeg.y * kToRad, p.rotationDeg.z * kToRad)) *
            Math::mat4::Translation(p.center);
    }

    void AddCircle(std::vector<Math::float3>& out, const Math::mat4& m, const Math::float3& centre, const Math::float3& a, const Math::float3& b, float fromRad, float toRad)
    {
        constexpr int kSegments = 24;
        Math::float3 previous;
        for (int i = 0; i <= kSegments; ++i)
        {
            const float t = fromRad + (toRad - fromRad) * static_cast<float>(i) / kSegments;
            const Math::float3 point = m.TransformPoint(centre + a * std::cos(t) + b * std::sin(t));
            if (i > 0)
            {
                out.push_back(previous);
                out.push_back(point);
            }
            previous = point;
        }
    }

    void AddPrimitiveLines(const physics::CollisionPrimitive& p, std::vector<Math::float3>& out)
    {
        constexpr float kPi = 3.14159265358979f;
        const Math::mat4 m = PrimitiveMatrix(p);
        const Math::float3 x(1.0f, 0.0f, 0.0f), y(0.0f, 1.0f, 0.0f), z(0.0f, 0.0f, 1.0f);
        if (p.kind == physics::PrimitiveKind::Box)
        {
            const Math::float3 h = p.halfExtents;
            for (int a = 0; a < 8; ++a)
            {
                const Math::float3 corner((a & 1) ? h.x : -h.x, (a & 2) ? h.y : -h.y, (a & 4) ? h.z : -h.z);
                for (int axis = 1; axis <= 4; axis <<= 1)
                {
                    if ((a & axis) != 0) { continue; }
                    const int other = a | axis;
                    out.push_back(m.TransformPoint(corner));
                    out.push_back(m.TransformPoint(Math::float3((other & 1) ? h.x : -h.x, (other & 2) ? h.y : -h.y, (other & 4) ? h.z : -h.z)));
                }
            }
            return;
        }
        const float r = p.radius;
        const Math::float3 origin(0.0f, 0.0f, 0.0f);
        if (p.kind == physics::PrimitiveKind::Sphere)
        {
            AddCircle(out, m, origin, x * r, y * r, 0.0f, 2.0f * kPi);
            AddCircle(out, m, origin, y * r, z * r, 0.0f, 2.0f * kPi);
            AddCircle(out, m, origin, z * r, x * r, 0.0f, 2.0f * kPi);
            return;
        }
        const Math::float3 top = y * p.halfHeight;
        const Math::float3 bottom = y * -p.halfHeight;
        AddCircle(out, m, top, x * r, z * r, 0.0f, 2.0f * kPi);
        AddCircle(out, m, bottom, x * r, z * r, 0.0f, 2.0f * kPi);
        if (p.kind == physics::PrimitiveKind::Capsule)
        {
            AddCircle(out, m, top, x * r, y * r, 0.0f, kPi);
            AddCircle(out, m, top, z * r, y * r, 0.0f, kPi);
            AddCircle(out, m, bottom, x * r, y * -r, 0.0f, kPi);
            AddCircle(out, m, bottom, z * r, y * -r, 0.0f, kPi);
        }
        for (const Math::float3& side : { x * r, x * -r, z * r, z * -r })
        {
            out.push_back(m.TransformPoint(top + side));
            out.push_back(m.TransformPoint(bottom + side));
        }
    }

    const char* PrimitiveLabel(physics::PrimitiveKind kind)
    {
        switch (kind)
        {
        case physics::PrimitiveKind::Sphere: return "Sphere";
        case physics::PrimitiveKind::Capsule: return "Capsule";
        case physics::PrimitiveKind::Cylinder: return "Cylinder";
        case physics::PrimitiveKind::Box: break;
        }
        return "Box";
    }

    // A primitive around these bounds. A capsule or a cylinder stands along their longest axis.
    physics::CollisionPrimitive FitTo(physics::PrimitiveKind kind, const physics::SolidBounds& bounds)
    {
        physics::CollisionPrimitive p;
        p.kind = kind;
        p.center = Math::float3(0.5f * (bounds.min.x + bounds.max.x), 0.5f * (bounds.min.y + bounds.max.y), 0.5f * (bounds.min.z + bounds.max.z));
        const Math::float3 half(0.5f * (bounds.max.x - bounds.min.x), 0.5f * (bounds.max.y - bounds.min.y), 0.5f * (bounds.max.z - bounds.min.z));
        p.halfExtents = Math::float3(std::max(half.x, kMinSize), std::max(half.y, kMinSize), std::max(half.z, kMinSize));
        p.radius = std::max({ half.x, half.y, half.z, kMinSize });
        if (kind == physics::PrimitiveKind::Capsule || kind == physics::PrimitiveKind::Cylinder)
        {
            float longest = half.y;
            float across = std::max(half.x, half.z);
            if (half.x >= half.y && half.x >= half.z)
            {
                longest = half.x;
                across = std::max(half.y, half.z);
                p.rotationDeg = Math::float3(0.0f, 0.0f, 90.0f);
            }
            else if (half.z >= half.y)
            {
                longest = half.z;
                across = std::max(half.x, half.y);
                p.rotationDeg = Math::float3(90.0f, 0.0f, 0.0f);
            }
            p.radius = std::max(across, kMinSize);
            p.halfHeight = std::max(kind == physics::PrimitiveKind::Capsule ? longest - p.radius : longest, kMinSize);
        }
        return p;
    }
}

void CollisionEditorTab::Reset()
{
    selected_ = -1;
    fitSlot_ = -1;
    previewKey_.clear();
    preview_ = physics::CollisionPreview{};
    lines_.clear();
    selectedLines_.clear();
    ++linesRevision_;
    gizmoWantsMouse_ = false;
}

void CollisionEditorTab::DrawPrimitives(physics::CollisionSettings& settings, const nlohmann::json& doc, const std::vector<std::string>& slots, bool& changed)
{
    ImGui::SeparatorText("Shapes");
    const auto add = [&](physics::PrimitiveKind kind, bool fit)
    {
        const std::string geometry = doc.value("geometry", std::string());
        std::vector<int> exclude = settings.excludeSlots;
        if (fitSlot_ >= 0)
        {
            exclude.clear();
            for (int s = 0; s < static_cast<int>(slots.size()); ++s)
            {
                if (s != fitSlot_) { exclude.push_back(s); }
            }
        }
        physics::SolidBounds bounds = physics::MeasureSolid(geometry, std::max(settings.lod, 0), exclude);
        if (!bounds.valid)
        {
            bounds.min = Math::float3(-0.5f, -0.5f, -0.5f);
            bounds.max = Math::float3(0.5f, 0.5f, 0.5f);
        }
        if (!fit)
        {
            // A new primitive is a quarter of the mesh's size, in its middle: visible, and clearly not the fit.
            const Math::float3 c(0.5f * (bounds.min.x + bounds.max.x), 0.5f * (bounds.min.y + bounds.max.y), 0.5f * (bounds.min.z + bounds.max.z));
            const float q = 0.125f * std::max({ bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y, bounds.max.z - bounds.min.z, 0.02f });
            bounds.min = Math::float3(c.x - q, c.y - q, c.z - q);
            bounds.max = Math::float3(c.x + q, c.y + q, c.z + q);
        }
        physics::CollisionPrimitive primitive = FitTo(kind, bounds);
        if (!fit && (kind == physics::PrimitiveKind::Capsule || kind == physics::PrimitiveKind::Cylinder))
        {
            primitive.rotationDeg = Math::float3(0.0f, 0.0f, 0.0f);
        }
        // A box fitted lies along the mesh, not on the mesh's axes: a door at an angle in its own space gets a box along its leaf.
        if (fit && kind == physics::PrimitiveKind::Box)
        {
            const physics::SolidBox box = physics::MeasureSolidBox(geometry, std::max(settings.lod, 0), exclude);
            if (box.valid)
            {
                primitive.center = box.centre;
                primitive.rotationDeg = Math::float3(0.0f, box.yawDeg, 0.0f);
                primitive.halfExtents = Math::float3(std::max(box.half.x, 0.005f), std::max(box.half.y, 0.005f), std::max(box.half.z, 0.005f));
            }
        }
        settings.primitives.push_back(primitive);
        selected_ = static_cast<int>(settings.primitives.size()) - 1;
        changed = true;
    };

    if (ImGui::Button("+ Box")) { add(physics::PrimitiveKind::Box, false); }
    ImGui::SameLine();
    if (ImGui::Button("+ Sphere")) { add(physics::PrimitiveKind::Sphere, false); }
    ImGui::SameLine();
    if (ImGui::Button("+ Capsule")) { add(physics::PrimitiveKind::Capsule, false); }
    ImGui::SameLine();
    if (ImGui::Button("+ Cylinder")) { add(physics::PrimitiveKind::Cylinder, false); }

    if (ImGui::Button("Fit box")) { add(physics::PrimitiveKind::Box, true); }
    ImGui::SameLine();
    if (ImGui::Button("Fit sphere")) { add(physics::PrimitiveKind::Sphere, true); }
    ImGui::SameLine();
    if (ImGui::Button("Fit capsule")) { add(physics::PrimitiveKind::Capsule, true); }
    ImGui::SameLine();
    if (ImGui::Button("Fit cylinder")) { add(physics::PrimitiveKind::Cylinder, true); }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    const std::string fitLabel = fitSlot_ >= 0 && fitSlot_ < static_cast<int>(slots.size()) ? slots[static_cast<size_t>(fitSlot_)] : std::string("every solid slot");
    if (ImGui::BeginCombo("##fitTo", fitLabel.c_str()))
    {
        if (ImGui::Selectable("every solid slot", fitSlot_ < 0)) { fitSlot_ = -1; }
        for (int s = 0; s < static_cast<int>(slots.size()); ++s)
        {
            ImGui::PushID(s);
            if (ImGui::Selectable(slots[static_cast<size_t>(s)].c_str(), fitSlot_ == s)) { fitSlot_ = s; }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("What Fit measures: the whole solid mesh, or one material slot (a palm's trunk, a boat's hull)."); }

    int removeAt = -1;
    for (int i = 0; i < static_cast<int>(settings.primitives.size()); ++i)
    {
        ImGui::PushID(i);
        const std::string label = std::to_string(i) + ": " + PrimitiveLabel(settings.primitives[static_cast<size_t>(i)].kind);
        if (ImGui::Selectable(label.c_str(), selected_ == i, 0, ImVec2(ImGui::GetContentRegionAvail().x - 80.0f, 0.0f))) { selected_ = i; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete")) { removeAt = i; }
        ImGui::PopID();
    }
    if (removeAt >= 0)
    {
        settings.primitives.erase(settings.primitives.begin() + removeAt);
        selected_ = std::min(selected_, static_cast<int>(settings.primitives.size()) - 1);
        changed = true;
    }
    if (settings.primitives.empty())
    {
        selected_ = -1;
        ImGui::TextDisabled("No shapes yet: copies of this mesh have no body.");
        return;
    }
    selected_ = std::clamp(selected_, 0, static_cast<int>(settings.primitives.size()) - 1); // the tab opens on a part with its gizmo

    physics::CollisionPrimitive& p = settings.primitives[static_cast<size_t>(selected_)];
    ImGui::SeparatorText(PrimitiveLabel(p.kind));
    int mode = static_cast<int>(gizmoMode_);
    ImGui::RadioButton("Move", &mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Rotate", &mode, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Size", &mode, 2);
    gizmoMode_ = static_cast<GizmoMode>(mode);
    changed = ImGui::DragFloat3("Centre (m)", &p.center.x, 0.01f, 0.0f, 0.0f, "%.3f") || changed;
    changed = ImGui::DragFloat3("Rotation (deg)", &p.rotationDeg.x, 0.5f, 0.0f, 0.0f, "%.1f") || changed;
    if (p.kind == physics::PrimitiveKind::Box)
    {
        changed = ImGui::DragFloat3("Half size (m)", &p.halfExtents.x, 0.005f, kMinSize, 1000.0f, "%.3f") || changed;
    }
    else
    {
        changed = ImGui::DragFloat("Radius (m)", &p.radius, 0.005f, kMinSize, 1000.0f, "%.3f") || changed;
        if (p.kind == physics::PrimitiveKind::Capsule || p.kind == physics::PrimitiveKind::Cylinder)
        {
            changed = ImGui::DragFloat("Half height (m)", &p.halfHeight, 0.005f, kMinSize, 1000.0f, "%.3f") || changed;
            if (p.kind == physics::PrimitiveKind::Capsule && ImGui::IsItemHovered()) { ImGui::SetTooltip("Of the straight part, along the capsule's own Y: the caps come on top of it."); }
        }
    }
}

void CollisionEditorTab::Draw(nlohmann::json& doc, const std::vector<std::string>& slots)
{
    physics::CollisionSettings settings;
    bool namedSlots = false;
    if (const auto block = doc.find("collision"); block != doc.end()) { physics::ReadCollisionSettings(*block, settings, &namedSlots); }
    // What the factory does for a block that names no slots: a leaf does not collide.
    std::vector<int> foliageSlots;
    if (const auto weights = doc.find("windFoliage"); weights != doc.end() && weights->is_array())
    {
        int slot = 0;
        for (const nlohmann::json& w : *weights)
        {
            if (w.is_number() && w.get<float>() > 0.0f) { foliageSlots.push_back(slot); }
            ++slot;
        }
    }
    if (!namedSlots) { settings.excludeSlots = foliageSlots; }
    bool changed = false;

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("For every placed copy. Whether a copy is static or falls is its own: Inspector > Physics.");
    ImGui::PopStyleColor();
    const char* const shapes[] = { "Auto", "Triangles", "Convex hull", "Box", "Sphere", "Capsule", "Custom shapes", "None" };
    int shape = static_cast<int>(settings.shape);
    if (ImGui::Combo("Shape", &shape, shapes, 8))
    {
        settings.shape = static_cast<physics::ShapeKind>(shape);
        changed = true;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Auto: a dynamic copy takes a convex hull. A static one takes a convex hull under 1 m and the triangles\n"
            "above it, of LOD 2 if the mesh has leaves (its LOD 0 trunk has thousands for no gain), of LOD 0 if not.\n"
            "Triangles on a dynamic copy still take the hull: two meshes cannot collide.\n"
            "Custom shapes: boxes, spheres and capsules placed by hand, UE's simple collision.");
    }

    if (settings.shape == physics::ShapeKind::Custom)
    {
        DrawPrimitives(settings, doc, slots, changed);
    }
    else
    {
        selected_ = -1;
        if (settings.shape != physics::ShapeKind::None)
        {
            int lod = settings.lod;
            if (ImGui::SliderInt("LOD", &lod, -1, 3, lod < 0 ? "automatic" : "%d"))
            {
                settings.lod = lod;
                changed = true;
            }
        }
        if (!slots.empty() && ImGui::TreeNode("Slots that collide"))
        {
            for (int slot = 0; slot < static_cast<int>(slots.size()); ++slot)
            {
                bool solid = std::find(settings.excludeSlots.begin(), settings.excludeSlots.end(), slot) == settings.excludeSlots.end();
                const std::string label = std::to_string(slot) + ": " + slots[static_cast<size_t>(slot)];
                if (ImGui::Checkbox(label.c_str(), &solid))
                {
                    if (solid) { settings.excludeSlots.erase(std::remove(settings.excludeSlots.begin(), settings.excludeSlots.end(), slot), settings.excludeSlots.end()); }
                    else { settings.excludeSlots.push_back(slot); }
                    std::sort(settings.excludeSlots.begin(), settings.excludeSlots.end());
                    namedSlots = settings.excludeSlots != foliageSlots;
                    changed = true;
                }
            }
            ImGui::TreePop();
        }
    }

    if (settings.shape != physics::ShapeKind::None)
    {
        ImGui::SeparatorText("Body");
        changed = ImGui::DragFloat("Density (kg/m3)", &settings.density, 10.0f, 1.0f, 20000.0f, "%.0f") || changed;
        changed = ImGui::DragFloat("Mass (kg)", &settings.mass, 0.5f, 0.0f, 100000.0f, settings.mass > 0.0f ? "%.1f" : "from density") || changed;
        changed = ImGui::SliderFloat("Friction", &settings.friction, 0.0f, 2.0f, "%.2f") || changed;
        changed = ImGui::SliderFloat("Restitution", &settings.restitution, 0.0f, 1.0f, "%.2f") || changed;
        changed = ImGui::DragFloat("Rolling drag", &settings.angularDamping, 0.05f, -1.0f, 20.0f, settings.angularDamping < 0.0f ? "default" : "%.2f") || changed;
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("How fast a dynamic copy stops turning (1/s). A round thing with little of it rolls down any slope for a long time."); }
        changed = ImGui::DragFloat("Rolling resistance", &settings.rollingResistance, 0.01f, -1.0f, 2.0f, settings.rollingResistance < 0.0f ? "default" : "%.2f") || changed;
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("The sine of the slope a dynamic copy still rests on. Jolt has no rolling friction: a sphere at 0 rolls down any slope into the sea."); }
        changed = ImGui::DragFloat("Float", &settings.floatFactor, 0.01f, 0.0f, 2.0f, "%.2f") || changed;
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("A dynamic copy on the ocean: 1 = neutral, less sinks, more floats."); }
        changed = ImGui::Checkbox("Two-sided", &settings.twoSided) || changed;
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("A triangle stops bodies on its front only. A sheet of cloth met from both sides wants this."); }
    }

    if (changed)
    {
        nlohmann::json block = physics::WriteCollisionSettings(settings, namedSlots);
        if (block.empty()) { doc.erase("collision"); } // an untouched asset stays byte-identical
        else { doc["collision"] = std::move(block); }
    }

    ImGui::SeparatorText("Result");
    ImGui::Checkbox("Preview as a dynamic copy", &previewAsDynamic_);
    const std::string geometry = doc.value("geometry", std::string());
    // Only what moves the lines: a drag of Friction must not rebuild a room's triangles every frame.
    physics::CollisionSettings shapeOnly = settings;
    shapeOnly.density = physics::CollisionSettings{}.density;
    shapeOnly.mass = physics::CollisionSettings{}.mass;
    shapeOnly.friction = physics::CollisionSettings{}.friction;
    shapeOnly.restitution = physics::CollisionSettings{}.restitution;
    shapeOnly.angularDamping = physics::CollisionSettings{}.angularDamping;
    shapeOnly.rollingResistance = physics::CollisionSettings{}.rollingResistance;
    shapeOnly.floatFactor = physics::CollisionSettings{}.floatFactor;
    const std::string key = geometry + (previewAsDynamic_ ? "|d|" : "|s|") + physics::WriteCollisionSettings(shapeOnly, true).dump() + "|" + std::to_string(selected_);
    if (key != previewKey_)
    {
        previewKey_ = key;
        physics::PreviewCollision(geometry, settings, previewAsDynamic_, preview_);
        RebuildOverlay(settings);
    }
    if (preview_.kind == physics::ShapeKind::None || (preview_.shapeTriangles == 0 && preview_.error.empty() && preview_.kind != physics::ShapeKind::Custom))
    {
        ImGui::TextDisabled("Nothing solid: copies of this mesh have no body.");
    }
    else if (preview_.kind == physics::ShapeKind::Custom)
    {
        ImGui::Text("%d custom shapes, built in %.1f ms", static_cast<int>(settings.primitives.size()), preview_.buildMs);
    }
    else
    {
        ImGui::TextWrapped("%s, LOD %u of %u: %u solid triangles, %u in the shape, built in %.1f ms", physics::ShapeKindName(preview_.kind),
            preview_.lodUsed, preview_.lodCount, preview_.sourceTriangles, preview_.shapeTriangles, preview_.buildMs);
    }
    if (!preview_.error.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.3f, 1.0f));
        ImGui::TextWrapped("%s", preview_.error.c_str());
        ImGui::PopStyleColor();
    }
}

void CollisionEditorTab::RebuildOverlay(const physics::CollisionSettings& settings)
{
    lines_.clear();
    selectedLines_.clear();
    ++linesRevision_;
    if (preview_.kind != physics::ShapeKind::Custom)
    {
        lines_ = preview_.lines;
        return;
    }
    for (int i = 0; i < static_cast<int>(settings.primitives.size()); ++i)
    {
        AddPrimitiveLines(settings.primitives[static_cast<size_t>(i)], i == selected_ ? selectedLines_ : lines_);
    }
}

void CollisionEditorTab::DrawGizmo(nlohmann::json& doc, ImDrawList* drawList, const ImVec2& min, const ImVec2& max, const Math::mat4& view, const Math::mat4& proj)
{
    gizmoWantsMouse_ = false;
    if (selected_ < 0) { return; }
    physics::CollisionSettings settings;
    bool namedSlots = false;
    const auto block = doc.find("collision");
    if (block == doc.end() || !physics::ReadCollisionSettings(*block, settings, &namedSlots)) { return; }
    if (settings.shape != physics::ShapeKind::Custom || selected_ >= static_cast<int>(settings.primitives.size())) { return; }
    physics::CollisionPrimitive& p = settings.primitives[static_cast<size_t>(selected_)];

    const Math::float3 size = Size(p);
    const float translation[3] = { p.center.x, p.center.y, p.center.z };
    const float rotation[3] = { p.rotationDeg.x, p.rotationDeg.y, p.rotationDeg.z };
    const float scale[3] = { size.x, size.y, size.z };
    float matrix[16];
    ImGuizmo::RecomposeMatrixFromComponents(translation, rotation, scale, matrix);

    DirectX::XMFLOAT4X4 viewFloats, projFloats;
    DirectX::XMStoreFloat4x4(&viewFloats, view.xm());
    DirectX::XMStoreFloat4x4(&projFloats, proj.xm());
    const ImGuizmo::OPERATION operation = gizmoMode_ == GizmoMode::Rotate ? ImGuizmo::ROTATE : gizmoMode_ == GizmoMode::Size ? ImGuizmo::SCALE : ImGuizmo::TRANSLATE;

    ImGuizmo::PushID("CollisionEditorTab"); // the viewport's gizmo keeps its own state
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(drawList);
    ImGuizmo::SetRect(min.x, min.y, max.x - min.x, max.y - min.y);
    ImGuizmo::Enable(true);
    const bool moved = ImGuizmo::Manipulate(&viewFloats.m[0][0], &projFloats.m[0][0], operation, ImGuizmo::LOCAL, matrix);
    gizmoWantsMouse_ = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
    ImGuizmo::PopID();
    if (!moved) { return; }

    float newTranslation[3], newRotation[3], newScale[3];
    ImGuizmo::DecomposeMatrixToComponents(matrix, newTranslation, newRotation, newScale);
    p.center = Math::float3(newTranslation[0], newTranslation[1], newTranslation[2]);
    p.rotationDeg = Math::float3(newRotation[0], newRotation[1], newRotation[2]);
    if (operation == ImGuizmo::SCALE) { SetSize(p, Math::float3(newScale[0], newScale[1], newScale[2])); }
    doc["collision"] = physics::WriteCollisionSettings(settings, namedSlots);
}

#endif // WITH_EDITOR
