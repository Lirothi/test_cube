#include "rendering/descriptors/InputLayoutManager.h"

InputLayoutManager::InputLayoutManager() {
    InitBuiltins(); // initialize built-in layouts
}

void InputLayoutManager::Register(const std::string& name,
                                  const std::vector<D3D12_INPUT_ELEMENT_DESC>& elems) {
    Stored s;
    s.names.reserve(elems.size());
    s.descs.resize(elems.size());

    for (size_t i = 0; i < elems.size(); ++i) {
        // Copy the semantic string into the manager's storage
        s.names.emplace_back(elems[i].SemanticName ? elems[i].SemanticName : "");
        s.descs[i] = elems[i];
        s.descs[i].SemanticName = s.names.back().c_str(); // stable pointer
    }
    map_[name] = std::move(s);
}

void InputLayoutManager::Builder::Build(InputLayoutManager& mgr, const std::string& name) {
    std::vector<D3D12_INPUT_ELEMENT_DESC> v;
    v.reserve(items_.size());
    for (auto& it : items_) {
        D3D12_INPUT_ELEMENT_DESC d{};
        d.SemanticName         = it.semantic.c_str(); // temporary; the manager will rebind it
        d.SemanticIndex        = it.semanticIndex;
        d.Format               = it.format;
        d.InputSlot            = it.inputSlot;
        d.AlignedByteOffset    = it.aligned;
        d.InputSlotClass       = it.cls;
        d.InstanceDataStepRate = it.step;
        v.push_back(d);
    }
    mgr.Register(name, v);
}

InputLayoutManager::View InputLayoutManager::Get(const std::string& name) const {
    auto it = map_.find(name);
    if (it == map_.end() || it->second.descs.empty()) {
        return {};
    }
    return { it->second.descs.data(), (UINT)it->second.descs.size() };
}

void InputLayoutManager::InitBuiltins() {
    // pos(float3), color(float4)
    Builder()
        .Add("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0)
        .Add("COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0)
        .Build(*this, "PosColor");

    // pos, normal, tangent, uv, wind-bake color (W7.1: COLOR_0 at offset 48; the VS ignores it
    // until W7.3, so meshes with no bake render byte-identically).
    Builder()
        .Add("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0)
        .Add("NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0)
        .Add("TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0)
        .Add("TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0)
        .Add("COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 48)
        .Build(*this, "PosNormTanUV");

    // pos(float2), color, uv, shadow params
    Builder()
        .Add("POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0)
        .Add("COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0)
        .Add("TEXCOORD", 0, DXGI_FORMAT_R16G16_UNORM, 0)
        .Add("TEXCOORD", 1, DXGI_FORMAT_R16G16_FLOAT, 0)
        .Build(*this, "PosColorUV");

    // pos only
    Builder()
        .Add("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0)
        .Build(*this, "PosOnly");

    // pos (slot 0) + per-instance caster id (slot 1) — Rung 0 indirect shadow VS. Slot 1 is the
    // visible-list stream (uint caster id), stepped once per instance.
    // This layout is NOT format-agnostic any more, whatever an older comment here claimed: W7.1
    // put COLOR at offset 48, so it hard-requires VertexPNTUV. A new caster vertex format has to
    // be checked against this file or shadows break silently.
    Builder()
        .Add("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0)
        .Add("COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 48) // W7.1: wind bake; depth-only VS needs the same weights
        .Add("CASTERID", 0, DXGI_FORMAT_R32_UINT, 1, D3D12_APPEND_ALIGNED_ELEMENT,
             D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1)
        .Build(*this, "PosOnly_InstCasterId");

    // pos+uv (slot 0) + per-instance caster id (slot 1) — C2 masked indirect shadow VS. UV sits
    // at offset 40 of VertexPNTUV; every caster mesh MeshManager produces is PNTUV, and the
    // masked PSO is only selected when the caster set contains masked (glTF) groups.
    Builder()
        .Add("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0)
        .Add("TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40)
        .Add("COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 48) // W7.1: wind bake (masked shadow VS)
        .Add("CASTERID", 0, DXGI_FORMAT_R32_UINT, 1, D3D12_APPEND_ALIGNED_ELEMENT,
             D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1)
        .Build(*this, "PosUV_InstCasterId");

    // S6 twins of the two above, with the vertex NORMAL the slope-scaled shadow-depth bias needs
    // (offset 12 of VertexPNTUV). SEPARATE layouts on purpose: only the Legacy CSM / spot / point
    // atlas PSOs bind these, so the VSM page render keeps fetching position alone and this step
    // costs it nothing. Both layouts already require VertexPNTUV (UV@40, COLOR@48), so the added
    // NORMAL@12 is not a new assumption -- see the note over PosOnly_InstCasterId.
    Builder()
        .Add("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0)
        .Add("NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12)
        .Add("COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 48)
        .Add("CASTERID", 0, DXGI_FORMAT_R32_UINT, 1, D3D12_APPEND_ALIGNED_ELEMENT,
             D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1)
        .Build(*this, "PosNrm_InstCasterId");

    Builder()
        .Add("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0)
        .Add("NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12)
        .Add("TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40)
        .Add("COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 48)
        .Add("CASTERID", 0, DXGI_FORMAT_R32_UINT, 1, D3D12_APPEND_ALIGNED_ELEMENT,
             D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1)
        .Build(*this, "PosNrmUV_InstCasterId");

    // Occlusion plan S4: the FULL gbuffer vertex (PosNormTanUV above, offsets of VertexPNTUV) plus
    // the per-instance caster id in slot 1 -- gbuffer_indirect.hlsl's input.
    Builder()
        .Add("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0)
        .Add("NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12)
        .Add("TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24)
        .Add("TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40)
        .Add("COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 48)
        .Add("CASTERID", 0, DXGI_FORMAT_R32_UINT, 1, D3D12_APPEND_ALIGNED_ELEMENT,
             D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1)
        .Build(*this, "PosNormTanUV_InstCasterId");

    // pos+color (slot 0) + instance matrix 4x4 in slot 1 (TEXCOORD4..7)
    Builder()
        .Add("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,     0, 0)
        .Add("COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT,  0)
        .AddInstanceMatrix4x4("TEXCOORD", 4, 1) // per-instance
        .Build(*this, "PosColor_InstMat4x4");

    // pos.xy (-1..1 quads), pos.z = clip level index, uv
    Builder()
        .Add("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0)
        .Add("TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0)
        .Build(*this, "PosLevelUV");

    Builder()
        .Add("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0)
        .Add("POSITION", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0)
        .Add("TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0)
        .Add("COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0)
        .Build(*this, "AxisLine");
}
