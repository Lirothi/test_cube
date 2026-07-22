#include "editor/assets/EditorPreviewRenderer.h"
#if WITH_EDITOR

#include <algorithm>
#include <cmath>
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
    // Mesh assets may have several material slots. Keep a distinct descriptor
    // and 256-byte CBV for every draw recorded into one thumbnail command list.
    // Slot 255 is reserved as a neutral fallback if a pathological asset has
    // more submeshes than the preview heap was sized for.
    constexpr std::uint32_t kPreviewMaterialSlots = 255;
    constexpr std::uint32_t kPreviewFallbackSlot = kPreviewMaterialSlots;
    constexpr std::uint32_t kPreviewDrawSlots = kPreviewMaterialSlots + 1;
    constexpr std::uint32_t kPreviewTexturesPerDraw = 3;
    constexpr std::uint32_t kPreviewSrvDescriptors =
        kPreviewDrawSlots * kPreviewTexturesPerDraw;
    constexpr std::uint32_t kPreviewConstantStride = 256;

    // Matches the PreviewCB cbuffer in shaders/editor_preview.hlsl (16-byte packed).
    struct PreviewConstants
    {
        dx::XMFLOAT4X4 mvp;
        dx::XMFLOAT4X4 model;
        dx::XMFLOAT4 lightDir;
        dx::XMFLOAT4 eyePosition;
        dx::XMFLOAT4 baseColor;
        dx::XMFLOAT4 metalRoughAlpha; // xy = MR, z = alpha cutoff, w = MR multiply
        dx::XMFLOAT4 texOffsScale;
        dx::XMFLOAT4 texFlags; // xyz = use albedo/MR/normal, w = normal strength
        dx::XMFLOAT4 materialFlags; // x = glTF MR, y = normal RG, z = double-sided, w = albedo exists
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

bool EditorPreviewRenderer::EnsureInitialized(ID3D12Device* device,
    std::uint32_t maxRenderSize)
{
    if (initialized_)
    {
        return maxRenderSize > 0 && std::all_of(renderSlots_.begin(), renderSlots_.end(),
            [maxRenderSize](const RenderSlot& slot)
            {
                return slot.depthSize >= maxRenderSize;
            });
    }

    if (!device || maxRenderSize == 0)
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
    srvRange.NumDescriptors = kPreviewTexturesPerDraw;
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
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
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
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    if (FAILED(device->CreateGraphicsPipelineState(&pso,
            IID_PPV_ARGS(&doubleSidedPipeline_))))
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

    srvDescriptorSize_ = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (RenderSlot& frame : renderSlots_)
    {
        // One RTV/DSV/SRV set per frame prevents CPU descriptor rewrites from
        // racing draws submitted by an older swapchain frame.
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = 1;
        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&frame.rtvHeap))))
        {
            return false;
        }
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&frame.dsvHeap))))
        {
            return false;
        }
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        heapDesc.NumDescriptors = kPreviewSrvDescriptors;
        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&frame.srvHeap))))
        {
            return false;
        }
        frame.rtvHandle = frame.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        frame.dsvHandle = frame.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        frame.srvCpuHandle = frame.srvHeap->GetCPUDescriptorHandleForHeapStart();
        frame.srvGpuHandle = frame.srvHeap->GetGPUDescriptorHandleForHeapStart();

        if (!CreateSharedDepth(device, maxRenderSize, frame))
        {
            return false;
        }

        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC cbDesc{};
        cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        cbDesc.Width = static_cast<UINT64>(kPreviewConstantStride) * kPreviewDrawSlots;
        cbDesc.Height = 1;
        cbDesc.DepthOrArraySize = 1;
        cbDesc.MipLevels = 1;
        cbDesc.Format = DXGI_FORMAT_UNKNOWN;
        cbDesc.SampleDesc.Count = 1;
        cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
                &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&frame.constantBuffer))))
        {
            return false;
        }
        D3D12_RANGE noRead{ 0, 0 };
        if (FAILED(frame.constantBuffer->Map(0, &noRead,
                reinterpret_cast<void**>(&frame.constantBufferMapped))))
        {
            return false;
        }

        // Mesh previews without an albedo use a null SRV. The shader branches
        // to a neutral tint, avoiding a synchronous fallback-texture upload.
        D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
        nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullSrv.Texture2D.MipLevels = 1;
        for (std::uint32_t descriptor = 0;
            descriptor < kPreviewSrvDescriptors;
            ++descriptor)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = frame.srvCpuHandle;
            handle.ptr += static_cast<SIZE_T>(descriptor) * srvDescriptorSize_;
            device->CreateShaderResourceView(nullptr, &nullSrv, handle);
        }
    }

    initialized_ = true;
    return true;
}

bool EditorPreviewRenderer::CreateSharedDepth(ID3D12Device* device,
    std::uint32_t size,
    RenderSlot& slot)
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
            D3D12_RESOURCE_STATE_COMMON, &clear, IID_PPV_ARGS(&slot.depthTarget))))
    {
        return false;
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = kDepthFormat;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(slot.depthTarget.Get(), &dsv, slot.dsvHandle);
    slot.depthSize = size;
    return true;
}

Microsoft::WRL::ComPtr<ID3D12Resource> EditorPreviewRenderer::CreateColorTarget(
    ID3D12Device* device,
    std::uint32_t width,
    std::uint32_t height)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
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
    // Mirror the app bootstrap (I0): legacy monolith first, per-file materials win on clashes.
    materials_.LoadPresetsFromJsonFile(L"data/materials.json");
    materials_.LoadPresetsFromDirectory(L"data/materials");
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
    const std::vector<std::shared_ptr<MaterialData>>& materials,
    std::uint32_t size,
    const OrbitCamera& camera,
    std::uint32_t renderSlot,
    ID3D12Resource* existingColorTarget)
{
    return RecordPreview(renderer,
        cl,
        mesh,
        materials,
        size,
        size,
        camera,
        PreviewLight{},
        renderSlot,
        existingColorTarget);
}

Microsoft::WRL::ComPtr<ID3D12Resource> EditorPreviewRenderer::RecordPreview(
    Renderer& renderer,
    ID3D12GraphicsCommandList* cl,
    const Mesh& mesh,
    const std::vector<std::shared_ptr<MaterialData>>& materials,
    std::uint32_t width,
    std::uint32_t height,
    const OrbitCamera& camera,
    const PreviewLight& light,
    std::uint32_t renderSlot,
    ID3D12Resource* existingColorTarget)
{
    ID3D12Device* device = renderer.GetDevice();
    if (!initialized_ || !device || !cl || renderSlot >= renderSlots_.size())
    {
        return nullptr;
    }
    RenderSlot& frame = renderSlots_[renderSlot];
    if (width == 0 || height == 0 ||
        width > frame.depthSize || height > frame.depthSize)
    {
        return nullptr;
    }
    if (!mesh.GetVertexBufferResource() || !mesh.GetIndexBufferResource() ||
        mesh.GetIndexCount() == 0)
    {
        return nullptr;
    }

    ComPtr<ID3D12Resource> color = existingColorTarget;
    if (!color)
    {
        color = CreateColorTarget(device, width, height);
    }
    if (!color)
    {
        return nullptr;
    }

    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = kColorFormat;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(color.Get(), &rtv, frame.rtvHandle);

    // Use the mesh's actual enclosing sphere so every preview follows the same
    // edge-to-edge framing. This covers the geometry without treating empty AABB
    // corners as mesh volume (especially important for the material preview sphere).
    const AABB& bounds = mesh.GetBoundingBox();
    const Math::float3 centerPt = bounds.IsValid()
        ? bounds.GetCenter()
        : Math::float3(0.0f, 0.0f, 0.0f);
    float radius = mesh.GetBoundingSphereRadius();
    if (radius <= 0.0f && bounds.IsValid())
    {
        radius = bounds.GetRadius();
    }
    radius = std::max(radius, 1.0e-3f);

    const float pitch = std::clamp(camera.pitch,
        dx::XMConvertToRadians(-89.0f), dx::XMConvertToRadians(89.0f));
    const float cosPitch = std::cos(pitch);
    const dx::XMVECTOR offset = dx::XMVector3Normalize(dx::XMVectorSet(
        cosPitch * std::sin(camera.yaw),
        std::sin(pitch),
        -cosPitch * std::cos(camera.yaw),
        0.0f));
    const dx::XMVECTOR worldUp = dx::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const dx::XMVECTOR right = dx::XMVector3Normalize(dx::XMVector3Cross(offset, worldUp));
    const dx::XMVECTOR cameraUp = dx::XMVector3Normalize(dx::XMVector3Cross(right, offset));
    dx::XMVECTOR center = dx::XMVectorSet(centerPt.x, centerPt.y, centerPt.z, 1.0f);
    center = dx::XMVectorAdd(center, dx::XMVectorScale(right, camera.panX * radius));
    center = dx::XMVectorAdd(center, dx::XMVectorScale(cameraUp, camera.panY * radius));
    const float fovY = dx::XMConvertToRadians(35.0f);
    // Fit the enclosing sphere against the tighter of the horizontal/vertical
    // field-of-view limits so rectangular editor panes never crop the mesh.
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float horizontalHalfFov = std::atan(std::tan(fovY * 0.5f) * aspect);
    const float framingHalfFov = std::min(fovY * 0.5f, horizontalHalfFov);
    const float framedDistance = radius / std::sin(framingHalfFov) *
        std::clamp(camera.zoom, 0.12f, 8.0f);
    const dx::XMVECTOR framedEye =
        dx::XMVectorAdd(center, dx::XMVectorScale(offset, framedDistance));
    const float framedNearZ = std::max(radius * 0.005f,
        framedDistance - radius * 1.05f);
    const float framedFarZ = std::max(framedNearZ + radius * 0.01f,
        framedDistance + radius * 1.05f);
    const dx::XMMATRIX framedView = dx::XMMatrixLookAtLH(framedEye, center, cameraUp);
    const dx::XMMATRIX proj = dx::XMMatrixPerspectiveFovLH(
        fovY, aspect, framedNearZ, framedFarZ);
    const dx::XMMATRIX model = dx::XMMatrixIdentity();
    const dx::XMMATRIX mvp = dx::XMMatrixMultiply(
        dx::XMMatrixMultiply(model, framedView), proj);

    dx::XMVECTOR lightDirection = dx::XMVectorSet(
        light.direction.x, light.direction.y, light.direction.z, 0.0f);
    if (dx::XMVectorGetX(dx::XMVector3LengthSq(lightDirection)) < 1.0e-8f)
    {
        lightDirection = dx::XMVectorSet(-0.4f, -0.8f, 0.5f, 0.0f);
    }
    lightDirection = dx::XMVector3Normalize(lightDirection);

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

    barrier(color.Get(), existingColorTarget
            ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
            : D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(frame.depthTarget.Get(), D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);

    cl->OMSetRenderTargets(1, &frame.rtvHandle, FALSE, &frame.dsvHandle);
    D3D12_VIEWPORT viewport{ 0.0f, 0.0f,
        static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    cl->RSSetViewports(1, &viewport);
    D3D12_RECT scissor{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    cl->RSSetScissorRects(1, &scissor);

    const float clearColor[4] = { 0.14f, 0.14f, 0.16f, 1.0f };
    cl->ClearRenderTargetView(frame.rtvHandle, clearColor, 0, nullptr);
    cl->ClearDepthStencilView(frame.dsvHandle,
        D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    cl->SetGraphicsRootSignature(rootSignature_.Get());
    cl->SetPipelineState(pipeline_.Get());
    ID3D12DescriptorHeap* heaps[] = { frame.srvHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
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

    const std::vector<Mesh::Submesh>& submeshes = mesh.GetSubmeshes();
    const std::uint32_t drawCount = std::max<std::uint32_t>(
        1u, static_cast<std::uint32_t>(submeshes.size()));
    for (std::uint32_t draw = 0; draw < drawCount; ++draw)
    {
        const bool hasSubmesh = draw < submeshes.size();
        const Mesh::Submesh& submesh = hasSubmesh
            ? submeshes[draw]
            : Mesh::Submesh{};
        const UINT indexCount = hasSubmesh ? submesh.indexCount : mesh.GetIndexCount();
        if (indexCount == 0)
        {
            continue;
        }

        // The reserved fallback slot keeps all geometry visible even for a
        // mesh with more material ranges than the fixed preview descriptor heap.
        const std::uint32_t slot = draw < kPreviewMaterialSlots
            ? draw
            : kPreviewFallbackSlot;
        const MaterialData* material = nullptr;
        if (draw < kPreviewMaterialSlots && hasSubmesh &&
            submesh.materialSlot < materials.size())
        {
            material = materials[submesh.materialSlot].get();
        }
        else if (draw < kPreviewMaterialSlots && !hasSubmesh && !materials.empty())
        {
            material = materials.front().get();
        }
        cl->SetPipelineState(material && material->doubleSided
            ? doubleSidedPipeline_.Get()
            : pipeline_.Get());

        ID3D12Resource* textures[kPreviewTexturesPerDraw]{};
        DXGI_FORMAT formats[kPreviewTexturesPerDraw] = {
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_R8G8_UNORM,
            DXGI_FORMAT_R8G8B8A8_UNORM
        };
        if (material)
        {
            if (material->hasAlbedo && material->albedo.GetResource())
            {
                textures[0] = material->albedo.GetResource();
                formats[0] = material->albedo.GetSrvFormat();
            }
            if (material->hasMR && material->mr.GetResource())
            {
                textures[1] = material->mr.GetResource();
                formats[1] = material->mr.GetSrvFormat();
            }
            if (material->hasNormal && material->normal.GetResource())
            {
                textures[2] = material->normal.GetResource();
                formats[2] = material->normal.GetSrvFormat();
            }
        }

        const std::uint32_t descriptorBase = slot * kPreviewTexturesPerDraw;
        for (std::uint32_t textureIndex = 0;
            textureIndex < kPreviewTexturesPerDraw;
            ++textureIndex)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = formats[textureIndex];
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = textures[textureIndex]
                ? std::max<UINT16>(textures[textureIndex]->GetDesc().MipLevels, 1)
                : 1;
            D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = frame.srvCpuHandle;
            srvHandle.ptr += static_cast<SIZE_T>(descriptorBase + textureIndex) *
                srvDescriptorSize_;
            device->CreateShaderResourceView(textures[textureIndex], &srv, srvHandle);
        }

        PreviewConstants cb{};
        dx::XMStoreFloat4x4(&cb.mvp, mvp);
        dx::XMStoreFloat4x4(&cb.model, model);
        dx::XMStoreFloat4(&cb.lightDir, lightDirection);
        cb.lightDir.w = std::max(0.0f, light.exposure);
        dx::XMStoreFloat4(&cb.eyePosition, framedEye);
        if (material)
        {
            const MaterialParams& params = material->fromGltf
                ? material->gltfDefaultParams
                : material->hasPresetParams
                    ? material->presetParams
                    : MaterialParams{};
            cb.baseColor = dx::XMFLOAT4{ params.baseColor.x, params.baseColor.y,
                params.baseColor.z, params.baseColor.w };
            cb.metalRoughAlpha = dx::XMFLOAT4{
                params.metalRough.x,
                params.metalRough.y,
                material->alphaMask ? material->alphaCutoff : -1.0f,
                params.mrMultiply };
            cb.texOffsScale = dx::XMFLOAT4{
                params.texOffsScale.x,
                params.texOffsScale.y,
                params.texOffsScale.z,
                params.texOffsScale.w };
            cb.texFlags = dx::XMFLOAT4{
                textures[0] && params.texFlags.x > 0.5f ? 1.0f : 0.0f,
                textures[1] && params.texFlags.y > 0.5f ? 1.0f : 0.0f,
                textures[2] && params.texFlags.z > 0.5f ? 1.0f : 0.0f,
                params.texFlags.w };
            cb.materialFlags = dx::XMFLOAT4{
                material->mrLayoutGltf ? 1.0f : 0.0f,
                material->normalIsRG ? 1.0f : 0.0f,
                material->doubleSided ? 1.0f : 0.0f,
                textures[0] ? 1.0f : 0.0f };
        }
        else
        {
            cb.baseColor = dx::XMFLOAT4{ 0.82f, 0.82f, 0.82f, 1.0f };
            cb.metalRoughAlpha = dx::XMFLOAT4{ 0.0f, 0.35f, -1.0f, 0.0f };
            cb.texOffsScale = dx::XMFLOAT4{ 0.0f, 0.0f, 1.0f, 1.0f };
            cb.texFlags = dx::XMFLOAT4{ 0.0f, 0.0f, 0.0f, 1.0f };
            cb.materialFlags = dx::XMFLOAT4{ 0.0f, 1.0f, 0.0f, 0.0f };
        }
        cb.ambient = dx::XMFLOAT4{
            std::max(0.0f, light.color.x),
            std::max(0.0f, light.color.y),
            std::max(0.0f, light.color.z),
            std::max(0.0f, light.ambient) };
        std::memcpy(frame.constantBufferMapped +
            static_cast<std::size_t>(slot) * kPreviewConstantStride, &cb, sizeof(cb));

        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = frame.srvGpuHandle;
        gpuHandle.ptr += static_cast<UINT64>(descriptorBase) * srvDescriptorSize_;
        cl->SetGraphicsRootConstantBufferView(0,
            frame.constantBuffer->GetGPUVirtualAddress() +
            static_cast<UINT64>(slot) * kPreviewConstantStride);
        cl->SetGraphicsRootDescriptorTable(1, gpuHandle);
        cl->DrawIndexedInstanced(indexCount, 1,
            hasSubmesh ? submesh.indexOffset : 0, 0, 0);
    }

    // Leave the thumbnail in a shader-read state, exactly like a loaded texture,
    // and return the depth target to COMMON for the next render.
    barrier(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    barrier(frame.depthTarget.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
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
    RenderSlot& frame = renderSlots_[0];

    ComPtr<ID3D12Resource> color = CreateColorTarget(device, size, size);
    if (!color)
    {
        return nullptr;
    }

    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = kColorFormat;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(color.Get(), &rtv, frame.rtvHandle);

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
    device->CreateShaderResourceView(cube.GetResource(), &srv, frame.srvCpuHandle);

    PreviewConstants constants{};
    std::memcpy(frame.constantBufferMapped, &constants, sizeof(constants));

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

    cl->OMSetRenderTargets(1, &frame.rtvHandle, FALSE, nullptr);
    D3D12_VIEWPORT viewport{ 0.0f, 0.0f,
        static_cast<float>(size), static_cast<float>(size), 0.0f, 1.0f };
    cl->RSSetViewports(1, &viewport);
    D3D12_RECT scissor{ 0, 0, static_cast<LONG>(size), static_cast<LONG>(size) };
    cl->RSSetScissorRects(1, &scissor);

    const float clearColor[4] = { 0.14f, 0.14f, 0.16f, 1.0f };
    cl->ClearRenderTargetView(frame.rtvHandle, clearColor, 0, nullptr);
    cl->SetGraphicsRootSignature(rootSignature_.Get());
    cl->SetPipelineState(cube.IsArray() ? cubeArrayPipeline_.Get() : cubePipeline_.Get());
    ID3D12DescriptorHeap* heaps[] = { frame.srvHeap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetGraphicsRootConstantBufferView(0,
        frame.constantBuffer->GetGPUVirtualAddress());
    cl->SetGraphicsRootDescriptorTable(1, frame.srvGpuHandle);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(3, 1, 0, 0);

    barrier(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return color;
}

#endif // WITH_EDITOR
