#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <wrl.h>
#include "Mesh.h"

class Renderer;

struct MeshLoadOptions {
    bool generateTangentSpace = true; // если в файле нет нормалей/тангентов — досчитаем
    bool wantCW = true;               // приводить трианги к CW (под D3D12 FrontCounterClockwise = FALSE)
    int  iBase  = 0;                  // базис индексов в "i a b c"
};

class MeshManager {
public:
    // Авто по расширению (.obj | .mesh.txt | .txt)
    std::shared_ptr<Mesh> Load(const std::string& path,
                               Renderer* renderer,
                               ID3D12GraphicsCommandList* uploadCmdList,
                               std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
                               const MeshLoadOptions& opt = {});

    // Явные варианты
    std::shared_ptr<Mesh> LoadText(const std::string& path,
                                   Renderer* renderer,
                                   ID3D12GraphicsCommandList* uploadCmdList,
                                   std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
                                   const MeshLoadOptions& opt = {});

    std::shared_ptr<Mesh> LoadOBJ(const std::string& path,
                                  Renderer* renderer,
                                  ID3D12GraphicsCommandList* uploadCmdList,
                                  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
                                  const MeshLoadOptions& opt = {});

    // Из памяти (если уже есть verts/indices)
    std::shared_ptr<Mesh> CreateFromMemory(const std::string& key,
                                           Renderer* renderer,
                                           const std::vector<VertexPNTUV>& verts,
                                           const std::vector<uint32_t>& indices,
                                           ID3D12GraphicsCommandList* uploadCmdList,
                                           std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
                                           bool generateTangentSpace = true);

    std::shared_ptr<Mesh> Get(const std::string& key) const;
    void Clear();

private:
    // внутренние парсеры
    bool ParseTextFile(const std::string& path,
                       std::vector<VertexPNTUV>& outVerts,
                       std::vector<uint32_t>& outIndices,
                       const MeshLoadOptions& opt);

    bool ParseOBJFile(const std::string& path,
                      std::vector<VertexPNTUV>& outVerts,
                      std::vector<uint32_t>& outIndices,
                      const MeshLoadOptions& opt);

private:
    std::unordered_map<std::string, std::shared_ptr<Mesh>> cache_;
};