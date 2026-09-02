#include "rendering/core/TextureCreate.h"
#include "core/logging/Log.h"

#include <atomic>
#include <cstdio>
#include <wrl/client.h>

#include "rendering/core/BarrierTranslation.h"

namespace render {
namespace {
std::atomic<bool> gEnhancedTextureCreation{ false };
} // namespace

void SetEnhancedTextureCreation(bool enable)
{
    gEnhancedTextureCreation.store(enable, std::memory_order_relaxed);
}

bool UsingEnhancedTextureCreation()
{
    return gEnhancedTextureCreation.load(std::memory_order_relaxed);
}

HRESULT CreateCommittedTexture(ID3D12Device* device,
                               const D3D12_HEAP_PROPERTIES& heapProps,
                               D3D12_HEAP_FLAGS heapFlags,
                               const D3D12_RESOURCE_DESC& desc,
                               D3D12_RESOURCE_STATES initialState,
                               const D3D12_CLEAR_VALUE* clearValue,
                               ID3D12Resource** out)
{
    if (device == nullptr || out == nullptr) { return E_INVALIDARG; }

    if (gEnhancedTextureCreation.load(std::memory_order_relaxed)) {
        // QueryInterface per creation rather than caching: resource creation is not a hot path
        // (a few hundred calls over a whole run), and caching would need invalidating on device
        // loss. A device that cannot produce Device10 falls through to the legacy call below —
        // the capability flag alone is not proof the interface exists.
        Microsoft::WRL::ComPtr<ID3D12Device10> device10;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device10))) && device10) {
            // D3D12_RESOURCE_DESC1 is a superset of D3D12_RESOURCE_DESC, but the two are NOT
            // layout-compatible — copied field by field so that stays explicit. The extra member
            // is the sampler-feedback mip region, zero for everything this engine creates.
            D3D12_RESOURCE_DESC1 desc1{};
            desc1.Dimension = desc.Dimension;
            desc1.Alignment = desc.Alignment;
            desc1.Width = desc.Width;
            desc1.Height = desc.Height;
            desc1.DepthOrArraySize = desc.DepthOrArraySize;
            desc1.MipLevels = desc.MipLevels;
            desc1.Format = desc.Format;
            desc1.SampleDesc = desc.SampleDesc;
            desc1.Layout = desc.Layout;
            desc1.Flags = desc.Flags;
            desc1.SamplerFeedbackMipRegion = D3D12_MIP_REGION{};

            const barriers::Translated t =
                barriers::LegacyStateToBarrier(initialState, /*isBuffer=*/false);
            // COMMON, and anything whose resting state implies no texture layout, keeps
            // LAYOUT_COMMON; everything else is created directly in the layout its resting state
            // implies, which is the entire point of declaring it up front.
            const D3D12_BARRIER_LAYOUT layout =
                (t.layout == D3D12_BARRIER_LAYOUT_UNDEFINED) ? D3D12_BARRIER_LAYOUT_COMMON : t.layout;

            const HRESULT hr = device10->CreateCommittedResource3(
                &heapProps, heapFlags, &desc1, layout, clearValue,
                /*pProtectedSession=*/nullptr,
                /*NumCastableFormats=*/0, /*pCastableFormats=*/nullptr,
                IID_PPV_ARGS(out));
            if (SUCCEEDED(hr)) { return hr; }

            // The enhanced creation was refused. Say WHICH resource and WHY, then fall back to the
            // legacy call so the rest of the run still happens — this step's bar is "textures still
            // create", and a silent hard failure would tell us nothing about which layout the
            // runtime rejected. Logged per call: a refusal is rare and worth every line.
            {
                char msg[240];
                std::snprintf(msg, sizeof(msg),
                              "[texcreate] CreateCommittedResource3 FAILED hr=0x%08X layout=%d "
                              "fmt=%d flags=0x%X dim=%d state=0x%X -> falling back to legacy\n",
                              static_cast<unsigned>(hr), static_cast<int>(layout),
                              static_cast<int>(desc.Format), static_cast<unsigned>(desc.Flags),
                              static_cast<int>(desc.Dimension), static_cast<unsigned>(initialState));
                logging::WriteRaw(logging::LogLevel::Warning, logging::LogCategory::RenderRhi, msg);
            }
            if (*out) { (*out)->Release(); *out = nullptr; }
        }
    }

    // Legacy path — byte-for-byte what every call site did before this helper existed.
    return device->CreateCommittedResource(&heapProps, heapFlags, &desc, initialState, clearValue,
                                           IID_PPV_ARGS(out));
}

} // namespace render
