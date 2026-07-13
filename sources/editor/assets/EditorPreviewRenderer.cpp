#include "editor/assets/EditorPreviewRenderer.h"
#if WITH_EDITOR

#include <algorithm>
#include <cstring>

#include <d3dcompiler.h>
#include <DirectXMath.h>

#include "core/math/AABB.h"
#include "materials/MaterialData.h"
#include "materials/TextureCube.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/meshes/Mesh.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace
{
    using Microsoft::WRL::ComPtr;
    namespace dx = DirectX;

    constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
    constexpr std::uint32_t kThumbnailSize = 256;

    // Matches the PreviewCB cbuffer in shaders/editor_preview.hlsl (16-byte packed).
    struct PreviewConstants
    {
        dx::XMFLOAT4X4 mvp;
        dx::XMFLOAT4X4 model;
        dx::XMFLOAT4 lightDir;
        dx::XMFLOAT4 baseColor; // a = hasAlbedo
        dx::XMFLOAT4 ambient;
    };
    static_assert(sizeof(PreviewConstants) <= 256, "PreviewConstants must fit one 256B CBV.");

    ComPtr<ID3DBlob> CompilePreviewShader(const char* entry, const char* target)
    {
        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompileFromFile(L"shaders/editor_preview.hlsl",
            nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entry,
            target,
            0,
            0,
            &blob,
            &errors);
        if (FAILED(hr))
        {
            if (errors)
            {
                OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
            }
            return nullptr;
        }
        return blob;
    }
}

bool EditorPreviewRenderer::EnsureInitialized(Renderer& renderer)
{
    if (initialized_)
    {
        return true;
    }

    ID3D12Device* device = renderer.GetDevice();
    if (!device)
    {
        return false;
    }

    ComPtr<ID3DBlob> vs = CompilePreviewShader("VSMain", "vs_5_0");
    ComPtr<ID3DBlob> ps = CompilePreviewShader("PSMain", "ps_5_0");
    ComPtr<ID3DBlob> cubeVs = CompilePreviewShader("CubeVSMain", "vs_5_0");
    ComPtr<ID3DBlob> cubePs = CompilePreviewShader("CubePSMain", "ps_5_0");
    ComPtr<ID3DBlob> cubeArrayPs = CompilePreviewShader("CubeArrayPSMain", "ps_5_0");
    if (!vs || !ps || !cubeVs || !cubePs || !cubeArrayPs)
    {
        return false;
    }

    // Root signature: CBV(b0) + table(SRV t0) + static linear sampler(s0).
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rsBlob;
    ComPtr<ID3DBlob> rsError;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            &rsBlob, &rsError)))
    {
        if (rsError)
        {
            OutputDebugStringA(static_cast<const char*>(rsError->GetBufferPointer()));
        }
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, rsBlob->GetBufferPointer(),
            rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature_))))
    {
        return false;
    }

    const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSignature_.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.InputLayout = { inputLayout, _countof(inputLayout) };
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // robust vs. unknown winding
    pso.RasterizerState.FrontCounterClockwise = FALSE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kColorFormat;
    pso.DSVFormat = kDepthFormat;
    pso.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline_))))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC cubePso{};
    cubePso.pRootSignature = rootSignature_.Get();
    cubePso.VS = { cubeVs->GetBufferPointer(), cubeVs->GetBufferSize() };
    cubePso.PS = { cubePs->GetBufferPointer(), cubePs->GetBufferSize() };
    cubePso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    cubePso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    cubePso.RasterizerState.DepthClipEnable = TRUE;
    cubePso.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    cubePso.DepthStencilState.DepthEnable = FALSE;
    cubePso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    cubePso.DepthStencilState.StencilEnable = FALSE;
    cubePso.SampleMask = UINT_MAX;
    cubePso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    cubePso.NumRenderTargets = 1;
    cubePso.RTVFormats[0] = kColorFormat;
    cubePso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    cubePso.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&cubePso, IID_PPV_ARGS(&cubePipeline_))))
    {
        return false;
    }

    cubePso.PS = { cubeArrayPs->GetBufferPointer(), cubeArrayPs->GetBufferSize() };
    if (FAILED(device->CreateGraphicsPipelineState(&cubePso, IID_PPV_ARGS(&cubeArrayPipeline_))))
    {
        return false;
    }

    // Descriptor heaps: 1 RTV, 1 DSV, 1 shader-visible SRV (one draw per list).
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap_))))
    {
        return false;
    }
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&dsvHeap_))))
    {
        return false;
    }
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap_))))
    {
        return false;
    }
    rtvHandle_ = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    dsvHandle_ = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    srvCpuHandle_ = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    srvGpuHandle_ = srvHeap_->GetGPUDescriptorHandleForHeapStart();

    if (!CreateSharedDepth(device, kThumbnailSize))
    {
        return false;
    }

    // Reusable 256-byte constant buffer (one draw per submit, so a single buffer
    // is safe to overwrite between fenced renders).
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC cbDesc{};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = 256;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.Format = DXGI_FORMAT_UNKNOWN;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
            &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&constantBuffer_))))
    {
        return false;
    }
    D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(constantBuffer_->Map(0, &noRead,
            reinterpret_cast<void**>(&constantBufferMapped_))))
    {
        return false;
    }

    // 1x1 white fallback so the pixel shader always has a valid SRV bound.
    {
        UploadBatch up;
        if (up.Begin(&renderer))
        {
            const std::uint8_t white[4] = { 255, 255, 255, 255 };
            whiteFallback_.CreateFromRGBA8(&renderer, up.CommandList(),
                white, 1, 1, up.KeepAlive());
            up.SubmitAndWait(&renderer);
        }
    }
    if (!whiteFallback_.GetResource())
    {
        return false;
    }

    initialized_ = true;
    return true;
}

bool EditorPreviewRenderer::CreateSharedDepth(ID3D12Device* device, std::uint32_t size)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = size;
    desc.Height = size;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kDepthFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = kDepthFormat;
    clear.DepthStencil.Depth = 1.0f;

    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, &clear, IID_PPV_ARGS(&depthTarget_))))
    {
        return false;
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = kDepthFormat;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(depthTarget_.Get(), &dsv, dsvHandle_);
    depthSize_ = size;
    return true;
}

Microsoft::WRL::ComPtr<ID3D12Resource> EditorPreviewRenderer::CreateColorTarget(
    ID3D12Device* device, std::uint32_t size)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = size;
    desc.Height = size;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kColorFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = kColorFormat;
    clear.Color[0] = 0.14f;
    clear.Color[1] = 0.14f;
    clear.Color[2] = 0.16f;
    clear.Color[3] = 1.0f;

    ComPtr<ID3D12Resource> target;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, &clear, IID_PPV_ARGS(&target))))
    {
        return nullptr;
    }
    return target;
}

void EditorPreviewRenderer::EnsurePresets()
{
    if (presetsLoaded_)
    {
        return;
    }
    presetsLoaded_ = true;
    materials_.LoadPresetsFromJsonFile(L"data/materials.json");
}

void EditorPreviewRenderer::ReloadPresets()
{
    materials_.ClearAll();
    presetsLoaded_ = false;
    EnsurePresets();
}

std::shared_ptr<Mesh> EditorPreviewRenderer::EnsureSphere(Renderer& renderer,
    UploadBatch& load)
{
    if (!sphere_)
    {
        sphere_ = meshes_.Load("models/sphere.obj", &renderer,
            load.CommandList(), load.KeepAlive());
    }
    return sphere_;
}

Microsoft::WRL::ComPtr<ID3D12Resource> EditorPreviewRenderer::RecordThumbnail(
    Renderer& renderer,
    ID3D12GraphicsCommandList* cl,
    const Mesh& mesh,
    ID3D12Resource* albedo,
    DXGI_FORMAT albedoSrvFormat,
    bool hasAlbedo,
    std::uint32_t size)
{
    ID3D12Device* device = renderer.GetDevice();
    if (!initialized_ || !device || !cl || size == 0 || size > depthSize_)
    {
        return nullptr;
    }
    if (!mesh.GetVertexBufferResource() || !mesh.GetIndexBufferResource() ||
        mesh.GetIndexCount() == 0)
    {
        return nullptr;
    }

    ComPtr<ID3D12Resource> color = CreateColorTarget(device, size);
    if (!color)
    {
        return nullptr;
    }

    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = kColorFormat;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(color.Get(), &rtv, rtvHandle_);

    const bool useAlbedo = hasAlbedo && albedo != nullptr;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = useAlbedo ? albedoSrvFormat : whiteFallback_.GetSrvFormat();
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(
        useAlbedo ? albedo : whiteFallback_.GetResource(), &srv, srvCpuHandle_);

    // Camera framing from the mesh bounds: a fixed three-quarter view.
    const AABB& bounds = mesh.GetBoundingBox();
    const Math::float3 centerPt = bounds.IsValid()
        ? bounds.GetCenter()
        : Math::float3(0.0f, 0.0f, 0.0f);
    float radius = bounds.IsValid() ? bounds.GetRadius() : 1.0f;
    radius = std::max(radius, 1.0e-3f);

    const dx::XMVECTOR center = dx::XMVectorSet(centerPt.x, centerPt.y, centerPt.z, 1.0f);
    const dx::XMVECTOR offset =
        dx::XMVector3Normalize(dx::XMVectorSet(0.8f, 0.7f, -1.0f, 0.0f));
    const float fovY = dx::XMConvertToRadians(35.0f);
    const float distance = (radius / std::tan(fovY * 0.5f)) * 1.35f;
    const dx::XMVECTOR eye =
        dx::XMVectorAdd(center, dx::XMVectorScale(offset, distance));
    const dx::XMVECTOR up = dx::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    const dx::XMMATRIX view = dx::XMMatrixLookAtLH(eye, center, up);
    const float nearZ = std::max(radius * 0.05f, distance - radius * 2.0f);
    const float farZ = distance + radius * 3.0f;
    const dx::XMMATRIX proj = dx::XMMatrixPerspectiveFovLH(fovY, 1.0f, nearZ, farZ);
    const dx::XMMATRIX model = dx::XMMatrixIdentity();
    const dx::XMMATRIX mvp = dx::XMMatrixMultiply(dx::XMMatrixMultiply(model, view), proj);

    PreviewConstants cb{};
    dx::XMStoreFloat4x4(&cb.mvp, mvp);
    dx::XMStoreFloat4x4(&cb.model, model);
    const dx::XMVECTOR lightDir =
        dx::XMVector3Normalize(dx::XMVectorSet(0.4f, 0.8f, -0.5f, 0.0f));
    dx::XMStoreFloat4(&cb.lightDir, lightDir);
    cb.baseColor = useAlbedo
        ? dx::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f }
        : dx::XMFLOAT4{ 0.82f, 0.82f, 0.82f, 0.0f };
    cb.ambient = dx::XMFLOAT4{ 0.28f, 0.30f, 0.34f, 1.0f };
    std::memcpy(constantBufferMapped_, &cb, sizeof(cb));

    auto barrier = [cl](ID3D12Resource* res,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
    };

    barrier(color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(depthTarget_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    cl->OMSetRenderTargets(1, &rtvHandle_, FALSE, &dsvHandle_);
    D3D12_VIEWPORT viewport{ 0.0f, 0.0f,
        static_cast<float>(size), static_cast<float>(size), 0.0f, 1.0f };
    cl->RSSetViewports(1, &viewport);
    D3D12_RECT scissor{ 0, 0, static_cast<LONG>(size), static_cast<LONG>(size) };
    cl->RSSetScissorRects(1, &scissor);

    const float clearColor[4] = { 0.14f, 0.14f, 0.16f, 1.0f };
    cl->ClearRenderTargetView(rtvHandle_, clearColor, 0, nullptr);
    cl->ClearDepthStencilView(dsvHandle_, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    cl->SetGraphicsRootSignature(rootSignature_.Get());
    cl->SetPipelineState(pipeline_.Get());
    ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetGraphicsRootConstantBufferView(0, constantBuffer_->GetGPUVirtualAddress());
    cl->SetGraphicsRootDescriptorTable(1, srvGpuHandle_);

    // Bind the mesh IA directly (Mesh::Draw uses a global bind cache tied to the
    // main render's command list, which must not be touched here).
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = mesh.GetVertexBufferResource()->GetGPUVirtualAddress();
    vbv.StrideInBytes = mesh.GetVertexStride();
    vbv.SizeInBytes = static_cast<UINT>(mesh.GetVertexBufferResource()->GetDesc().Width);
    D3D12_INDEX_BUFFER_VIEW ibv{};
    ibv.BufferLocation = mesh.GetIndexBufferResource()->GetGPUVirtualAddress();
    ibv.Format = mesh.GetIndexFormat();
    ibv.SizeInBytes = static_cast<UINT>(mesh.GetIndexBufferResource()->GetDesc().Width);

    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->IASetVertexBuffers(0, 1, &vbv);
    cl->IASetIndexBuffer(&ibv);
    cl->DrawIndexedInstanced(mesh.GetIndexCount(), 1, 0, 0, 0);

    // Leave the thumbnail in a shader-read state, exactly like a loaded texture,
    // and return the depth target to COMMON for the next render.
    barrier(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    barrier(depthTarget_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_COMMON);

    return color;
}

Microsoft::WRL::ComPtr<ID3D12Resource> EditorPreviewRenderer::RecordCubeThumbnail(
    Renderer& renderer,
    ID3D12GraphicsCommandList* cl,
    const TextureCube& cube,
    std::uint32_t size)
{
    ID3D12Device* device = renderer.GetDevice();
    if (!initialized_ || !device || !cl || size == 0 || !cube.GetResource())
    {
        return nullptr;
    }

    ComPtr<ID3D12Resource> color = CreateColorTarget(device, size);
    if (!color)
    {
        return nullptr;
    }

    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = kColorFormat;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(color.Get(), &rtv, rtvHandle_);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = cube.GetFormat();
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (cube.IsArray())
    {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        srv.TextureCubeArray.MostDetailedMip = 0;
        srv.TextureCubeArray.MipLevels = cube.GetMips();
        srv.TextureCubeArray.First2DArrayFace = 0;
        srv.TextureCubeArray.NumCubes = cube.GetArraySize() / 6;
        srv.TextureCubeArray.ResourceMinLODClamp = 0.0f;
    }
    else
    {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv.TextureCube.MostDetailedMip = 0;
        srv.TextureCube.MipLevels = cube.GetMips();
        srv.TextureCube.ResourceMinLODClamp = 0.0f;
    }
    device->CreateShaderResourceView(cube.GetResource(), &srv, srvCpuHandle_);

    PreviewConstants constants{};
    std::memcpy(constantBufferMapped_, &constants, sizeof(constants));

    auto barrier = [cl](ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER transition{};
        transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        transition.Transition.pResource = resource;
        transition.Transition.StateBefore = before;
        transition.Transition.StateAfter = after;
        transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &transition);
    };

    barrier(color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);

    cl->OMSetRenderTargets(1, &rtvHandle_, FALSE, nullptr);
    D3D12_VIEWPORT viewport{ 0.0f, 0.0f,
        static_cast<float>(size), static_cast<float>(size), 0.0f, 1.0f };
    cl->RSSetViewports(1, &viewport);
    D3D12_RECT scissor{ 0, 0, static_cast<LONG>(size), static_cast<LONG>(size) };
    cl->RSSetScissorRects(1, &scissor);

    const float clearColor[4] = { 0.14f, 0.14f, 0.16f, 1.0f };
    cl->ClearRenderTargetView(rtvHandle_, clearColor, 0, nullptr);
    cl->SetGraphicsRootSignature(rootSignature_.Get());
    cl->SetPipelineState(cube.IsArray() ? cubeArrayPipeline_.Get() : cubePipeline_.Get());
    ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetGraphicsRootConstantBufferView(0, constantBuffer_->GetGPUVirtualAddress());
    cl->SetGraphicsRootDescriptorTable(1, srvGpuHandle_);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(3, 1, 0, 0);

    barrier(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return color;
}

#endif // WITH_EDITOR
