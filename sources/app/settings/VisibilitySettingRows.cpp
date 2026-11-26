#include "app/scene/Scene.h"
#include "app/settings/SettingContext.h"
#include "core/settings/SettingsRegistry.h"
#include "rendering/core/RasterSettings.h"
#include "rendering/core/RenderStats.h"
#include "rendering/core/VisibilityStats.h"
#include "rendering/shadows/ShadowSettings.h"
#include "rendering/visibility/OcclusionHistory.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// --set / --sweep / MCP set_settings names vis.*, gbuffer.*, raster.*, stats.*, textures.* (visibility, the G-buffer's
// draws, the samplers) (architecture plan R2.2). Each row is the branch App.cpp's ApplySweepValue had, its clamp
// included, and the reader ReadSweepValue had (R2.2 gave every simple name one first, so --settings-dump compared the
// move); a row without a reader had none.
namespace settings
{
namespace
{
const SettingDesc kVisibilityRows[] = {
    // The material samplers' anisotropy (S3; UE r.MaxAnisotropy): 1, 2, 4, 8 or 16, the next frame's.
    TC_SETTING("textures.anisotropy", Int, SettingPersistent | SettingScalable | SettingCostNextFrame,
               [](SettingContext&, float value) { render::g_textureAnisotropy_game = render::ClampAnisotropy(static_cast<std::uint32_t>(std::max(value, 1.0f))); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(static_cast<std::uint64_t>(render::g_textureAnisotropy_game)); return true; }),
    // Occlusion plan S1: per-chunk / per-GI-instance frustum mask below the object cull. 0 = rollback.
    TC_SETTING("vis.chunkMask", Bool, SettingPersistent,
               [](SettingContext&, float value) { render::g_visChunkMask_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_visChunkMask_game); return true; }),
    // Occlusion plan S3a: hardware occlusion queries with history. method 0 off / 1 queries /
    // 2 hzb (S3b); latency in frames (1 = UE desktop, a fence wait per frame); the UEngine
    // config values the plan documents as unverified.
    TC_SETTING("vis.method", Float, SettingPersistent,
               [](SettingContext&, float value) { vis::g_occlusion_game.method = std::clamp((int)value, 0, 2); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vis::g_occlusion_game.method); return true; }),
    TC_SETTING("vis.queryLatency", Float, SettingPersistent,
               [](SettingContext&, float value) { vis::g_occlusion_game.queryLatency = std::clamp((int)value, 1, (int)vis::kOcclusionBufferedFrames); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vis::g_occlusion_game.queryLatency); return true; }),
    TC_SETTING("vis.probablyVisibleTime", Float, SettingTransient,
               [](SettingContext&, float value) { vis::g_occlusion_game.probablyVisibleTime = std::max(0.0f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vis::g_occlusion_game.probablyVisibleTime); return true; }),
    TC_SETTING("vis.maxPixelsFraction", Float, SettingTransient,
               [](SettingContext&, float value) { vis::g_occlusion_game.maxPixelsFraction = std::clamp(value, 0.001f, 1.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vis::g_occlusion_game.maxPixelsFraction); return true; }),
    TC_SETTING("vis.cutAngle", Float, SettingTransient,
               [](SettingContext&, float value) { vis::g_occlusion_game.cutAngleDeg = std::max(0.0f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vis::g_occlusion_game.cutAngleDeg); return true; }),
    TC_SETTING("vis.cutDistance", Float, SettingTransient,
               [](SettingContext&, float value) { vis::g_occlusion_game.cutDistance = std::max(0.0f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vis::g_occlusion_game.cutDistance); return true; }),
    TC_SETTING("vis.neverTestDistance", Float, SettingTransient,
               [](SettingContext&, float value) { vis::g_occlusion_game.neverTestDistance = std::max(0.0f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(vis::g_occlusion_game.neverTestDistance); return true; }),
    // occlusion plan S6
    TC_SETTING("vis.indirectQueries", Bool, SettingPersistent,
               [](SettingContext&, float value) { vis::g_occlusion_game.indirectQueries = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vis::g_occlusion_game.indirectQueries); return true; }),
    // occlusion plan S4
    TC_SETTING("gbuffer.indirect", Bool, SettingPersistent,
               [](SettingContext&, float value) { render::g_indirectGBufferEnabled_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_indirectGBufferEnabled_game); return true; }),
    // texture streaming A6
    TC_SETTING("raster.bindless", Bool, SettingPersistent,
               [](SettingContext&, float value) { render::g_rasterBindless_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_rasterBindless_game); return true; }),
    // occlusion plan S5
    TC_SETTING("gbuffer.hzb", Bool, SettingPersistent,
               [](SettingContext&, float value) { render::g_gbufferHzbCullEnabled_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_gbufferHzbCullEnabled_game); return true; }),
    // GPU triangle statistics for the Frame tab: 0 off, 1 always, 2 while the tab is open.
    TC_SETTING("stats.pipeline", Int, SettingTransient,
               [](SettingContext&, float value) { render::g_pipelineStatsMode_game = std::clamp(static_cast<int>(value), 0, 2); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_pipelineStatsMode_game); return true; }),
};
TC_SETTINGS(kVisibilityRows);
}
}
