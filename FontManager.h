#pragma once
#include "third_party/robin_hood.h"
#include <memory>
#include <string>
#include <vector>
#include <wrl/client.h>
#include "FontAtlas.h"

class Renderer;

class FontManager {
public:
    void Init(Renderer* r) {
        renderer_ = r;
    }

    // Scan a folder: grab every *.json and look for a sibling *.tga with the same name
    void LoadFromFolder(Renderer* r,
                        ID3D12GraphicsCommandList* uploadCl,
                        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
                        const std::wstring& folder);

    // Return a font by name (file basename without extension). nullptr if absent.
    FontAtlas* Get(const std::wstring& name);

    // Return the default font (the first one loaded successfully)
    FontAtlas* GetDefault();

    // For UI/debug purposes—list all loaded names
    std::vector<std::wstring> List() const;

    void Clear();

private:
    Renderer* renderer_ = nullptr;
    robin_hood::unordered_map<std::wstring, std::unique_ptr<FontAtlas>> fonts_;
    std::wstring defaultName_;
};