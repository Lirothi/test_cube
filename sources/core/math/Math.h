#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <DirectXMath.h>

namespace Math
{
    using namespace DirectX;

    // --- Constants ---
    constexpr float  PI        = 3.14159265358979323846f;
    constexpr float  TWO_PI    = 6.28318530717958647692f;
    constexpr float  HALF_PI   = 1.57079632679489661923f;
    constexpr float  DEG2RAD   = PI / 180.0f;
    constexpr float  RAD2DEG   = 180.0f / PI;
    constexpr float  EPS       = 1e-6f;

    // --- Utilities (scalars) ---
    template<typename T> inline T Clamp(T v, T lo, T hi) {
        if (v < lo) { return lo; }
        if (v > hi) { return hi; }
        return v;
    }
    inline float Saturate(float v) { return Clamp(v, 0.0f, 1.0f); }

    template<typename T> inline T Lerp(const T& a, const T& b, float t) {
        return T(a + (b - a) * t);
    }
    inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

    inline float SmoothStep(float a, float b, float t) {
        if (t <= 0.0f) { return a; }
        if (t >= 1.0f) { return b; }
        const float x = t * t * (3.0f - 2.0f * t);
        return a + (b - a) * x;
    }

    inline uint32_t FloatToUint32(float value) {
        static_assert(sizeof(uint32_t) == sizeof(float), "FloatToUint32 requires 32-bit float");
        uint32_t result;
        std::memcpy(&result, &value, sizeof(result));
        return result;
    }

    inline float Step(float edge, float x) { return x < edge ? 0.0f : 1.0f; }

    inline float Remap(float v, float inMin, float inMax, float outMin, float outMax) {
        const float t = (v - inMin) / (inMax - inMin + EPS);
        return outMin + (outMax - outMin) * t;
    }

    inline bool NearlyEqual(float a, float b, float eps = EPS) {
        return std::fabs(a - b) <= eps * std::max(1.0f, std::max(std::fabs(a), std::fabs(b)));
    }

    // --- Vectors: lightweight wrappers over XMFLOAT* + helpers ---
    struct float2 {
        float x, y;
        float2() : x(0), y(0) {}
        float2(float _x, float _y) : x(_x), y(_y) {}
        float2(float _v) : x(_v), y(_v) {}
        explicit float2(const XMFLOAT2& v) : x(v.x), y(v.y) {}
        XMFLOAT2 xf() const { return XMFLOAT2(x, y); }
        XMVECTOR xm() const { return DirectX::XMVectorSet(x, y, 0.0f, 0.0f); }
        static float2 FromXM(XMVECTOR v) { XMFLOAT2 t{}; XMStoreFloat2(&t, v); return float2(t); }

        float2 operator+(const float2& o) const { return { x + o.x, y + o.y }; }
        float2 operator-(const float2& o) const { return { x - o.x, y - o.y }; }
        float2 operator*(float s) const { return { x * s, y * s }; }
        float2 operator/(float s) const { return { x / s, y / s }; }
        float  Dot(const float2& o) const { return x * o.x + y * o.y; }
        float  Length() const { return std::sqrt(x * x + y * y); }
        float2 Normalized() const {
            const float len = Length();
            if (len < EPS) { return { 0, 0 }; }
            return { x / len, y / len };
        }
        static float2 Lerp(const float2& a, const float2& b, float t) { return a + (b - a) * t; }
    };

    struct float3 {
        float x, y, z;
        float3() : x(0), y(0), z(0) {}
        float3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
        float3(float _v) : x(_v), y(_v), z(_v) {}
        explicit float3(const XMFLOAT3& v) : x(v.x), y(v.y), z(v.z) {}
        float2 xy() const { return { x,y }; }
        XMFLOAT3 xf() const { return XMFLOAT3(x, y, z); }
        XMVECTOR xm() const { return DirectX::XMVectorSet(x, y, z, 0.0f); }
        static float3 FromXM(XMVECTOR v) { XMFLOAT3 t{}; XMStoreFloat3(&t, v); return float3(t); }

        float3 operator+(const float3& o) const { return { x + o.x, y + o.y, z + o.z }; }
        float3& operator+=(const float3& o) { x += o.x; y += o.y; z += o.z; return *this; }
        
        float3 operator-(const float3& o) const { return { x - o.x, y - o.y, z - o.z }; }
        float3& operator-=(const float3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
        
        float3 operator*(float s) const { return { x * s, y * s, z * s }; }
        float3 operator*(const float3& s) const { return { x * s.x, y * s.y, z * s.z }; }
        float3& operator*= (const float3 & o){ x *= o.x; y *= o.y; z *= o.z; return *this; }
        
        float3 operator/(float s) const { return { x / s, y / s, z / s }; }
        float3 operator/(const float3& s) const { return { x / s.x, y / s.y, z / s.z }; }
        float3& operator/= (const float3& o) { x /= o.x; y /= o.y; z /= o.z; return *this; }

        float  Dot(const float3& o) const { return x * o.x + y * o.y + z * o.z; }
        float3 Cross(const float3& o) const { return FromXM(XMVector3Cross(xm(), o.xm())); }
        float  Length() const { return std::sqrt(x * x + y * y + z * z); }
        float3 Normalized() const {
            const float len = Length();
            if (len < EPS) { return { 0, 0, 0 }; }
            return { x / len, y / len, z / len };
        }
        static float3 Lerp(const float3& a, const float3& b, float t) { return a + (b - a) * t; }
        static float3 Min(const float3& a, const float3& b) { return { std::min(a.x,b.x), std::min(a.y,b.y), std::min(a.z,b.z) }; }
        static float3 Max(const float3& a, const float3& b) { return { std::max(a.x,b.x), std::max(a.y,b.y), std::max(a.z,b.z) }; }
    };

    struct float4 {
        float x, y, z, w;
        float4() : x(0), y(0), z(0), w(0) {}
        float4(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}
        float4(const float3& _v, float _w) : x(_v.x), y(_v.y), z(_v.z), w(_w) {}
        explicit float4(const XMFLOAT4& v) : x(v.x), y(v.y), z(v.z), w(v.w) {}
        float2 xy() const { return { x,y }; }
        float3 xyz() const { return { x,y,z }; }
        XMFLOAT4 xf() const { return XMFLOAT4(x, y, z, w); }
        XMVECTOR xm() const { return DirectX::XMVectorSet(x, y, z, w); }
        static float4 FromXM(XMVECTOR v) { XMFLOAT4 t{}; XMStoreFloat4(&t, v); return float4(t); }

        float4 operator+(const float4& o) const { return { x + o.x, y + o.y, z + o.z, w + o.w }; }
        float4 operator-(const float4& o) const { return { x - o.x, y - o.y, z - o.z, w - o.w }; }
        float4 operator*(float s) const { return { x * s, y * s, z * s, w * s }; }
        float4 operator*(const float4& s) const { return { x * s.x, y * s.y, z * s.z, w * s.w }; }
        float4 operator/(float s) const { return { x / s, y / s, z / s, w / s }; }
        float4 operator/(const float4& s) const { return { x / s.x, y / s.y, z / s.z, w / s.w }; }
    };

    // --- Color (float and RGBA8) ---
    inline uint32_t PackRGBA(float r, float g, float b, float a = 1.0f) {
        const uint32_t R = (uint32_t)(Saturate(r) * 255.0f + 0.5f);
        const uint32_t G = (uint32_t)(Saturate(g) * 255.0f + 0.5f);
        const uint32_t B = (uint32_t)(Saturate(b) * 255.0f + 0.5f);
        const uint32_t A = (uint32_t)(Saturate(a) * 255.0f + 0.5f);
        return (R << 24) | (G << 16) | (B << 8) | (A);
    }
    inline float4 UnpackRGBA(uint32_t rgba) {
        const float r = ((rgba >> 24) & 0xFF) / 255.0f;
        const float g = ((rgba >> 16) & 0xFF) / 255.0f;
        const float b = ((rgba >> 8)  & 0xFF) / 255.0f;
        const float a = ( rgba        & 0xFF) / 255.0f;
        return float4(r,g,b,a);
    }

    // --- Quaternion ---
    struct quat {
        float x, y, z, w; // (x,y,z,w)
        quat() : x(0), y(0), z(0), w(1) {}
        quat(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}
        explicit quat(const XMFLOAT4& q) : x(q.x), y(q.y), z(q.z), w(q.w) {}
        XMFLOAT4 xf() const { return XMFLOAT4(x,y,z,w); }
        XMVECTOR xm() const { return DirectX::XMVectorSet(x, y, z, w); }
        static quat FromXM(FXMVECTOR q) { XMFLOAT4 t{}; XMStoreFloat4(&t, q); return quat(t); }

        static quat Identity() { return quat(0,0,0,1); }
        static quat FromAxisAngle(const float3& axis, float radians) {
            return FromXM(XMQuaternionRotationAxis(axis.xm(), radians));
        }
        static quat FromYawPitchRoll(float yaw, float pitch, float roll) {
            return FromXM(XMQuaternionRotationRollPitchYaw(pitch, yaw, roll));
        }
        quat Normalized() const {
            return FromXM(XMQuaternionNormalize(xm()));
        }
        static quat Slerp(const quat& a, const quat& b, float t) {
            return FromXM(XMQuaternionSlerp(a.xm(), b.xm(), t));
        }
        static quat Multiply(const quat& a, const quat& b) {
            return FromXM(XMQuaternionMultiply(a.xm(), b.xm()));
        }
    };

    // --- 4x4 matrix (row-major interface, DirectX LH) ---
    struct mat4 {
        XMFLOAT4X4 m;
        mat4() { XMStoreFloat4x4(&m, XMMatrixIdentity()); }
        explicit mat4(const XMFLOAT4X4& _m) : m(_m) {}

        static mat4 Identity() {
            mat4 r; XMStoreFloat4x4(&r.m, XMMatrixIdentity()); return r;
        }
        static mat4 Translation(const float3& t) {
            mat4 r; XMStoreFloat4x4(&r.m, XMMatrixTranslation(t.x, t.y, t.z)); return r;
        }
        static mat4 Scaling(float sx, float sy, float sz) {
            mat4 r; XMStoreFloat4x4(&r.m, XMMatrixScaling(sx, sy, sz)); return r;
        }
        static mat4 Scaling(float3 s) {
            return Scaling(s.x, s.y, s.z);
        }
        static mat4 RotationX(float r) { mat4 o; XMStoreFloat4x4(&o.m, XMMatrixRotationX(r)); return o; }
        static mat4 RotationY(float r) { mat4 o; XMStoreFloat4x4(&o.m, XMMatrixRotationY(r)); return o; }
        static mat4 RotationZ(float r) { mat4 o; XMStoreFloat4x4(&o.m, XMMatrixRotationZ(r)); return o; }
        // Rotation from Euler angles (DX: Roll=X? No — see argument order)
        static mat4 RotationRollPitchYaw(float pitch, float yaw, float roll) {
            mat4 o;
            DirectX::XMStoreFloat4x4(&o.m, DirectX::XMMatrixRotationRollPitchYaw(pitch, yaw, roll));
            return o;
        }
        // Same variants with degree input
        static mat4 RotationRollPitchYawDegrees(float pitchDeg, float yawDeg, float rollDeg) {
            return RotationRollPitchYaw(pitchDeg * Math::DEG2RAD, yawDeg * Math::DEG2RAD, rollDeg * Math::DEG2RAD);
        }
        static mat4 RotationYawPitchRollDegrees(float yawDeg, float pitchDeg, float rollDeg) {
            return RotationRollPitchYawDegrees(pitchDeg, yawDeg, rollDeg);
        }
        static mat4 RotationFromEulerXYZRad(const float3& e)
        {
            const XMMATRIX Rx = XMMatrixRotationX(e.x);
            const XMMATRIX Ry = XMMatrixRotationY(e.y);
            const XMMATRIX Rz = XMMatrixRotationZ(e.z);

            mat4 out;
            XMStoreFloat4x4(&out.m, XMMatrixMultiply(XMMatrixMultiply(Rx, Ry), Rz));
            return out;
        }
        static mat4 FromQuaternion(const quat& q) {
            mat4 r; XMStoreFloat4x4(&r.m, XMMatrixRotationQuaternion(q.xm())); return r;
        }
        static mat4 TRS(const float3& t, const quat& q, const float3& s) {
            mat4 r;
            XMStoreFloat4x4(&r.m,
                XMMatrixScaling(s.x, s.y, s.z) *
                XMMatrixRotationQuaternion(q.xm()) *
                XMMatrixTranslation(t.x, t.y, t.z));
            return r;
        }

        static mat4 LookAtLH(const float3& eye, const float3& at, const float3& up) {
            mat4 r; XMStoreFloat4x4(&r.m, XMMatrixLookAtLH(eye.xm(), at.xm(), up.xm())); return r;
        }
        static mat4 PerspectiveFovLH(float fovY, float aspect, float zn, float zf) {
            mat4 r; XMStoreFloat4x4(&r.m, XMMatrixPerspectiveFovLH(fovY, aspect, zn, zf)); return r;
        }
        static mat4 OrthoLH(float w, float h, float zn, float zf) {
            mat4 r; XMStoreFloat4x4(&r.m, XMMatrixOrthographicLH(w, h, zn, zf)); return r;
        }
        static mat4 OrthoOffCenterLH(float ViewLeft, float ViewRight, float ViewBottom, float ViewTop, float NearZ, float FarZ) {
            mat4 r; XMStoreFloat4x4(&r.m, XMMatrixOrthographicOffCenterLH(ViewLeft, ViewRight, ViewBottom, ViewTop, NearZ, FarZ)); return r;
        }

        XMMATRIX xm() const { return XMLoadFloat4x4(&m); }
        static mat4 Multiply(const mat4& a, const mat4& b) {
            mat4 r; XMStoreFloat4x4(&r.m, XMMatrixMultiply(a.xm(), b.xm())); return r;
        }
        static mat4 Transpose(const mat4& a) {
            mat4 r; XMStoreFloat4x4(&r.m, XMMatrixTranspose(a.xm())); return r;
        }
        static mat4 Inverse(const mat4& a) {
            mat4 r; XMVECTOR det; XMStoreFloat4x4(&r.m, XMMatrixInverse(&det, a.xm())); return r;
        }

        // Point (x,y,z,1): equivalent to XMVector3TransformCoord
        float3 TransformPoint(const float3& p) const {
            using namespace DirectX;
            return float3::FromXM(XMVector3TransformCoord(p.xm(), this->xm()));
        }

        // Direction (x,y,z,0): equivalent to XMVector3TransformNormal (ignores translation)
        float3 TransformDirection(const float3& v) const {
            using namespace DirectX;
            return float3::FromXM(XMVector3TransformNormal(v.xm(), this->xm()));
        }

        // Full 4D vector: equivalent to XMVector4Transform
        float4 Transform(const float4& v) const {
            using namespace DirectX;
            return float4::FromXM(XMVector4Transform(v.xm(), this->xm()));
        }

        // Normal with non-uniform scale: multiply by inverse-transpose(3x3)
        float3 TransformNormalSafe(const float3& n) const {
            using namespace DirectX;
            XMMATRIX invT = XMMatrixTranspose(XMMatrixInverse(nullptr, this->xm()));
            return float3::FromXM(XMVector3TransformNormal(n.xm(), invT));
        }

        // Matrix multiplication operators (matching your existing ones) plus vector helpers:
        float3 operator*(const float3& p) const { // m * p (treated as a point)
            return TransformPoint(p);
        }
        float4 operator*(const float4& v) const { // m * v (4D)
            return Transform(v);
        }

        mat4 operator*(const mat4& rhs) const {
            mat4 r;
            DirectX::XMStoreFloat4x4(&r.m, DirectX::XMMatrixMultiply(this->xm(), rhs.xm()));
            return r;
        }

        // Accumulated multiplication: this = this * rhs
        mat4& operator*=(const mat4& rhs) {
            DirectX::XMMATRIX mm = DirectX::XMMatrixMultiply(this->xm(), rhs.xm());
            DirectX::XMStoreFloat4x4(&this->m, mm);
            return *this;
        }
    };

    // --- Additional vector utilities ---
    inline float  Dot(const float3& a, const float3& b) { return a.Dot(b); }
    inline float3 Cross(const float3& a, const float3& b) { return a.Cross(b); }
    inline float3 Normalize(const float3& v) { return v.Normalized(); }
    inline float2 Normalize(const float2& v) { return v.Normalized(); }

    inline float3 Lerp(const float3& a, const float3& b, float t) { return float3::Lerp(a, b, t); }
    inline float2 Lerp(const float2& a, const float2& b, float t) { return float2::Lerp(a, b, t); }
    inline float2 Abs(const float2& a) { return float2{ std::abs(a.x), std::abs(a.y) }; }
    inline float3 Abs(const float3& a) { return float3{ std::abs(a.x), std::abs(a.y), std::abs(a.z) }; }
    inline float3 Floor(const float3& a) { return float3{ std::floor(a.x), std::floor(a.y), std::floor(a.z) }; }

    // --- Free functions (analogues of DirectXMath) ---
    inline float3 TransformPoint(const float3& p, const mat4& m) {
        return m.TransformPoint(p);
    }
    inline float3 TransformDirection(const float3& v, const mat4& m) {
        return m.TransformDirection(v);
    }
    inline float3 TransformNormalSafe(const float3& n, const mat4& m) {
        return m.TransformNormalSafe(n);
    }
    inline float4 Transform(const float4& v, const mat4& m) {
        return m.Transform(v);
    }

    // --- Random helpers (minimal) ---
    inline float Rand01(uint32_t& state) {
        // xorshift32
        if (state == 0u) { state = 0x12345678u; }
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (state & 0x00FFFFFFu) / 16777215.0f;
    }
    inline float3 RandUnitSphere(uint32_t& state) {
        // Simple sampling
        float z = Rand01(state) * 2.0f - 1.0f;
        float t = Rand01(state) * TWO_PI;
        float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        return { r * std::cos(t), r * std::sin(t), z };
    }
} // namespace Math

using namespace Math;