#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <wrl/client.h>
#include "Material.h"
#include "RenderContext.h"
#include "Math.h"

class FontAtlas;
class Renderer;

class TextManager {
public:
    void Init(Renderer* r);
    void Begin(UINT vpW, UINT vpH, float dpiScale = 1.0f);
    void AddText(int x, int y, const float4& color, float px, const std::string& utf8);
    void AddTextf(int x, int y, const float4& color, float px, const char* fmt, ...);

    // важное изменение: Build пишет в persistent UPLOAD-буферы (без UploadManager/keepAlive)
    void Build(Renderer* r, ID3D12GraphicsCommandList* cl);
    void Draw(Renderer* r, ID3D12GraphicsCommandList* cl);

    void SetFont(FontAtlas* f) { font_ = f; }

    void Clear();

private:
    struct Vertex {
        float3 pos;
        float4 col;
        float2 uv;
    };

    FontAtlas* font_ = nullptr;
    std::shared_ptr<Material> mat_;
    
	std::vector<Vertex>      verts_;
    std::vector<uint32_t>    idx_;

    D3D12_VERTEX_BUFFER_VIEW vbv_{};
    D3D12_INDEX_BUFFER_VIEW  ibv_{};

    UINT  vpW_ = 1, vpH_ = 1;
    float dpi_ = 1.0f;
};