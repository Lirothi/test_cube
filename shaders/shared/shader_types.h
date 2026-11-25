// One layout for C++ and HLSL (architecture plan R5): a header under shaders/shared/ is compiled by both. HLSL has its
// own float2..float4x4 and uint; C++ gets the same names from core/math, in namespace shader_shared, so a constant
// buffer written once here is the shader's cbuffer and the C++ upload's struct byte for byte (a static_assert on its
// size stays at the upload, as before). Packing is HLSL's: a member never straddles a 16-byte row, so a C++ mirror
// keeps the rows full the way the existing *Constants structs already do.
//
// HLSL includes it as "shared/shader_types.h" (the include handler resolves from shaders/); C++ as
// "shaders/shared/shader_types.h" ($(ProjectDir) is on the include path). A shipped build packs shaders/shared/*.h
// with the sources (Material::CookShaders).
#ifndef TC_SHADERS_SHARED_SHADER_TYPES_H
#define TC_SHADERS_SHARED_SHADER_TYPES_H

// A register named by a number a shared header gives (architecture plan R5.3): `Texture2D GB0 : TC_REGISTER(t,
// LIGHTING_T_GB0);` is `register(t0)`. The two levels let the slot macro expand before the paste.
#define TC_REGISTER_PASTE(kind, slot) register(kind##slot)
#define TC_REGISTER(kind, slot) TC_REGISTER_PASTE(kind, slot)

#ifdef __cplusplus
#include <cstdint>
#include "core/math/Math.h"

namespace shader_shared
{
using float2 = Math::float2;
using float3 = Math::float3;
using float4 = Math::float4;
using float4x4 = Math::mat4;
using uint = std::uint32_t;
#define TC_SHARED_BEGIN namespace shader_shared {
#define TC_SHARED_END }
}
#else
#define TC_SHARED_BEGIN
#define TC_SHARED_END
#endif

TC_SHARED_BEGIN
// R5.1's probe: the layout of water_composite_cs.hlsl's WaterCompositeCB (a matrix, then three full rows), checked from
// C++ (SceneRenderer_Reflections.cpp) against the 112 bytes the upload's struct has. It proves both compilers read this
// file the same way before a real constant buffer moves here (R5.3).
struct SharedLayoutProbe
{
    float4x4 m;
    float3 a;
    float b;
    float2 c;
    float d;
    uint e;
    float4 f;
};
TC_SHARED_END

#endif
