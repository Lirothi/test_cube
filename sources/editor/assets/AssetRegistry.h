#pragma once
#if WITH_EDITOR

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Metadata-only asset discovery for the level editor. Scans known asset roots
// and records cheap file metadata plus texture header metadata where available.
// It never creates runtime assets or touches the GPU; loading happens only when
// an asset is spawned or previewed in later steps.

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

enum class EditorTextureKind
{
    Unknown,
    Texture2D,
    TextureCube
};

const char* ToString(EditorTextureKind kind);

// H4 import freshness for assets that have a corresponding source under
// import_staging/. Untracked assets are authored directly in the project tree.
enum class EditorAssetImportStatus
{
    Untracked,
    Staged,
    UpToDate,
    SourceNewer,
    Incomplete
};

const char* ToString(EditorAssetImportStatus status);

// Compares an import source (directory, or one .hdr) with its project output.
// Manifest-backed imports compare exact source/output inventories and source
// content hashes; older imports fall back to timestamps and DDS inspection.
EditorAssetImportStatus InspectAssetImportStatus(std::string_view sourcePath,
    std::string_view projectPath);

struct EditorTextureInfo
{
    bool scanned = false;
    bool valid = false;
    EditorTextureKind kind = EditorTextureKind::Unknown;
    std::string format;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
    uint32_t mipLevels = 0;
    uint32_t arraySize = 1;
};

struct EditorAssetId
{
    EditorAssetType type = EditorAssetType::Unknown;
    std::string key;
};

struct EditorAssetRecord
{
    EditorAssetId id;
    std::string path;
    std::string virtualPath;
    std::string virtualFolder;
    std::string displayName;
    std::string extension;
    uint64_t fileWriteTime = 0;
    EditorTextureInfo texture;
    EditorAssetImportStatus importStatus = EditorAssetImportStatus::Untracked;
    std::string importSourcePath;
    // Staging-relative files that produce this specific resource. This keeps
    // freshness badges and reimport work at resource granularity.
    std::vector<std::string> importSourceFiles;
};

struct EditorAssetFolder
{
    std::string path;
    std::string name;
    std::string parentPath;
    std::vector<std::string> childPaths;
    std::vector<EditorAssetId> directAssetIds;
    size_t directAssetCount = 0;
    size_t recursiveAssetCount = 0;
};

class AssetRegistry
{
public:
    // Rebuild the registry from all asset roots, including the metadata needed
    // by the Content Browser and its previews.
    void Refresh();

    // Metadata-only scan of the asset roots. Used by the editor's throttled
    // auto-refresh poll; it avoids parsing texture headers or material JSON.
    bool HasChangedOnDisk() const;

    const std::vector<EditorAssetRecord>& Assets() const { return assets_; }
    const std::vector<EditorAssetFolder>& Folders() const { return folders_; }
    // Changes after every completed Refresh. Consumers can amortize derived
    // metadata validation until the registry has actually changed.
    std::uint64_t Revision() const { return revision_; }

    // Returns records whose display name or path contains `text`
    // (case-insensitive). Pass EditorAssetType::Unknown for no type filter.
    std::vector<const EditorAssetRecord*> Search(std::string_view text,
        EditorAssetType typeFilter) const;
    std::vector<const EditorAssetRecord*> SearchInFolder(std::string_view folderPath,
        bool recursive,
        std::string_view text,
        EditorAssetType typeFilter) const;

    const EditorAssetRecord* FindById(const EditorAssetId& id) const;
    const EditorAssetRecord* FindByPath(std::string_view path) const;
    const EditorAssetFolder* FindFolder(std::string_view virtualPath) const;

    size_t CountByType(EditorAssetType type) const;

private:
    std::vector<EditorAssetRecord> assets_;
    std::vector<EditorAssetFolder> folders_;
    uint64_t contentSignature_ = 0;
    bool contentSignatureInitialized_ = false;
    std::uint64_t revision_ = 0;

    uint64_t ComputeContentSignature() const;
};

#endif // WITH_EDITOR
