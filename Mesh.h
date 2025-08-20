#pragma once
#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <cstdint>

using namespace Microsoft::WRL;

// СТАРЫЙ формат (совместимость)
struct Vertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT4 color;
};

// НОВЫЙ “полноценный” формат под текстуры/свет
// порядок соответствует пресету лейаута "PosNormTanUV":
// POSITION (float3), NORMAL (float3), TANGENT (float4), TEXCOORD (float2)
struct VertexPNTUV {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;    // можно оставить нули — сгенерим
    DirectX::XMFLOAT4 tangent;   // xyz = tangent, w = handedness (+1/-1)
    DirectX::XMFLOAT2 uv;
};

class Mesh {
public:
    Mesh() = default;
    
    // Гибкий аплоад произвольного вершинного формата (укажи stride явно)
    void CreateGPUFlexible(ID3D12Device* device,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
        const void* vertices, UINT vertexCount, UINT vertexStride,
        const void* indices, UINT indexCount,
        DXGI_FORMAT indexFormat = DXGI_FORMAT_R16_UINT);

    // Аплоад нового формата + (опционально) генерация нормалей/тангентов на CPU
    void CreateGPU_PNTUV(ID3D12Device* device,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
        std::vector<VertexPNTUV>& verts,       // по ссылке: можем модифицировать
        const uint32_t* indices, UINT indexCount,
        bool generateTangentSpace = true);

    // Рендер
    void Draw(ID3D12GraphicsCommandList* cmdList) const;
    void DrawInstanced(ID3D12GraphicsCommandList* cmdList, UINT instanceCount) const;

    UINT GetIndexCount() const { return indexCount_; }

    ID3D12Resource* GetVertexBufferResource() const { return vertexBuffer_.Get(); }
    ID3D12Resource* GetIndexBufferResource()  const { return indexBuffer_.Get(); }

    UINT GetVertexStride() const { return vertexStride_; }
    DXGI_FORMAT GetIndexFormat() const { return indexFormat_; }

private:
    // Генерация нормалей/тангентов (простая: на треугольниках, с усреднением по вершинам)
    static void GenerateNormalsTangents(std::vector<VertexPNTUV>& verts,
        const uint32_t* indices, UINT indexCount);

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer_;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView_ = {};
    D3D12_INDEX_BUFFER_VIEW  indexBufferView_ = {};
    UINT  vertexStride_ = sizeof(Vertex);      // по умолчанию старый формат
    DXGI_FORMAT indexFormat_ = DXGI_FORMAT_R16_UINT;
    UINT  indexCount_ = 0;
};