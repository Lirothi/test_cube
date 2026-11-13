#include "text/FontManager.h"
#include "core/logging/Log.h"
#include "core/vfs/Vfs.h"
#include "rendering/core/Renderer.h"
#include "text/FontGenerator.h"
#include "input/InputManager.h"
#include "app/Systems.h"
#include <algorithm>

using Microsoft::WRL::ComPtr;

static bool FileExistsW_(const std::wstring& path) {
    return vfs::IsFile(path);
}

namespace {
std::wstring JoinPath(const std::wstring& base, const std::wstring& leaf)
{
    if (leaf.empty()) {
        return base;
    }

    std::wstring result = base;
    if (!result.empty()) {
        const wchar_t back = result.back();
        if (back != L'\\' && back != L'/') {
            result += L'\\';
        }
    }
    result += leaf;
    return result;
}
}

void FontManager::LoadFromFolder(Renderer* r,
                                 ID3D12GraphicsCommandList* uploadCl,
                                 std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive,
                                 const std::wstring& folder)
{
    renderer_ = r;
    fontsFolder_ = folder;

    // The folder's *.json in the order the file system lists them (the order FindFirstFileW gave).
    vfs::List(folder, "*.json", [&](const vfs::ListEntry& entry) {
        if (entry.directory) {
            return;
        }
        const std::wstring fileName = vfs::Widen(entry.name);

        std::wstring json = folder;
        if (!json.empty() && json.back() != L'\\' && json.back() != L'/') {
            json += L'\\';
        }
        json += fileName;

        // basename without the extension
        std::wstring base = fileName;
        size_t dot = base.find_last_of(L'.');
        if (dot != std::wstring::npos) {
            base.erase(dot);
        }

        std::wstring tga = folder;
        if (!tga.empty() && tga.back() != L'\\' && tga.back() != L'/') {
            tga += L'\\';
        }
        tga += base + L".tga";
        if (!FileExistsW_(tga)) {
            // no matching .tga file—skip it
            return;
        }

        auto atlas = std::make_unique<FontAtlas>();
        if (atlas->Load(r, uploadCl, uploadKeepAlive, json, tga)) {
            const FontAtlas::Type atlasType = atlas->GetType();
            fonts_[base] = std::move(atlas);
            if (defaultName_.empty()) {
                defaultName_ = base;
            } else if (atlasType == FontAtlas::Type::SDF) {
                auto itDefault = fonts_.find(defaultName_);
                if (itDefault == fonts_.end() || itDefault->second->GetType() != FontAtlas::Type::SDF) {
                    defaultName_ = base;
                }
            }
        }
    });
}

FontAtlas* FontManager::Get(const std::wstring& name) {
    auto it = fonts_.find(name);
    if (it == fonts_.end()) {
        return nullptr;
    }
    return it->second.get();
}

FontAtlas* FontManager::GetDefault() {
    if (defaultName_.empty()) {
        return nullptr;
    }
    return Get(defaultName_);
}

std::vector<std::wstring> FontManager::List() const {
    std::vector<std::wstring> out;
    out.reserve(fonts_.size());
    for (const auto& kv : fonts_) {
        out.push_back(kv.first);
    }
    return out;
}

void FontManager::Clear()
{
    fonts_.clear();
    defaultName_.clear();
    fontsFolder_.clear();
    renderer_ = nullptr;
}

void FontManager::Tick()
{
    if (fontsFolder_.empty()) {
        return;
    }

    return; // turn on only if needed

    auto& input = Systems::GetInput();

    if (!input.WasActionPressed("GenerateFont")) {
        return;
    }

    const bool generateSdf = input.IsKeyDown(VK_SHIFT);
    const FontGenerator::OutputType type = generateSdf ? FontGenerator::OutputType::Sdf : FontGenerator::OutputType::Coverage;

    FontGenerator generator;
    FontGenerator::Params params;
    params.fontFile = JoinPath(fontsFolder_, L"Consolas.ttf");
    params.pixelHeight = 32;
    params.type = type;
    params.fontsFolder = fontsFolder_;
    if (generateSdf) {
        params.spread = std::max(8, params.pixelHeight / 2 + 2);
    }
    else {
        params.spread = 6;
    }

    if (!generator.Generate(params)) {
        LOG_ERROR(logging::LogCategory::Asset, "font atlas generation failed");
    }
}
