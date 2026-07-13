# Tropical Atoll Scene — Implementation Plan

Goal: a new level `data/levels/atoll.json` — a tropical atoll (sandy island ring + turquoise
lagoon) with realistic palm trees and a rock cave containing a burning campfire (flames, smoke,
sparks, flickering light). Art style: **realistic PBR**. The scene doubles as the dogfooding
milestone for the level editor.

Decisions locked with the user:
- Realistic PBR assets (Poly Haven / Sketchfab / ambientCG), not stylized low-poly.
- **Full particle system, GPU-simulated** (compute spawn/update, instanced billboard draw) for
  the fire (flames + smoke + sparks) — not an emissive-only fake and not a CPU sim. Rationale:
  the engine is CPU-submission-bound and the codebase direction is GPU-driven; GPU headroom
  exists after the VSM work.
- **glTF import added to the engine** (cgltf), not manual Blender→OBJ conversion per asset.
- **Skybox stays a DDS cubemap** — the HDRI is converted offline (no runtime .hdr reader).
  Verified: `sources/materials/TextureCube.cpp` passes the DX10-header `dxgiFormat` straight
  through (line ~291) and uploads via `GetCopyableFootprints` (format-agnostic), so a
  BC6H_UF16 (or RGBA16F) cubemap with mips loads with **zero engine changes**. Hard
  requirement: the DDS must carry a DX10 header (legacy-header HDR formats are rejected).

## Current engine state (verified 2026-07-13)

Ready to use:
- **Ocean**: full FFT clipmap ocean (`sources/ocean/`, presets `data/ocean/*.json`) with shore
  depth/foam, SSS, height fog, its own reflection path. Level JSON: `"ocean": { "enabled": true,
  "preset": "data/ocean/default.json" }` (+ `windForce`, `windDirectionDeg` overrides).
  This is the lagoon — it renders as an infinite camera-following clipmap; the island just rises
  above sea level.
- **Skybox**: DDS cubemap per level (`"skybox": {"texture": "textures/skybox.dds"}`,
  `sources/rendering/lighting/Skybox.cpp`). Also feeds ambient/specular fallback.
- **Lights**: directional (CSM+VSM), spot + point with shadows, authored in level JSON and
  spawnable in the editor (`CreateEnvironmentCommand`). Campfire = shadowed point light.
- **Materials**: albedo(sRGB) + `mr`(R=metal,G=rough) + normal, DDS (mipped, BC) or PNG (WIC,
  **no mips**). Presets in `data/materials.json`, per-object overrides in level JSON
  (`ApplyStaticMeshJsonProperties`, `sources/app/scene/SceneObjectFactory.cpp`).
- **Meshes**: OBJ + custom `.txt` only (`sources/rendering/meshes/MeshManager.cpp`), fixed
  `VertexPNTUV`, auto-LOD via meshoptimizer, one material per placed object, `.mtl` ignored.
- **Editor**: AssetRegistry scans `models/` (`.obj/.mesh.txt/.txt`), `textures/` (`.dds/.png`);
  SpawnMeshCommand + CompositeCommand exist.

Gaps this plan closes:
1. No glTF/GLB import (Part A).
2. No alpha-test (masked) or two-sided rendering — required for palm fronds (Part B).
3. Emissive G-buffer target exists (`RT2` in `shaders/gbuffer_common.hlsl`, composed in
   `shaders/compose_cs.hlsl`) but stock `shaders/gbuffer.hlsl` writes 0 (Part C).
4. No particle/billboard system at all (Part D).
5. No light flicker animation (Part E).

Out of scope (explicitly): audio (no backend exists), global atmospheric fog (ocean height fog
only), bloom, DLSS-RR, RT handling of masked geometry (fronds will reflect as opaque in RT
reflections — acceptable; optionally exclude fronds from BLAS).

## Conventions for the executor

- One part = one or more commits; every step ends with a **Release x64 build + run** check.
  Build from PowerShell (never bash):
  `msbuild test_cube.sln /p:Configuration=Release /p:Platform=x64 /m /v:m`
  (match existing configuration names in the sln if they differ).
- **No `dynamic_cast`** — engine forbids it. Use the internal-RTTI virtual accessor pattern
  (`AsRenderableObject()`-style, add `AsParticleEmitter()` etc.).
- New GPU buffers that grow or die with scene objects: remember the LightManager use-after-free
  lesson — never free/reallocate a buffer the in-flight frames still reference. Ring-buffer
  per-frame uploads (pattern already used for light buffers) and pre-size on load.
- After Parts D and F, run the level-switch stress harness: `--scene-stress=30` cycling
  `atoll` ↔ `demo` — new object types (particle emitters) must survive churn with 0 device hangs.
- Headless run/verify recipe (no user present): launch exe, `FindWindow` by class+title to
  confirm alive, capture DBWIN/OutputDebugString for log verdict, screenshot for visual checks.
  Trust the log verdict, not the process exit code (known DLSS teardown flake at exit).
- **Asset gate — stop early when inputs are missing.** Before starting any step that consumes
  user-supplied assets (HDRI, sand/rock textures, palms, boulders, campfire, fire/smoke
  flipbooks), check `import_staging/` for the required files. If they are missing — or unusable
  (wrong format, no license note, absurd poly count) — **end the step early and ask the user to
  download them**. Do NOT silently substitute placeholders and do NOT go hunting for assets
  yourself (license/quality choices are the user's). The request must be self-sufficient:
  restate the relevant instruction from the asset guide below (what to get, selection criteria,
  target folder `import_staging/<name>/` + `source.txt` with the license link) and include a
  direct link, e.g.:
  - HDRI: https://polyhaven.com/hdris (search `beach` / `sunny`, 4K `.hdr`)
  - Sand/rock textures: https://ambientcg.com/list?q=sand , https://polyhaven.com/textures
  - Palms/campfire: https://sketchfab.com/search?features=downloadable&type=models&q=coconut+palm
    (remind: set license filter to CC0/CC Attribution, download as glTF)
  - Boulders: https://polyhaven.com/models (search `rock`)
  - Flipbooks: https://opengameart.org/art-search-advanced?keys=fire+flipbook ,
    https://kenney.nl/assets/particle-pack
  While blocked on an asset, continue with whatever steps don't need it (engine parts A–E are
  never asset-blocked; F0 island is procedural).

---

## Part A — glTF import

Rationale: nearly all free realistic assets ship as glTF/GLB. One-time engine work kills the
per-asset Blender routine.

**A1 — vendor cgltf.** Add `third_party/cgltf/cgltf.h` (single header, MIT). Wire include path
into `test_cube.vcxproj` (`third_party` is already on the include path — just add the file to the
project + filters). No behavior change; build-verify.

**A2 — geometry import.** In `MeshManager::Load`, dispatch `.gltf`/`.glb` → `ParseGltfFile`:
- Walk the node hierarchy, bake node world transforms into vertices.
- For each mesh primitive: positions, normals, uv0, tangents (if absent → reuse the existing
  tangent generation used for OBJ); indices u16/u32 → engine format; build `VertexPNTUV`.
- **Sub-mesh addressing**: engine mesh = one material, so one glTF file yields N engine meshes.
  Support `models/palm.glb#0`, `#1`, … (primitive groups merged by material). Plain
  `models/palm.glb` = primitive 0 (or all merged, executor's call — but keep `#N` exact).
- Reuse `GenerateLods` (meshopt) exactly as for OBJ.
- **Axis/winding gotcha**: glTF is right-handed Y-up; verify against engine handedness with a
  test asset that has readable chirality (text or an asymmetric prop). Expect a Z-flip +
  triangle-winding reverse; confirm visually before proceeding.
- Embedded GLB textures: decode via the existing WIC path from memory blob (WIC supports
  `IWICImagingFactory::CreateDecoderFromStream`); external URIs resolve relative to the file.

**A3 — material import.** From each primitive's material, extract into `MaterialParams`:
- `baseColorTexture`/`baseColorFactor` → albedo/tint.
- `metallicRoughnessTexture`: **glTF packs G=roughness, B=metallic; engine expects R=metal,
  G=rough.** Add a `mrLayout` flag (engine|gltf) to `MaterialParams` + a swizzle in
  `gbuffer_common.hlsl` sampling — do NOT re-encode pixels at load.
- `normalTexture` (+ scale → `normalStrength`).
- `alphaMode`/`alphaCutoff` + `doubleSided` → recorded now, consumed by Part B.
- `emissiveFactor`/`emissiveTexture` → recorded now, consumed by Part C.
- Runtime-only auto-materials (no writes to `data/materials.json`); the editor may later save a
  preset explicitly.

**A4 — editor integration.** AssetRegistry: add `.gltf`/`.glb` to the models root extension
list; enumerate primitives so the content browser can show `palm.glb#fronds`. Spawning a
multi-primitive file = one `CompositeCommand` wrapping N `SpawnMeshCommand`s (one object per
primitive, correct per-primitive material), so undo/redo is atomic and the group is selected
after spawn. Verify: drop a downloaded GLB palm into `models/`, spawn it in the editor, undo,
redo, save level, reload — round-trips clean.

**A5 (optional, do only if PNG shimmer is objectionable in F)** — generate mips for the WIC
path, or batch-convert imported PNGs to BC7 DDS via texconv at import time.

## Part B — masked + two-sided foliage

**B1 — masked G-buffer variant.** Add alpha-test to the G-buffer shader: either
`shaders/gbuffer_masked.hlsl` or a `#define ALPHA_TEST` permutation of `gbuffer.hlsl` —
`clip(albedo.a - cutoff)`. Plumb `staticMesh` JSON knobs `"alphaTest": true`,
`"alphaCutoff": 0.5`, `"twoSided": true` through `SceneObjectFactory` → material/pipeline
(`Material.h` already exposes CullMode; two-sided = `D3D12_CULL_MODE_NONE`). glTF spawn (A4)
sets these automatically from `alphaMode`/`doubleSided`. Note: alpha-tested foliage can shimmer
under DLSS jitter — tune cutoff, accept for now (hashed alpha is a future item).

**B2 — masked shadow passes.** Depth-only shadow paths (CSM, VSM page render, point/spot) treat
everything as opaque today → fronds would cast solid-blob shadows. Add a masked depth variant
(bind albedo SRV + clip) for the **sun path (CSM + VSM) first** — palms are sunlit; point/spot
masked shadows can lag behind (campfire is inside a cave of solid rocks). Watch VSM perf: fronds
are static, so cached/per-page-culled pages keep the cost bounded. Acceptable interim state
after B1 alone: solid shadows (visible but not blocking).

## Part C — emissive meshes

Extend `gbuffer.hlsl` (and the masked variant) with per-object `emissiveColor` (rgb) ×
`emissiveStrength`, optional `emissiveTexture` (from A3). Write into the existing emissive
target (`RT2`); `compose_cs.hlsl` already adds it. JSON knobs on `staticMesh`:
`"emissiveColor": [r,g,b]`, `"emissiveStrength": x`, `"emissiveTexture": "..."`. Default 0 =
zero-cost for existing content. Used for: campfire embers/coals, flame cards inside the mesh
pile. Note: with no bloom pass the glow is subtle — the point light (Part E) carries the effect.

## Part D — particle system (GPU-simulated)

Scope: GPU sim (compute spawn + update) with an instanced billboard draw. Per-frame CPU cost per
emitter = one CB update + 2 dispatches + 1 draw, regardless of particle count — consistent with
the engine's GPU-driven direction and its known CPU-submission bottleneck. Budgets stay modest
for the campfire (≤ ~2K particles per emitter), but the design scales.

**D1 — GPU sim core.** `sources/vfx/ParticleEmitter.{h,cpp}` (+ `ParticleTypes.h`),
`shaders/particle_spawn_cs.hlsl`, `shaders/particle_update_cs.hlsl`:
- `EmitterDesc` (JSON-serializable, unchanged by the GPU choice): `maxParticles`, `spawnRate`,
  `lifetime` [min,max], `initialSpeed` [min,max] + cone (direction, angle), `gravity`
  (negative = buoyancy), `drag`, `sizeOverLife` (start→end), `colorOverLife` (≤4 gradient
  keys, RGBA — A drives fade), `rotation` [min,max] + `spin`, `flipbook` {cols, rows, fps,
  randomStart, frameBlend}, `blendMode` (additive|alpha), `texture`, `localSpace` (bool),
  `sortParticles` (bool).
- **Buffers per emitter** (DEFAULT heap, persistent): particle state buffer
  `Particle[maxParticles]` (UAV/SRV), dead-list `uint[maxParticles]` + atomic counter (small
  counter buffer). One-time init dispatch fills the dead list. **Slot-array + dead-list scheme;
  no alive-list and no indirect draw needed** (see D2's degenerate-quad trick) — upgrade to
  `ExecuteIndirect` later when GPU-driven submission infra (shadow Rung 0) lands.
- **Spawn CS**: CPU accumulates fractional `spawnRate*dt` and passes an integer spawn count via
  root constants; CS consumes slots from the dead list, initializes particles. GPU RNG =
  PCG/Wang hash of (slot, frameIndex, emitterSeed) — no CPU randomness.
- **Update CS**: integrate velocity/gravity/drag, age, kill (push slot back onto the dead list),
  evaluate nothing that the VS can evaluate later (keep state minimal: pos, vel, age, life,
  rot, spin, seed).
- Curves/gradients (`sizeOverLife`, `colorOverLife`) pack into the per-emitter CB and are
  evaluated in-shader from normalized age — editor tweaks = CB update only, no buffer rebuild.
- Sim driven from the object `Tick` (same hook as `RotatingObject`) recording dispatches;
  editor pause stops Ticks → sim freezes naturally. Add a `vfx::g_freeze` debug toggle and an
  optional alive-count readback (debug HUD) — GPU sims are otherwise opaque to debug.

**D2 — rendering.** New renderable `ParticleEmitterObject` (subclass `RenderableObjectBase`,
`IsTransparent()=true`, `RenderLayer::Transparent`) → lands in the sorted `TransparentSimple`
bucket and draws inside `Pass_Transparent` (`SceneRenderer.cpp`, `Main_Transparent` — blending
already runs in sorted-queue order there):
- `shaders/particles.hlsl`: VS reads the particle state buffer as SRV, indexed from
  `SV_VertexID/6`; **dead slots emit degenerate (zero-area) triangles** — this is what lets us
  draw `maxParticles` quads unconditionally without an indirect draw or CPU-visible count.
  Alive slots expand a camera-facing quad; PS samples the flipbook atlas (frame from
  normalized age; optional frame-blend between adjacent frames — cheap and hides low flipbook
  fps); depth-test ON, depth-write OFF; additive or premultiplied-alpha blend state per emitter.
- No per-frame CPU upload of particle data at all; only the emitter CB.
- Particles are absent from G-buffer/shadow/RT — intended (no reflected/shadow-casting fire).
- **D2b (optional polish)**: soft-particle depth fade using the scene depth SRV already
  available to the transparent pass.
- **D2c — sorting for `alpha` emitters (smoke)**: single-workgroup bitonic sort of alive slots
  by view depth into a small index buffer, VS indexes through it (fine up to ~1–2K particles —
  enforce `maxParticles` ≤ sort capacity when `sortParticles` is set). Additive emitters (fire,
  sparks) skip it. Interim state before D2c lands: keep smoke opacity low — premultiplied alpha
  at low opacity hides most order artifacts.
- **Hazards** (both bit us before — see scene-stress history): (1) per-emitter DEFAULT-heap
  UAV buffers die with the object → release must be deferred/GPU-idle-safe on level switch;
  (2) UAV↔SRV transitions each frame go through the ResourceStateTracker, and the sim writes
  while a *previous frame's* draw may still read → either double-buffer the state buffer or
  prove the render-graph ordering makes it safe. `--scene-stress=30` is the gate.

**D3 — authoring + editor.** Register `"particleEmitter"` in `SceneObjectRegistry` /
`SceneObjectFactory`. Level JSON mirrors the ocean pattern:
`{"type":"particleEmitter", "preset":"data/particles/fire.json", "position":[...],
"overrides":{...}}`. Editor: spawnable (SpawnMesh-style or via CreateEnvironmentCommand path),
selectable, movable with the gizmo, properties in InspectorPanel, round-trips through document
save/load like meshes do. Respect the editor ID model from the level-editor plan.

**D4 — presets + tuning.** `data/particles/fire.json`, `smoke.json`, `sparks.json`:
- Fire: additive flame flipbook, buoyant (gravity ≈ −2..−4), lifetime 0.5–1.0 s, grows then
  shrinks, orange→deep-red gradient.
- Smoke: alpha-blend, slow, long lifetime (2–4 s), grows steadily, gray with low alpha, mild
  cone spread, sorted.
- Sparks: tiny additive dots (or 1×1 flipbook), high initial speed, real gravity, short life.
Verify inside the actual cave lighting, not in the void.

## Part E — flickering point light

`pointLights[]` JSON: `"flicker": {"amplitude": 0.35, "frequencyHz": 7, "seed": 3}` —
modulate intensity (and optionally radius ±10%) in a Tick using layered sines / value noise
(NOT white noise per frame — that strobes). Editor inspector support. Keep `shadowsEnabled:
true` for the campfire — pointlight shadows exist and are cheap for one light.

## Part F — scene assembly (`data/levels/atoll.json`)

Prefer assembling **in the editor** (this is the dogfooding goal), saving via the document
pipeline; hand-edit JSON only for things the editor can't author yet.

**F0 — procedural island mesh.** No good free atoll meshes exist; generate one:
`tools/gen_island.py` (pure Python, writes OBJ directly — no Blender dependency): a ring-shaped
heightfield (radial gaussian ring + low-frequency noise), gentle beach slope crossing y=0 (ocean
shore-depth params need real underwater geometry to fade against), a flattened area for the
cave/camp, planar top-down UVs sized so sand tiles via `texOffsScale`. ~50–150k tris. Also
generate a simple lagoon-floor disc (sand, slightly below sea level) so the lagoon reads
turquoise-over-sand rather than deep-ocean.
**Sea level convention: ocean plane sits at y=0; author everything against that.**

**F1 — environment.** Skybox pipeline (offline, scriptable — add `tools/hdri_to_cubemap.*`):
1. equirect `.hdr` → 6 cube faces: cmft CLI, or a ~50-line Python script (executor-written;
   simple gnomonic projection, keep float precision — write `.hdr`/`.exr` faces);
2. faces → cubemap DDS: `texassemble cube -f R16G16B16A16_FLOAT ...` (DirectXTex reads `.hdr`);
3. compress + mips: `texconv -f BC6H_UF16 -m 0` (BC6H needs the DX10 header — texconv writes
   one automatically for BC6H; keep full mip chain, the skybox doubles as ambient/specular
   source). Fallback if BC6H misbehaves: stay RGBA16F (loader takes both), 4× size.
Smoke-test first: load the converted cubemap in place of `textures/skybox.dds` on the demo
level before building anything else on top. Cube face order/orientation mistakes are the
classic failure — verify sun position matches the HDRI.
Sun: warm, ~35–55° elevation, azimuth chosen so
palm shadows rake across the beach. Ocean: enable with a copied+tuned preset
`data/ocean/atoll.json` — lower wind, turquoise SSS/scatter tint, strong shore foam; check
`GetShoreDepthParams` behavior against the island slope.

**F2 — island dressing.** Island mesh (`renderLayer: "Terrain"`, sand material from the asset
guide, tiled). Palms: imported GLB via A4 (trunk+fronds object pairs), 8–15 around the ring,
varied yaw/scale; if count grows, switch repeated palms to `instancedModels`. Rocks: 5–10
photoscanned boulders composed into an outcrop + walk-in cave (assembling this in the editor is
the point); a dark interior "cap" rock kills skylight leaks.

**F3 — campfire.** In the cave: logs+stones mesh (asset guide), embers with emissive material
(Part C), three emitters — fire, smoke (drifting toward the cave mouth via cone direction),
sparks — and the flickering shadowed point light (Part E) at flame height.

**F4 — polish + verification.** Camera start on the beach facing the cave. Tune: ocean foam at
the shoreline, palm shadow quality (B2), fire readability from the beach at dusk-ish exposure.
Screenshot set: beach wide shot / cave interior / fire close-up. Run `--scene-stress=30`
(atoll↔demo). Confirm visuals with the user before calling it done (per screenshot-verification
rule).

## Risks / gotchas summary

- glTF handedness/winding vs engine — verify first thing in A2 with an asymmetric test asset.
- glTF MR channel layout (G=rough, B=metal) vs engine `mr` (R=metal, G=rough) — shader-side
  `mrLayout` flag, not pixel re-encode.
- WIC-loaded PNG has **no mips** → distant shimmer; final textures should be BC7/BC5 DDS
  (texconv), or do A5.
- One material per engine object — glTF multi-primitive spawn (A4) is the workaround; palm =
  trunk object + fronds object grouped by CompositeCommand.
- Alpha-test + DLSS jitter shimmer; masked geometry opaque in RT reflections (accept/exclude).
- Particle GPU buffers: frame-in-flight safety on destroy (deferred release), UAV↔SRV hazard
  between sim and last frame's draw (double-buffer or prove ordering), scene-stress the level
  switches.
- GPU sim debuggability: no CPU-side particle state — rely on `vfx::g_freeze`, alive-count
  readback, RenderDoc. Budget extra time for the first "why is nothing spawning" hour.
- Skybox DDS must have a DX10 header (TextureCube rejects legacy-header HDR formats); cube
  face order/orientation is the classic conversion bug — smoke-test on the demo level first.
- Emissive without bloom is subtle — the flickering point light sells the fire, not the glow.

---

# Гайд по ассетам (для человека)

Всё складывай в `import_staging/<имя-ассета>/` как скачалось (glTF/GLB + текстуры, PNG/JPG/HDR
как есть). Конвертацию в DDS, упаковку metal/rough в MR, разбиение по материалам — делает
ИИ-исполнитель (texconv из DirectXTex + скрипты), тебе руками ничего конвертировать не нужно.
Рядом кидай текстовый файл `source.txt` со ссылкой и лицензией — исполнитель соберёт CREDITS.md.

Качать всё заранее не обязательно: если на каком-то шаге исполнителю не хватит ассета, он
остановит шаг и попросит недостающее, повторив инструкцию и дав прямую ссылку (это прописано в
конвенциях выше, «Asset gate»). Так что можно начинать с пустым `import_staging/` — движковые
части A–E от ассетов не зависят.

## Что качать

**1. Skybox (небо):** [polyhaven.com/hdris](https://polyhaven.com/hdris) → категория
Skies/Outdoor, поиск `beach`, `tropical`, `sunny`. Нужен солнечный день с кучевыми облаками,
солнце не в зените. Качай **4K .hdr** (не .exr, не tonemapped). Лицензия CC0. Одного файла
достаточно. Конвертировать ничего не нужно: .hdr → DDS-кубмапа (BC6H) делается офлайн-скриптом
исполнителя, движок это уже читает (проверено — TextureCube принимает DX10-форматы как есть).

**2. Текстуры песка и камня:** [ambientcg.com](https://ambientcg.com) (CC0) и
[polyhaven.com/textures](https://polyhaven.com/textures) (CC0):
- Песок пляжный с рябью: на ambientCG ищи `Sand` (например «Sand с ripples»), на Poly Haven —
  `beach`, `coast sand`.
- Скала/камень для пещеры: `Rock`, `Cliff` (серо-коричневый, не лава).
- Качай **2K, формат PNG**, набор каналов: Color/Albedo, Normal (GL или DX — исполнитель
  разберётся, но отметь в source.txt какой), Roughness. Metallic для песка/камня не нужен.

**3. Пальмы (самое важное):** [sketchfab.com](https://sketchfab.com) → поиск `coconut palm` /
`palm tree`, слева фильтры **Downloadable** и License = **CC0** или **CC Attribution**. Качай в
**glTF** формате. Критерии выбора:
- реалистичная (не мультяшная), листья текстурой с прозрачностью (это норма), не «на миллион
  полигонов» — до ~100k треугольников;
- желательно 2–3 разных пальмы (высокая, наклонённая, молодая) для разнообразия.
Также проверь [polyhaven.com/models](https://polyhaven.com/models) — у них появляются
фотосканы деревьев/камней (CC0, идеальное качество). И если ты успел забрать бесплатную
библиотеку **Quixel Megascans на Fab** (акция была до начала 2025) — это лучший источник камней
и растительности; на [fab.com](https://fab.com) есть и текущий free-раздел (лицензия Fab
Standard позволяет использовать в своём движке).

**4. Камни для пещеры:** Poly Haven models → rocks/boulders (фотосканы, CC0) — 4–6 разных
валунов; либо Sketchfab `rock scan`, `boulder` (CC0/CC-BY, glTF). Пещеру соберём из валунов
прямо в редакторе. Если найдёшь готовый цельный `cave` меш с интерьером — тоже неси, но валуны
надёжнее.

**5. Костёр:** Sketchfab `campfire` (CC0/CC-BY, glTF) — брёвна + круг камней. Отдельно хорошо
бы текстуру углей/жара для emissive (подойдёт albedo углей из самого ассета или `Lava`/`Coals`
с ambientCG).

**6. Спрайты огня и дыма (для частиц):**
- [opengameart.org](https://opengameart.org) — поиск `fire flipbook`, `fire sprite sheet`,
  `smoke sheet` (фильтр по лицензии CC0). Нужен атлас кадров огня (сетка типа 4×4/8×8) и
  мягкие клубы дыма.
- [kenney.nl/assets](https://kenney.nl/assets) → Particle Pack (CC0) — хорошие мягкие дымы и
  вспышки, пригодятся как минимум для дыма/искр.
- Запасной вариант: исполнитель сгенерит флипбук сам (Blender Mantaflow) — но готовый атлас
  быстрее.

**7. Остров:** качать не нужно — меш острова исполнитель сгенерит процедурно
(`tools/gen_island.py`), песок натянем тайлингом из п.2.

## Лицензии
CC0 — бери не думая. CC-BY — можно, но нужно указать автора: просто сохрани ссылку в
`source.txt`, исполнитель заполнит CREDITS.md. Ничего с пометкой Editorial/NoAI/
NonCommercial-без-нужды лучше не брать, чтобы не разбираться.

## Порядок работ (рекомендация)
Части A и B (glTF + листва) — до того, как понадобятся пальмы; часть D (частицы) можно делать
параллельно со сбором ассетов. Минимальный первый визуальный результат: A + F0 + F1 (остров,
океан, небо, солнце) — уже смотрибельно и мотивирует; дальше пальмы (B), пещера, костёр (C+D+E).
