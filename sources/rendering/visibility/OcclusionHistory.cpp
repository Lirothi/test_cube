#include "rendering/visibility/OcclusionHistory.h"

#include <algorithm>
#include <cmath>

#include "app/camera/Camera.h"

namespace vis
{
void OcclusionQueryPlan::Clear()
{
    frameNumber = 0;
    method = OcclusionMethod::Off;
    boxes.clear();
    batches.clear();
    queryCount = 0;
    groupedQueries = 0;
    individualQueries = 0;
    droppedQueries = 0;
}

void OcclusionHistory::Reset()
{
    entries_.clear();
    plan_.Clear();
    grouped_.Clear();
    individual_.Clear();
    enabled_ = false;
    everRan_ = false;
    lastHistoryRevision_ = ~0ull;
    lastSceneVersion_ = ~0u;
}

float OcclusionHistory::RandomFraction()
{
    // UE: GOcclusionRandomStream, a fixed table. Any deterministic stream does: the point is that
    // the re-test cadence is not a function of the frame number alone.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<float>(rng_ & 0xFFFFFFu) / static_cast<float>(0x1000000u);
}

void OcclusionHistory::BeginFrame(std::uint64_t frameNumber, double nowSec, const Camera& camera,
                                  std::uint32_t renderWidth, std::uint32_t renderHeight,
                                  std::uint32_t sceneVersion, const FrameResults& results,
                                  OcclusionMethod method)
{
    plan_.Clear();
    grouped_.Clear();
    individual_.Clear();
    queryCount_ = 0;
    testedQueries_ = 0;
    method_ = method;
    enabled_ = method != OcclusionMethod::Off;
    if (!enabled_) { return; }

    frame_ = frameNumber;
    now_ = nowSec;
    latencyFrames_ = static_cast<std::uint32_t>(std::clamp(g_occlusion.queryLatency, 1, static_cast<int>(kOcclusionBufferedFrames)));
    issueSlot_ = static_cast<unsigned>(frame_ % kOcclusionBufferedFrames);
    lookupSlot_ = static_cast<unsigned>((frame_ + kOcclusionBufferedFrames - latencyFrames_) % kOcclusionBufferedFrames);
    results_ = results;
    results_.frameNumber = (results.samples || results.hzbVisible) ? results.frameNumber : ~0ull;

    viewOrigin_ = camera.GetPosition();
    const Math::mat4& rot = camera.GetRotationMatrix(); // rows: right, up, forward
    nearNormal_ = Math::float3(rot.m._31, rot.m._32, rot.m._33);
    nearOffset_ = -(nearNormal_.Dot(viewOrigin_) + camera.GetZNear());
    oneOverNumPossiblePixels_ = (renderWidth > 0 && renderHeight > 0)
        ? 1.0f / (static_cast<float>(renderWidth) * static_cast<float>(renderHeight)) : 0.0f;

    // SceneVisibility.cpp:5367-5380 -- when existing results cannot be trusted, nothing is culled
    // this frame and every entry re-queries. Ours adds the scene-set version: an object removed
    // and another allocated at the same address would otherwise inherit a history.
    const Math::float3 forward = nearNormal_;
    bool largeMovement = false;
    if (everRan_)
    {
        const float cosAngle = std::clamp(forward.Dot(prevForward_), -1.0f, 1.0f);
        const float angleDeg = std::acos(cosAngle) * 57.2957795f;
        const float dist = (viewOrigin_ - prevOrigin_).Length();
        largeMovement = angleDeg > g_occlusion.cutAngleDeg || dist > g_occlusion.cutDistance;
    }
    const bool sceneChanged = sceneVersion != lastSceneVersion_;
    ignoreExistingQueries_ = !everRan_ ||
                             camera.GetHistoryRevision() != lastHistoryRevision_ ||
                             lastRenderTime_ + static_cast<double>(g_occlusion.probablyVisibleTime) < now_ ||
                             largeMovement || sceneChanged;
    disableQuerySubmissions_ = false;
    if (sceneChanged && everRan_) { entries_.clear(); }
    everRan_ = true;
    lastHistoryRevision_ = camera.GetHistoryRevision();
    lastSceneVersion_ = sceneVersion;
    lastRenderTime_ = now_;
    prevForward_ = forward;
    prevOrigin_ = viewOrigin_;

    Trim();

    plan_.frameNumber = frame_;
    plan_.method = method_;
    plan_.viewProj = camera.GetViewProjMatrix();
    plan_.viewToClip = camera.GetProjMatrix();
}

std::uint32_t OcclusionHistory::AddHzbBounds(const OcclusionBox& box)
{
    // FHZBOcclusionTester::AddBounds (SceneOcclusion.cpp:831-838): the box's index in this
    // frame's list is its test index; past the capacity the primitive keeps last frame's state.
    if (queryCount_ >= kMaxHzbTests) { return kNoQuery; }
    plan_.boxes.push_back(box);
    return queryCount_++;
}

void OcclusionHistory::Trim()
{
    // FSceneViewState::TrimOcclusionHistory: every 6 frames, entries not considered for
    // PrimitiveProbablyVisibleTime (or with a time from the future -- a clock reset) go.
    if (frame_ % 6 != 0) { return; }
    const double minHistoryTime = now_ - static_cast<double>(g_occlusion.probablyVisibleTime);
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        const Entry& e = it->second;
        if (e.lastConsideredTime < minHistoryTime || e.lastConsideredTime > now_) { it = entries_.erase(it); }
        else { ++it; }
    }
}

std::uint32_t OcclusionHistory::BatchPrimitive(Batcher& b, const OcclusionBox& box)
{
    // FOcclusionQueryBatcher::BatchPrimitive: open a batch (= one query) when none is open or the
    // current one is full; the box joins the open batch.
    if (b.inCurrent == 0 || b.inCurrent >= b.maxPerBatch)
    {
        if (queryCount_ >= kMaxOcclusionQueries) { return kNoQuery; }
        OcclusionBatch batch;
        batch.queryIndex = queryCount_++;
        batch.firstBox = static_cast<std::uint32_t>(b.boxes.size());
        batch.boxCount = 0;
        batch.grouped = b.maxPerBatch > 1;
        b.batches.push_back(batch);
        b.inCurrent = 0;
    }
    b.boxes.push_back(box);
    ++b.inCurrent;
    ++b.batches.back().boxCount;
    return b.batches.back().queryIndex;
}

bool OcclusionHistory::Consider(const OcclusionKey& key, const AABB& worldBounds, bool allowGrouped, bool& outDefinite)
{
    outDefinite = false;
    if (!enabled_ || !worldBounds.IsValid()) { return false; }

    bool occluded = false;
    bool definite = false;

    auto found = entries_.find(key);
    Entry* e = nullptr;
    if (found == entries_.end())
    {
        // :2706-2712 -- a primitive seen for the first time is visible and not definite.
        e = &entries_[key];
    }
    else
    {
        e = &found->second;
        if (ignoreExistingQueries_)
        {
            // :2715-2719 -- definitely unoccluded; "definite" only when no query will be sent.
            definite = disableQuerySubmissions_;
        }
        else
        {
            // :2738-2795 -- read the pending result of the frame that can be read.
            bool haveResult = false;
            const std::uint32_t q = e->pendingQuery[lookupSlot_];
            const bool resultForThisEntry = q != kNoQuery && q < results_.count &&
                                            e->pendingQueryFrame[lookupSlot_] == results_.frameNumber;
            if (resultForThisEntry && method_ == OcclusionMethod::Hzb && results_.hzbVisible)
            {
                // S3b, :2727-2733 -- the HZB verdict is binary and definite. No pixel count
                // exists; the percentage is set as for a missing result, and the stochastic
                // re-test never consults it on this path (every entry is tested every frame).
                occluded = results_.hzbVisible[q] == 0u;
                e->lastPixelsPercentage = occluded ? 0.0f : g_occlusion.maxPixelsFraction;
                definite = true;
                haveResult = true;
                ++testedQueries_;
            }
            else if (resultForThisEntry && method_ == OcclusionMethod::Queries && results_.samples)
            {
                const std::uint64_t samples = results_.samples[q];
                occluded = samples == 0;
                e->lastPixelsPercentage = occluded ? 0.0f : static_cast<float>(samples) * oneOverNumPossiblePixels_;
                definite = !e->grouped[lookupSlot_];
                haveResult = true;
                ++testedQueries_;
            }
            if (!haveResult)
            {
                if (latencyFrames_ > 1)
                {
                    // No query to read: assume whatever it was last frame.
                    occluded = e->wasOccludedLastFrame;
                    definite = e->stateWasDefiniteLastFrame;
                }
                else
                {
                    // Latency 1: visible iff proven visible recently; the state was definite last
                    // frame, otherwise a query would have been run.
                    occluded = e->lastProvenVisibleTime + static_cast<double>(g_occlusion.probablyVisibleTime) < now_;
                    definite = true;
                }
                e->lastPixelsPercentage = occluded ? 0.0f : g_occlusion.maxPixelsFraction;
            }
        }
    }

    // :2801-2925 -- decide whether to ask the GPU this frame.
    {
        bool skipNewlyConsidered = false;
        const bool expandActive = g_occlusion.expandNewlyTested > 0.0f;
        if (expandActive)
        {
            if (e->becameEligibleForQueryCooldown == 0 &&
                e->lastConsideredFrame != ~0ull &&
                frame_ - e->lastConsideredFrame > static_cast<std::uint64_t>(g_occlusion.framesNotTestedToExpand))
            {
                e->becameEligibleForQueryCooldown = static_cast<std::uint8_t>(std::clamp(g_occlusion.framesToExpandNewlyTested, 0, 63));
            }
            skipNewlyConsidered = e->becameEligibleForQueryCooldown != 0;
            if (skipNewlyConsidered) { --e->becameEligibleForQueryCooldown; }
        }

        const float expand = kOcclusionSlop + g_occlusion.expandAllTested + (skipNewlyConsidered ? g_occlusion.expandNewlyTested : 0.0f);
        const Math::float3 mn = worldBounds.GetMin() - Math::float3(expand, expand, expand);
        const Math::float3 mx = worldBounds.GetMax() + Math::float3(expand, expand, expand);
        const Math::float3 center = (mn + mx) * 0.5f;
        const Math::float3 extent = (mx - mn) * 0.5f;

        bool allowBoundsTest = false;
        const float neverDist = g_occlusion.neverTestDistance;
        const Math::float3 toCenter = center - viewOrigin_;
        if (neverDist > 0.0f && toCenter.Dot(toCenter) < neverDist * neverDist)
        {
            allowBoundsTest = false; // :2827-2830
        }
        else
        {
            // :2831-2846 -- the box must lie entirely beyond the near plane; a box the camera's
            // near plane cuts cannot be rasterised whole and is visible, definite, unqueried.
            const float pushOut = std::abs(nearNormal_.x) * extent.x + std::abs(nearNormal_.y) * extent.y + std::abs(nearNormal_.z) * extent.z;
            allowBoundsTest = nearNormal_.Dot(center) + nearOffset_ > pushOut;
        }

        if (allowBoundsTest)
        {
            e->lastTestFrame = frame_;
            bool runQuery = true;
            bool groupedQuery = false;
            if (method_ == OcclusionMethod::Hzb)
            {
                // S3b, :2859-2862 -- AddHZBBounds unconditionally: the whole set is one dispatch,
                // so neither grouping nor the stochastic re-test buys anything.
            }
            else if (occluded)
            {
                // Occluded last frame -> queried every frame, grouped where grouping is allowed.
                groupedQuery = allowGrouped;
            }
            else if (definite)
            {
                // Stochastic re-test of the definitely visible, rarer the more pixels it covers.
                // DELTA from UE: applied to sub-primitives too. UE queries a sub-primitive every
                // frame ("the custom code knows what it is doing and will group internally"); our
                // sub-primitives are 60 m terrain chunks, and on an open view 615 of them re-tested
                // every frame were 0.7 ms of CPU record for a 3 % cull (island K=4, 2026-09-04).
                const float rnd = RandomFraction();
                const float mult = std::max(e->lastPixelsPercentage / g_occlusion.maxPixelsFraction, 1.0f);
                runQuery = (mult * rnd) < g_occlusion.maxPixelsFraction;
            }
            if (runQuery)
            {
                const OcclusionBox box{ mn, mx };
                const std::uint32_t q = method_ == OcclusionMethod::Hzb
                    ? AddHzbBounds(box)
                    : BatchPrimitive(groupedQuery ? grouped_ : individual_, box);
                if (q != kNoQuery)
                {
                    e->pendingQuery[issueSlot_] = q;
                    e->pendingQueryFrame[issueSlot_] = frame_;
                    e->grouped[issueSlot_] = groupedQuery;
                }
                else
                {
                    ++plan_.droppedQueries;
                }
            }
        }
        else
        {
            occluded = false;
            definite = true;
        }
    }

    e->lastConsideredTime = now_;
    if (!occluded && definite) { e->lastProvenVisibleTime = now_; }
    e->lastConsideredFrame = frame_;
    e->wasOccludedLastFrame = occluded;
    e->stateWasDefiniteLastFrame = definite;
    outDefinite = definite;
    return occluded;
}

void OcclusionHistory::EndConsider()
{
    if (!enabled_) { return; }
    if (method_ == OcclusionMethod::Hzb)
    {
        // S3b: the boxes went straight into the plan (AddHzbBounds), no batches -- one dispatch.
        plan_.queryCount = queryCount_;
        plan_.individualQueries = queryCount_;
        plan_.groupedQueries = 0;
        return;
    }
    // Dispatch order 0: grouped queries before individual (SceneOcclusion.cpp:1383-1404).
    plan_.boxes.clear();
    plan_.batches.clear();
    plan_.boxes.reserve(grouped_.boxes.size() + individual_.boxes.size());
    plan_.batches.reserve(grouped_.batches.size() + individual_.batches.size());
    plan_.boxes.insert(plan_.boxes.end(), grouped_.boxes.begin(), grouped_.boxes.end());
    for (const OcclusionBatch& b : grouped_.batches) { plan_.batches.push_back(b); }
    const std::uint32_t base = static_cast<std::uint32_t>(plan_.boxes.size());
    plan_.boxes.insert(plan_.boxes.end(), individual_.boxes.begin(), individual_.boxes.end());
    for (OcclusionBatch b : individual_.batches)
    {
        b.firstBox += base;
        plan_.batches.push_back(b);
    }
    plan_.queryCount = queryCount_;
    plan_.groupedQueries = static_cast<std::uint32_t>(grouped_.batches.size());
    plan_.individualQueries = static_cast<std::uint32_t>(individual_.batches.size());
}
} // namespace vis
