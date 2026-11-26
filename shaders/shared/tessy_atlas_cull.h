// tessy_atlas_cull_cs.hlsl's AtlasCullCB -- the fields ONCE (architecture plan R5.3): HLSL's `cbuffer AtlasCullCB`
// (tessy_atlas_cull_cs.hlsl) and C++'s AtlasCullCB (ShadowGpuData::RecordTessyAtlasCull) spell TESSY_ATLAS_CULL_FIELDS. A
// field list, not a struct: HLSL reads a cbuffer as globals. Comments are /* */ (a // inside a continued line would
// swallow the next field). Include shared/shader_types.h first. New fields are APPENDED. STATIC_VIEW_ROWS: the uint4
// rows of a ShadowGpuData::ViewMask (HLSL (TC_SHADOW_VIEW_CEILING + 127u) / 128u, C++ (render::kShadowViewCeiling +
// 127u) / 128u).
#ifndef TC_SHADERS_SHARED_TESSY_ATLAS_CULL_H
#define TC_SHADERS_SHARED_TESSY_ATLAS_CULL_H

#define TESSY_ATLAS_CULL_FIELDS(STATIC_VIEW_ROWS) \
    uint gNumGroups;   /* virtual groups (a row's width) */                                                           \
    /* the row of local view 0 in the args (the main rows: kShadowViewCascades; S7's static rows and the camera: 0) */ \
    uint gFirstView;                                                                                                  \
    uint gNumViews;    /* local views (the camera: 1, or with kFlagByRun its G-buffer's PSO runs) */                  \
    /* 0 = the main rows, 1 = S7's static rows, 2 = the camera's row (the cluster view, N5's G-buffer), 3 = the SDSM */ \
    /* partitions' rows (tessy.sdsm: the sun, re-culled by Main_SdsmCull): its TessyAtlasViewInfo / args slot is */   \
    /* gRowSet * gViewStride + view, its claim cursor gCounterAt.x */                                                 \
    uint gRowSet;                                                                                                     \
    uint gViewStride;  /* render::kShadowViewCeiling */                                                               \
    uint gFrustumFirst; /* the frustum slot of local view 0 (render::kShadowViewCascades) */                          \
    uint gRecordBase;  /* this row set's part of TessyAtlasVisible: [gRecordBase, gRecordBase + gRecordCap) */        \
    uint gRecordCap;                                                                                                  \
    float4 gWind0;     /* the frame's wind, as the views' PerView carries it: (time, prevTime, dir.x, dir.z) */       \
    float4 gWind1;     /* (sway amplitude, frequency, gust, previous gust); a rigid light's view is padded anyway */  \
    uint gWindPad;     /* tessy.windPad */                                                                            \
    uint gFlags;       /* kFlag* (the kernel's) */                                                                    \
    uint gClassicCountOffset; /* kFlagByRun: where in TessyAtlasArgs each run's classic command count goes (bytes) */ \
    uint gCamDeferCap; /* N5's two-pass occlusion: TessyCamDeferred's capacity (entries); 0 = no pyramid test */      \
    /* N7d: the camera's DAG cut (the camera's runs only). xyz = the camera, w = pixels per unit at depth 1 / */      \
    /* lod.pixelError (/ lod.distanceScale); gDagNear floors the distance (a sphere around the camera takes its */    \
    /* leaves). */                                                                                                    \
    float4 gDagCam;                                                                                                   \
    uint gDagOn;                                                                                                      \
    float gDagNear;                                                                                                   \
    uint gDagBand; /* N7e: 1 = each caster tests the band of levels its distance can cut (tessy.lodBand), 0 = all */  \
    /* the frustum planes a box is tested against: a local view's 6; an SDSM partition's 16 (its cull volume and its */ \
    /* box, the tail accept-all -- shadow_cull_cs tests the same 16) */                                               \
    uint gPlanes;                                                                                                     \
    /* Where this row set counts in TessyAtlasCounter (bytes): x its claimed records, y its views that did not fit, */ \
    /* z its clusters tested, w its triangles written (+4: its DAG clusters), ~0 = not counted. Row sets 0-2 share */ \
    /* y / z. */                                                                                                      \
    uint4 gCounterAt;                                                                                                 \
    /* Groups a view (the SDSM partitions: ShadowGpuData::kTessySunSplit; the camera's G-buffer runs: */              \
    /* tessy.camSplit): group g takes view g / gSplit and writes its own run at slot g -- a partition its virtual */  \
    /* groups vg % gSplit == g % gSplit, a camera run the (g % gSplit)-th contiguous part of its commands (the order */ \
    /* the classic draw has). One group a view left the GPU idle: six camera runs kept six SMs busy for 0.25 ms of */ \
    /* DAG tests. 1 else. */                                                                                          \
    uint gSplit;                                                                                                      \
    float2 gRectSize;  /* kFlagRectCull on the SDSM partitions' rows: the partition's viewport (the tile's rect) */   \
    uint gRectCountAt; /* kFlagRectCull: where in TessyAtlasCounter the clusters it dropped are counted (bytes) */    \
    /* S7's rows (gRowSet 1): one bit per view slot, the views that draw their static rows this frame (a cache */     \
    /* miss, an uncached view). A hit copies them from the pool, so its rows are left as they are (nothing draws */   \
    /* them). */                                                                                                      \
    uint4 gStaticViews[STATIC_VIEW_ROWS];

#endif
