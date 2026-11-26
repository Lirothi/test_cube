// editor_preview_lines.hlsl's LinesCB -- the fields ONCE (architecture plan R5.3): HLSL's `cbuffer LinesCB`
// (editor_preview_lines.hlsl; compiled by FXC, vs_5_0 / ps_5_0) and C++'s PreviewLinesConstants
// (editor/assets/EditorPreviewRenderer.cpp; its rows are DirectX's XMFLOAT4 / XMFLOAT4X4) spell
// EDITOR_PREVIEW_LINES_FIELDS. A field list, not a struct: HLSL reads a cbuffer as globals. TC_INIT is the C++ mirror's
// initial value (nothing in HLSL). Comments are /* */ (a // inside a continued line would swallow the next field).
// Include shared/shader_types.h first. New fields are APPENDED.
#ifndef TC_SHADERS_SHARED_EDITOR_PREVIEW_LINES_H
#define TC_SHADERS_SHARED_EDITOR_PREVIEW_LINES_H

#define EDITOR_PREVIEW_LINES_FIELDS \
    TC_ROW_MAJOR float4x4 gViewProj; /* mesh space -> clip, the picture's own */                                      \
    float4 gViewport; /* xy = the target's size in pixels */

#endif
