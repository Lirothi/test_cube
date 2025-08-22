//RootSignature: TABLE(SRV(t0)) TABLE(SAMPLER(s0))
Texture2D HDRColor : register(t0);
SamplerState gSmp  : register(s0);

struct VSOut { float4 H:SV_POSITION; float2 UV:TEXCOORD0; };

VSOut VSMain(uint vid : SV_VertexID)
{
    VSOut o;
    float2 p = float2(vid == 2 ? 3 : -1, vid == 1 ? 3 : -1);
    o.H = float4(p, 0, 1);
    o.UV = float2(p.x * .5 + .5, 1.0 - (p.y * .5 + .5));
    return o;
}

// ---- named constants ----
static const float kGammaOut = 2.2;
static const float kDitherAmplitude = 1.0 / 255.0; // достаточно, чтобы сломать бэндинг
static const float kEps = 1e-6;

// ACES fitted (K. Narkowicz)
float3 TonemapACES(float3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// стабильный cheap hash от координаты пикселя
float Dither(uint2 p)
{
    // [0,1)
    float n = frac(sin(dot(float2(p), float2(12.9898, 78.233))) * 43758.5453);
    return n - 0.5; // [-0.5, 0.5)
}

float4 PSMain(VSOut i) : SV_Target
{
    float3 hdr = HDRColor.Sample(gSmp, i.UV).rgb;

    float3 mapped = TonemapACES(hdr);
    float3 ldr = pow(max(mapped, 0.0.xxx), 1.0 / max(kEps, kGammaOut));

    // добавим одинаковый шум на каналы — достаточно, чтобы сломать полосы
    uint2 pix = (uint2) i.H.xy;
    float d = Dither(pix) * kDitherAmplitude;
    ldr += d;

    return float4(saturate(ldr), 1.0);
}