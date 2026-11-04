#include "physics/PhysicsInternal.h"

#include <Jolt/ConfigurationString.h>

#include <mimalloc.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "core/logging/Log.h"
#include "core/memory/MemScope.h"
#include "core/profiling/Profiler.h"

namespace physics
{
    namespace
    {
        // Through the process's operator new (core/memory/MemNew.cpp: mimalloc + the counters), under Jolt's own tag whichever
        // thread asks: its pool's threads have no scope of ours around them.
        std::atomic<std::int64_t> g_bytes{ 0 };

        void* JoltAllocate(size_t size)
        {
            MEM_SCOPE(MemTag::Physics);
            void* block = ::operator new(size);
            g_bytes.fetch_add(static_cast<std::int64_t>(mi_usable_size(block)), std::memory_order_relaxed);
            return block;
        }

        void JoltFree(void* block)
        {
            g_bytes.fetch_sub(static_cast<std::int64_t>(mi_usable_size(block)), std::memory_order_relaxed);
            ::operator delete(block);
        }

        void* JoltReallocate(void* block, size_t oldSize, size_t newSize)
        {
            if (newSize == 0)
            {
                JoltFree(block);
                return nullptr;
            }
            void* grown = JoltAllocate(newSize);
            if (block != nullptr)
            {
                std::memcpy(grown, block, oldSize < newSize ? oldSize : newSize);
                JoltFree(block);
            }
            return grown;
        }

        void* JoltAlignedAllocate(size_t size, size_t alignment)
        {
            MEM_SCOPE(MemTag::Physics);
            void* block = ::operator new(size, std::align_val_t(alignment < 16 ? 16 : alignment));
            g_bytes.fetch_add(static_cast<std::int64_t>(mi_usable_size(block)), std::memory_order_relaxed);
            return block;
        }

        void JoltAlignedFree(void* block)
        {
            // Jolt does not say the alignment back. mi_free_aligned only checks that the pointer has it: every aligned block
            // here has at least 16.
            g_bytes.fetch_sub(static_cast<std::int64_t>(mi_usable_size(block)), std::memory_order_relaxed);
            ::operator delete(block, std::align_val_t(16));
        }

        void JoltTrace(const char* format, ...)
        {
            char line[1024];
            va_list args;
            va_start(args, format);
            std::vsnprintf(line, sizeof(line), format, args);
            va_end(args);
            LOG_INFO(logging::LogCategory::Physics, "jolt: {}", line);
        }

#ifdef JPH_ENABLE_ASSERTS
        bool JoltAssertFailed(const char* expression, const char* message, const char* file, JPH::uint line)
        {
            // Fatal: flushed synchronously, so the record survives whatever the dialog's button does.
            LOG_FATAL(logging::LogCategory::Physics, "jolt assert: {} ({}) at {}:{}", expression, message ? message : "", file, line);
            return true; // the breakpoint
        }
#endif

        std::mutex g_mutex;
        int g_references = 0;
    }

    std::uint64_t JoltBytes()
    {
        return static_cast<std::uint64_t>(std::max<std::int64_t>(g_bytes.load(std::memory_order_relaxed), 0));
    }

    void AcquireJolt()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_references++ > 0) { return; }
        JPH::Allocate = &JoltAllocate;
        JPH::Reallocate = &JoltReallocate;
        JPH::Free = &JoltFree;
        JPH::AlignedAllocate = &JoltAlignedAllocate;
        JPH::AlignedFree = &JoltAlignedFree;
        JPH::Trace = &JoltTrace;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = &JoltAssertFailed;)
        JPH::Factory::sInstance = new JPH::Factory();
        // Aborts on a library built with other defines, naming them through Trace: the ABI's check.
        JPH::RegisterTypes();
        LOG_INFO(logging::LogCategory::Physics, "physics: Jolt {}.{}.{} ({})", JPH_VERSION_MAJOR, JPH_VERSION_MINOR, JPH_VERSION_PATCH,
            JPH::GetConfigurationString());
    }

    void ReleaseJolt()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (--g_references > 0) { return; }
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }

    LayerTables::LayerTables()
        : broadPhase(layers::kCount, 2)
        , pairs(layers::kCount)
    {
        broadPhase.MapObjectToBroadPhaseLayer(layers::kNonMoving, JPH::BroadPhaseLayer(0));
        broadPhase.MapObjectToBroadPhaseLayer(layers::kMoving, JPH::BroadPhaseLayer(1));
        // Jolt names a broad phase's zones in a trace after its layers; left out, every one of them reads "Undefined".
        broadPhase.SetBroadPhaseLayerName(JPH::BroadPhaseLayer(0), "Broad phase: static");
        broadPhase.SetBroadPhaseLayerName(JPH::BroadPhaseLayer(1), "Broad phase: moving");
        pairs.EnableCollision(layers::kMoving, layers::kNonMoving);
        pairs.EnableCollision(layers::kMoving, layers::kMoving);
        objectVsBroadPhase = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(broadPhase, 2, pairs, layers::kCount);
    }
} // namespace physics

// Jolt's own zones (JPH_EXTERNAL_PROFILE) as scopes of ours, during a --trace only: a frame that nobody records pays one atomic read for each. Not in the HUD's rows,
// then: `Scene::TickPhysics` and `Physics.Step` are what it shows. Only the step and its jobs: Jolt has a zone per contact, pair and island too, thousands a step, which took
// a 300-frame heap of 3000 bodies to a 480 MB trace and its step to 1.5x. kTraceEveryZone = true brings them back, to see what inside a job costs.
namespace
{
    constexpr bool kTraceEveryZone = false;
    constexpr size_t kZoneActiveByte = 63;

    bool IsCoarseZone(const std::string& name)
    {
        static const std::unordered_set<std::string> kCoarse = {
            "JPH::PhysicsSystem::Update", "JPH::JobSystemWithBarrier::WaitForJobs", "Executing Jobs",
            // PhysicsSystem::Update's CreateJob names.
            "ApplyGravity", "BodySetIslandIndex", "BuildIslandsFromConstraints", "ContactRemovedCallbacks", "DetermineActiveConstraints", "FinalizeIslands", "FindCCDContacts", "FindCollisions",
            "IntegrateVelocity", "PostIntegrateVelocity", "PreIntegrateVelocity", "ResolveCCDContacts", "SetupVelocityConstraints", "SoftBodyCollide", "SoftBodyFinalize", "SoftBodyPrepare",
            "SoftBodySimulate", "SolvePositionConstraints", "SolveVelocityConstraints", "StartNextStep", "StepListeners", "UpdateBroadPhaseFinalize", "UpdateBroadPhasePrepare" };
        return kTraceEveryZone || kCoarse.count(name) != 0;
    }

    struct Zone
    {
        Profiler::ScopeNameKey key;
        bool traced = false;
    };

    const Zone& ZoneOf(const char* name)
    {
        // Jolt's names are literals, so the pointer says it, on this thread without a lock. A literal key lasts: a dynamic one goes back to the
        // profiler's free list with its capture, and a key kept past that would read another name in the next one.
        thread_local std::unordered_map<const char*, Zone> local;
        const auto found = local.find(name);
        if (found != local.end()) { return found->second; }
        const std::string text(name);
        Zone zone;
        zone.traced = IsCoarseZone(text);
        if (zone.traced)
        {
            const std::wstring wide(text.begin(), text.end());
            zone.key = Profiler::RegisterTraceLiteral(wide.c_str());
        }
        return local.emplace(name, zone).first->second;
    }
}

JPH_NAMESPACE_BEGIN

ExternalProfileMeasurement::ExternalProfileMeasurement(const char* inName, uint32)
{
    static_assert(sizeof(::Profiler::ScopedCpu) <= kZoneActiveByte, "the zone keeps its scope in Jolt's 64 bytes");
    mUserData[kZoneActiveByte] = 0;
    if (!::Profiler::Get().IsTraceCapturing()) { return; }
    const Zone& zone = ZoneOf(inName);
    if (!zone.traced) { return; }
    new (mUserData) ::Profiler::ScopedCpu(zone.key);
    mUserData[kZoneActiveByte] = 1;
}

ExternalProfileMeasurement::~ExternalProfileMeasurement()
{
    if (mUserData[kZoneActiveByte] != 0) { reinterpret_cast<::Profiler::ScopedCpu*>(mUserData)->~ScopedCpu(); }
}

JPH_NAMESPACE_END
