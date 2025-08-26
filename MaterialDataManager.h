#pragma once
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>
#include <wrl/client.h>

#include "MaterialData.h"

class Renderer;
struct ID3D12GraphicsCommandList;

// Конфигурация пресета (пути к текстурам + фичи)
struct MaterialPreset {
    std::wstring albedoPath;
    std::wstring mrPath;
    std::wstring normalPath;
    bool normalIsRG = true;
    bool useTBN     = true;
};

// Менеджер пресетов и кэша загруженных MaterialData
class MaterialDataManager {
public:
    // Зарегистрировать или заменить пресет
    void RegisterPreset(const std::string& name, const MaterialPreset& preset);

    // Есть такой пресет?
    bool HasPreset(const std::string& name) const;

    // Получить/создать MaterialData по имени пресета (ленивая загрузка текстур)
    std::shared_ptr<MaterialData> GetOrCreate(Renderer* renderer,
                                              ID3D12GraphicsCommandList* uploadCmdList,
                                              std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
                                              const std::string& name);

    // Прямой доступ к уже загруженному (или nullptr)
    std::shared_ptr<MaterialData> FindLoaded(const std::string& name) const;

    // Очистить только кэш (оставив пресеты)
    void ClearCache();

    // Полная очистка (пресеты + кэш)
    void ClearAll();

private:
    std::unordered_map<std::string, MaterialPreset> presets_;
    std::unordered_map<std::string, std::shared_ptr<MaterialData>> cache_;
};