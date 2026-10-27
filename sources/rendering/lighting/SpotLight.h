#pragma once

#include <cstdint>

#include "core/math/AABB.h"
#include "core/math/Math.h"
#include "core/math/OBB.h"
#include "rendering/lighting/NightLamps.h"

struct SpotLightDesc {
    Math::float3 position = Math::float3(0.0f, 0.0f, 0.0f);
    float        range = 10.0f;
    Math::float3 direction = Math::float3(0.0f, -1.0f, 0.0f);
    float        innerAngle = DirectX::XMConvertToRadians(15.0f);
    Math::float3 color = Math::float3(1.0f, 1.0f, 1.0f);
    float        outerAngle = DirectX::XMConvertToRadians(25.0f);
    float        luminousFluxLm = 1000.0f; // P16.5: lumens, see PhotographicSettings.h
    float        shadowNormalBias = 0.01f;
    float        volumetricIntensity = 1.0f; // plan A4d: UE VolumetricScatteringIntensity -- this light's weight in the fog volume
    float        shadowDepthBias = 0.001f;
    // The shadow projection's near plane, metres (level key "shadowNear", plan S8): a caster nearer the light is not in
    // the map. 0 = automatic (plan S4, render::SpotShadowNear): UE's 1 mm on the atlases' linear depth, 0.1 m on a
    // perspective one. Read the near the light actually uses from SpotLight::GetShadowNear.
    float        nearPlane = 0.0f;
    bool         shadowsEnabled = false; // if false, the spot lights the scene but never casts a shadow
    LampSwitch   onAt = LampSwitch::Always; // plan L: "onAt" -- comes on at night (or by day), NightLamps.h
    float        dayBrightness = 1.0f;   // "dayBrightness": this light's own multiplier by day, gone as the night comes in (Scene::TickNightLamps)
    // Local-light shadow plan S1: UE's ShadowBias / ShadowSlopeBias (LightComponent.cpp:466-467, both 0.5). The depth
    // pass's constant is 15.36/Res m x 2 x shadowBias, its slope part that x 3 x shadowSlopeBias x tan (<= 1).
    // Under the texel bias (the atlas default) they scale its normal / slope push instead, 0.5 = the global texels
    // (render::LocalShadowNormalTexels / LocalShadowPushTexels, plan S8).
    // `shadowDepthBias` above is the OLD receiver bias (NDC), read only with shadow.localBiasUe off.
    float        shadowBias = 0.5f;
    float        shadowSlopeBias = 0.5f;
    // Plan S2: UE's ShadowSharpen (LightComponent.cpp:468, 0): narrows the filtered penumbra, x (7 x this + 1).
    float        shadowSharpen = 0.0f;
};

class SpotLight {
public:
    SpotLight();
    explicit SpotLight(const SpotLightDesc& desc);

    void SetDesc(const SpotLightDesc& d);
    void SetPosition(const Math::float3& position);
    void SetDirection(const Math::float3& direction);
    void SetRange(float range);
    void SetAngles(float inner, float outer);
    void SetColor(const Math::float3& color);
    void SetIntensity(float intensity);
    void SetShadowNormalBias(float bias);
    void SetShadowDepthBias(float bias);
    void SetNearPlane(float nearPlane);

    const SpotLightDesc& GetDesc() const { return desc_; }

    // Plan L: the night lamps' level (NightLamps::Level). The desc keeps the authored flux; what lights the frame
    // and earns a shadow slot is EffectiveFluxLm.
    void SetLampLevel(float level) { lampLevel_ = level; }
    float GetLampLevel() const { return lampLevel_; }
    float EffectiveFluxLm() const { return desc_.luminousFluxLm * lampLevel_; }

    const Math::float3& GetDirection() const { return direction_; }
    float GetInnerAngle() const { return innerAngle_; }
    float GetOuterAngle() const { return outerAngle_; }
    float GetCosInner() const { return cosInner_; }
    float GetCosOuter() const { return cosOuter_; }
    float GetInvAngleRange() const { return invAngleRange_; }
    float GetShadowNormalBias() const { return desc_.shadowNormalBias; }
    float GetShadowDepthBias() const { return desc_.shadowDepthBias; }

    const AABB& GetConeBounds() const { return coneBounds_; }
    const OBB& GetConeOBB() const { return coneObb_; }

    bool PointInsideCone(const Math::float3& point) const;
    bool SphereIntersectsCone(const Math::float3& center, float radius) const;
    bool AABBIntersectsCone(const AABB& bounds) const;

    const Math::mat4& GetViewMatrix() const { return view_; }
    const Math::mat4& GetProjMatrix() const { return proj_; }
    // The near / far planes GetProjMatrix was built with (plan S4: the near resolved by render::SpotShadowNear). The
    // shadow views, the depth pass's linear depth and every receiver read THESE -- one copy of the arithmetic.
    float GetShadowNear() const { return shadowNear_; }
    float GetShadowFar() const { return shadowFar_; }
    Math::mat4 GetViewProjMatrix() const { return view_ * proj_; }

    struct DebugConeParams
    {
        Math::float3 apex;
        Math::float3 direction;
        float        height = 0.0f;
        float        radius = 0.0f;
    };

    DebugConeParams GetDebugConeParams() const;

    void UpdateCachedData();

    // Rung 1 (Step 11) foundation: monotonic version bumped whenever the light's cached
    // (shadow-relevant) transform is rebuilt — a shadow cache compares it to detect a moved
    // light. No consumer yet.
    std::uint32_t GetTransformVersion() const { return transformVersion_; }

private:
    SpotLightDesc desc_{};
    Math::float3 direction_ = Math::float3(0.0f, -1.0f, 0.0f);
    float        innerAngle_ = 0.0f;
    float        outerAngle_ = 0.0f;
    float        cosInner_ = 0.0f;
    float        cosOuter_ = 0.0f;
    float        invAngleRange_ = 0.0f;
    Math::mat4   view_ = Math::mat4::Identity();
    Math::mat4   proj_ = Math::mat4::Identity();
    float        shadowNear_ = 0.1f;
    float        shadowFar_ = 10.0f;
    AABB  coneBounds_{};
    OBB   coneObb_{};
    bool         dirty_ = true;
    float        lampLevel_ = 1.0f; // plan L
    std::uint32_t transformVersion_ = 0; // bumped in UpdateCachedData on an actual change (Step 11)
};

