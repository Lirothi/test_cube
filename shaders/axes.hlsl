// RootSignature: CBV(b0)
#pragma pack_matrix(row_major)

cbuffer MVP_Axis : register(b0)
{
    float4x4 modelViewProj;
    float4 viewportThickness; // xy = viewport (w,h), z = thicknessPx, w = pad
};

struct VSIn
{
    float3 a : POSITION; // начало
    float3 b : POSITION1; // конец
    float3 corner : TEXCOORD0; // xy = (-1|+1, -1|+1), z = edgeBiasPx
    float4 color : COLOR;
};
struct VSOut
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

// Клип к near-плоскости z=0 в clip-space, линейной интерполяцией между P и Q.
// Если оба конца за near, вернёмся в минимум — треугольник задеградирует и ничего не нарисуется.
static void ClipEndToNear(inout float4 P, float4 Q)
{
    // если P за near (z<0), подвинем его к пересечению с плоскостью z=0
    if (P.z < 0.0f)
    {
        float denom = (Q.z - P.z);
        // если отрезок почти параллелен/дегенеративен — дальше всё равно задеградирует
        if (abs(denom) > 1e-8f)
        {
            float t = (-P.z) / denom; // t in [0..1] обычно
            t = clamp(t, 0.0f, 1.0f);
            P = lerp(P, Q, t);
        }
    }
}

VSOut VSMain(VSIn i)
{
    VSOut o;

    // clip-концы
    float4 A = mul(float4(i.a, 1.0f), modelViewProj);
    float4 B = mul(float4(i.b, 1.0f), modelViewProj);

    // near-clip (z >= 0)
    ClipEndToNear(A, B);
    ClipEndToNear(B, A);

    float wA = max(A.w, 1e-6f);
    float wB = max(B.w, 1e-6f);

    float2 Andc = A.xy / wA;
    float2 Bndc = B.xy / wB;

    // экранная толщина
    float2 viewport = max(viewportThickness.xy, float2(1.0f, 1.0f));
    float thickPx = max(viewportThickness.z, 0.5f);
    float2 px2ndc = 2.0 / viewport;

    float2 dir_ndc = Bndc - Andc;
    float2 dir_px = dir_ndc / px2ndc;
    float len_px = max(length(dir_px), 1e-5f);
    float2 t_px = dir_px / len_px;
    float2 n_px = float2(-t_px.y, t_px.x);
    float2 off_ndc = n_px * (0.5f * thickPx) * px2ndc;

    // продольная координата вдоль оси: 0 на A-краю, 1 на B-краю
    float v = (i.corner.y < 0.0f) ? 0.0f : 1.0f;

    // базовая точка по продольной координате (в NDC)
    float2 base = lerp(Andc, Bndc, v);

    // боковой сдвиг
    float2 ndc = base + off_ndc * i.corner.x;

    // ВНИМАНИЕ: z и w — линейная интерполяция между A и B по той же v!
    float w = max(lerp(wA, wB, v), 1e-6f);
    float z = lerp(A.z, B.z, v);

    o.position = float4(ndc * w, z, w);
    o.color = i.color;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    return i.color;
}