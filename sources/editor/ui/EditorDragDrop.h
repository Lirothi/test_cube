#pragma once
#if WITH_EDITOR

#include <cstdio>
#include <cstring>
#include <string>

#include "editor/assets/AssetRegistry.h"
#include "imgui.h"

namespace EditorDragDrop
{
    constexpr const char* kAssetPayloadType = "TC_ASSET";
    constexpr const char* kFolderPayloadType = "TC_FOLDER";
    constexpr size_t kPayloadTextCapacity = 512;

    struct AssetPayload
    {
        EditorAssetType type = EditorAssetType::Unknown;
        char key[kPayloadTextCapacity] = {};
    };

    struct FolderPayload
    {
        char virtualPath[kPayloadTextCapacity] = {};
    };

    inline AssetPayload MakeAssetPayload(const EditorAssetId& id)
    {
        AssetPayload payload;
        payload.type = id.type;
        std::snprintf(payload.key, sizeof(payload.key), "%s", id.key.c_str());
        return payload;
    }

    inline FolderPayload MakeFolderPayload(const std::string& virtualPath)
    {
        FolderPayload payload;
        std::snprintf(payload.virtualPath, sizeof(payload.virtualPath), "%s", virtualPath.c_str());
        return payload;
    }

    inline bool DecodeAssetPayload(const ImGuiPayload* payload, EditorAssetId& out)
    {
        if (!payload || !payload->IsDataType(kAssetPayloadType) ||
            payload->DataSize != sizeof(AssetPayload))
        {
            return false;
        }

        const AssetPayload& assetPayload =
            *static_cast<const AssetPayload*>(payload->Data);
        if (assetPayload.key[0] == '\0')
        {
            return false;
        }

        out.type = assetPayload.type;
        out.key = assetPayload.key;
        return true;
    }

    inline bool DecodeFolderPayload(const ImGuiPayload* payload, std::string& out)
    {
        if (!payload || !payload->IsDataType(kFolderPayloadType) ||
            payload->DataSize != sizeof(FolderPayload))
        {
            return false;
        }

        const FolderPayload& folderPayload =
            *static_cast<const FolderPayload*>(payload->Data);
        if (folderPayload.virtualPath[0] == '\0')
        {
            return false;
        }

        out = folderPayload.virtualPath;
        return true;
    }
}

#endif // WITH_EDITOR
