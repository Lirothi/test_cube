#include "rendering/diagnostics/RetireSelfTest.h"

#include "core/logging/Log.h"
#include "core/memory/MemTags.h"
#include "core/threading/ThreadRole.h"
#include "rhi/DeferredRelease.h"
#include "rhi/GpuAlloc.h"
#include "rendering/core/RenderGraph.h" // the pass arm: render::g_passSelfTestHook
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>

namespace render
{
namespace
{
constexpr int kStartFrame = 20;        // the scene has settled
constexpr int kGiveUpFrames = 120;     // a release later than this is a leak
constexpr int kControlFrames = 10;     // the control arm's list has gone to the GPU by then
constexpr UINT64 kBytes = 64 * 1024;

bool g_armed = false;
RetireSelfTestMode g_mode = RetireSelfTestMode::Retire;
int g_frames = 0;
int g_exitCode = -1;
bool g_started = false;
std::uint64_t g_retiredAt = 0;
std::uint64_t g_mustComplete = 0;                  // the frame that has to be done when the release step runs
std::atomic<std::uint64_t> g_releasedAt{ 0 };     // the frame the release step ran in; 0 = not yet
std::atomic<bool> g_completeAtRelease{ false };   // IsFrameNumberComplete(g_mustComplete) when it ran
Microsoft::WRL::ComPtr<ID3D12Resource> g_target;  // the copy's destination: lives until the verdict
// The default arm's crowd (the second review of wave 1, item 4): this many entries retired in one frame must cost the
// boundaries a fence read per distinct frame number, not one each -- after a level unload thousands wait at once.
constexpr int kCrowd = 512;
constexpr std::uint64_t kMaxQuestionsPerBoundary = 32; // the distinct frame numbers waiting, with room to spare
std::atomic<int> g_crowdReleased{ 0 };
std::uint64_t g_questionsBefore = 0;  // GetStats().questions at the previous boundary
std::uint64_t g_maxQuestions = 0;     // the most one boundary asked since the crowd was retired
// The pass arm (the second review of wave 1): the copy is recorded, and its source retired, inside a render-graph pass's
// task (render::g_passSelfTestHook) -- the service's stamp taken on a worker, in the frame the pass records.
const Renderer* g_passRenderer = nullptr;
Microsoft::WRL::ComPtr<ID3D12Resource> g_passSource;

Microsoft::WRL::ComPtr<ID3D12Resource> MakeBuffer(Renderer& renderer, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                                                  const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    if (FAILED(gpu::CreateCommittedResource(renderer.GetDevice(), &heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                            MemTag::Renderer, name, IID_PPV_ARGS(&buffer))))
    {
        return nullptr;
    }
    return buffer;
}

void Finish(int exitCode)
{
    g_exitCode = exitCode;
    g_target.Reset();
}

// A release step that only records when it ran and whether `mustComplete` was done by then.
void RetireSentinel(const Renderer& renderer, std::uint64_t mustComplete, std::function<void()> alsoRelease)
{
    const Renderer* r = &renderer;
    g_mustComplete = mustComplete;
    deferred::Retire([r, mustComplete, alsoRelease]() {
        g_completeAtRelease.store(r->IsFrameNumberComplete(mustComplete));
        g_releasedAt.store(r->GetTotalFrameNumber());
        if (alsoRelease) { alsoRelease(); }
    }, kBytes, "retire self-test");
}

const char* ModeSuffix()
{
    return g_mode == RetireSelfTestMode::Upload ? " (upload)" : g_mode == RetireSelfTestMode::Pass ? " (pass)" : "";
}

// On the worker that runs the armed pass's task, its list just opened (RenderGraphPassContext::BeginCL).
void PassHook(ID3D12GraphicsCommandList* cl)
{
    if (!cl || !g_passRenderer || !g_passSource || !g_target) { return; }
    cl->CopyBufferRegion(g_target.Get(), 0, g_passSource.Get(), 0, kBytes);
    g_retiredAt = g_passRenderer->GetTotalFrameNumber(); // the frame this pass records
    RetireSentinel(*g_passRenderer, g_retiredAt, [source = std::move(g_passSource)]() mutable { source.Reset(); });
    LOG_INFO(logging::LogCategory::RenderRhi, "retire self-test (pass): a copy recorded in pass 'Tonemap' and its source "
             "retired from the pass's task in frame {}", g_retiredAt);
}
} // namespace

void ArmRetireSelfTest(RetireSelfTestMode mode)
{
    g_armed = true;
    g_mode = mode;
    LOG_INFO(logging::LogCategory::RenderRhi, "retire self-test: armed{}",
             mode == RetireSelfTestMode::Control ? " (control: released at once)"
             : mode == RetireSelfTestMode::Upload ? " (upload: an UploadBatch submitted from the game half, dropped at once)"
                                                  : "");
}

void RetireSelfTestGameHalf(Renderer& renderer)
{
    if (!g_armed || g_mode != RetireSelfTestMode::Upload || g_started || g_frames < kStartFrame) { return; }
    g_started = true;
    // The source lives ONLY in the batch's keep-alive; the destination is ours.
    Microsoft::WRL::ComPtr<ID3D12Resource> upload =
        MakeBuffer(renderer, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, L"RetireSelfTest.Upload");
    g_target = MakeBuffer(renderer, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, L"RetireSelfTest.Target");
    const bool inFlight = threading::RenderThreadInFlight();
    {
        UploadBatch batch;
        if (!upload || !g_target || !batch.Begin(&renderer))
        {
            LOG_ERROR(logging::LogCategory::RenderRhi, "retire self-test (upload): FAIL -- could not make its buffers or list");
            Finish(1);
            return;
        }
        void* mapped = nullptr;
        if (SUCCEEDED(upload->Map(0, nullptr, &mapped)) && mapped)
        {
            std::memset(mapped, 0x5A, static_cast<size_t>(kBytes));
            upload->Unmap(0, nullptr);
        }
        batch.CommandList()->CopyBufferRegion(g_target.Get(), 0, upload.Get(), 0, kBytes);
        batch.KeepAlive()->push_back(std::move(upload));
        threading::RoleScope gameRole(threading::ThreadRole::Game);
        batch.Submit(&renderer);
        // `batch` goes out of scope here: before R16.8 its allocator went with it, under a list still to be submitted.
    }
    g_retiredAt = renderer.GetTotalFrameNumber();
    // Retired in the same call as the batch's own entry (the same stamp): it runs in the same boundary, and checks that
    // the frame after this one -- the one a deferred list goes with -- was done by then.
    RetireSentinel(renderer, g_retiredAt + 1, nullptr);
    LOG_INFO(logging::LogCategory::RenderRhi, "retire self-test (upload): a batch submitted from the game half in frame {} "
             "(render thread {}), dropped at once; its list goes with the next hand-over", g_retiredAt,
             inFlight ? "drawing" : "idle");
}

bool RetireSelfTestFrame(Renderer& renderer)
{
    if (!g_armed || g_exitCode >= 0) { return false; }
    ++g_frames;
    if (g_frames == kStartFrame && g_mode == RetireSelfTestMode::Pass)
    {
        g_started = true;
        g_passSource = MakeBuffer(renderer, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, L"RetireSelfTest.PassSource");
        g_target = MakeBuffer(renderer, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, L"RetireSelfTest.Target");
        if (!g_passSource || !g_target)
        {
            LOG_ERROR(logging::LogCategory::RenderRhi, "retire self-test (pass): FAIL -- could not make its buffers");
            Finish(1);
            return true;
        }
        g_passRenderer = &renderer;
        g_passSelfTestHookPass.store(static_cast<int>(RenderPass::Main_Tonemap), std::memory_order_relaxed);
        g_passSelfTestHook.store(&PassHook);
        LOG_INFO(logging::LogCategory::RenderRhi, "retire self-test (pass): armed in frame {} -- the next 'Tonemap' pass "
                 "copies and retires from its task", renderer.GetTotalFrameNumber());
        return false;
    }
    if (g_frames == kStartFrame && g_mode != RetireSelfTestMode::Upload && g_mode != RetireSelfTestMode::Pass)
    {
        g_started = true;
        Microsoft::WRL::ComPtr<ID3D12Resource> source =
            MakeBuffer(renderer, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, L"RetireSelfTest.Source");
        g_target = MakeBuffer(renderer, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, L"RetireSelfTest.Target");
        UploadBatch batch;
        if (!source || !g_target || !batch.Begin(&renderer))
        {
            LOG_ERROR(logging::LogCategory::RenderRhi, "retire self-test: FAIL -- could not make its buffers or list");
            Finish(1);
            return true;
        }
        batch.CommandList()->CopyBufferRegion(g_target.Get(), 0, source.Get(), 0, kBytes);
        {
            // As the game half submits: unparked, the list waits for the hand-over (Renderer::ExecuteOrDefer).
            threading::RoleScope gameRole(threading::ThreadRole::Game);
            batch.Submit(&renderer);
        }
        g_retiredAt = renderer.GetTotalFrameNumber();
        if (g_mode == RetireSelfTestMode::Control)
        {
            source.Reset(); // its last reference: destroyed while the deferred list still names it
            LOG_INFO(logging::LogCategory::RenderRhi, "retire self-test (control): the source released at once in frame "
                     "{}, its copy not yet on the GPU", g_retiredAt);
        }
        else
        {
            RetireSentinel(renderer, g_retiredAt, [source]() mutable { source.Reset(); });
            for (int i = 0; i < kCrowd; ++i)
            {
                deferred::Retire([]() { g_crowdReleased.fetch_add(1, std::memory_order_relaxed); }, 0,
                                 "retire self-test crowd");
            }
            g_questionsBefore = deferred::GetStats().questions;
            LOG_INFO(logging::LogCategory::RenderRhi, "retire self-test: the source retired in frame {}, with a crowd of {} "
                     "entries; its copy goes with the hand-over", g_retiredAt, kCrowd);
        }
        return false;
    }
    if (!g_started) { return false; }
    if (g_mode == RetireSelfTestMode::Retire)
    {
        // Called right after Renderer::BeginFrame: the difference is what this frame's boundary asked.
        const std::uint64_t questions = deferred::GetStats().questions;
        g_maxQuestions = std::max(g_maxQuestions, questions - g_questionsBefore);
        g_questionsBefore = questions;
    }

    if (g_mode == RetireSelfTestMode::Control)
    {
        if (g_frames < kStartFrame + kControlFrames || !renderer.IsFrameNumberComplete(g_retiredAt + 1)) { return false; }
        LOG_INFO(logging::LogCategory::RenderRhi, "retire self-test (control): done -- the debug layer / GBV lines above "
                 "must name the released source (ERROR or CORRUPTION id=)");
        Finish(0);
        return true;
    }
    const std::uint64_t releasedAt = g_releasedAt.load();
    if (releasedAt == 0)
    {
        if (g_frames - kStartFrame < kGiveUpFrames) { return false; }
        LOG_ERROR(logging::LogCategory::RenderRhi, "retire self-test: FAIL -- retired in frame {}, not released {} frames "
                  "later", g_retiredAt, kGiveUpFrames);
        Finish(1);
        return true;
    }
    const bool early = !g_completeAtRelease.load() || releasedAt <= g_retiredAt;
    if (early)
    {
        LOG_ERROR(logging::LogCategory::RenderRhi, "retire self-test{}: FAIL -- retired in frame {}, released in frame {} "
                  "before the GPU had finished frame {}", ModeSuffix(),
                  g_retiredAt, releasedAt, g_mustComplete);
        Finish(1);
        return true;
    }
    if (g_mode == RetireSelfTestMode::Retire)
    {
        const int crowdReleased = g_crowdReleased.load(std::memory_order_relaxed);
        if (crowdReleased < kCrowd)
        {
            if (g_frames - kStartFrame < kGiveUpFrames) { return false; } // the crowd goes at the same boundary, or soon
            LOG_ERROR(logging::LogCategory::RenderRhi, "retire self-test: FAIL -- {} of the crowd's {} entries released {} "
                      "frames after they were retired", crowdReleased, kCrowd, g_frames - kStartFrame);
            Finish(1);
            return true;
        }
        if (g_maxQuestions > kMaxQuestionsPerBoundary)
        {
            LOG_ERROR(logging::LogCategory::RenderRhi, "retire self-test: FAIL -- a boundary asked the fences {} times with "
                      "a crowd of {} entries waiting (at most {}: one a distinct frame number)", g_maxQuestions, kCrowd,
                      kMaxQuestionsPerBoundary);
            Finish(1);
            return true;
        }
    }
    const deferred::Stats stats = deferred::GetStats();
    LOG_INFO(logging::LogCategory::RenderRhi, "retire self-test{}: PASS -- retired in frame {}, released in frame {} ({} "
             "frames later) with the GPU done with frame {}; the service holds {} ({} bytes), released {} in all{}",
             ModeSuffix(), g_retiredAt, releasedAt, releasedAt - g_retiredAt,
             g_mustComplete, stats.waiting, stats.waitingBytes, stats.released,
             g_mode == RetireSelfTestMode::Retire
                 ? " -- the crowd of " + std::to_string(kCrowd) + " cost a boundary at most " +
                       std::to_string(g_maxQuestions) + " fence questions"
                 : std::string());
    Finish(0);
    return true;
}

int RetireSelfTestExitCode()
{
    return g_exitCode;
}
} // namespace render
