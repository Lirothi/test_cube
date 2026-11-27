#pragma once
// Architecture plan R9.5 (docs/architecture_review_plan.md, section 11.2, wave 3): THE one place that knows a level's
// older formats. Both readers of a level -- the loader (JsonLevel::Load) and the editor's document
// (EditorSceneDocument) -- migrate the file through Migrate() first and read only today's names after it.
//
// What it moves (no version number in a level: the old format is recognised by its keys, as before):
//   - the five look groups -- cameraExposure, colorPipeline, gtao, heightFog, bloom -- under "postProcess" (P8B); a
//     group already under "postProcess" wins over a top-level copy, and the top-level ones go;
//   - the height fog's old name "atmosphere" read as "heightFog" ("Atmosphere" alone names the SKY);
//   - the P3B local-exposure keys a colour pipeline still carries, onto the camera where it does not own them
//     (was PhotographicSettingsJson::MigrateLegacyLocalExposure at apply time; the camera's reader now clamps them
//     like its own keys -- no level in data/levels carries one the camera does not own);
//   - the old sun, "directionalLight", as a "timeOfDay" section that lights the level the same
//     (TimeOfDaySettingsJson::FromDirectionalLight, time-of-day plan T3), unless the level already has one --
//     except with `convertDirectionalLight` false (`--tod-legacy-light`), when it stays and the loader lights the level
//     with it directly.
// It is idempotent: a level in today's format comes back unchanged.
#include <string>
#include <vector>

#include "third_party/json/json.hpp"

namespace levelmigrate
{
struct Report
{
    bool sunConverted = false;          // directionalLight became timeOfDay
    std::vector<std::string> moved;     // "gtao -> postProcess/gtao", "postProcess/atmosphere -> postProcess/heightFog"
};

// By value: the loader moves its parsed level in (`j = Migrate(std::move(j), ...)`), the editor copies it as before.
nlohmann::json Migrate(nlohmann::json level, bool convertDirectionalLight, Report* report = nullptr);
} // namespace levelmigrate
