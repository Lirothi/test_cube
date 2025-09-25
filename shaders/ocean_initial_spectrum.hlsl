// RootSignature: CONSTANTS(b0,count=8) TABLE(SRV(t0) SRV(t1)) TABLE(UAV(u4) UAV(u5) UAV(u6))
static const float PI = 3.14159265358979323846f;

cbuffer InitialSpectrumParams : register(b0)
{
    uint Size;
    float LengthScale;
    float CutoffHigh;
    float CutoffLow;
    float GravityAcceleration;
    float Depth;
    float _pad0;
    float _pad1;
};

Texture2D<float2> Noise : register(t0);

struct SpectrumParameters
{
    float scale;
    float angle;
    float spreadBlend;
    float swell;
    float alpha;
    float peakOmega;
    float gamma;
    float shortWavesFade;
};
StructuredBuffer<SpectrumParameters> Spectrums : register(t1);

RWTexture2D<float4> H0 : register(u4);
RWTexture2D<float4> WavesData : register(u5);
RWTexture2D<float2> H0K : register(u6);

float Frequency(float k, float g, float depth)
{
    return sqrt(g * k * tanh(min(k * depth, 20.0f)));
}

float FrequencyDerivative(float k, float g, float depth)
{
    float th = tanh(min(k * depth, 20.0f));
    float ch = cosh(k * depth);
    return g * (depth * k / (ch * ch) + th) / Frequency(k, g, depth) * 0.5f;
}

float NormalisationFactor(float s)
{
    float s2 = s * s;
    float s3 = s2 * s;
    float s4 = s3 * s;
    if (s < 5.0f)
    {
        return -0.000564f * s4 + 0.00776f * s3 - 0.044f * s2 + 0.192f * s + 0.163f;
    }
    return -4.80e-08f * s4 + 1.07e-05f * s3 - 9.53e-04f * s2 + 5.90e-02f * s + 3.93e-01f;
}

float DonelanBannerBeta(float x)
{
    if (x < 0.95f)
    {
        return 2.61f * pow(abs(x), 1.3f);
    }
    if (x < 1.6f)
    {
        return 2.28f * pow(abs(x), -1.3f);
    }
    float p = -0.4f + 0.8393f * exp(-0.567f * log(x * x));
    return pow(10.0f, p);
}

float DonelanBanner(float theta, float omega, float peakOmega)
{
    float beta = DonelanBannerBeta(omega / peakOmega);
    float sech = 1.0f / cosh(beta * theta);
    return beta * 0.5f / tanh(beta * PI) * sech * sech;
}

float Cosine2s(float theta, float s)
{
    return NormalisationFactor(s) * pow(abs(cos(0.5f * theta)), 2.0f * s);
}

float SpreadPower(float omega, float peakOmega)
{
    if (omega > peakOmega)
    {
        return 9.77f * pow(abs(omega / peakOmega), -2.5f);
    }
    return 6.97f * pow(abs(omega / peakOmega), 5.0f);
}

float DirectionSpectrum(float theta, float omega, SpectrumParameters pars)
{
    float s = SpreadPower(omega, pars.peakOmega) + 16.0f * tanh(min(omega / pars.peakOmega, 20.0f)) * pars.swell * pars.swell;
    return lerp(2.0f / PI * cos(theta) * cos(theta), Cosine2s(theta - pars.angle, s), pars.spreadBlend);
}

float TMACorrection(float omega, float g, float depth)
{
    float omegaH = omega * sqrt(depth / g);
    if (omegaH <= 1.0f)
    {
        return 0.5f * omegaH * omegaH;
    }
    if (omegaH < 2.0f)
    {
        float t = 2.0f - omegaH;
        return 1.0f - 0.5f * t * t;
    }
    return 1.0f;
}

float JONSWAP(float omega, float g, float depth, SpectrumParameters pars)
{
    float sigma = (omega <= pars.peakOmega) ? 0.07f : 0.09f;
    float r = exp(-(omega - pars.peakOmega) * (omega - pars.peakOmega) /
        (2.0f * sigma * sigma * pars.peakOmega * pars.peakOmega));

    float oneOverOmega = 1.0f / omega;
    float peakOmegaOverOmega = pars.peakOmega / omega;
    float powTerm = pow(abs(peakOmegaOverOmega), 4.0f);
    return pars.scale * TMACorrection(omega, g, depth) * pars.alpha * g * g *
        pow(oneOverOmega, 5.0f) * exp(-1.25f * powTerm * powTerm) * pow(abs(pars.gamma), r);
}

float ShortWavesFade(float kLength, SpectrumParameters pars)
{
    return exp(-pars.shortWavesFade * pars.shortWavesFade * kLength * kLength);
}

[numthreads(8, 8, 1)]
void CalculateInitialSpectrum(uint3 id : SV_DispatchThreadID)
{
    float deltaK = 2.0f * PI / LengthScale;
    int nx = int(id.x) - int(Size / 2u);
    int nz = int(id.y) - int(Size / 2u);
    float2 k = float2(nx, nz) * deltaK;
    float kLength = length(k);

    if (kLength <= CutoffHigh && kLength >= CutoffLow)
    {
        float kAngle = atan2(k.y, k.x);
        float omega = Frequency(kLength, GravityAcceleration, Depth);
        WavesData[id.xy] = float4(k.x, 1.0f / max(kLength, 1e-6f), k.y, omega);
        float dOmegadk = FrequencyDerivative(kLength, GravityAcceleration, Depth);

        float spectrum = JONSWAP(omega, GravityAcceleration, Depth, Spectrums[0]) *
            DirectionSpectrum(kAngle, omega, Spectrums[0]) * ShortWavesFade(kLength, Spectrums[0]);
        if (Spectrums[1].scale > 0.0f)
        {
            spectrum += JONSWAP(omega, GravityAcceleration, Depth, Spectrums[1]) *
                DirectionSpectrum(kAngle, omega, Spectrums[1]) * ShortWavesFade(kLength, Spectrums[1]);
        }

        float scale = sqrt(2.0f * spectrum * abs(dOmegadk) / max(kLength, 1e-6f) * deltaK * deltaK);
        H0K[id.xy] = Noise[id.xy] * scale;
    }
    else
    {
        H0K[id.xy] = float2(0.0f, 0.0f);
        WavesData[id.xy] = float4(k.x, 1.0f, k.y, 0.0f);
    }
}

[numthreads(8, 8, 1)]
void CalculateConjugatedSpectrum(uint3 id : SV_DispatchThreadID)
{
    float2 h0K = H0K[id.xy];
    uint2 mirror = uint2((Size - id.x) % Size, (Size - id.y) % Size);
    float2 h0MinusK = H0K[mirror];
    H0[id.xy] = float4(h0K.x, h0K.y, h0MinusK.x, -h0MinusK.y);
}
