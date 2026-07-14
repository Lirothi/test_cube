// Single translation unit that compiles the cgltf implementation.
// The header is included for its API everywhere else; the implementation must
// live in exactly one .cpp (this one). See A2 in docs/tropical_atoll_scene_plan.md.
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
