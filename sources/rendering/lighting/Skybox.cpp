#include "rendering/lighting/Skybox.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadManager.h"
#include "rendering/core/FrameResource.h"
#include "app/camera/Camera.h"

#include <cstdio>
#include "core/diagnostics/DiagPaths.h"
#include <filesystem>
#include <memory>
#include <system_error>

using Microsoft::WRL::ComPtr;

namespace
{
// Which IBL path a level took gets asked about after the fact, from a headless capture, so
// it goes to the diagnostic log as well as the debugger. A silent fallback to the legacy
// mip chain is exactly the thing that looks like "F8 did nothing".
void LogIbl(const char* text)
{
    OutputDebugStringA(text);
    OutputDebugStringA("\n");
    std::FILE* f = nullptr;
    if (fopen_s(&f, diag::LogPath("ibl.log").c_str(), "a") == 0 && f)
    {
        std::fprintf(f, "%s\n", text);
        std::fclose(f);
    }
}

class SkyboxUniformBinder final : public RenderableObject::UniformBinder
{
public:
    explicit SkyboxUniformBinder(Skybox& owner) : owner_(owner) {}

    void RebuildHandles(RenderableObject& owner) override
    {
        viewHandle_ = {};
        projHandle_ = {};
        prevViewHandle_ = {};
        prevProjHandle_ = {};
        projNoJitterHandle_ = {};
        prevProjNoJitterHandle_ = {};
        exposureHandle_ = {};

        if (Material* material = owner.GetGraphicsMaterial())
        {
            viewHandle_ = material->ComputeCBFieldHandle(0, "view");
            projHandle_ = material->ComputeCBFieldHandle(0, "proj");
            prevViewHandle_ = material->ComputeCBFieldHandle(0, "prevView");
            prevProjHandle_ = material->ComputeCBFieldHandle(0, "prevProj");
            projNoJitterHandle_ = material->ComputeCBFieldHandle(0, "projNoJitter");
            prevProjNoJitterHandle_ = material->ComputeCBFieldHandle(0, "prevProjNoJitter");
            exposureHandle_ = material->ComputeCBFieldHandle(0, "exposure");
        }
    }

    void UpdateMainCB(RenderableObject& owner, Renderer* /*renderer*/, const Camera& camera, uint8_t* cbData) override
    {
        Material* material = owner.GetGraphicsMaterial();
        if (!material) { return; }

        UpdateUniform(owner, viewHandle_, material, camera.GetViewMatrix(), cbData);
        UpdateUniform(owner, projHandle_, material, camera.GetProjMatrix(), cbData);
        UpdateUniform(owner, prevViewHandle_, material, camera.GetPrevViewMatrix(), cbData);
        UpdateUniform(owner, prevProjHandle_, material, camera.GetPrevProjMatrix(), cbData);
        UpdateUniform(owner, projNoJitterHandle_, material, camera.GetProjMatrixNoJitter(), cbData);
        UpdateUniform(owner, prevProjNoJitterHandle_, material, camera.GetPrevProjMatrixNoJitter(), cbData);
        UpdateUniform(owner, exposureHandle_, material, owner_.GetExposure(), cbData);
    }

private:
    Skybox& owner_;
    Material::CBFieldHandle viewHandle_{};
    Material::CBFieldHandle projHandle_{};
    Material::CBFieldHandle prevViewHandle_{};
    Material::CBFieldHandle prevProjHandle_{};
    Material::CBFieldHandle projNoJitterHandle_{};
    Material::CBFieldHandle prevProjNoJitterHandle_{};
    Material::CBFieldHandle exposureHandle_{};
};
} // namespace

void Skybox::Init(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    if (!renderer) {
        return;
    }

    // If the texture has not been loaded via LoadDDS yet, we can optionally try it here
    if (!cube_.GetResource() && !path_.empty()) {
        (void)cube_.CreateFromDDS(renderer, uploadCmdList, path_, uploadKeepAlive);
    }

    // F8: pick up the F7 IBL siblings, if this sky was imported with them. Discovery is by NAME
    // rather than by a level field on purpose -- the importer writes them next to the cube, so a
    // path is all the information needed, and a level cannot get into a state where it points at a
    // sky whose derivatives belong to a different one.
    //
    // All three must load or none is used. A half-set would mean sampling a prefiltered cube with
    // no BRDF term, which is not "slightly wrong", it is a different lighting model.
    if (!hasIbl_ && cube_.GetResource() && !path_.empty())
    {
        const std::filesystem::path base(path_);
        std::filesystem::path stem = base;
        stem.replace_extension();
        const std::wstring specPath = stem.wstring() + L"_spec.dds";
        const std::wstring diffPath = stem.wstring() + L"_diffuse.dds";
        // Scene independent and shared, so it normally lives in the textures root -- but look
        // beside the cube first, so a sky folder straight out of the importer (or a staging
        // directory) is self-contained and testable without installing anything.
        std::error_code ec;
        const std::wstring lutBeside = (base.parent_path() / L"brdf_lut.dds").wstring();
        const std::wstring lutPath = std::filesystem::exists(lutBeside, ec)
            ? lutBeside
            : std::wstring(L"textures/brdf_lut.dds");

        ec.clear();
        const bool filesPresent =
            std::filesystem::exists(specPath, ec) &&
            std::filesystem::exists(diffPath, ec) &&
            std::filesystem::exists(lutPath, ec);

        if (filesPresent)
        {
            const bool specOk = specCube_.CreateFromDDS(renderer, uploadCmdList, specPath, uploadKeepAlive);
            const bool diffOk = irradianceCube_.CreateFromDDS(renderer, uploadCmdList, diffPath, uploadKeepAlive);
            Texture2D::CreateDesc lutDesc;
            lutDesc.path = lutPath;
            lutDesc.usage = Texture2D::Usage::LinearData;
            const bool lutOk = brdfLut_.CreateFromFile(renderer, uploadCmdList, lutDesc, uploadKeepAlive);
            hasIbl_ = specOk && diffOk && lutOk;

            char msg[512];
            std::snprintf(msg, sizeof(msg),
                "[ibl] %s  spec=%d diffuse=%d lut=%d  specMips=%u",
                hasIbl_ ? "split-sum ON" : "FAILED, falling back to the sky mip chain",
                (int)specOk, (int)diffOk, (int)lutOk, specCube_.GetMips());
            LogIbl(msg);
        }
        else
        {
            LogIbl("[ibl] no F7 derivatives beside this skybox; using the legacy sky mip "
                   "chain (re-import the HDRI to get them)");
        }
    }

    // Build the cube geometry
    BuildCubeMesh_(renderer, uploadCmdList, uploadKeepAlive);

    if (!GetUniformBinder())
    {
        SetUniformBinder(std::make_unique<SkyboxUniformBinder>(*this));
    }

    RenderableObject::Init(renderer, uploadCmdList, uploadKeepAlive);
}

void Skybox::RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx, const Camera& camera, uint8_t* cbData)
{
    ctx.srvTable[0] = cube_.GetSRVForFrame(renderer);
    ctx.samplerTable[0] = renderer->GetSamplerManager()->Get(renderer, *SamplerManager::LinearClamp());

    RenderableObject::RecordGraphics(renderer, cl, ctx, camera, cbData);
}

void Skybox::BuildCubeMesh_(Renderer* r,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* keepAlive)
{
    std::vector<VertexPNTUV> cubeVerts;
    std::vector<uint32_t> cubeIndices;
    BuildCubeCW(cubeVerts, cubeIndices);

    SetMesh(std::make_shared<Mesh>()); // 5b: own the mesh explicitly (no base default)
    GetMesh()->CreateGPU_PNTUV(r->GetDevice(), uploadCmdList, keepAlive, cubeVerts, cubeIndices.data(), (UINT)cubeIndices.size(), true);
}

void Skybox::ConfigureGraphicsPipeline(Renderer* renderer, Material::GraphicsDesc& desc) const
{
    RenderableObject::ConfigureGraphicsPipeline(renderer, desc);

    desc.numRT = 2;
    if (renderer)
    {
        desc.rtvFormats[0] = renderer->GetLightTargetFormat();
        desc.rtvFormats[1] = renderer->GetGBufferVelocityFormat();
    }
    desc.depth.DepthEnable = TRUE;
    desc.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;      // do not write depth
    desc.depth.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL; // reverse-Z depth test for the sky
    desc.raster.CullMode = D3D12_CULL_MODE_NONE;             // render from the inside
    desc.blend.RenderTarget[0].BlendEnable = FALSE;
    desc.blend.RenderTarget[1].BlendEnable = FALSE;
    desc.blend.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
}
