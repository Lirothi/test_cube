#pragma once
// Architecture plan R4.4 (docs/architecture_review_plan.md, section 11.2, wave 3): what the direct (forward) draws --
// the glass panes and glass material slots, the water surface, the lit particles -- read of the frame, in one place.
// Their draws get the renderer and not the frame (RenderableObjectBase's virtuals), so the scene renderer's passes
// publish it here before they draw: Pass_Transparent / Pass_Translucent the shadow transports and the glass table, the
// graph's builder the particles' two inputs. Was five setter / getter pairs on Renderer and two globals of
// SceneFrameData.h (g_skyIrradianceSrv, g_particleDepthWaterSrv). Written and read on the render half only.
#include <d3d12.h>

#include <array>

#include "shaders/shared/glass_slots.h" // GLASS_PASS_SRV_COUNT: glass.hlsl's per-pass table

namespace render
{
struct DirectPassInputs
{
    // The sun's shadow for glass and the water. Null until a pass first publishes them; from then on a buffer or texture
    // slot holds the renderer's inert stand-in when its source is absent (Renderer::VsmDummy*Srv).
    D3D12_CPU_DESCRIPTOR_HANDLE vsmPageTable{};    // Step 21: the VSM page table (glass t9), or the dummy buffer
    D3D12_CPU_DESCRIPTOR_HANDLE vsmPool{};         // Step 21: the VSM page pool (glass t10), or the dummy texture
    D3D12_CPU_DESCRIPTOR_HANDLE sdsmPartitions{};  // S15: the SDSM partitions (glass t13), or the dummy buffer
    // S16: what the sun-atlas slot (glass t1) binds -- the EVSM moments under S16; null = the depth atlas.
    D3D12_CPU_DESCRIPTOR_HANDLE sdsmMoments{};
    // Plan S7, the direct read: the local shadow cache pool (glass t14); null = none, the draw binds its spot atlas.
    D3D12_CPU_DESCRIPTOR_HANDLE localShadowPool{};

    // glass.hlsl's per-pass table (rendering/renderables/GlassPassSrvs.h), gathered once by Pass_Translucent after the
    // transports above; Glass() null = not this frame (a shadow map was missing), the glass draws skip.
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, GLASS_PASS_SRV_COUNT> glass{};
    bool glassValid = false;
    const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, GLASS_PASS_SRV_COUNT>* Glass() const
    {
        return glassValid ? &glass : nullptr;
    }

    // The sky's diffuse irradiance cube this frame (Skybox::IrradianceSrv, E / pi: the same fill lighting_cs gives
    // opaque surfaces -- the sky by day, the moon's sky, the night sky and Ultra Dynamic Sky's layer by night) and its
    // scale (lighting_cs skyIrradianceScale), for forward draws without lighting constants of their own (lit particles:
    // the air's light on a drop). Null = none. Set by the scene renderer once a frame, before the forward passes.
    D3D12_CPU_DESCRIPTOR_HANDLE skyIrradiance{};
    float skyIrradianceScale = 0.0f;
    // The depth WITH the water in it (Main_Translucent copies it once Main_Transparent has drawn the water), for the
    // particles' soft fade: a spray drop falling into the sea fades out over its softFade instead of being cut by the
    // water's depth test. Null = none this frame (the particles keep the opaque depthCopy). Set by the Main_Translucent
    // builder, read by its draws. particles.softWater 0 = the opaque copy, the A/B (g_particleSoftWater).
    D3D12_CPU_DESCRIPTOR_HANDLE particleDepthWater{};
};
} // namespace render
