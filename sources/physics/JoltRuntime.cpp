#include "physics/PhysicsInternal.h"

#include <Jolt/ConfigurationString.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>

#include "core/logging/Log.h"
#include "core/memory/MemScope.h"

namespace physics
{
    namespace
    {
        // Through the process's operator new (core/memory/MemNew.cpp: mimalloc + the counters), under Jolt's own tag whichever
        // thread asks: its pool's threads have no scope of ours around them.
        void* JoltAllocate(size_t size)
        {
            MEM_SCOPE(MemTag::Physics);
            return ::operator new(size);
        }

        void JoltFree(void* block)
        {
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
            return ::operator new(size, std::align_val_t(alignment < 16 ? 16 : alignment));
        }

        void JoltAlignedFree(void* block)
        {
            // Jolt does not say the alignment back. mi_free_aligned only checks that the pointer has it: every aligned block
            // here has at least 16.
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
        pairs.EnableCollision(layers::kMoving, layers::kNonMoving);
        pairs.EnableCollision(layers::kMoving, layers::kMoving);
        objectVsBroadPhase = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(broadPhase, 2, pairs, layers::kCount);
    }
} // namespace physics
