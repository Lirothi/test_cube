# Transparent Objects Lighting and Shadowing in Modern Rendering Engines

## Overview
Transparent or translucent materials allow light to pass through while still interacting with illumination and shadowing cues. Rendering them convincingly is challenging because transparency breaks the single-depth assumption underpinning standard rasterization pipelines and shadow maps. Modern game and real-time engines therefore employ specialized data structures, shading models, and compositing passes to balance physical accuracy with performance.

Key goals for transparent rendering include:

* **Accurate depth ordering.** Ensuring fragments are blended in the correct back-to-front order so that lighting contributions accumulate properly.
* **Consistent lighting response.** Supporting direct lighting, indirect lighting, and reflections in a way that matches opaque surfaces.
* **Shadow integration.** Allowing transparent receivers to cast and receive shadows, including colored transmission and partial occlusion.
* **Performance scalability.** Keeping algorithms tractable across consumer hardware by limiting per-pixel complexity.

## Transparency Classification

Modern engines differentiate between several transparency classes:

1. **Binary transparency** (alpha-tested or masked) uses a hard cutoff and can remain in the opaque pass. It inherits all opaque lighting and shadow features because the geometry writes depth.
2. **Additive or emissive transparency** (e.g., particles) is usually rendered in a forward+ or blended pass without shadow reception to reduce cost.
3. **Physically based transmissive materials** (glass, water, thin film) require refraction, reflection, and partial shadowing. These assets typically use dedicated material domains or shading models with added lighting support.

## Depth Ordering Techniques

Because the GPU depth buffer stores only one surface per pixel, transparent fragments must be resolved in a separate pass. Engines choose between:

* **Sorted draw calls:** Geometry is sorted back-to-front on the CPU. Simple and common for low overdraw scenes (Unity standard transparent queue, Unreal forward shading). Draw order becomes difficult for intersecting surfaces.
* **Depth peeling:** Multiple passes peel successive layers using depth min/max tests. Accurate but expensive; often limited to tools or cinematics.
* **Weighted blended order-independent transparency (WBOIT):** Uses per-pixel color and weight accumulation buffers to approximate sorted blending in a single pass. Supported in engines like Unity's Scriptable Render Pipeline and Unreal's Niagara for particles.
* **Per-pixel linked lists (PPLL):** Records all fragments in GPU memory for sorting per pixel. Offers higher quality but is memory intensive; used selectively in custom rendering pipelines.

## Lighting Models for Transparent Materials

### Forward Shading Path

Many engines evaluate lighting for transparent surfaces in a forward shading pass after opaque rendering. The material shader samples:

* **Direct lighting:** Calculated per-light, respecting surface normal, Fresnel, and transmittance. Forward+ clustering or tiled lighting keeps light counts manageable.
* **Specular reflections:** Sampled via reflection probes or screen-space reflections (SSR). Transparent materials often favor planar or cubemap probes because SSR can fail without a depth hit.
* **Refraction:** Obtained from scene color buffers (grab pass) warped by the surface normal or using ray-marched signed distance fields for thick glass.
* **Transmission and attenuation:** Beer-Lambert absorption models the tint picked up while light travels through the medium.

### Deferred or Hybrid Approaches

Classic deferred shading struggles with transparency because G-buffers cannot store multiple layers. Engines therefore mix deferred opaque passes with forward transparent passes (deferred+). Unreal Engine 5, Unity HDRP, and Frostbite all follow this hybrid pipeline, enabling transparent materials to reuse lighting data (e.g., light lists, shadow maps) computed for opaque geometry.

## Shadow Reception Strategies

Transparent receivers need soft, partial shadows to avoid looking detached. Techniques include:

* **Shadow map sampling with alpha:** Transparent objects use the alpha of their material to modulate shadow map depth tests. Unreal's "Masked" materials and Unity's alpha-tested materials do this in the main shadow pass.
* **Transmission in shadow maps:** For translucent surfaces, engines store opacity or thickness in shadow map texels. Unreal's "Per-Pixel Translucency Shadow" and Unity HDRP's "Thin Transmittance" allow directional lights to evaluate Beer-Lambert absorption during shadow lookups, creating colored lighting through stained glass.
* **Screen-space contact shadows:** After the main shadow pass, a screen-space ray marching step (SSCS) refines contact shadows for thin geometry that missed in the coarse shadow map.
* **Ray traced shadows:** With hardware ray tracing, transparent materials can participate in physically correct shadowing (transmission, caustics). Unreal's Lumen and Unity's HDRP ray-traced shadows provide options to include translucent geometry, although often at higher cost.

## Engine-Specific Implementations

### Unreal Engine (4 & 5)

* Uses a deferred main pass for opaques and forward shading for translucency.
* Translucent materials can enable **"Render After DOF"** and **"Lighting Mode"** options (Surface ForwardShading, Volumetric NonDirectional, etc.) to select lighting features.
* Supports **Translucency Lighting Volume (TLV)** for indirect light and **Separate Translucency** for post-process effects.
* Shadowing: directional lights offer **"Per-Pixel Translucency Shadows"**; local lights provide **"Contact Shadows"** or ray-traced translucency. Materials expose parameters for **"Transmission Color"** and **"Opacity"** to control tint.

### Unity (Built-in & SRP)

* Built-in pipeline sorts transparent objects and shades them in forward passes. Lights affecting transparent objects are limited to four per object unless using deferred + forward add.
* High Definition Render Pipeline (HDRP) adds **"Lit"** and **"Unlit"** transparent material types with features like refraction models (Proxy, Box, Sphere) and screen-space refraction.
* HDRP implements **"Density Volume"** and **"Fog"** for volumetric contributions, and allows transparent receivers to sample shadow maps via **shadow matte** options.
* Universal Render Pipeline (URP) supports approximate WBOIT for particles and uses **"Transparent Receive Shadows"** toggle to opt-in per material.

### Frostbite & Other AAA Engines

* Employ cluster-based or tiled forward lighting for transparency.
* Store transmittance or thickness in auxiliary buffers (e.g., "deep G-buffer") to feed into global illumination and shadowing.
* Integrate volumetric fog and lighting to ensure transparent media participates in atmospheric scattering.

## Volumetric and Participating Media

Transparent rendering extends to volumetric effects (smoke, fog). Engines render these using 3D textures or voxel grids, accumulating lighting along view rays. Shadows are injected via light volumes or froxel grids. Techniques such as **epipolar sampling** and **temporal reprojection** keep volumetric lighting stable and performant.

## Best Practices for Production

1. **Limit overlap complexity:** Reduce self-intersecting transparent surfaces or use WBOIT to avoid sorting artifacts.
2. **Use thickness maps:** Provide per-pixel thickness so absorption and shadowing behave consistently across the asset.
3. **Leverage hybrid lighting:** Keep opaques in deferred passes and transparents in forward passes to share shadow data without duplicating work.
4. **Adjust shadow quality per asset:** High-quality translucency shadows are expensive; reserve them for hero assets.
5. **Balance post-processing:** Effects like bloom, depth of field, and TAA interact strongly with transparent passes; ensure they are configured to avoid ghosting.

## Emerging Trends

* **Hardware-accelerated ray tracing** enables more accurate refraction, caustics, and multi-bounce transmission for glass and water. Engines increasingly expose ray-traced translucency options for next-gen platforms.
* **Stochastic transparency and reservoir sampling** apply Monte Carlo techniques to approximate multiple scattering and caustics in real time.
* **Neural rendering** approaches (neural radiance fields, learned denoisers) aim to reduce the cost of multi-layer light transport and may eventually handle transparency with fewer hacks.

## Suggested Next Steps for the Test Cube Renderer

1. **Stabilize depth ordering in the transparent pass.** `Scene::Pass_Transparent` replays the `TransparentSimple` and `TransparentComplex` buckets without sorting them by eye-space depth or performing order-independent blending, so the draw order currently depends on how objects were inserted into the scene list. Introducing per-frame depth sorting (back-to-front for blending, front-to-back for weighted additive) or adopting a single-pass approximation such as weighted blended order-independent transparency (WBOIT) would keep overlapping glass, particle, and ocean layers consistent even as the camera moves or assets animate.
2. **Disable depth writes when blending is enabled.** Transparency is detected through `RenderableObject::IsTransparent`, which checks whether `GraphicsDesc::blend.RenderTarget[0].BlendEnable` is set. However, `GraphicsDesc::FillDefaultsTriangle` still initializes every pipeline with `DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL`, meaning blended draws can still occlude later layers and break refraction or fog composition. For any material that enables blending, explicitly switch the depth write mask to zero (while keeping depth testing enabled) or provide a material flag that lets artists control depth writes per asset.
3. **Add transmission inputs for forward-lit materials.** The forward shading path used by ocean and other transparent objects currently binds scene color and depth but does not expose per-pixel thickness or shadow-matte data beyond the opaque G-buffer (which stores only albedo, metalness, roughness, and normals). Adding lightweight transmission parameters—such as Beer-Lambert absorption coefficients, thickness maps, or dual-source color outputs—to the transparent shaders would let directional and punctual lights tint through glass, water, or volumetric layers instead of relying solely on additive highlights.

## References & Further Reading

* Unreal Engine Documentation – Translucency Overview
* Unity HDRP Documentation – Transparent Surface Type
* NVIDIA Developer – Advanced Order-Independent Transparency
* SIGGRAPH Courses on Real-Time Rendering Techniques

