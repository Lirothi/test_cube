#include "FontManager.h"
#include "Renderer.h"

using Microsoft::WRL::ComPtr;

static bool FileExistsW_(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return (a != INVALID_FILE_ATTRIBUTES) && ((a & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

void FontManager::LoadFromFolder(Renderer* r,
                                 ID3D12GraphicsCommandList* uploadCl,
                                 std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive,
                                 const std::wstring& folder)
{
    renderer_ = r;

    std::wstring pattern = folder;
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') {
        pattern += L'\\';
    }
    pattern += L"*.json";

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }

        std::wstring json = folder;
        if (!json.empty() && json.back() != L'\\' && json.back() != L'/') {
            json += L'\\';
        }
        json += fd.cFileName;

        // basename без расширения
        std::wstring base = fd.cFileName;
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
            // нет соответствующего .tga — пропускаем
            continue;
        }

        auto atlas = std::make_unique<FontAtlas>();
        if (atlas->Load(r, uploadCl, uploadKeepAlive, json, tga)) {
            fonts_[base] = std::move(atlas);
            if (defaultName_.empty()) {
                defaultName_ = base;
            }
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
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