#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <wrl/client.h>
#include "Material.h"
#include "RenderContext.h"

class FontAtlas;
class Renderer;

class TextManager {
public:
    void Init(Renderer* r);
    void Begin(UINT vpW, UINT vpH, float dpiScale = 1.0f);
    void AddText(int x, int y, uint32_t rgba, float px, const std::string& utf8);
    void AddTextf(int x, int y, uint32_t rgba, float px, const char* fmt, ...);
    void AddTextf(int x, int y, float r, float g, float b, float a, float px, const char* fmt, ...);

    // важное изменение: Build пишет в persistent UPLOAD-буферы (без UploadManager/keepAlive)
    void Build(Renderer* r, ID3D12GraphicsCommandList* cl);
    void Draw(Renderer* r, ID3D12GraphicsCommandList* cl);

    void SetFont(FontAtlas* f) { font_ = f; }

    static uint32_t RGBA(float r, float g, float b, float a = 1.0f) {
        auto clamp01 = [](float v) -> float {
            if (v < 0.0f) { return 0.0f; }
            if (v > 1.0f) { return 1.0f; }
            return v;
            };
        const uint32_t R = (uint32_t)(clamp01(r) * 255.0f + 0.5f);
        const uint32_t G = (uint32_t)(clamp01(g) * 255.0f + 0.5f);
        const uint32_t B = (uint32_t)(clamp01(b) * 255.0f + 0.5f);
        const uint32_t A = (uint32_t)(clamp01(a) * 255.0f + 0.5f);
        return (R << 24) | (G << 16) | (B << 8) | (A);
    }

private:
    struct Vertex {
        float x, y, z;
        float r, g, b, a;
        float u, v;
    };

    FontAtlas* font_ = nullptr;
    Material    mat_;
    RenderContext rc_;

	std::vector<Vertex>      verts_;
    std::vector<uint32_t>    idx_;

    D3D12_VERTEX_BUFFER_VIEW vbv_{};
    D3D12_INDEX_BUFFER_VIEW  ibv_{};

    UINT  vpW_ = 1, vpH_ = 1;
    float dpi_ = 1.0f;
};