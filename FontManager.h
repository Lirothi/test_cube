#pragma once
#include <unordered_map>
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

    // Сканирует папку: берёт все *.json и ищет соседний *.tga с тем же именем
    void LoadFromFolder(Renderer* r,
                        ID3D12GraphicsCommandList* uploadCl,
                        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
                        const std::wstring& folder);

    // Отдаёт шрифт по имени (basename файла, без расширения). nullptr если нет.
    FontAtlas* Get(const std::wstring& name);

    // Возвращает дефолтный (первый успешно загруженный)
    FontAtlas* GetDefault();

    // Для UI/отладки — список загруженных имён
    std::vector<std::wstring> List() const;

    void Clear();

private:
    Renderer* renderer_ = nullptr;
    std::unordered_map<std::wstring, std::unique_ptr<FontAtlas>> fonts_;
    std::wstring defaultName_;
};