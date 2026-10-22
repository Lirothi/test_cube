#include "rendering/core/GpuAlloc.h"

#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <string>

#include "core/logging/Log.h"
#include "core/memory/MemTrace.h"
#include "rendering/core/TextureCreate.h"

namespace gpu
{
namespace
{
// Private-data key of the census record riding on each D3D object.
// {7B3E9C41-5A2D-4F18-9C6E-2D8B4A1F7E63}
constexpr GUID kCensusRecordGuid = { 0x7b3e9c41, 0x5a2d, 0x4f18, { 0x9c, 0x6e, 0x2d, 0x8b, 0x4a, 0x1f, 0x7e, 0x63 } };

enum class Kind : std::uint8_t
{
    Buffer,
    Texture,
    DescriptorHeap,
    QueryHeap,
};

struct Record
{
    MemTag tag = MemTag::Untagged;
    MemPool pool = MemPool::Local;
    Kind kind = Kind::Buffer;
    std::uint64_t bytes = 0; // resources: committed size; heaps: the estimate
    std::uint64_t slack = 0; // buffers: committed size - width
    std::string name;
};

class CensusRecord;

// Never destroyed: a record can be released by D3D during process teardown, after every static
// destructor would have run.
struct Census
{
    std::mutex mtx;
    CensusRecord* head = nullptr;
    CensusTotals totals;
    bool traced = false; // memory trace (M4): Add/Remove emit events -- under mtx, like the start snapshot

    static Census& Get()
    {
        static Census* census = new Census();
        return *census;
    }

    void Apply(const Record& r, int sign)
    {
        TagTotals* rows[2] = { &totals.tags[static_cast<std::uint32_t>(r.tag)], &totals.all };
        for (TagTotals* t : rows)
        {
            if (r.kind == Kind::DescriptorHeap || r.kind == Kind::QueryHeap)
            {
                t->descriptorBytes += static_cast<std::uint64_t>(sign) * r.bytes;
                t->heaps += static_cast<std::uint32_t>(sign);
            }
            else
            {
                t->bytes[static_cast<int>(r.pool)] += static_cast<std::uint64_t>(sign) * r.bytes;
                t->bufferSlack += static_cast<std::uint64_t>(sign) * r.slack;
                t->resources += static_cast<std::uint32_t>(sign);
            }
        }
    }

    void Add(CensusRecord* rec, std::uint32_t stack);
    void Remove(CensusRecord* rec);
};

// The trace's view of a record: the key is the record (the object's identity while it lives).
std::uint8_t TraceHeap(const Record& r)
{
    return r.pool == MemPool::NonLocal ? memtrace::kHeapGpuNonLocal : memtrace::kHeapGpuLocal;
}

std::uint8_t TraceKind(const Record& r)
{
    switch (r.kind)
    {
    case Kind::Buffer: return memtrace::kKindBuffer;
    case Kind::Texture: return memtrace::kKindTexture;
    case Kind::DescriptorHeap: return memtrace::kKindDescriptorHeap;
    case Kind::QueryHeap: return memtrace::kKindQueryHeap;
    }
    return memtrace::kKindBuffer;
}

// The object D3D holds as private data. Its last Release is the D3D object's destruction.
class CensusRecord final : public IUnknown
{
public:
    explicit CensusRecord(Record r) : record(std::move(r)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (out == nullptr) { return E_POINTER; }
        if (riid == __uuidof(IUnknown))
        {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (left == 0)
        {
            Census::Get().Remove(this);
            delete this;
        }
        return left;
    }

    Record record;
    CensusRecord* prev = nullptr;
    CensusRecord* next = nullptr;

private:
    std::atomic<ULONG> refs_{ 1 };
};

void Census::Add(CensusRecord* rec, std::uint32_t stack)
{
    std::lock_guard<std::mutex> lk(mtx);
    rec->next = head;
    if (head != nullptr) { head->prev = rec; }
    head = rec;
    Apply(rec->record, +1);
    ++totals.created;
    if (traced)
    {
        const Record& r = rec->record;
        memtrace::EmitAlloc(reinterpret_cast<std::uintptr_t>(rec), r.bytes, static_cast<std::uint8_t>(r.tag), TraceHeap(r),
                            TraceKind(r), stack, r.name.c_str());
    }
}

void Census::Remove(CensusRecord* rec)
{
    std::lock_guard<std::mutex> lk(mtx);
    if (rec->prev != nullptr) { rec->prev->next = rec->next; }
    else { head = rec->next; }
    if (rec->next != nullptr) { rec->next->prev = rec->prev; }
    Apply(rec->record, -1);
    ++totals.released;
    if (traced) { memtrace::EmitFree(reinterpret_cast<std::uintptr_t>(rec), TraceHeap(rec->record)); }
}

// memtrace's GPU side: start = events on + every live object as a synthetic alloc; stop = events
// off + the totals the trace's End record carries. Both under the census lock.
void TraceHook(bool start, memtrace::GpuEndTotals* out)
{
    Census& c = Census::Get();
    std::lock_guard<std::mutex> lk(c.mtx);
    c.traced = start;
    if (start)
    {
        for (const CensusRecord* rec = c.head; rec != nullptr; rec = rec->next)
        {
            const Record& r = rec->record;
            memtrace::EmitAlloc(reinterpret_cast<std::uintptr_t>(rec), r.bytes, static_cast<std::uint8_t>(r.tag), TraceHeap(r),
                                static_cast<std::uint8_t>(TraceKind(r) | memtrace::kKindSynthetic), 0, r.name.c_str());
        }
        return;
    }
    if (out == nullptr) { return; }
    for (std::uint32_t t = 0; t < kMemTagCount; ++t)
    {
        const TagTotals& tt = c.totals.tags[t];
        out->local[t] = tt.bytes[static_cast<int>(MemPool::Local)];
        out->nonLocal[t] = tt.bytes[static_cast<int>(MemPool::NonLocal)];
        out->descriptor[t] = tt.descriptorBytes;
        out->resources[t] = tt.resources;
        out->heaps[t] = tt.heaps;
    }
}

// Registered before main: a trace configured at boot finds the hook in place.
const bool g_traceHookRegistered = (memtrace::SetGpuHook(&TraceHook), true);

std::string Narrow(const wchar_t* name)
{
    if (name == nullptr || name[0] == L'\0') { return "(unnamed)"; }
    char buf[512];
    const int n = WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, static_cast<int>(sizeof(buf)), nullptr, nullptr);
    return n > 0 ? std::string(buf) : std::string("(unnamed)");
}

MemPool PoolOf(const D3D12_HEAP_PROPERTIES& hp)
{
    switch (hp.Type)
    {
    case D3D12_HEAP_TYPE_UPLOAD:
    case D3D12_HEAP_TYPE_READBACK:
        return MemPool::NonLocal;
    case D3D12_HEAP_TYPE_CUSTOM:
        return hp.MemoryPoolPreference == D3D12_MEMORY_POOL_L0 ? MemPool::NonLocal : MemPool::Local;
    default: // DEFAULT, GPU_UPLOAD
        return MemPool::Local;
    }
}

// Attach a record to `object`. On any failure the object simply stays out of the census.
void Track(ID3D12Object* object, Record record, const wchar_t* name)
{
    if (object == nullptr) { return; }
    if (name != nullptr && name[0] != L'\0') { object->SetName(name); }
    const std::uint32_t stack = memtrace::CaptureGpuStack(); // 0 unless a GPU trace is running; outside the lock
    auto* rec = new CensusRecord(std::move(record));
    Census::Get().Add(rec, stack);
    // D3D takes its own reference; ours goes right after, so D3D's is the last one. Replacing an
    // earlier record (the same object tracked twice) releases that one, which takes it out.
    if (FAILED(object->SetPrivateDataInterface(kCensusRecordGuid, rec)))
    {
        LOG_WARNING_ONCE(logging::LogCategory::RenderRhi,
                         "gpu census: SetPrivateDataInterface failed -- an object is not counted");
    }
    rec->Release();
}

void TrackResource(ID3D12Device* device, ID3D12Resource* resource, const D3D12_HEAP_PROPERTIES& hp,
                   const D3D12_RESOURCE_DESC& desc, MemTag tag, const wchar_t* name)
{
    Record r;
    r.tag = tag;
    r.pool = PoolOf(hp);
    r.kind = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? Kind::Buffer : Kind::Texture;
    const D3D12_RESOURCE_ALLOCATION_INFO info = device->GetResourceAllocationInfo(0, 1, &desc);
    // UINT64_MAX is the "desc invalid" answer; the object exists, so count what the desc says.
    r.bytes = info.SizeInBytes != UINT64_MAX ? info.SizeInBytes : desc.Width;
    if (r.kind == Kind::Buffer && r.bytes > desc.Width) { r.slack = r.bytes - desc.Width; }
    r.name = Narrow(name);
    Track(resource, std::move(r), name);
}

ID3D12Resource* AsResource(void* object, REFIID riid)
{
    if (object == nullptr) { return nullptr; }
    if (riid == __uuidof(ID3D12Resource)) { return static_cast<ID3D12Resource*>(object); }
    // Another interface of the same object: the census record goes on the object, whichever
    // interface handed it out. The QI reference is dropped at once.
    ID3D12Resource* resource = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(object)->QueryInterface(IID_PPV_ARGS(&resource))))
    {
        resource->Release();
    }
    return resource;
}

std::uint64_t QueryBytes(D3D12_QUERY_HEAP_TYPE type)
{
    switch (type)
    {
    case D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS: return sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
    case D3D12_QUERY_HEAP_TYPE_SO_STATISTICS: return sizeof(D3D12_QUERY_DATA_SO_STATISTICS);
    default: return sizeof(std::uint64_t);
    }
}
} // namespace

HRESULT CreateCommittedResource(ID3D12Device* device, const D3D12_HEAP_PROPERTIES* heapProps,
                                D3D12_HEAP_FLAGS heapFlags, const D3D12_RESOURCE_DESC* desc,
                                D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* clearValue,
                                MemTag tag, const wchar_t* name, REFIID riid, void** out)
{
    if (device == nullptr) { return E_INVALIDARG; }
    const HRESULT hr = device->CreateCommittedResource(heapProps, heapFlags, desc, initialState, clearValue, riid, out);
    if (SUCCEEDED(hr) && out != nullptr && heapProps != nullptr && desc != nullptr)
    {
        TrackResource(device, AsResource(*out, riid), *heapProps, *desc, tag, name);
    }
    return hr;
}

HRESULT CreateCommittedTexture(ID3D12Device* device, const D3D12_HEAP_PROPERTIES& heapProps,
                               D3D12_HEAP_FLAGS heapFlags, const D3D12_RESOURCE_DESC& desc,
                               D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* clearValue,
                               MemTag tag, const wchar_t* name, ID3D12Resource** out)
{
    const HRESULT hr = render::CreateCommittedTexture(device, heapProps, heapFlags, desc, initialState, clearValue, out);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr)
    {
        TrackResource(device, *out, heapProps, desc, tag, name);
    }
    return hr;
}

HRESULT CreateDescriptorHeap(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc,
                             MemTag tag, const wchar_t* name, REFIID riid, void** out)
{
    if (device == nullptr || desc == nullptr) { return E_INVALIDARG; }
    const HRESULT hr = device->CreateDescriptorHeap(desc, riid, out);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr)
    {
        Record r;
        r.tag = tag;
        r.kind = Kind::DescriptorHeap;
        // Shader-visible heaps are what the GPU reads; on a discrete card the driver keeps them in
        // memory it maps -- non-local in effect. CPU-only heaps are plain driver memory.
        r.pool = (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0 ? MemPool::NonLocal : MemPool::Local;
        r.bytes = static_cast<std::uint64_t>(desc->NumDescriptors) * device->GetDescriptorHandleIncrementSize(desc->Type);
        r.name = Narrow(name);
        ID3D12Object* object = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&object))))
        {
            Track(object, std::move(r), name);
            object->Release();
        }
    }
    return hr;
}

HRESULT CreateQueryHeap(ID3D12Device* device, const D3D12_QUERY_HEAP_DESC* desc,
                        MemTag tag, const wchar_t* name, REFIID riid, void** out)
{
    if (device == nullptr || desc == nullptr) { return E_INVALIDARG; }
    const HRESULT hr = device->CreateQueryHeap(desc, riid, out);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr)
    {
        Record r;
        r.tag = tag;
        r.kind = Kind::QueryHeap;
        r.bytes = static_cast<std::uint64_t>(desc->Count) * QueryBytes(desc->Type);
        r.name = Narrow(name);
        ID3D12Object* object = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&object))))
        {
            Track(object, std::move(r), name);
            object->Release();
        }
    }
    return hr;
}

void TrackExternalResource(ID3D12Device* device, ID3D12Resource* resource, MemTag tag, const wchar_t* name)
{
    if (device == nullptr || resource == nullptr) { return; }
    D3D12_HEAP_PROPERTIES hp{};
    D3D12_HEAP_FLAGS flags = D3D12_HEAP_FLAG_NONE;
    if (FAILED(resource->GetHeapProperties(&hp, &flags))) { hp.Type = D3D12_HEAP_TYPE_DEFAULT; }
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    TrackResource(device, resource, hp, desc, tag, name);
}

void ReadCensus(CensusTotals& out)
{
    Census& c = Census::Get();
    std::lock_guard<std::mutex> lk(c.mtx);
    out = c.totals;
}

void SnapshotRecords(std::vector<RecordInfo>& out)
{
    out.clear();
    Census& c = Census::Get();
    std::lock_guard<std::mutex> lk(c.mtx);
    for (const CensusRecord* rec = c.head; rec != nullptr; rec = rec->next)
    {
        const Record& r = rec->record;
        RecordInfo info;
        info.name = r.name;
        info.tag = r.tag;
        info.pool = r.pool;
        switch (r.kind)
        {
        case Kind::Buffer: info.kind = "buffer"; break;
        case Kind::Texture: info.kind = "texture"; break;
        case Kind::DescriptorHeap: info.kind = "descheap"; break;
        case Kind::QueryHeap: info.kind = "queryheap"; break;
        }
        info.bytes = r.bytes;
        info.slack = r.slack;
        out.push_back(std::move(info));
    }
}
} // namespace gpu
