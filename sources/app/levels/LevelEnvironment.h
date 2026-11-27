#pragma once
// Architecture plan R9.4 (docs/architecture_review_plan.md, section 11.2, wave 3): ONE road from a level's
// environment to the runtime -- the level loader (JsonLevel::Load) and the editor's runtime apply
// (EnvironmentRuntime::Apply, RebuildLights) call the same functions. A section is read over its struct's defaults
// and handed to the scene whole, so "the section is not in the file" and "the Inspector cleared it" mean one thing.
//
// The two readers differ in ONE thing, said by the caller: a level file is read Strict -- a value of another type
// throws, and JsonLevel turns that into an error naming the file (AGENTS.md "A Field of Another Type") -- while the
// editor reads its document Tolerant: an edit made through MCP or the Inspector runs in the frame, where a throw
// would end the process, so a number of another type keeps its default.
#include "third_party/json/json.hpp"

class Scene;
struct SpotLightDesc;
struct PointLightDesc;

namespace levelenv
{
enum class JsonRead
{
    Strict,   // value(): a key of another type throws (the level file)
    Tolerant, // a number of another type keeps the default (the editor's document, in the frame)
};

// A local light's keys over the desc's defaults (P16.5's `luminousFluxLm`, else the legacy `intensity` converted).
// `enabled` is the caller's: a disabled light is not built at all.
void ReadSpotLight(const nlohmann::json& light, SpotLightDesc& desc, JsonRead mode);
void ReadPointLight(const nlohmann::json& light, PointLightDesc& desc, JsonRead mode);

// One environment section -- null = absent, the struct's defaults -- into the scene.
void ApplySkyAtmosphere(Scene& scene, const nlohmann::json* section);
void ApplyVolumetricCloud(Scene& scene, const nlohmann::json* section);
void ApplyGtao(Scene& scene, const nlohmann::json* section);
void ApplyHeightFog(Scene& scene, const nlohmann::json* section);
void ApplyBloom(Scene& scene, const nlohmann::json* section);
void ApplyColorPipeline(Scene& scene, const nlohmann::json* section);
// The camera. R9.5: the P3B local-exposure keys an older level carries in its colour pipeline were moved onto the
// camera by levelmigrate::Migrate before this reads it.
void ApplyCameraExposure(Scene& scene, const nlohmann::json* exposure);
} // namespace levelenv
