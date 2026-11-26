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

// A C++ default of a shared field: `float skyIrradianceScale TC_INIT(1.0f);` is `= 1.0f` in C++ and nothing in HLSL
// (a cbuffer has no initial values; the C++ mirrors kept theirs, which the uploads rely on). ONE argument, no comma
// outside parentheses -- `TC_INIT(uint4(1, 0, 0, 0))`, never `TC_INIT({ 1, 0, 0, 0 })`: FXC (Material's SM5 fallback,
// the editor preview) has no variadic macros. TC_ROW_MAJOR is HLSL's row_major (C++'s Math::mat4 is row-major storage
// already).
#ifdef __cplusplus
#include <cstddef>
#include <cstdint>
#include "core/math/Math.h"

#define TC_INIT(value) = value
#define TC_ROW_MAJOR

namespace shader_shared
{
using float2 = Math::float2;
using float3 = Math::float3;
using float4 = Math::float4;
using float4x4 = Math::mat4;
using uint = std::uint32_t;
using uint2 = Math::uint2;
using int2 = Math::int2;

// HLSL's int3 / int4 / uint3 / uint4: x..w, and an index (the mirrors before R5.3 were int32_t[4] filled by index).
// `{}` is zero (the defaulted constructor value-initializes); `uint4(1, 0, 0, 0)` or `{ 1, 0, 0, 0 }` sets each.
template<class T, int N> struct IntVector;
template<class T> struct IntVector<T, 3>
{
    T x, y, z;
    IntVector() = default;
    constexpr IntVector(T x_, T y_, T z_) : x(x_), y(y_), z(z_) {}
    T& operator[](std::size_t i) { return i == 0 ? x : (i == 1 ? y : z); }
    const T& operator[](std::size_t i) const { return i == 0 ? x : (i == 1 ? y : z); }
};
template<class T> struct IntVector<T, 4>
{
    T x, y, z, w;
    IntVector() = default;
    constexpr IntVector(T x_, T y_, T z_, T w_) : x(x_), y(y_), z(z_), w(w_) {}
    T& operator[](std::size_t i) { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }
    const T& operator[](std::size_t i) const { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }
};
using int3 = IntVector<std::int32_t, 3>;
using int4 = IntVector<std::int32_t, 4>;
using uint3 = IntVector<std::uint32_t, 3>;
using uint4 = IntVector<std::uint32_t, 4>;
static_assert(sizeof(int4) == 16 && sizeof(uint3) == 12, "HLSL's integer vectors are packed 4-byte components");
}
#else
#define TC_INIT(value)
#define TC_ROW_MAJOR row_major
#endif

#endif
