#pragma once
#if WITH_EDITOR

#include <cstdint>

// Shared mesh visualization contract for editor mini-scenes. Thumbnail rendering always uses
// Lit; interactive mesh tools can select the diagnostic modes without duplicating renderer APIs.
enum class EditorPreviewMode : std::uint8_t
{
    Lit,
    Wireframe,
    VertexNormals
};

#endif // WITH_EDITOR
