#pragma once
// Architecture plan R4.1 (docs/architecture_review_plan.md, section 11.2, wave 3): the renderer's services -- the
// managers that sit above the device and the frame (materials, meshes, samplers, input layouts, text, debug draw, the
// recording contexts). The renderer owns one and hands it out whole (Renderer::Services()); a consumer includes the
// header of the manager it uses, not Renderer.h's whole closure (R4.2).
//
// The members keep the order they had as Renderer members: they are constructed in this order and destroyed in the
// reverse one, after the ImGui layer and before the bindless heap, as before.
#include "rendering/descriptors/SamplerManager.h"
#include "materials/Material.h"
#include "rendering/descriptors/InputLayoutManager.h"
#include "rendering/meshes/MeshManager.h"
#include "text/FontManager.h"
#include "text/TextManager.h"
#include "materials/MaterialDataManager.h"
#include "rendering/core/RenderContextPool.h"
#include "rendering/debug/DebugDraw.h"

struct RenderServices
{
    SamplerManager samplers;
    MaterialManager materials;
    InputLayoutManager inputLayouts;
    MeshManager meshes;
    FontManager fonts;
    TextManager text;
    MaterialDataManager materialData;
    RenderContextPool contexts;
    DebugDrawSystem debugDraw;
};
