// RootSignature: CONSTANTS(b0,count=4) TABLE(SRV(t0)) TABLE(UAV(u0))

cbuffer FoamParams : register(b0)
{
    uint Resolution;
    uint CascadesCount;
    float DeltaTime;
    float DecayRate;
};

Texture2DArray<float4> DisplacementDerivatives : register(t0);
RWTexture2DArray<float4> FoamTurbulence : register(u0);

float4 LoadDerivativeSlice(uint2 coord, uint cascade, uint slice)
{
    return DisplacementDerivatives.Load(int4(coord, cascade * 2u + slice, 0));
}

float4 SimulateFoamForCascade(uint2 coord, uint cascade)
{
    float4 deriv0 = LoadDerivativeSlice(coord, cascade, 0);
    float4 deriv1 = LoadDerivativeSlice(coord, cascade, 1);

    float jxx = 1.0f + deriv1.z;
    float jzz = 1.0f + deriv1.w;
    float jxz = deriv0.w;

    float jacobian = jxx * jzz - jxz * jxz;
    float delta = jxx - jzz;
    float root = sqrt(max(delta * delta + 4.0f * jxz * jxz, 0.0f));
    float jminus = 0.5f * (jxx + jzz - root);

    float2 current = float2(-jminus, -jacobian) + 1.0f;

    float4 previous = FoamTurbulence[int3(coord, cascade)];
    float2 persistent = previous.zw;
    persistent = max(current, persistent - DecayRate * DeltaTime);

    return float4(current, persistent);
}

[numthreads(8, 8, 1)]
void SimulateFoam(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= Resolution || dispatchThreadId.y >= Resolution || dispatchThreadId.z >= CascadesCount)
    {
        return;
    }

    FoamTurbulence[dispatchThreadId] = SimulateFoamForCascade(dispatchThreadId.xy, dispatchThreadId.z);
}

[numthreads(8, 8, 1)]
void InitializeFoam(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= Resolution || dispatchThreadId.y >= Resolution || dispatchThreadId.z >= CascadesCount)
    {
        return;
    }

    FoamTurbulence[dispatchThreadId] = float4(-5.0f, -5.0f, -5.0f, -5.0f);
}
