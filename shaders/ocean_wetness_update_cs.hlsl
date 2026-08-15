#define OCEAN_WETNESS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

Texture2D<uint> PreviousWetness : register(t0);
Texture2D<uint> PreviousStamp : register(t1);
RWTexture2D<uint> NextWetness : register(u0);
RWTexture2D<uint> NextStamp : register(u1);

cbuffer WetnessUpdateCB : register(b0)
{
    uint Resolution;
    float WetAmount;
    float DryAmount;
    uint ClearHistory;
    int ShiftX;
    int ShiftY;
};

[numthreads(8, 8, 1)]
[RootSignature(OCEAN_WETNESS_RS)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 destination = dispatchThreadId.xy;
    if (destination.x >= Resolution || destination.y >= Resolution)
    {
        return;
    }

    // Keep the history world-anchored while the camera-centred window moves.
    const int2 source = int2(destination) + int2(ShiftX, ShiftY);
    float wetness = 0.0f;
    float stamp = 0.0f;
    if (ClearHistory == 0u && all(source >= 0) && all(source < int2(Resolution, Resolution)))
    {
        wetness = PreviousWetness.Load(int3(source, 0)) * (1.0f / 65535.0f);
        stamp = PreviousStamp.Load(int3(source, 0)) * (1.0f / 65535.0f);
    }

    // The ocean writes only a per-frame coverage mask. Integration happens once per texel here,
    // so wetting speed is independent of screen resolution and of how many water fragments map
    // onto the same history texel. While water is present, approach its coverage over Wet Time;
    // after it retreats, decay linearly over Dry Time.
    if (stamp > 0.0f)
    {
        wetness = min(max(wetness, stamp), wetness + saturate(WetAmount));
    }
    else
    {
        wetness = max(wetness - saturate(DryAmount), 0.0f);
    }

    NextWetness[destination] = (uint)round(saturate(wetness) * 65535.0f);
    NextStamp[destination] = 0u;
}
