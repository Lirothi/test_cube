#pragma once
#if WITH_EDITOR

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Metadata-only asset discovery for the level editor. Scans known asset roots
// and records cheap file metadata (path, name, extension, write time). It never
// loads mesh/texture data or touches the GPU; loading happens only when an asset
// is spawned or previewed in later steps.

enum class EditorAssetType
{
    Mesh,
    MaterialPreset,
    Texture,
    Level,
    Shader,
    Unknown
};

const char* ToString(EditorAssetType type);

struct EditorAssetId
{
    EditorAssetType type = EditorAssetType::Unknown;
    std::string key;
};

struct EditorAssetRecord
{
    EditorAssetId id;
    std::string path;
    std::string displayName;
    std::string extension;
    uint64_t fileWriteTime = 0;
};

class AssetRegistry
{
public:
    // Rescan all asset roots. Cheap: stats files only, no loading.
    void Refresh();

    const std::vector<EditorAssetRecord>& Assets() const { return assets_; }

    // Returns records whose display name or path contains `text`
    // (case-insensitive). Pass EditorAssetType::Unknown for no type filter.
    std::vector<const EditorAssetRecord*> Search(std::string_view text,
        EditorAssetType typeFilter) const;

    const EditorAssetRecord* FindById(const EditorAssetId& id) const;
    const EditorAssetRecord* FindByPath(std::string_view path) const;

    size_t CountByType(EditorAssetType type) const;

private:
    std::vector<EditorAssetRecord> assets_;
};

#endif // WITH_EDITOR
