#include "rendering/renderables/GlassPassSrvs.h"

#include <algorithm>

#include "rendering/core/Renderer.h"
#include "rendering/lighting/LightManager.h"
#include "rendering/lighting/Skybox.h"

namespace render
{
bool GatherGlassPassSrvs(Renderer* renderer, LightManager& lights, Skybox* sky, GlassPassSrvs& out)
{
    out = {};
    if (!renderer) { return false; }
    const auto& deferred = renderer->GetDeferredForFrame();

    const size_t pointCount = lights.PointLights().size();
    const size_t spotCount = lights.GetSpotLightCount();
    if (!lights.EnsurePointLightBuffer(renderer, std::max<size_t>(pointCount, size_t(1))) ||
        !lights.EnsureSpotLightBuffer(renderer, std::max<size_t>(spotCount, size_t(1))))
    {
        return false;
    }

    // Defensive: the deferred shadow SRVs staged below must be valid (see the
    // SceneRenderer Pass_SpotLights note); skip the glass draws for the frame if not.
    if (deferred.shadowSRV.ptr == 0 || deferred.spotShadowSRV.ptr == 0 ||
        deferred.pointShadowSRV.ptr == 0)
    {
        return false;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE sceneColorSrv =
        deferred.sceneOpaqueSRV.ptr != 0 ? deferred.sceneOpaqueSRV : deferred.sceneSRV;
    // t7 = the off-screen RT glass reflection (S15b). A normal texture on all HW — the glass
    // PS samples it only when rtEnabled (lightCounts.z in b1); fall back to scene color so the
    // slot is always a valid SRV (it's unread when RT is off / on non-RT HW).
    const D3D12_CPU_DESCRIPTOR_HANDLE glassReflSrv =
        deferred.glassReflectionSRV.ptr != 0 ? deferred.glassReflectionSRV : sceneColorSrv;

    // t9/t10 = VSM page table + pool (Step 21), set by Pass_Translucent (valid once a level is
    // loaded). Skip the draws if not set (the root sig binds them; a null staged SRV is invalid).
    if (renderer->GetVsmPageTableSrv().ptr == 0 || renderer->GetVsmPoolSrv().ptr == 0)
    {
        return false;
    }
    // P5 (t11): the GGX-prefiltered sky. Falls back to the display cube so the table is never
    // half-populated; the shader gates on camDirWS.w (the mip count), which is 0 without it.
    const D3D12_CPU_DESCRIPTOR_HANDLE skyDisplaySrv =
        sky ? sky->EnvironmentSrv() : deferred.sceneSRV;
    const D3D12_CPU_DESCRIPTOR_HANDLE skySpecSrv =
        (sky && sky->HasIbl() && sky->SpecularSrv().ptr != 0)
            ? sky->SpecularSrv()
            : skyDisplaySrv;

    out = {
        sceneColorSrv,
        // t1: the SUN atlas -- DEPTH normally, EVSM4 MOMENTS when S16 is on. glass.hlsl reads
        // whichever through csm_sample, which decides from the partition's exponents.
        renderer->GetSdsmMomentsSrv().ptr != 0 ? renderer->GetSdsmMomentsSrv() : deferred.shadowSRV,
        deferred.spotShadowSRV,
        skyDisplaySrv,
        lights.GetPointLightSrv(renderer->GetCurrentFrameIndex()),
        lights.GetSpotLightSrv(renderer->GetCurrentFrameIndex()),
        D3D12_CPU_DESCRIPTOR_HANDLE{},    // t6: the draw's normal map
        glassReflSrv,
        deferred.pointShadowSRV,          // t8: omnidirectional point shadow cube (B3)
        renderer->GetVsmPageTableSrv(),   // t9: VSM page table
        renderer->GetVsmPoolSrv(),        // t10: VSM pool
        skySpecSrv,                       // t11: P5 prefiltered sky
        // t12: the volumetric fog's integrated volume (plan A5); Main_Transparent declares it
        // PS-readable every frame, the shader gates on fogVolumeParams.x.
        deferred.fogIntegratedSRV.ptr != 0 ? deferred.fogIntegratedSRV : sceneColorSrv,
        // t13: S15, this frame's SDSM partitions (the dummy buffer in Legacy and VSM -- the shader
        // gates on csmSdsmParams.x, which is 0 there).
        renderer->GetSdsmPartitionSrv().ptr != 0 ? renderer->GetSdsmPartitionSrv()
                                                 : renderer->VsmDummyBufferSrv(),
        // t14: plan S7, the local shadow cache pool (a light with a negative atlas page reads there); the spot atlas,
        // an array of the same kind, stands in while there is no pool.
        renderer->GetLocalShadowPoolSrv().ptr != 0 ? renderer->GetLocalShadowPoolSrv() : deferred.spotShadowSRV
    };
    return true;
}
} // namespace render
