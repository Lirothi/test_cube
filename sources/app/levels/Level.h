#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

struct LevelLoadContext
{
    ID3D12GraphicsCommandList* uploadCmdList = nullptr;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive = nullptr;
};

class Level
{
public:
    virtual ~Level() = default;

    virtual std::string_view GetName() const = 0;
    virtual void Load(const LevelLoadContext& ctx) = 0;
    virtual void Unload(const LevelLoadContext& ctx);
    virtual void Tick(float deltaTime) { (void)deltaTime; }
};

