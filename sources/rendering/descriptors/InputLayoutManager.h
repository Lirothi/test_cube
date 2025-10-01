#pragma once
#include <d3d12.h>
#include <string>
#include <vector>
#include "third_party/robin_hood.h"
#include <cstdint>

class InputLayoutManager {
public:
    struct View {
        const D3D12_INPUT_ELEMENT_DESC* desc = nullptr;
        UINT count = 0;
        bool valid() const { return desc != nullptr && count > 0; }
    };

    InputLayoutManager();

    // Register a ready-made layout under the given name (overwrites when the name already exists).
    void Register(const std::string& name, const std::vector<D3D12_INPUT_ELEMENT_DESC>& elems);

    // Builder — convenient helper to assemble elements while keeping string storage alive.
    class Builder {
    public:
        Builder& Add(const char* semanticName,
                     UINT semanticIndex,
                     DXGI_FORMAT format,
                     UINT inputSlot,
                     UINT alignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT,
                     D3D12_INPUT_CLASSIFICATION slotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                     UINT instanceStepRate = 0) {
            items_.push_back({semanticName, semanticIndex, format, inputSlot,
                              alignedByteOffset, slotClass, instanceStepRate});
            return *this;
        }

        // Convenience helper: 4x4 matrix as four consecutive semantics (e.g., TEXCOORD4..7), per-instance
        Builder& AddInstanceMatrix4x4(const char* baseSemantic,
                                      UINT baseIndex,
                                      UINT inputSlot) {
            for (UINT r = 0; r < 4; ++r) {
                Add(baseSemantic, baseIndex + r, DXGI_FORMAT_R32G32B32A32_FLOAT,
                    inputSlot, D3D12_APPEND_ALIGNED_ELEMENT,
                    D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1);
            }
            return *this;
        }

        // Generate and register the layout
        void Build(InputLayoutManager& mgr, const std::string& name);

    private:
        struct Item {
            std::string semantic;
            UINT semanticIndex;
            DXGI_FORMAT format;
            UINT inputSlot;
            UINT aligned;
            D3D12_INPUT_CLASSIFICATION cls;
            UINT step;
        };
        std::vector<Item> items_;
    };

    // Fetch a layout by name
    View Get(const std::string& name) const;

private:
    void InitBuiltins();

    struct Stored {
        std::vector<std::string>   names; // keep .SemanticName buffers alive
        std::vector<D3D12_INPUT_ELEMENT_DESC> descs;
    };
    robin_hood::unordered_map<std::string, Stored> map_;
};