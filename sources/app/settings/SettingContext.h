#pragma once

class Scene;
struct SceneRenderSettings;

namespace settings
{
// What a settings registry row reaches (core/settings/SettingsRegistry.h, architecture plan R2.1): the scene (its level
// sections, its world) and the render settings -- what App.cpp's ApplySweepValue / ReadSweepValue were handed. A row's
// read never writes through it.
struct SettingContext
{
    Scene& scene;
    SceneRenderSettings& renderSettings;
};
}
