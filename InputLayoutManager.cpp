#include "InputLayoutManager.h"

InputLayoutManager::InputLayoutManager() {
    InitBuiltins(); // инициализируем стандартные лейауты
}

void InputLayoutManager::Register(const std::string& name,
                                  const std::vector<D3D12_INPUT_ELEMENT_DESC>& elems) {
    Stored s;
    s.names.reserve(elems.size());
    s.descs.resize(elems.size());

    for (size_t i = 0; i < elems.size(); ++i) {
        // Копируем строку семантики внутрь менеджера
        s.names.emplace_back(elems[i].SemanticName ? elems[i].SemanticName : "");
        s.descs[i] = elems[i];
        s.descs[i].SemanticName = s.names.back().c_str(); // стабильный указатель
    }
    map_[name] = std::move(s);
}

void InputLayoutManager::Builder::Build(InputLayoutManager& mgr, const std::string& name) {
    std::vector<D3D12_INPUT_ELEMENT_DESC> v;
    v.reserve(items_.size());
    for (auto& it : items_) {
        D3D12_INPUT_ELEMENT_DESC d{};
        d.SemanticName         = it.semantic.c_str(); // временно; менеджер перепривяжет
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

    // pos, normal, tangent, uv
    Builder()
        .Add("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0)
        .Add("NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0)
        .Add("TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0)
        .Add("TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0)
        .Build(*this, "PosNormTanUV");

    // pos, color, uv
    Builder()
        .Add("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0)
        .Add("COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0)
        .Add("TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0)
        .Build(*this, "PosColorUV");

    // pos only
    Builder()
        .Add("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0)
        .Build(*this, "PosOnly");

    // pos+color (slot 0) + instance matrix 4x4 в slot 1 (TEXCOORD4..7)
    Builder()
        .Add("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,     0, 0)
        .Add("COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT,  0)
        .AddInstanceMatrix4x4("TEXCOORD", 4, 1) // per-instance
        .Build(*this, "PosColor_InstMat4x4");

    Builder()
        .Add("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0)
        .Add("POSITION", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0)
        .Add("TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0)
        .Add("COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0)
        .Build(*this, "AxisLine");
}