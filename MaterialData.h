#pragma once
#include <memory>
#include <string>
#include <vector>
#include <wrl/client.h>
#include <d3d12.h>

#include "Texture2D.h"
#include "RenderContext.h"
#include "Material.h"
#include "Math.h"

class Renderer;

// ---------------------
// Пер-объектные параметры (в b0)
// ---------------------
struct MaterialParams
{
    // линейные значения; для альбедо sRGB-выборка делает SRV
    float4 baseColor   = {1.f, 1.f, 1.f, 1.f};  // .rgb — tint
    float2 metalRough  = {0.0f, 0.35f};         // x=metallic, y=roughness
    // x=useAlbedo, y=useMR, z=useNormal, w=normalStrength (XY до восстановления Z)
    float4 texFlags    = {1.f, 1.f, 1.f, 1.f};

    void SetUseAlbedo(bool b){ texFlags.x = b ? 1.f : 0.f; }
    void SetUseMR(bool b)    { texFlags.y = b ? 1.f : 0.f; }
    void SetUseNormal(bool b){ texFlags.z = b ? 1.f : 0.f; }
    void SetNormalStrength(float s){ texFlags.w = s; }
};

// ---------------------
// Данные материала (ассет): текстуры + статические фичи
// ---------------------
class MaterialData {
public:
    // фичи (могут пойти в defines при сборке варианта шейдера)
    bool normalIsRG = true; // RG/BC5 vs RGB(A)
    bool useTBN     = true; // TBN-путь (иначе derivatives)

    // владение текстурами
    bool      hasAlbedo = false;
    bool      hasMR     = false;
    bool      hasNormal = false;
    Texture2D albedo; // sRGB
    Texture2D mr;     // UNORM: R=metal, G=rough
    Texture2D normal; // UNORM: RG (или RGB, если normalIsRG=false)

    // загрузка
    bool LoadAlbedo(Renderer* r, ID3D12GraphicsCommandList* upload, const std::wstring& path,
                    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);
    bool LoadMR    (Renderer* r, ID3D12GraphicsCommandList* upload, const std::wstring& path,
                    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);
    bool LoadNormal(Renderer* r, ID3D12GraphicsCommandList* upload, const std::wstring& path,
                    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);

    // сконфигурировать defines для GBuffer-варианта (NORMALMAP_IS_RG / USE_TBN)
    void ConfigureDefinesForGBuffer(Material::GraphicsDesc& gd) const;

    // собрать SRV-таблицу и сэмплер для стандартного GBuffer-пасса:
    // TABLE(SRV(t0) SRV(t1) SRV(t2)) + TABLE(SAMPLER(s0))
    void StageGBufferBindings(Renderer* r, RenderContext& ctx,
                              UINT srvTableRegister = 0, UINT samplerTableRegister = 0);

    // для инстанс-пути (t0 = instances), дописать t1..t3
    void AppendGBufferSRVs(std::vector<D3D12_CPU_DESCRIPTOR_HANDLE>& dst) const;

private:
    struct SrvCache {
        UINT frame = UINT_MAX;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    } gbufferSrvCache_;
    std::mutex cacheMtx_;
};