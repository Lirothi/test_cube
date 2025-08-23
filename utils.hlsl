#pragma pack_matrix(row_major)

static const uint kRM_RBits = 5u; // roughness
static const uint kRM_MBits = 3u; // metallic
static const uint kRM_MaxU8 = 255u;
static const uint kRM_MMask = (1u << kRM_MBits) - 1u;
static const float kRM_RScale = float((1u << kRM_RBits) - 1u);
static const float kRM_MScale = float((1u << kRM_MBits) - 1u);

// pack [0..1]x[0..1] -> A8_UNORM
float PackRM(float rough, float metal)
{
    uint r = (uint) round(saturate(rough) * kRM_RScale);
    uint m = (uint) round(saturate(metal) * kRM_MScale);
    uint packed = (r << kRM_MBits) | m; // [rrrrr][mmm]
    return float(packed) / float(kRM_MaxU8);
}