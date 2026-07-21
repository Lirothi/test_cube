#pragma once
#if WITH_EDITOR

#include <string>

// Part I3 — write a schema-v2 material file (data/materials/<name>.json) from a glTF material,
// shared by the importer (auto-materials -> named files at import) and the inspector's
// "Save slot as material" button. Uses MeshManager::DescribeGltfMaterial so the ordinal->material
// mapping matches the runtime exactly; textures come out as the glTF's resolved paths (H2 resolves
// the DDS sibling); glTF factors are already baked into the imported DDS by H6, so a param is only
// written when the corresponding texture is absent.
namespace materialgen
{
    // Write data/materials/<name>.json for material `ordinal` of `geometry` (a glTF/GLB path, with
    // an optional #node: selector). If `overwrite` is false an existing file is preserved (keeps
    // material-editor edits across a re-import). Returns `name` when a material was found/kept,
    // "auto" for a null-material slot, or "" on parse failure.
    std::string WriteFromGltf(const std::string& geometry, int ordinal,
        const std::string& name, bool overwrite = false);
}

#endif // WITH_EDITOR
