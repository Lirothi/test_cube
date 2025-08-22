#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <cstring>   // memcpy
#include <cassert>
#include <initializer_list>

// --- Типы полей ---
enum class CBFieldType {
    Float,         // float
    Float2,        // float2
    Float3,        // float3 (align 16)
    Float4,        // float4
    Matrix4x4      // float4x4
};

// --- Размеры и выравнивание ---
inline uint32_t GetCBFieldTypeSize(CBFieldType t) {
    switch (t) {
    case CBFieldType::Float:     return 4;
    case CBFieldType::Float2:    return 8;
    case CBFieldType::Float3:    return 12;
    case CBFieldType::Float4:    return 16;
    case CBFieldType::Matrix4x4: return 64;
    }
    return 0;
}

inline uint32_t GetCBFieldTypeAlignment(CBFieldType t) {
    switch (t) {
    case CBFieldType::Float:     return 4;
    case CBFieldType::Float2:    return 8;
    case CBFieldType::Float3:    return 16; // важно!
    case CBFieldType::Float4:    return 16;
    case CBFieldType::Matrix4x4: return 16;
    }
    return 16;
}

// --- Описание поля буфера ---
struct CBField {
    std::string name;
    CBFieldType type;
    uint32_t offset; // в байтах от начала буфера
};

// --- Layout буфера ---
class ConstantBufferLayout {
public:
	ConstantBufferLayout() = default;
    ConstantBufferLayout(std::initializer_list<std::pair<std::string, CBFieldType>> fields) {
        uint32_t offset = 0;
        for (const auto& f : fields) {
            uint32_t align = GetCBFieldTypeAlignment(f.second);
            if (offset % align != 0) offset = ((offset / align) + 1) * align;
            fields_.push_back({ f.first, f.second, offset });
            nameToIndex_[f.first] = fields_.size() - 1;
            offset += GetCBFieldTypeSize(f.second);
        }
        // D3D12 CBV: размер кратен 256
        size_ = ((offset + 255) & ~255);
    }

    uint32_t GetSize() const { return size_; }

    const CBField* GetField(const std::string& name) const {
        auto it = nameToIndex_.find(name);
        if (it != nameToIndex_.end())
            return &fields_[it->second];
        return nullptr;
    }

    const std::vector<CBField>& GetFields() const { return fields_; }

    // --- Лаконичный API: запись значения по имени поля ---
    template<typename T>
    bool SetField(const std::string& name, const T& value, uint8_t* data) const {
        const CBField* field = GetField(name);
        if (!field) return false;
        // sizeof(T) и GetCBFieldTypeSize могут не совпадать (например, XMMATRIX = 64, XMFLOAT4 = 16)
        static_assert(sizeof(T) <= 64, "T too big for CB"); // just sanity check
        memcpy(data + field->offset, &value, GetCBFieldTypeSize(field->type));
        return true;
    }

    // Для сырых массивов/данных:
    bool SetFieldRaw(const std::string& name, const void* src, size_t size, uint8_t* data) const {
        const CBField* field = GetField(name);
        if (!field) return false;
        memcpy(data + field->offset, src, std::min(size, size_t(GetCBFieldTypeSize(field->type))));
        return true;
    }

private:
    std::vector<CBField> fields_;
    std::unordered_map<std::string, size_t> nameToIndex_;
    uint32_t size_ = 0;
};

// --- Менеджер разметок ---
class ConstantBufferLayoutManager {
public:
    ConstantBufferLayoutManager() {
        layouts_.reserve(32);
        RegisterDefaults();
	}
    ConstantBufferLayout* RegisterLayout(const std::string& name, ConstantBufferLayout layout) {
        ConstantBufferLayout& entry = layouts_[name];
        entry = std::move(layout);
        return &entry;
    }

    const ConstantBufferLayout* GetLayout(const std::string& name) const {
        auto it = layouts_.find(name);
        if (it != layouts_.end()) return &it->second;
        return nullptr;
    }

    void RegisterDefaults() {
        RegisterLayout("MVP", { {"modelViewProj", CBFieldType::Matrix4x4} });
        RegisterLayout("VP", { {"viewProj", CBFieldType::Matrix4x4} });
        RegisterLayout("MVP_Axis", {
        {"modelViewProj", CBFieldType::Matrix4x4},
        {"viewportThickness",  CBFieldType::Float4}
        });
        RegisterLayout("GBufferPO", {
        { "world",     CBFieldType::Matrix4x4 },
        { "view",      CBFieldType::Matrix4x4 },
        { "proj",      CBFieldType::Matrix4x4 },
        { "baseColor", CBFieldType::Float4    },
        { "mr",        CBFieldType::Float2    },
        { "texFlags",  CBFieldType::Float2    },
        });
        RegisterLayout("LightingPF", {
	    { "sunDirWS",        CBFieldType::Float3 }, // куда светит (например (0,-1,0))
	    { "ambientIntensity",CBFieldType::Float  }, // 0..1
	    { "lightColor",      CBFieldType::Float3 }, // RGB
	    { "exposure",        CBFieldType::Float  }, // 1..2
	    { "cameraPosWS",     CBFieldType::Float3 },
	    { "_pad0",           CBFieldType::Float  }, // заполнитель (для выравнивания)
	    { "invView",         CBFieldType::Matrix4x4 },
	    { "invProj",         CBFieldType::Matrix4x4 },
        });
    }

private:
    std::unordered_map<std::string, ConstantBufferLayout> layouts_;
};