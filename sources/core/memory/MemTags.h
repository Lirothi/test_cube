#pragma once
// Memory tags -- who owns a byte (memory profiling plan, docs/memory_profiling_plan.md §4.1, agreed
// with the owner 2026-10-22).
//
// A tag is the OWNER by meaning, never the kind: an upload buffer, a readback buffer or a
// descriptor heap goes to the tag of whoever created it; texture/buffer and local/non-local are
// columns of the census, not tags. One tag spans CPU and GPU (the `ocean` row carries the CPU h0
// data and the GPU simulation). Names are short and dotted, like the providers of the memory line;
// the parent is the prefix before the dot, at most two levels (UE derives the parent from the tag
// path the same way, LLM.cpp:3045-3089). A closed set, uint8, like ELLMTag without dynamic tags.
//
// A new owner of GPU resources gets a row here before its first gpu::Create* call.

#include <cstdint>

// X(Id, "name", Parent) -- Parent is another Id, or Root for a top-level row.
#define MEM_TAG_LIST(X)                                  \
    X(Untagged,       "untagged",        Root)           \
    X(Frame,          "frame",           Root)           \
    X(FrameGbuffer,   "frame.gbuffer",   Frame)          \
    X(FrameColor,     "frame.color",     Frame)          \
    X(FrameUpload,    "frame.upload",    Frame)          \
    X(FrameSwapchain, "frame.swapchain", Frame)          \
    X(Tex,            "tex",             Root)           \
    X(TexMaterial,    "tex.material",    Tex)            \
    X(TexStream,      "tex.stream",      Tex)            \
    X(TexDecode,      "tex.decode",      Tex)            \
    X(Mesh,           "mesh",            Root)           \
    X(MeshInstances,  "mesh.instances",  Mesh)           \
    X(Shadow,         "shadow",          Root)           \
    X(ShadowCsm,      "shadow.csm",      Shadow)         \
    X(ShadowSdsm,     "shadow.sdsm",     Shadow)         \
    X(ShadowVsm,      "shadow.vsm",      Shadow)         \
    X(ShadowLocal,    "shadow.local",    Shadow)         \
    X(ShadowGpuData,  "shadow.gpudata",  Shadow)         \
    X(Rt,             "rt",              Root)           \
    X(RtAs,           "rt.as",           Rt)             \
    X(RtBindless,     "rt.bindless",     Rt)             \
    X(Reflect,        "reflect",         Root)           \
    X(Ao,             "ao",              Root)           \
    X(Bloom,          "bloom",           Root)           \
    X(Fog,            "fog",             Root)           \
    X(Shafts,         "shafts",          Root)           \
    X(Sky,            "sky",             Root)           \
    X(Cloud,          "cloud",           Root)           \
    X(Ocean,          "ocean",           Root)           \
    X(Lights,         "lights",          Root)           \
    X(Exposure,       "exposure",        Root)           \
    X(Dlss,           "dlss",            Root)           \
    X(Vis,            "vis",             Root)           \
    X(Vfx,            "vfx",             Root)           \
    X(Text,           "text",            Root)           \
    X(Ui,             "ui",              Root)           \
    X(Editor,         "editor",          Root)           \
    X(EditorImport,   "editor.import",   Editor)         \
    X(EditorIntent,   "editor.intent",   Editor)         \
    X(Renderer,       "renderer",        Root)           \
    X(Scene,          "scene",           Root)           \
    X(Tasks,          "tasks",           Root)           \
    X(Log,            "log",             Root)           \
    X(Profiler,       "profiler",        Root)           \
    X(Memtrack,       "memtrack",        Root)

enum class MemTag : std::uint8_t
{
#define MEM_TAG_ENUM(id, name, parent) id,
    MEM_TAG_LIST(MEM_TAG_ENUM)
#undef MEM_TAG_ENUM
    Count,
    Root = 0xFF, // "no parent" -- never a tag of its own
};

inline constexpr std::uint32_t kMemTagCount = static_cast<std::uint32_t>(MemTag::Count);

inline constexpr const char* kMemTagNames[] = {
#define MEM_TAG_NAME(id, name, parent) name,
    MEM_TAG_LIST(MEM_TAG_NAME)
#undef MEM_TAG_NAME
};

inline constexpr MemTag kMemTagParents[] = {
#define MEM_TAG_PARENT(id, name, parent) MemTag::parent,
    MEM_TAG_LIST(MEM_TAG_PARENT)
#undef MEM_TAG_PARENT
};

static_assert(sizeof(kMemTagNames) / sizeof(kMemTagNames[0]) == kMemTagCount);

inline const char* MemTagName(MemTag tag)
{
    const auto i = static_cast<std::uint32_t>(tag);
    return i < kMemTagCount ? kMemTagNames[i] : "?";
}

inline MemTag MemTagParent(MemTag tag)
{
    const auto i = static_cast<std::uint32_t>(tag);
    return i < kMemTagCount ? kMemTagParents[i] : MemTag::Root;
}
