// Editor Content Browser thumbnail preview shader (Step 12E).
//
// A compact material-aware forward pass for asset thumbnails and editor mini-scenes.
// It consumes the real albedo/MR/normal textures and material flags, including
// alpha-test foliage, while keeping the preview scene isolated and lit by one
// directional key light. Compiled at runtime as vs_5_0 / ps_5_0.

#include "utils.hlsli"
#include "terrain_tiling.hlsli"
// THE RENDERER'S OWN LIGHTING, when gPhysical.x is set: the level's sun in lux, its sky through
// the same split-sum IBL the lighting pass uses, and the tonemap pass's exposure, grade and curve.
// These are the renderer's headers, not copies of them -- the point is that a material looks here
// the way it looks in the level. Left out, knowingly: shadows, AO, local exposure and bloom (image
// operators with no neighbourhood to work on here), and the sun's metal-specular boost.
#include "ibl_common.hlsli"
#include "color_grade.hlsli"
#include "tone_curves.hlsli"

cbuffer PreviewCB : register(b0)
{
    row_major float4x4 gMVP;    // model * view * proj (row-vector convention)
    row_major float4x4 gModel;  // object to preview-world
    float4 gLightDir;           // xyz = light ray direction, w = exposure
    float4 gEyePosition;        // xyz = preview camera position
    float4 gBaseColor;          // real material baseColor factor
    float4 gMetalRoughAlpha;    // xy = metal/rough, z = alpha cutoff, w = MR multiply
    float4 gTexOffsScale;       // xy = UV offset, zw = UV scale
    float4 gTexFlags;           // xyz = use albedo/MR/normal, w = normal strength
    float4 gMaterialFlags;      // x = glTF MR, y = normal RG, z = double-sided, w = albedo exists
    float4 gSurfaceParams;      // rgb = subsurface color, w = transmission strength
    float4 gSurfaceFlags;       // x = shading model ID, y = albedo power, z = normal weight,
                                // w = highlight 0..1 (Mesh Editor hover over a slot's controls)
    float4 gTerrainTiling;      // x = zone size, y = rotation radians, z = scale variance, w = blend
    float4 gTerrainEdgeParams;  // x = edge breakup, y = edge detail, zw reserved
    float4 gAmbient;            // rgb = light color, w = ambient intensity
    float4 gEnvironmentParams;  // x = available, y = exposure, z = max mip, w reserved
    float4 gDebugParams;        // x = normal length, yzw = diagnostic line color
    float4 gMarkerParams;       // x = light-position marker, yzw = marker color
    float4 gSkyboxRight;
    float4 gSkyboxUp;
    float4 gSkyboxForward;
    float4 gSkyboxParams;       // xy = horizontal/vertical tan half-FOV, z = exposure
    // ---- the renderer's lighting (appended: every field above keeps its offset) ----
    float4 gPhysical;           // x = on, y = prefiltered specular mips (0 = none), z = sky
                                // intensity (Skybox::GetExposure), w = sky fill strength
    float4 gSunIlluminance;     // rgb = lux (DirectionalLight::GetEffectiveColor), w = the flat
                                // ambient fraction, used only when there is no irradiance cube
    float4 gGroundAlbedo;       // rgb = ground albedo for the bounce, w = irradiance cube present
    float4 gSunParams;          // x = sun half-apex (radians), y = the light's legacy exposure
    float4 gCamera;             // x = exposure multiplier, y = tone curve (0 ACES, 1 AgX, 2 film)
    float4 gGrade;              // saturation, contrast, gamma, gain
    float4 gGradeOffsetAgx;     // x = grade offset, yzw = AgX slope, power, saturation
    float4 gFilm;               // slope, toe, shoulder, black clip
    float4 gFilmWhiteClip;      // x = white clip
};

Texture2D gAlbedo : register(t0);
Texture2D gMR : register(t1);
Texture2D gNormalMap : register(t2);
TextureCube gEnvironment : register(t3);
TextureCube gSkySpecular : register(t4);    // prefiltered radiance, as the lighting pass reads it
TextureCube gSkyIrradiance : register(t5);  // cosine-convolved, already divided by PI
Texture2D gBrdfLut : register(t6);          // the split-sum environment BRDF
SamplerState gSampler : register(s0);

// What the tonemap pass does to a pixel, minus local exposure and bloom: exposure, then the grade
// in scene-referred linear, then the selected curve. The target is _SRGB and encodes on store, so
// the display code values are decoded first -- the target then stores exactly the bytes the
// tonemap pass would have written.
float3 PreviewCameraResponse(float3 hdr)
{
    hdr *= gCamera.x;
    ColorGradeParams grade;
    grade.saturation = gGrade.x;
    grade.contrast = gGrade.y;
    grade.gamma = gGrade.z;
    grade.gain = gGrade.w;
    grade.offset = gGradeOffsetAgx.x;
    if (!ColorGradeIsNeutral(grade))
    {
        hdr = ApplyColorGrade(hdr, grade);
    }
    FilmCurveParams film;
    film.slope = gFilm.x;
    film.toe = gFilm.y;
    film.shoulder = gFilm.z;
    film.blackClip = gFilm.w;
    film.whiteClip = gFilmWhiteClip.x;
    const float3 display = ToneCurveToDisplay(hdr, (uint)round(gCamera.y), film,
        gGradeOffsetAgx.y, gGradeOffsetAgx.z, gGradeOffsetAgx.w);
    return SrgbToLinear(display);
}

// lighting_cs's GroundBounceOverPi, on this pass's inputs: what comes back up off a ground of the
// level's albedo, lit by the sun and the sky's up-facing irradiance, seen with the view factor of a
// flat plane. The preview's "up" is the asset's +Y, which is how every asset is authored.
float3 PreviewGroundBounceOverPi(float3 N)
{
    if (dot(gGroundAlbedo.rgb, gGroundAlbedo.rgb) <= 0.0f)
    {
        return 0.0f.xxx;
    }
    const float3 sunOnGroundOverPi = gSunIlluminance.rgb * saturate(-gLightDir.y) * kInvPi;
    const float3 skyOnGroundOverPi =
        gSkyIrradiance.SampleLevel(gSampler, float3(0.0f, 1.0f, 0.0f), 0).rgb *
        gPhysical.z * gPhysical.w;
    const float groundViewFactor = (1.0f - N.y) * 0.5f;
    return gGroundAlbedo.rgb * (sunOnGroundOverPi + skyOnGroundOverPi) * groundViewFactor;
}

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 tangent  : TANGENT;
    float2 uv       : TEXCOORD;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 worldPos : POSITION;
    float3 normalW  : NORMAL;
    float4 tangentW : TANGENT;
    float2 uv       : TEXCOORD;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), gMVP);
    output.worldPos = mul(float4(input.position, 1.0), gModel).xyz;
    output.normalW = normalize(mul(float4(input.normal, 0.0), gModel).xyz);
    output.tangentW = float4(
        normalize(mul(float4(input.tangent.xyz, 0.0), gModel).xyz), input.tangent.w);
    output.uv = input.uv;
    return output;
}

struct VertexNormalVSOutput
{
    float3 worldPos : POSITION;
    float3 normalW : NORMAL;
};

VertexNormalVSOutput VertexNormalVS(VSInput input)
{
    VertexNormalVSOutput output;
    // Normal diagnostics are only emitted for the untransformed asset mesh. The light marker uses
    // the regular triangle pipeline and must not make this GS path apply its model matrix twice.
    output.worldPos = input.position;
    output.normalW = normalize(input.normal);
    return output;
}

struct VertexNormalGSOutput
{
    float4 position : SV_POSITION;
};

[maxvertexcount(2)]
void VertexNormalGS(point VertexNormalVSOutput input[1],
    inout LineStream<VertexNormalGSOutput> stream)
{
    VertexNormalGSOutput output;
    output.position = mul(float4(input[0].worldPos, 1.0), gMVP);
    stream.Append(output);
    output.position = mul(float4(
        input[0].worldPos + input[0].normalW * gDebugParams.x, 1.0), gMVP);
    stream.Append(output);
}

float4 DebugPS(float4 position : SV_POSITION) : SV_TARGET
{
    return float4(gDebugParams.yzw, 1.0);
}

float4 PSMain(VSOutput input, bool isFrontFace : SV_IsFrontFace) : SV_TARGET
{
    if (gMarkerParams.x > 0.5)
    {
        const float3 markerNormal = normalize(input.normalW);
        const float3 markerView = normalize(gEyePosition.xyz - input.worldPos);
        const float facing = saturate(dot(markerNormal, markerView));
        const float3 markerColor = max(gMarkerParams.yzw, float3(0.08, 0.08, 0.08));
        const float sphereShade = 0.2 + 0.8 * sqrt(facing);
        const float rim = pow(1.0 - facing, 3.0);
        return float4(markerColor * sphereShade + rim * 0.25, 1.0);
    }

    float2 uv = input.uv * gTexOffsScale.zw + gTexOffsScale.xy;
    const uint shadingModel = (uint)round(gSurfaceFlags.x);
    const bool terrain = shadingModel == kShadingModelTerrain;

    TerrainTileSample terrainTile0 = TerrainIdentityTileSample(uv, 1.0);
    TerrainTileSample terrainTile1 = TerrainIdentityTileSample(uv, 0.0);
    TerrainTileSample terrainTile2 = TerrainIdentityTileSample(uv, 0.0);
    if (terrain)
    {
        TerrainBuildTileSamples(
            uv, gTerrainTiling, gTerrainEdgeParams,
            terrainTile0, terrainTile1, terrainTile2);
    }

    float4 albedoSample = float4(1.0, 1.0, 1.0, 1.0);
    if (gMaterialFlags.w > 0.5)
    {
        albedoSample = terrain
            ? TerrainSampleTextureColor(
                gAlbedo, gSampler, terrainTile0, terrainTile1, terrainTile2)
            : gAlbedo.Sample(gSampler, uv);
    }
    if (gMetalRoughAlpha.z >= 0.0)
    {
        clip(albedoSample.a * gBaseColor.a - gMetalRoughAlpha.z);
    }

    // The tint multiplies the texture in both layouts, as in the G-buffer shaders.
    float3 albedo = gBaseColor.rgb;
    if (gTexFlags.x > 0.5)
    {
        albedo = albedoSample.rgb * gBaseColor.rgb;
    }

    float2 mr = gMetalRoughAlpha.xy;
    if (gTexFlags.y > 0.5)
    {
        float4 packedMR = terrain
            ? TerrainSampleTextureColor(
                gMR, gSampler, terrainTile0, terrainTile1, terrainTile2)
            : gMR.Sample(gSampler, uv);
        float2 texturedMR = gMaterialFlags.x > 0.5
            ? packedMR.bg
            : packedMR.rg;
        texturedMR = lerp(texturedMR,
            texturedMR * gMetalRoughAlpha.xy,
            gMetalRoughAlpha.w);
        mr = texturedMR;
    }
    float metallic = saturate(mr.x);
    float roughness = clamp(mr.y, 0.04, 1.0);

    float3 N = normalize(input.normalW);
    if (gMaterialFlags.z > 0.5 && !isFrontFace)
    {
        N = -N;
    }
    if (gTexFlags.z > 0.5)
    {
        float3 normalTS;
        if (terrain)
        {
            const float2 blendedDerivative = TerrainBlendNormalDerivatives(
                gNormalMap, gSampler, terrainTile0, terrainTile1, terrainTile2,
                gMaterialFlags.y > 0.5);
            normalTS = normalize(float3(-blendedDerivative * gTexFlags.w, 1.0));
        }
        else if (gMaterialFlags.y > 0.5)
        {
            float2 xy = gNormalMap.Sample(gSampler, uv).rg * 2.0 - 1.0;
            float z = sqrt(saturate(1.0 - dot(xy, xy)));
            normalTS = normalize(float3(xy * gTexFlags.w, max(z, 1e-4)));
        }
        else
        {
            normalTS = gNormalMap.Sample(gSampler, uv).xyz * 2.0 - 1.0;
            normalTS.xy *= gTexFlags.w;
            normalTS = normalize(normalTS);
        }
        float3 T = normalize(input.tangentW.xyz);
        float3 B = normalize(cross(N, T) * input.tangentW.w);
        N = normalize(T * normalTS.x + B * normalTS.y + N * normalTS.z);
    }

    float3 L = normalize(-gLightDir.xyz);
    float3 V = normalize(gEyePosition.xyz - input.worldPos);
    BRDFInput bi;
    bi.albedo = albedo;
    bi.rough = roughness;
    bi.metal = metallic;
    bi.N = N;
    bi.V = V;
    bi.L = L;

    const bool hasSky = gEnvironmentParams.x > 0.5;
    const bool foliageModel = shadingModel == kShadingModelTwoSidedFoliage;
    float3 subsurfacePayload = 0.0f.xxx;
    if (foliageModel)
    {
        const float albedoPower = max(gSurfaceFlags.y, 0.0f);
        const float3 albedoTransmission =
            pow(max(saturate(albedo), 1.0e-3), albedoPower);
        subsurfacePayload = gSurfaceParams.rgb * gSurfaceParams.w * albedoTransmission;
    }

    float3 lit;
    if (gPhysical.x > 0.5)
    {
        // lighting_cs, term for term, in its own units: the sun in lux, the fill from the sky's
        // irradiance (or, without one, the flat ambient -- a FRACTION OF THE SUN, which is what
        // that number was authored to mean), the sky's specular through the split-sum IBL added
        // after the light's legacy exposure exactly as the lighting pass adds it, and then the
        // tonemap pass's camera response. With the sun and the sky in the same calibrated units
        // the sky can no longer outshine the sun tenfold, which is what sank lighting the diffuse
        // from the sky in the uncalibrated path below.
        const float3 sunLux = gSunIlluminance.rgb;
        float3 hdr;
        if (gGroundAlbedo.w > 0.5)
        {
            const float3 irradiance = gSkyIrradiance.SampleLevel(gSampler, N, 0).rgb;
            hdr = albedo * (1.0 - metallic) *
                (irradiance * gPhysical.z * gPhysical.w + PreviewGroundBounceOverPi(N));
        }
        else
        {
            hdr = albedo * (1.0 - metallic) * gSunIlluminance.w * sunLux;
        }
        if (foliageModel)
        {
            FoliageResult foliage = EvalFoliageBRDF(
                bi, subsurfacePayload, gSunParams.x, saturate(gSurfaceFlags.z));
            if (foliage.NdotL > 0.0)
            {
                hdr += (foliage.diffBRDF + foliage.specBRDF) * foliage.NdotL * sunLux;
            }
            hdr += foliage.transBRDF * sunLux;
        }
        else
        {
            BRDFResult brdf = EvalBRDF(bi, gSunParams.x);
            if (brdf.NdotL > 0.0)
            {
                hdr += (brdf.diffBRDF + brdf.specBRDF) * brdf.NdotL * sunLux;
            }
        }
        hdr *= gSunParams.y;
        if (hasSky)
        {
            const float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
            const float3 R = reflect(-V, N);
            const uint specularMips = (uint)round(gPhysical.y);
            const float3 skyCol = IblSkyRadiance(gSkySpecular, gEnvironment, gSampler, R,
                roughness, specularMips, gPhysical.z);
            const float cosT = saturate(dot(N, V));
            hdr += skyCol * IblSpecularWeight(gBrdfLut, gSampler, F0, cosT, roughness,
                specularMips);
        }
        lit = PreviewCameraResponse(hdr);
    }
    else
    {
        const float3 radiance = gAmbient.rgb * gLightDir.w;
        // THE DIFFUSE FILL STAYS A CONSTANT on this path, and that was measured, not assumed.
        // Lighting it with the sky itself was tried: the preview's own sun is a radiance of one while
        // a sky cube holds HDR values several times that, so the sky outshone the sun about tenfold
        // and a dark bark texture came out flat blue-grey. The calibrated path above does it properly.
        lit = albedo * (1.0 - metallic) * gAmbient.w * radiance;
        if (foliageModel)
        {
            FoliageResult foliage = EvalFoliageBRDF(
                bi, subsurfacePayload, 0.0f, saturate(gSurfaceFlags.z));
            lit += ((foliage.diffBRDF + foliage.specBRDF) * foliage.NdotL +
                foliage.transBRDF) * radiance;
        }
        else
        {
            BRDFResult brdf = EvalBRDF(bi);
            lit += (brdf.diffBRDF + brdf.specBRDF) * brdf.NdotL * radiance;
        }

        // The sky in the reflections: metals have no diffuse, so without this they are black apart
        // from the one direct highlight. TWO CORRECTIONS, both UE's, and together they are why a
        // rough stick no longer shines like lacquer:
        //  * THE BLUR follows UE's ComputeReflectionCaptureMipFromRoughness over the WHOLE chain. It
        //    was `roughness * min(mips - 1, 5)`, capped at mip 5 -- on a big sky cube a 64-texel
        //    face -- so at roughness 0.8 the sky was still recognisable in the surface.
        //  * THE WEIGHT is EnvBRDFApprox (Karis, mobile split-sum), not raw Schlick. Schlick on N.V
        //    ignores roughness and goes to 1 at grazing angles, which put a bright rim on every
        //    silhouette: at roughness 0.8 a dielectric reflected 20 % of the sky edge-on where the
        //    split-sum integral says about 3 %. `saturate(1 - roughness)` was trying to undo that.
        if (hasSky)
        {
            const float3 reflectionDir = reflect(-V, N);
            const float maxMip = max(gEnvironmentParams.z, 0.0);
            const float levelFrom1x1 = 1.0 - 1.2 * log2(max(roughness, 0.001));
            const float environmentMip = clamp(maxMip - 1.0 - levelFrom1x1, 0.0, maxMip);
            const float3 environmentRadiance = gEnvironment.SampleLevel(
                gSampler, reflectionDir, environmentMip).rgb * gEnvironmentParams.y;
            const float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
            const float NoV = saturate(dot(N, V));
            const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
            const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
            const float4 r = roughness * c0 + c1;
            const float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
            const float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
            lit += environmentRadiance * (f0 * ab.x + ab.y);
        }
    }

    // Mesh Editor: tint the submesh whose material slot / wind-foliage control is hovered, so it is
    // obvious WHICH part of the model a control affects. A rim term makes it readable even on a
    // submesh that is mostly facing away or in shadow.
    const float highlight = saturate(gSurfaceFlags.w);
    if (highlight > 0.0)
    {
        const float rim = pow(saturate(1.0 - saturate(dot(N, V))), 2.0);
        const float3 tint = float3(0.15, 0.55, 1.0);
        lit = lerp(lit, lit * 0.45 + tint * 0.55, highlight * 0.65);
        lit += tint * (rim * highlight * 1.4);
    }
    return float4(lit, 1.0);
}

// Cubemap thumbnails intentionally show the +X face. A fullscreen triangle
// keeps the result in the same 2D target layout as mesh/material previews.
TextureCube gCube : register(t0);

struct CubeVSOutput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD;
};

CubeVSOutput CubeVSMain(uint vertexId : SV_VertexID)
{
    const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };
    const float2 uvs[3] = {
        float2(0.0, 1.0),
        float2(0.0, -1.0),
        float2(2.0, 1.0)
    };

    CubeVSOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    return output;
}

float4 CubePSMain(CubeVSOutput input) : SV_TARGET
{
    const float2 faceUv = input.uv * 2.0 - 1.0;
    const float3 direction = normalize(float3(1.0, -faceUv.y, faceUv.x));
    return float4(gCube.SampleLevel(gSampler, direction, 0.0).rgb, 1.0);
}

float4 PreviewSkyboxPSMain(CubeVSOutput input) : SV_TARGET
{
    const float2 viewPlane = float2(
        (input.uv.x * 2.0 - 1.0) * gSkyboxParams.x,
        (1.0 - input.uv.y * 2.0) * gSkyboxParams.y);
    const float3 direction = normalize(
        gSkyboxForward.xyz +
        gSkyboxRight.xyz * viewPlane.x +
        gSkyboxUp.xyz * viewPlane.y);
    const float3 color = gEnvironment.SampleLevel(gSampler, direction, 0.0).rgb *
        max(gSkyboxParams.z, 0.0);
    // The sky behind the model goes through the same camera as the model in front of it, or the
    // two are in different units and the picture lies about which is brighter.
    return float4(gPhysical.x > 0.5 ? PreviewCameraResponse(color) : color, 1.0);
}

TextureCubeArray gCubeArray : register(t0);

float4 CubeArrayPSMain(CubeVSOutput input) : SV_TARGET
{
    const float2 faceUv = input.uv * 2.0 - 1.0;
    const float3 direction = normalize(float3(1.0, -faceUv.y, faceUv.x));
    return float4(gCubeArray.SampleLevel(gSampler, float4(direction, 0.0), 0.0).rgb, 1.0);
}
