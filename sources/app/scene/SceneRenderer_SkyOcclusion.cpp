// Plan S's fallback WITHOUT ray tracing (`skyVis.fallback`): the world drawn from above, reduced, and traced into the
// sky visibility's target. The map's resources and placement are SkyOcclusionMap; the shader sky_occlusion_cs.hlsl
// says what it computes; the builders are in SceneRenderer_Graph.cpp (Main_SkyOcclusionMap, Main_SkyOcclusionReduce in
// the prologue, Main_SkyVisFallback where the RT trace would be).
#include "app/scene/SceneRenderer.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "app/camera/Camera.h"
#include "app/scene/SkyOcclusionMap.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "materials/Material.h"
#include "rendering/core/ComputeDispatch.h"
#include "rendering/core/RenderGraph.h"
#include "rendering/core/Renderer.h"
#include "rendering/lighting/DirectionalLight.h"
#include "rendering/lighting/Skybox.h"
#include "rendering/meshes/Mesh.h"
#include "rendering/renderables/RenderableObject.h"
#include "vfx/WindState.h"

#include "app/scene/SceneRenderInternal.h"
using namespace scene_internal;

namespace
{
// Mirrors `cbuffer SkyOcclusionCB` in sky_occlusion_cs.hlsl (row-major; four matrices, then rows of 4). One block for the
// three entry points.
struct SkyOcclusionConstants
{
    Math::mat4 invView;
    Math::mat4 invProj;
    Math::mat4 topViewProj;
    Math::mat4 bottomViewProj;
    Math::float4 mapParams;
    Math::float4 heightParams;
    std::uint32_t sizes[4] = { 0, 0, 0, 0 };
    Math::float4 traceParams;
    Math::float4 fadeParams;
};
static_assert(sizeof(SkyOcclusionConstants) == 4 * 64 + 5 * 16, "SkyOcclusionConstants mirrors sky_occlusion_cs.hlsl");

// A caster draws into the map at a coarse LOD (its LOD 2, or its last): a 0.25 m texel seen from 100 m up has no use for
// a palm's LOD 0, and the map is redrawn as the camera crosses its snap steps. A GPU-instanced caster at LOD 0, the only
// LOD its shadow path draws (Pass_CSM's rule); the terrain at LOD 0, the ground every direction stops on.
UINT MapLod(const RenderableObjectBase* obj, bool terrain)
{
    if (terrain || !obj || obj->IsGpuInstancedCaster()) { return 0u; }
    const RenderableObject* ro = obj->AsRenderableObject();
    const Mesh* mesh = ro ? ro->GetMesh() : nullptr;
    return mesh ? mesh->ClampExplicitLod(2u) : 0u;
}

// The receiver's lift off its surface along its normal before it marches.
constexpr float kNormalLift = 0.1f;
} // namespace

void SceneRenderer::Pass_SkyOcclusionMap(Renderer* renderer, RenderGraphPassContext ctx, SkyOcclusionMap& map,
                                         std::uint32_t point)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSkyOcclusionMap);
        renderer->EmitPoint(t.cl, point); // the three depths -> DEPTH_WRITE
        const SkyOcclusionMap::Placement& p = map.Current();
        const float size = static_cast<float>(map.DepthSize());
        const D3D12_VIEWPORT vp{ 0.0f, 0.0f, size, size, 0.0f, 1.0f };
        const D3D12_RECT sc{ 0, 0, static_cast<LONG>(size), static_cast<LONG>(size) };
        const auto draw = [&](D3D12_CPU_DESCRIPTOR_HANDLE dsv, const SceneView& view, const Math::mat4& viewMatrix,
                              bool terrain) {
            t.cl->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
            t.cl->RSSetViewports(1, &vp);
            t.cl->RSSetScissorRects(1, &sc);
            t.cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildShadowViewCB(renderer, viewMatrix, p.proj, frame_->wind);
            const auto& visibleBuckets = view.queue.VisibleBuckets();
            for (auto bucket : { SceneRenderQueue::BucketType::OpaqueSimple, SceneRenderQueue::BucketType::OpaqueComplex })
            {
                for (auto* obj : visibleBuckets[BucketIndex(bucket)])
                {
                    if (obj)
                    {
                        obj->RenderShadow(renderer, t.cl, viewMatrix, p.proj, viewCB, MapLod(obj, terrain));
                    }
                }
            }
        };
        draw(map.TopDsv(), map.ObjectsView(), p.topView, false);
        draw(map.BottomDsv(), map.ObjectsView(), p.bottomView, false);
        draw(map.TerrainDsv(), map.TerrainView(), p.topView, true);
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_SkyOcclusionReduce(Renderer* renderer, RenderGraphPassContext ctx, const SkyOcclusionMap& map,
                                            std::uint32_t point, std::uint32_t pointFar, std::uint32_t pointRest)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSkyOcclusionReduce);
        renderer->EmitPoint(t.cl, point); // the depths readable, the near level -> UAV
        renderer->BindDescriptorHeaps(t.cl);
        const SkyOcclusionMap::Placement& p = map.Current();
        SkyOcclusionConstants c{};
        c.topViewProj = p.topView * p.proj;
        c.bottomViewProj = p.bottomView * p.proj;
        c.mapParams = SkyOcclusionMap::MapParams(p);
        c.heightParams = SkyOcclusionMap::HeightParams(p);
        c.sizes[2] = map.DepthSize();
        const auto writeCb = [&c](std::uint8_t* dest) { std::memcpy(dest, &c, sizeof(c)); };
        RecordComputeDispatch(renderer, t.cl, map.ReduceMaterial(), static_cast<UINT>(sizeof(c)), writeCb,
                              { map.TopSrv(), map.BottomSrv(), map.TerrainSrv() }, { map.NearUav() }, {},
                              map.NearSize(), map.NearSize());
        renderer->EmitPoint(t.cl, pointFar); // the near level readable, the far one -> UAV
        RecordComputeDispatch(renderer, t.cl, map.ReduceFarMaterial(), static_cast<UINT>(sizeof(c)), writeCb,
                              { map.NearSrv() }, { map.FarUav() }, {},
                              map.FarSize(), map.FarSize());
        renderer->EmitPoint(t.cl, pointRest); // the far level at rest
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_SkyVisFallback(Renderer* renderer, RenderGraphPassContext ctx, const Camera& camera,
                                        const SkyOcclusionMap& map, std::uint32_t point, std::uint32_t pointRest)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSkyVisFallback);
        renderer->EmitPoint(t.cl, point); // depth / GB1 / the levels readable, the target -> UAV
        renderer->BindDescriptorHeaps(t.cl);
        const auto& D = renderer->GetDeferredForFrame();
        const SceneRenderSettings& s = frame_->settings;
        const SkyOcclusionMap::Placement& p = map.Drawn();
        SkyOcclusionConstants c{};
        c.invView = camera.GetInvViewMatrix();
        c.invProj = camera.GetInvProjMatrix(); // jittered, as the depth it reads
        c.mapParams = SkyOcclusionMap::MapParams(p);
        c.heightParams = SkyOcclusionMap::HeightParams(p);
        // The RT trace's half resolution (Pass_SkyVis): the target and its consumers' bilateral upsample.
        c.sizes[0] = std::max(1u, (renderer->GetRenderWidth() + 1u) / 2u);
        c.sizes[1] = std::max(1u, (renderer->GetRenderHeight() + 1u) / 2u);
        c.sizes[2] = map.DepthSize();
        const float reach = std::max(s.skyVisMaxDistance, 0.5f);
        c.traceParams = Math::float4(reach, std::clamp(s.skyVisFallbackOpacity, 0.0f, 1.0f),
                                     std::max(s.skyVisFallbackStart, 0.0f), kNormalLift);
        c.fadeParams = SkyOcclusionMap::FadeParams(p, camera.GetPosition(), reach, s.skyVisFallbackFade);
        RecordComputeDispatch(renderer, t.cl, map.TraceMaterial(), static_cast<UINT>(sizeof(c)),
                              [&c](std::uint8_t* dest) { std::memcpy(dest, &c, sizeof(c)); },
                              { D.depthSRV, D.gbSRV[1], map.NearSrv(), map.FarSrv() }, { D.skyVisUAV }, {},
                              c.sizes[0], c.sizes[1]);
        renderer->EmitPoint(t.cl, pointRest); // the target back at rest for lighting and compose
    }
    ctx.EndCL(t);
}
