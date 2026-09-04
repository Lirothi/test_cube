#pragma once
// Occlusion plan S3a (docs/occlusion_culling_plan.md): hardware occlusion queries with history.
//
// Transcription of UE's FPrimitiveOcclusionHistory (ScenePrivate.h:108-266), the per-primitive
// decision tree of FGPUOcclusionPacket::ProcessPrimitive (SceneVisibility.cpp:2700-2935), the two
// query batchers (FOcclusionQueryBatcher, SceneOcclusion.cpp:465-534) and the reset rules
// (SceneVisibility.cpp:5367-5380). Names keep UE's where the thing is the same.
//
// The shape of a frame:
//   BeginFrame   main thread, after the camera's matrices are final and before the camera prepare
//                task: takes the sample counts of the frame that can be read (latency frames ago),
//                decides whether existing results are to be ignored (camera cut, big move, scene
//                set change, long idle), trims stale entries, opens an empty query plan.
//   Consider     camera prepare task, once per (primitive, sub) the frustum kept: reads that
//                entry's pending result into an occluded/definite verdict, decides whether to
//                issue a query this frame (grouped for last frame's occluded, stochastic re-test
//                for the definitely visible, individual for everything else), appends the box to
//                a batcher. Returns the verdict; the caller drops the primitive when occluded.
//   EndConsider  same task, after the last Consider: the batches are laid out in dispatch order
//                (grouped first, r.OcclusionQueryDispatchOrder=0) for the pass body to draw.
//   The pass (OcclusionQueries.h) draws the plan after the G-buffer and resolves the counts; the
//   results come back through BeginFrame `latency` frames later.
//
// Rules that are NOT optional (each is a UE rule with a reason the plan quotes):
//   new entry -> visible, not definite; no result -> inherit last frame (latency > 1) or the
//   probably-visible clock (latency 1); crossing the near plane -> visible, definite, no query;
//   camera cut / big move / long idle / scene set change -> every result ignored; sub-primitives
//   (terrain chunks) are never grouped and an object is occluded only when ALL its considered
//   chunks are; lastProvenVisibleTime advances only on visible && definite.
//
// Only the CAMERA consults this. Shadow views never do (UE: r.Shadow.OcclusionCullCascadedShadowMaps
// = 0, cascade 0 never, VSM never) -- Scene::ApplyOcclusion asserts the view type.
//
// S3b (plan): the same history under UE's r.HZBOcclusion=1 path. The producer is
// HzbOcclusionTester (one compute dispatch against the furthest HZB, vis_test_cs.hlsl) instead of
// the query heap; the plan's boxes are the test list (AddHzbBounds = FHZBOcclusionTester::AddBounds),
// the verdict comes back as one uint per box and is always definite (SceneVisibility.cpp:2727-2733),
// and every considered primitive is tested every frame (:2859-2862) -- no grouping, no stochastic
// re-test. Everything else (near-plane rule, reset rules, trim, latency slots) is shared.

#include <cstdint>
#include <vector>

#include "core/math/AABB.h"
#include "core/math/Math.h"
#include "rendering/core/RenderConstants.h"
#include "third_party/robin_hood.h"

class Camera;

namespace vis
{
// One region of the query heap per frame in flight; results of frame N are read at N + latency.
inline constexpr unsigned kOcclusionBufferedFrames = render::kFrameCount;
// Queries per frame -- a literal, the heap and the readback are sized from it. UE caps through
// GRHIMaximumInFlightQueries with a 10 %-progress throttle; ours is a hard ceiling with a
// `droppedQueries` count in the readout (a dropped primitive keeps last frame's state).
inline constexpr unsigned kMaxOcclusionQueries = 16384;
// S3b: boxes per frame of the HZB tester -- UE's SizeX * SizeY = 256 * 256. A literal: the box
// ring, the results buffer and the readback are sized from it.
inline constexpr unsigned kMaxHzbTests = 65536;
// UE FOcclusionQueryBatcher::OccludedPrimitiveQueryBatchSize.
inline constexpr unsigned kOccludedPrimitiveQueryBatchSize = 16;
// UE OCCLUSION_SLOP = 1.0 (centimetres); the engine's unit is the metre.
inline constexpr float kOcclusionSlop = 0.01f;
inline constexpr std::uint32_t kNoQuery = ~0u;

enum class OcclusionMethod : int { Off = 0, Queries = 1, Hzb = 2 /* S3b */ };

// `--set=vis.*` (App.cpp) and the Render tab. UE keeps the first four as UPROPERTY(config) of
// UEngine (Engine.h:1711-1725); the drop carries no Config/, so these are the documented
// defaults and UNVERIFIED against a shipping DefaultEngine.ini.
struct OcclusionSettings
{
    // Queries by default since 2026-09-04, as UE desktop: wall K=4 -5.5 % GPU / -5 % CPU frame,
    // open island K=4 within noise after the chunk re-test delta (docs/occlusion_culling_plan.md S3a).
    int   method = static_cast<int>(OcclusionMethod::Queries);
    int   queryLatency = static_cast<int>(kOcclusionBufferedFrames); // 1 = UE desktop (a fence wait per frame)
    float probablyVisibleTime = 8.0f;   // s, UEngine::PrimitiveProbablyVisibleTime
    float maxPixelsFraction = 0.1f;     // UEngine::MaxOcclusionPixelsFraction
    float cutAngleDeg = 45.0f;          // UEngine::CameraRotationThreshold
    float cutDistance = 100.0f;         // m, UEngine::CameraTranslationThreshold (10000 cm)
    float neverTestDistance = 0.0f;     // m, r.NeverOcclusionTestDistance
    int   framesNotTestedToExpand = 5;  // r.GFramesNotOcclusionTestedToExpandBBoxes
    int   framesToExpandNewlyTested = 2; // r.FramesToExpandNewlyOcclusionTestedBBoxes
    float expandNewlyTested = 0.0f;     // m, r.ExpandNewlyOcclusionTestedBBoxesAmount
    float expandAllTested = 0.0f;       // m, r.ExpandAllOcclusionTestedBBoxesAmount
};
inline OcclusionSettings g_occlusion;

// A primitive is a RenderableObjectBase; `sub` 0 is the object itself, 1 + chunk index a terrain
// chunk (UE: FPrimitiveOcclusionHistoryKey{PrimitiveId, CustomIndex}).
struct OcclusionKey
{
    const void* primitive = nullptr;
    std::uint32_t sub = 0;
    bool operator==(const OcclusionKey& o) const noexcept { return primitive == o.primitive && sub == o.sub; }
};
struct OcclusionKeyHash
{
    std::size_t operator()(const OcclusionKey& k) const noexcept
    {
        std::uint64_t h = reinterpret_cast<std::uintptr_t>(k.primitive);
        h ^= (static_cast<std::uint64_t>(k.sub) + 0x9E3779B97F4A7C15ull) + (h << 6) + (h >> 2);
        return static_cast<std::size_t>(h);
    }
};

struct OcclusionBox
{
    Math::float3 min;
    Math::float3 max;
};

// One BeginQuery / draw / EndQuery: `boxCount` boxes starting at `firstBox` of the plan.
struct OcclusionBatch
{
    std::uint32_t queryIndex = 0;
    std::uint32_t firstBox = 0;
    std::uint32_t boxCount = 0;
    bool grouped = false;
};

// What the camera prepare decided to ask the GPU this frame; consumed by Pass_OcclusionQueries
// (method Queries: `batches` over `boxes`) or Pass_VisTest (method Hzb: `boxes` alone, index =
// test index, `batches` empty).
struct OcclusionQueryPlan
{
    std::uint64_t frameNumber = 0;
    OcclusionMethod method = OcclusionMethod::Off; // which pass this plan is for
    Math::mat4 viewProj;                 // jittered -- see occlusion_query.hlsl
    Math::mat4 viewToClip;               // the camera's (jittered) projection; S3b's near/far verdicts
    std::vector<OcclusionBox> boxes;     // grouped batches' boxes first, then the individual ones
    std::vector<OcclusionBatch> batches;
    std::uint32_t queryCount = 0;        // query indices used: [0, queryCount)
    std::uint32_t groupedQueries = 0;
    std::uint32_t individualQueries = 0;
    std::uint32_t droppedQueries = 0;    // primitives that wanted a query past kMaxOcclusionQueries
    void Clear();
};

class OcclusionHistory
{
public:
    struct FrameResults
    {
        const std::uint64_t* samples = nullptr;    // method Queries: per query index of that frame's plan
        const std::uint32_t* hzbVisible = nullptr; // method Hzb: per test index, 1 = visible
        std::uint32_t count = 0;
        std::uint64_t frameNumber = 0;             // the frame those queries / tests were issued in
    };

    // `method` Off = the plan stays empty, the history is left as is (so switching back on does
    // not start from scratch) and Consider() is never called. The caller passes the method whose
    // producer actually has its device objects, not the setting.
    void BeginFrame(std::uint64_t frameNumber, double nowSec, const Camera& camera,
                    std::uint32_t renderWidth, std::uint32_t renderHeight, std::uint32_t sceneVersion,
                    const FrameResults& results, OcclusionMethod method);
    bool Enabled() const { return enabled_; }
    OcclusionMethod Method() const { return method_; }

    // Camera prepare task only. Returns true when the primitive counts as OCCLUDED this frame.
    // `allowGrouped` is false for sub-primitives (UE: "sub queries are never grouped").
    bool Consider(const OcclusionKey& key, const AABB& worldBounds, bool allowGrouped, bool& outDefinite);
    void EndConsider();

    const OcclusionQueryPlan& Plan() const { return plan_; }
    std::uint32_t LatencyFrames() const { return latencyFrames_; }
    std::size_t EntryCount() const { return entries_.size(); }
    std::uint32_t TestedQueries() const { return testedQueries_; }
    bool IgnoredExistingQueries() const { return ignoreExistingQueries_; }

    void Reset();

private:
    // FPrimitiveOcclusionHistory. Query indices are into the plan of `pendingQueryFrame[slot]`.
    struct Entry
    {
        std::uint32_t pendingQuery[kOcclusionBufferedFrames];
        std::uint64_t pendingQueryFrame[kOcclusionBufferedFrames];
        bool grouped[kOcclusionBufferedFrames];
        std::uint64_t lastTestFrame = ~0ull;
        std::uint64_t lastConsideredFrame = ~0ull;
        double lastProvenVisibleTime = 0.0;
        double lastConsideredTime = 0.0;
        float lastPixelsPercentage = 0.0f;
        std::uint8_t becameEligibleForQueryCooldown = 0;
        bool wasOccludedLastFrame = false;
        bool stateWasDefiniteLastFrame = false;
        // S3b: UE's HZBTestIndex is not a separate field -- the test index rides the pendingQuery
        // slots, since the tester's results are read back through the same latency ring.
        Entry()
        {
            for (unsigned i = 0; i < kOcclusionBufferedFrames; ++i)
            {
                pendingQuery[i] = kNoQuery;
                pendingQueryFrame[i] = 0;
                grouped[i] = false;
            }
        }
    };

    struct Batcher
    {
        std::uint32_t maxPerBatch = 1;
        std::vector<OcclusionBox> boxes;
        std::vector<OcclusionBatch> batches;
        std::uint32_t inCurrent = 0; // boxes in the open batch (0 = none open)
        void Clear() { boxes.clear(); batches.clear(); inCurrent = 0; }
    };

    std::uint32_t BatchPrimitive(Batcher& b, const OcclusionBox& box);
    std::uint32_t AddHzbBounds(const OcclusionBox& box); // S3b: FHZBOcclusionTester::AddBounds
    float RandomFraction();
    void Trim();

    robin_hood::unordered_map<OcclusionKey, Entry, OcclusionKeyHash> entries_;
    OcclusionQueryPlan plan_;
    Batcher grouped_{ kOccludedPrimitiveQueryBatchSize };
    Batcher individual_{ 1u };

    bool enabled_ = false;
    OcclusionMethod method_ = OcclusionMethod::Off;
    bool ignoreExistingQueries_ = false;
    bool disableQuerySubmissions_ = false;
    std::uint64_t frame_ = 0;
    double now_ = 0.0;
    std::uint32_t latencyFrames_ = kOcclusionBufferedFrames;
    unsigned issueSlot_ = 0;
    unsigned lookupSlot_ = 0;
    FrameResults results_{};
    Math::float3 viewOrigin_{};
    Math::float3 nearNormal_{};   // camera forward
    float nearOffset_ = 0.0f;     // plane: dot(nearNormal_, p) + nearOffset_ = 0 at the near plane
    float oneOverNumPossiblePixels_ = 0.0f;
    std::uint32_t queryCount_ = 0;
    std::uint32_t testedQueries_ = 0;
    std::uint32_t rng_ = 0x9E3779B9u;

    // Reset detection (SceneVisibility.cpp:5367-5380 + our scene-set version)
    bool everRan_ = false;
    std::uint64_t lastHistoryRevision_ = ~0ull;
    std::uint32_t lastSceneVersion_ = ~0u;
    double lastRenderTime_ = 0.0;
    Math::float3 prevForward_{};
    Math::float3 prevOrigin_{};
};
} // namespace vis
