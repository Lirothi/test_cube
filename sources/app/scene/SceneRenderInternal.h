#pragma once
// INTERNAL to the SceneRenderer translation units. Nothing outside sources/app/scene/ may include
// this: it exists so the pass bodies can be split across several .cpp files (refactor plan R1,
// docs/scene_renderer_refactor_plan.md) while the helpers they all use keep ONE definition.
//
// Everything here was file-local to SceneRenderer.cpp before the split and moved verbatim. The
// only changes are structural: the anonymous namespace became the named `scene_internal` (so the
// same symbols are not silently duplicated per translation unit) and the free functions became
// `inline` (a header included by more than one TU would otherwise multiply-define them).

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <d3d12.h>

#include "app/camera/Camera.h"
#include "app/scene/SceneFrameData.h"
#include "app/scene/SceneRenderQueue.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/core/RenderPass.h"
#include "rendering/core/Renderer.h"
#include "rendering/renderables/GBufferRenderable.h"
#include "rendering/renderables/RenderableObjectBase.h"
#include "rendering/shadows/VirtualShadowMap.h"
#include "rendering/shadows/ShadowSettings.h"
#include "vfx/WindState.h"

namespace scene_internal
{
#if WITH_EDITOR
    constexpr UINT kSelectionStencilBit = 0x80u;
    constexpr uint32_t kSelectionStencilGBufferLocalOrder = 0xfffffffeu;
    constexpr uint32_t kSelectionStencilTransparentLocalOrder = 0xfffffffeu;
#endif

    constexpr size_t BucketIndex(SceneRenderQueue::BucketType type)
    {
        return static_cast<size_t>(type);
    }

#if WITH_EDITOR
    inline bool IsSelectedEditorObject(const SceneFrameData& frame, std::uint64_t id)
    {
        if (id == 0)
        {
            return false;
        }
        for (std::uint32_t i = 0; i < frame.selectedEditorObjectCount; ++i)
        {
            if (frame.selectedEditorObjectIds[i] == id)
            {
                return true;
            }
        }
        return false;
    }

    inline bool ShouldRenderSelectionStencil(const SceneFrameData& frame,
        const SceneView& view,
        const RenderableObjectBase& object,
        bool transparent)
    {
        if (!IsSelectedEditorObject(frame, object.GetEditorObjectId()) ||
            !object.IsVisible() || object.IsTransparent() != transparent)
        {
            return false;
        }

        if ((object.GetRenderLayerMask() & view.renderLayerMask) == 0)
        {
            return false;
        }

        const AABB& bounds = object.GetWorldBounds();
        if (view.frustum.IsValid() && bounds.IsValid() && !view.frustum.Intersects(bounds))
        {
            return false;
        }
        return true;
    }
#endif

    inline void FilterShadowCasters(SceneRenderQueue& queue)
    {
        auto filterBucket = [](SceneRenderQueue::ObjectBucket& bucket)
        {
            bucket.erase(std::remove_if(bucket.begin(), bucket.end(), [](RenderableObjectBase* obj)
            {
                return !obj || !obj->CastsShadow();
            }), bucket.end());
        };

        filterBucket(queue.GetBucket(SceneRenderQueue::BucketType::OpaqueSimple));
        filterBucket(queue.GetBucket(SceneRenderQueue::BucketType::OpaqueComplex));
        filterBucket(queue.GetBucket(SceneRenderQueue::BucketType::TransparentSimple));
        filterBucket(queue.GetBucket(SceneRenderQueue::BucketType::TransparentComplex));
    }

    // ---- Step 2: shared per-view/per-frame constant buffers (b1) ----
    // Filled once per pass and bound for every object in that pass, replacing the
    // old per-object duplication of view/light/cascade data.

    // Matches gbuffer_common.hlsli `cbuffer PerView : register(b1)`. The depth-only
    // shadow shaders consume only viewProj (the other two are left identity/unused).
    struct PerViewCB
    {
        mat4 viewProj;
        mat4 viewProjNoJitter;
        mat4 prevViewProjNoJitter;
        // W3: global wind, consumed by the gbuffer VS (W4) and — since W5 — by the shadow VS too
        // (shadow_indirect_csm.hlsl declares the same tail at offset 192). Layout matches the HLSL
        // `cbuffer PerView` in gbuffer_common.hlsli: time, prevTime, float2 windDirXZ, then
        // swayAmp, swayFreq, gustMul, prevGustMul.
        float windTime = 0.0f;
        float windPrevTime = 0.0f;
        float windDirX = 1.0f;
        float windDirZ = 0.0f;
        float windSwayAmp = 0.0f;
        float windSwayFreq = 0.0f;
        float windGustMul = 1.0f;
        float windPrevGustMul = 1.0f;
        // S6: the shadow-depth pass's bias parameters, at bytes 224..239. Zero on every path but
        // Pass_CSM -- ApplyShadowDepthBias early-outs on all-zero, so this tail costs the gbuffer
        // and the VSM paths nothing.
        //
        // NOTE, deliberately NOT at 240 as the plan drafted: 224 is occupied only in the VSM PAGE
        // slot (gWindFade), and the VSM_PAGE shader permutation declares no `cbuffer PerView` at
        // all -- it reads its per-view data out of that slot instead. So this CB and that slot are
        // two independent layouts, and there is nothing here to keep in step with byte 224 there.
        float shadowConstBias = 0.0f;   // 224
        float shadowSlopeBias = 0.0f;   // 228
        float shadowMaxSlope  = 0.0f;   // 232
        float shadowClampNear = 0.0f;   // 236 (S7 pancaking; plumbed here, still unused)
    };
    static_assert(sizeof(PerViewCB) == 240, "PerViewCB must match the gbuffer/shadow HLSL layout");

    // Matches glass.hlsl `cbuffer GlassView : register(b1)`.
    struct GlassViewCB
    {
        mat4   view;
        mat4   proj;
        mat4   viewProj;
        mat4   viewProjNoJitter;
        mat4   prevViewProjNoJitter;
        mat4   invView;
        mat4   invProj;
        float4 camPosSky;
        float4 sunDirAmbient;
        float4 sunColorExposure;
        float4 camDirWS;
        float4 screenSizeInv;
        float4 shadowAtlasSizeInv;
        float4 shadowBiasNDC;
        float4 cascadeTexelWS;
        float4 cascadeSplitsVS;
        float4 cascadeScaleBias[4];
        float4 spotShadowInfo;
        float4 lightCounts;
        mat4   lightViewProj[4];
        float4 vsmParams;             // x = useVsm, y = refDist, z = depth-bias floor, w = clip blend width
        float4 clipmapParams;         // Step 24f: x = baseExtent, y = normalBias (texels), z = depthBias (NDC)
        mat4   clipmapViewProj[8];    // Step 24f: directional clipmap level viewProjs
        mat4   clipmapUvNormal;       // P16.16: receiver-plane transform, mirrors lighting_cs
        float4 preExposureParams;     // P16.1: x = pre-exposure, yzw reserved
        float4 csmFilterMode;         // S8: x = kernel, y = receiver bias, z = sharpen, w = over-blur
        float4 csmFilterParams;       // w = receiver normal bias in texels
    };

    // MIRRORS the OceanReflectionCB in shaders/ocean_reflection_cs.hlsl. This one is uploaded by
    // raw memcpy rather than through named field handles, so the offsets have to agree: every
    // block below is laid out to land on the same 16-byte rows HLSL packs it into.
    struct OceanReflectionConstants
    {
        mat4 view{};
        mat4 proj{};
        mat4 invView{};
        mat4 invProj{};
        float depthA = 0.0f;
        float depthB = 0.0f;
        float2 screenSize{};
        float2 invScreenSize{};
        float2 outputSize{};
        float3 camPosWS{};
        float waterHeight = 0.0f;
        // P13: the ocean plane traces with the same search the deferred SSR pass uses.
        uint32_t technique = 0u;
        uint32_t useHzb = 0u;
        uint32_t hzbMipCount = 1u;
        uint32_t frameIndexMod8 = 0u;
        float2 hzbSize{};
        float2 hzbInvSize{};
        // Retired by the byte-for-byte UE port (hardcoded 1.0/4.0; the confirm/refine
        // guard was never UE's). Pads keep the memcpy'd layout at 368 bytes.
        float _ueRetired0 = 0.0f;
        float _ueRetired1 = 0.0f;
        uint32_t _ueRetired2 = 0u;
        uint32_t _ueRetired3 = 0u;
        uint32_t ueNumSteps = 8u;
        uint32_t pad0 = 0u;
        uint32_t pad1 = 0u;
        uint32_t pad2 = 0u;
    };

    // A memcpy'd constant buffer has no field names to bind by, so nothing but this catches a
    // member added on one side only. 368 = 256 matrices + 48 scalars/vectors + 64 of P13 block.
    static_assert(sizeof(OceanReflectionConstants) == 368,
        "OceanReflectionConstants must stay byte-identical to OceanReflectionCB in "
        "shaders/ocean_reflection_cs.hlsl -- it is uploaded by raw memcpy.");

    template <typename T>
    D3D12_GPU_VIRTUAL_ADDRESS UploadFrameCB(Renderer* renderer, const T& data)
    {
        constexpr UINT kAlign = render::kConstantBufferAlignment;
        const UINT sizeBytes = static_cast<UINT>((sizeof(T) + (kAlign - 1)) & ~(kAlign - 1));
        auto alloc = renderer->GetFrameResource()->AllocDynamic(sizeBytes, kAlign);
        std::memcpy(alloc.cpu, &data, sizeof(T));
        return alloc.gpu;
    }

    // W3/W5: the ONE place the wind tail of PerView is filled. The gbuffer and the shadow views
    // must carry identical wind values, or the shadow bends out of step with the tree it belongs to.
    inline void ApplyWind(PerViewCB& vc, const vfx::WindState* wind)
    {
        if (!wind) { return; } // no wind state -> defaults (swayAmp 0 => WindOffset returns 0)
        vc.windTime = wind->time;
        vc.windPrevTime = wind->prevTime;
        vc.windDirX = wind->windDirXZ.x;
        vc.windDirZ = wind->windDirXZ.y;
        vc.windSwayAmp = wind->swayAmplitude;
        vc.windSwayFreq = wind->swayFrequency;
        vc.windGustMul = wind->gustMul;
        vc.windPrevGustMul = wind->prevGustMul;
    }

    inline D3D12_GPU_VIRTUAL_ADDRESS BuildGBufferViewCB(Renderer* renderer, const Camera& camera,
                                                        const vfx::WindState* wind)
    {
        PerViewCB vc{};
        vc.viewProj = camera.GetViewProjMatrix();
        vc.viewProjNoJitter = camera.GetViewProjMatrixNoJitter();
        vc.prevViewProjNoJitter = camera.GetPrevViewProjMatrixNoJitter();
        ApplyWind(vc, wind); // W3: W4's VS reads it for the sway + the prev-position motion vectors
        return UploadFrameCB(renderer, vc);
    }

    // S6: `constBias`/`slopeBias` are NDC, `maxSlope` is a tangent, `clampNear` is a flag. They
    // default to zero so only Pass_CSM has to pass anything -- shore depth, spot and point all get
    // the early-out. Spot/point are excluded on purpose: their bias lives in vsm_sample.hlsli's
    // local-light path (see [[vsm-local-shadow-bias]]) and mixing the two would double it.
    inline D3D12_GPU_VIRTUAL_ADDRESS BuildShadowViewCB(Renderer* renderer, const mat4& lightView, const mat4& lightProj,
                                                       const vfx::WindState* wind,
                                                       float constBias = 0.0f, float slopeBias = 0.0f,
                                                       float maxSlope = 0.0f, float clampNear = 0.0f)
    {
        PerViewCB vc{};
        vc.viewProj = lightView * lightProj; // viewProjNoJitter/prevViewProjNoJitter unused by shadow shaders
        ApplyWind(vc, wind); // W5: the shadow VS sways casters with the SAME params as the gbuffer
        vc.shadowConstBias = constBias;
        vc.shadowSlopeBias = slopeBias;
        vc.shadowMaxSlope  = maxSlope;
        vc.shadowClampNear = clampNear;
        return UploadFrameCB(renderer, vc);
    }

    inline D3D12_GPU_VIRTUAL_ADDRESS BuildGlassViewCB(Renderer* renderer, const Camera& camera, const SceneFrameData& frame,
                                                      bool glassReflActive)
    {
        GlassViewCB vc{};
        vc.view = camera.GetViewMatrix();
        vc.proj = camera.GetProjMatrix();
        vc.viewProj = camera.GetViewProjMatrix();
        vc.viewProjNoJitter = camera.GetViewProjMatrixNoJitter();
        vc.prevViewProjNoJitter = camera.GetPrevViewProjMatrixNoJitter();
        vc.invView = camera.GetInvViewMatrix();
        vc.invProj = camera.GetInvProjMatrix();

        float skyIntensity = 1.0f;
        if (frame.skybox) { skyIntensity = frame.skybox->GetExposure(); }
        vc.camPosSky = float4(camera.GetPosition(), skyIntensity);

        if (frame.dirLight)
        {
            const DirectionalLight& dirLight = *frame.dirLight;
            // P4: the ocean and glass paths compute `sunColor.xyz * sunColor.w`, so handing them
            // the effective colour with a retired 1.0 multiplier is the same product they had.
            // The fill reaches only `LitFoamColor` in the ocean shaders and the glass tint -- the
            // water surface itself has no sky fill at all. See the note in DirectionalLight.h.
            vc.sunDirAmbient = float4(dirLight.GetDirection(), dirLight.GetAmbient());
            vc.sunColorExposure = float4(dirLight.GetEffectiveColor(), dirLight.GetExposure());
        }

        float3 camDir = camera.GetDirection();
        if (camDir.Length() > Math::EPS) { camDir = camDir.Normalized(); }
        else { camDir = float3(0.0f, 0.0f, 1.0f); }
        // P5: w carries the prefiltered sky's mip count so glass can use the shared
        // roughness <-> mip mapping. 0 means this sky has no derivatives and glass keeps its
        // old guess. The slot was documented as unused.
        vc.camDirWS = float4(camDir,
            (frame.skybox && frame.skybox->HasIbl()) ? (float)frame.skybox->GetSpecMips() : 0.0f);

        const float width = static_cast<float>(std::max(renderer->GetRenderWidth(), 1u));
        const float height = static_cast<float>(std::max(renderer->GetRenderHeight(), 1u));
        vc.screenSizeInv = float4(width, height, width > 0.0f ? 1.0f / width : 0.0f, height > 0.0f ? 1.0f / height : 0.0f);

        const auto& deferred = renderer->GetDeferredForFrame();
        const float shadowRes = static_cast<float>(std::max(deferred.shadowRes, 1u));
        const float invShadow = shadowRes > 0.0f ? 1.0f / shadowRes : 0.0f;
        vc.shadowAtlasSizeInv = float4(shadowRes, shadowRes, invShadow, invShadow);

        const SceneFrameData::CascadeData& cascades = frame.cascades;
        vc.shadowBiasNDC = float4(cascades.depthBiasNDC[0], cascades.depthBiasNDC[1], cascades.depthBiasNDC[2], cascades.depthBiasNDC[3]);
        vc.cascadeTexelWS = float4(cascades.cascadeTexelWS[0], cascades.cascadeTexelWS[1], cascades.cascadeTexelWS[2], cascades.cascadeTexelWS[3]);
        vc.cascadeSplitsVS = float4(cascades.splitsVS[0], cascades.splitsVS[1], cascades.splitsVS[2], cascades.splitsVS[3]);
        for (int i = 0; i < 4; ++i)
        {
            vc.cascadeScaleBias[i] = float4(cascades.atlasScale[i].x, cascades.atlasScale[i].y, cascades.atlasBias[i].x, cascades.atlasBias[i].y);
            vc.lightViewProj[i] = cascades.lightView[i] * cascades.lightProj[i];
        }

        const float spotRes = static_cast<float>(std::max(deferred.spotShadowRes, 1u));
        const float invSpot = spotRes > 0.0f ? 1.0f / spotRes : 0.0f;
        vc.spotShadowInfo = float4(spotRes, spotRes, invSpot, invSpot);

        const float pointCount = frame.lightManager ? static_cast<float>(frame.lightManager->PointLights().size()) : 0.0f;
        const float spotCount = frame.lightManager ? static_cast<float>(frame.lightManager->GetSpotLightCount()) : 0.0f;
        // z = traced glass reflections active (SSR/RT); w = skybox surface
        // reflection enabled (SkyOnly/SSR/RT, but not None).
        const bool skySpecularActive = frame.settings.reflectionSource != ReflectionSource::None;
        vc.lightCounts = float4(pointCount, spotCount,
            glassReflActive ? 1.0f : 0.0f,
            skySpecularActive ? 1.0f : 0.0f);

        // Step 21: VSM sampling for glass — on when the gate is on and the pool is allocated.
        const bool vsmOn = render::VsmActive() && frame.vsm && frame.vsm->IsAllocated();
        // .z = the depth-bias floor, texels -> NDC (same conversion as the lighting CB: a level's
        // depth range is 6x its extent, virtual res 2048, both scale with the extent).
        vc.vsmParams = float4(vsmOn ? 1.0f : 0.0f, vsm::g_refDist,
                              vsm::g_clipmapDepthBiasFloorTexels / (6.0f * (float)vsm::kVirtualRes),
                              vsm::ClipmapBlendWidth());
        // Step 24f: directional clipmap for glass (matches lighting_cs). Same tunables + level viewProjs.
        // .y carries the SAME scaled value the lighting CB gets (UE divide by 1000 on the CPU);
        // .w = the per-level depth-bias decay (see VsmClipmapShadow).
        vc.clipmapParams = float4(vsm::g_clipmapBaseExtent, vsm::g_clipmapNormalBias * 0.001f,
                                  vsm::g_clipmapDepthBias, vsm::g_clipmapDepthBiasDecay);
        if (frame.clipmapViews)
        {
            for (size_t i = 0; i < 8 && i < frame.clipmapViews->size(); ++i)
            {
                const SceneView& cv = (*frame.clipmapViews)[i];
                vc.clipmapViewProj[i] = cv.view * cv.proj;
            }
            // P16.16: built from level 0; the gradient it feeds is a ratio in which the level's
            // extent cancels, so one matrix serves them all. Same construction as UE's
            // CalcTranslatedWorldToShadowUVNormalMatrix.
            vc.clipmapUvNormal = vsm::CalcClipmapUvNormalMatrix(vc.clipmapViewProj[0]);
        }

        // P16.1: glass draws in the transparent pass, after compose, so compose's scaling never
        // reaches it and it applies the factor itself -- and its refraction tap reads the opaque
        // scene copy, which already has the factor on it. Both halves live in glass.hlsl.
        vc.preExposureParams = float4(render::g_preExposure, 0.0f, 0.0f, 0.0f);
        // S8: glass must filter cascades exactly like the geometry beside it (that is what S3 was for).
        const CascadeShadowConfig* csmCfg = frame.cascadeConfig;
        vc.csmFilterMode = float4(static_cast<float>(csmCfg ? csmCfg->filterMode : 1u),
                                  csmCfg ? csmCfg->csmReceiverBias : 0.9f,
                                  csmCfg ? (csmCfg->shadowFilterSharpen * 7.0f + 1.0f) : 1.0f,
                                  (csmCfg && !csmCfg->pcfOverBlurCorrection) ? 0.0f : 1.0f);
        // S10 rides the three spare slots of glass's existing float4 (see glass.hlsl).
        vc.csmFilterParams = float4(cascades.splitsVS[SceneFrameData::kCascades],
                                    csmCfg ? csmCfg->blendFraction : 0.1f,
                                    csmCfg ? csmCfg->distanceFadeFraction : 0.1f,
                                    csmCfg ? csmCfg->normalBiasInTexels : 1.0f);

        return UploadFrameCB(renderer, vc);
    }
}
