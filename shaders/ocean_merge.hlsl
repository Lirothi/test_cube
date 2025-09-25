// RootSignature: CONSTANTS(b0,count=2) TABLE(SRV(t0) SRV(t1) SRV(t2) SRV(t3)) TABLE(UAV(u4) UAV(u5) UAV(u6))
cbuffer MergeParams : register(b0)
{
    float Lambda;
    float DeltaTime;
};

RWTexture2D<float3> Displacement : register(u4);
RWTexture2D<float4> Derivatives : register(u5);
RWTexture2D<float4> Turbulence : register(u6);

Texture2D<float2> Dx_Dz : register(t0);
Texture2D<float2> Dy_Dxz : register(t1);
Texture2D<float2> Dyx_Dyz : register(t2);
Texture2D<float2> Dxx_Dzz : register(t3);

[numthreads(8, 8, 1)]
void FillResultTextures(uint3 id : SV_DispatchThreadID)
{
    float2 DxDz = Dx_Dz[id.xy];
    float2 DyDxz = Dy_Dxz[id.xy];
    float2 DyxDyz = Dyx_Dyz[id.xy];
    float2 DxxDzz = Dxx_Dzz[id.xy];

    Displacement[id.xy] = float3(Lambda * DxDz.x, DyDxz.x, Lambda * DxDz.y);
    Derivatives[id.xy] = float4(DyxDyz, DxxDzz * Lambda);

    float jacobian = (1.0f + Lambda * DxxDzz.x) * (1.0f + Lambda * DxxDzz.y) - Lambda * Lambda * DyDxz.y * DyDxz.y;
    float current = Turbulence[id.xy].x;
    float updated = current + DeltaTime * 0.5f / max(jacobian, 0.5f);
    updated = min(jacobian, updated);
    Turbulence[id.xy] = float4(updated, updated, updated, updated);
}
