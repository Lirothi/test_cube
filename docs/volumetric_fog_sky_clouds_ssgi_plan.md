# Объёмный туман, процедурное небо, объёмные облака, SSGI — план

Написан 2026-09-05. Четыре части, каждая — своя цепочка шагов с замером, коммит за юзером после
каждого шага. Порядок: **A** объёмный туман (froxel, лучи света сквозь кроны) → **D** SSGI (отскок от
песка в тенях, независим от A) → **B** процедурное небо (Hillaire/UE SkyAtmosphere, время суток) →
**C** объёмные облака (UE VolumetricCloud, нужны LUT неба). Связи между частями — §4.

## 0. Правила работы (обязательно к прочтению)

1. **Читать UE-дроп первым, транскрибировать, не выводить.** Все четыре фичи у Epic есть в
   `D:/Programming/ue_strip/Shaders/Private/` и `Source/Runtime/Renderer/Private/`; файлы и строки
   ниже. Каждая формула, взятая из UE, помечается `file:line`; каждая ДЕЛЬТА (единицы, reverse-Z,
   наш IBL вместо их skylight) записывается в разделе шага явно. Урок `height_fog.hlsli`: первая
   версия, выведенная «с нуля», расходилась с UE в четырёх местах, и все четыре были видны.
2. **Единицы.** UE — сантиметры, мы — метры. Их `FogDensity 0.02` не переносится; их безразмерные
   константы (`DirectionalInscatteringExponent 4`, `HistoryWeight 0.9`, `PhaseG 0.2`,
   `DepthDistributionScale 32`) переносятся. Атмосфера у UE в километрах — переносим в км с явным
   масштабом мира (`kMetresPerKm`).
3. **Один бинарь на A/B, пол шума ДО замера, ≥ 3 прогона пола** ([[verify-artefact-not-log]],
   [[justify-the-metric]]). Замер только там, где пасс реально идёт (VSM-урок S5b.2: статичная камера
   с `--wind-freeze` пропускает апдейт). **Паритет VSM снимать с `--set=vsm.smrtRayCount:0`** (дизер
   SMRT крутится по номеру кадра, пол 6 %).
4. **Диагностика — только LOG_* в session-лог и debug-view в композе**, новых `logs/<name>.log` нет.
5. **Ручки не лгут:** дефолт включается только после приёмки шага; инертный контрол удаляется.
6. **Гейт по типу правки:** новые пассы/ресурсы/барьеры → `--scene-stress-gbv=20` (Legacy и VSM),
   `--log-stress` 0/0, три конфига (Debug / Release / Release_Editor). Шейдерная математика → паритет
   и глаза, не GBV.
7. **Кросс-кадровое состояние коммитится в билдере** (pass-flow), не в записи; readback-кольца по слоту.
8. **Смотреть глазами** ([[metric-cannot-see-image]]): лучи света, облака и GI — это КАРТИНКА; метрика
   ловит регрессии, вердикт по виду за юзером (`--shot`, side-by-side через `pair_diff.py`).

## 1. Текущее состояние (baseline, 2026-09-05, HEAD c04616b + S6 uncommitted)

### F1. Что есть и что мы переиспользуем
* **Туман:** `shaders/height_fog.hlsli` — аналитический exponential height fog, транскрипция
  `HeightFogCommon.ush` (`CalculateLineIntegralShared`, exp2, Taylor у нуля, floor maxOpacity с
  освобождением), сэмплится в `compose_cs.hlsl:349-410` (только геометрия; небо = «туман бесконечной
  глубины», не туманится) и в обоих океанских шейдерах. Параметры — `AtmosphereSettings`
  (`SceneFrameData.h:156`), CB-блок `fogParams0..2`, `fogSunDir`, `fogSunColor`, `fogDebugView`
  (1 = transmittance, 2 = inscatter). Солнце: `SceneRenderer_Lighting.cpp:745` (`toSun`,
  `dirLight->GetEffectiveColor()`). **В шапке файла: «NO FROXEL VOLUME — первая версия аналитическая».**
* **Тени солнца из compute:** `lighting_cs.hlsl:324 SampleSunShadow(P, N, ndl, cascade, pixel)` — CSM
  (`csm_sample.hlsli:273 CsmSampleShadow`) или VSM (`vsm_sample.hlsli:235 VsmClipmapShadow`, SMRT,
  screen-ray) + контактные. Для тумана нужен ТОТ ЖЕ сэмплинг без receiver-bias и без контактных
  теней — обёртка над теми же include с `N = -sunDir`, `ndl = 1`, `rayCount = 0`.
* **Локальные источники:** `spotlight_cs.hlsl` / `pointlight_cs.hlsl` читают буферы LightManager с их
  атласами/VSM-страницами (`VsmSpotShadow`, `VsmPointShadow`, `vsm_sample.hlsli:128/149`).
* **HZB:** `Pass_Hzb` (furthest, min reverse-Z, половина разрешения, `D.hzb/hzbSRV/hzbMips`), «closest»
  цепочка `D.hzbClosest` СТРОИТСЯ при `ssrHiz` и держится в коде ровно под SSGI (комментарий в
  `SceneRenderer.cpp` DecideFrame: «P9's screen-space GI is the next consumer»). После S5 пирамида
  строится дважды (HzbA/Hzb).
* **Небо и IBL:** `skybox.hlsl` — HDRI-кубмапа `sky` × exposure; IBL — `SkySpecular` (префильтр,
  `skySpecMipCount`) + `SkyboxTex`, `IblSkyRadiance(...)` в `ibl_common.hlsli`; irradiance для
  диффуза — там же. Свёртка при загрузке HDRI (статическая).
* **Deferred ring:** `RenderTargetManager::DeferredTargets` × 3 слота (`GetDeferredForFrame` /
  `GetDeferredForPrevFrame`), история GTAO/SSR живёт так; 3D-текстур в кольце пока нет (создание —
  `RenderTargetManager.cpp:135..593`, `render::CreateCommittedTexture`).
* **Async compute:** `Main_BuildAS`, `Main_ObjectCompute`, `Main_RTTrace` на второй очереди
  (`SceneRenderer_Graph.cpp:83/192/885`); правила D7 (NON_PIXEL для compute-очереди, явные mtDeps).
* **Temporal-инфра:** GTAO temporal (`gtao_temporal_cs.hlsl`, история по слоту, `gtaoHistoryFrames_`),
  SSR temporal (`ssr_temporal_cs.hlsl`), камера: `GetPrevViewProjMatrix()` (джиттерная),
  `GetPrevViewProjMatrixNoJitter()`, `GetHistoryRevision()` (cut).
* **Экспозиция:** `preExposure_` решается в `DecideFrame`, сцена хранится pre-exposed; UE держат
  froxel-текстуры pre-exposed и снимают экспозицию при чтении (`HeightFogCommon.ush:448`) — делаем так же.
* **Ветер:** `wind.hlsli`, `frame.wind` (для сдвига облаков и шума тумана — опционально).

### F2. Чего нет
Froxel-объём, лучи света, объёмный туман на прозрачных/частицах; процедурное небо (только HDRI),
солнечный диск, время суток, динамический IBL; облака любые (кроме HDRI-запечённых); SSGI/любой
динамический GI; карта теней облаков.

### F3. Числа (Release, `--profdump`, 30 с; из плана окклюжена)
Роща `80.08 6.32 40.58` с ветром: GPU.Frame ≈ 2.0 мс (VSM), Pass_VsmPageRender 0.19, Pass_Lighting
≈ 0.10, GTAO ≈ 0.10, Pass_Hzb 0.03. Стена K=4 Legacy: 2.72 мс. **Бюджеты этого плана:** туман
≤ 0.30 мс @1080p (UE на тех же 16-пиксельных фрокселях — 0.2–0.5), LUT неба ≤ 0.10 мс (только при
смене солнца), облака ≤ 1.0 мс на половинном разрешении, SSGI ≤ 0.8 мс на половинном. Всё
измеряется на одном бинаре против выключенной ручки.

### F4. Камеры для замеров
* **Роща под солнце** — направление света в уровне `(-0.5047, -0.4737, -0.7218)` (`wind_test.json:20`),
  высота солнца ≈ 28°. Из точки рощи `80.08,6.32,40.58` взгляд на солнце `(+0.50,+0.47,+0.72)` смотрит
  В ОТКРЫТОЕ МОРЕ (пальмы кольцом x −83..83, z −89..85; эта точка — край кольца), кадр = небо + вода.
  Рабочий рецепт (A2, 2026-09-05): камера на 22 м «вниз по солнцу» от самого плотного кластера пальм
  на юго-западной дуге (26 пальм в радиусе 12 м, ещё 73 вдоль луча солнца), над водой:
  `--cam-pos=-37.61,2.50,-98.03 --cam-rot=-0.0997,0.2987,0.0314,0.9486` (тангаж +12°) или
  `--cam-rot=-0.1656,0.2958,0.0522,0.9393` (+20°). Кватернион — по рецепту look-at из памяти
  [[csm-scissor-optim]] (проверен на камере теней с точностью 1e-3).
* **Камера теней** `--cam-pos=15.07,5.13,69.20 --cam-rot=-0.0495,0.9505,-0.2171,-0.2167` (паритет).
* **Остров** `--cam-pos=-312.27,218.14,119.23 --cam-rot=0.1445,0.8409,-0.2736,0.4440` (аэроперспектива,
  небо, облака, горизонт).
* **Стена C** `--cam-pos=20.00,40.00,30.00 --cam-rot=0.3330,0.1891,-0.0684,0.9212` (SSGI: тень стены на
  песке, отскок от стены).
Рецепт паритета: `--dlss=off --wind-freeze --shot-delay=8 --set=exposure.autoExposure:0`, VSM с
`vsm.smrtRayCount:0`, пол 0.03 % пикселей.

## 2. Как это устроено у UE 5.6 (разведка 2026-09-05, файлы дропа)

### 2.1. Volumetric fog (`VolumetricFog.usf` 1130 строк, `VolumetricFog.cpp` 1997, `HeightFogCommon.ush` 538)
* **Сетка:** `r.VolumetricFog.GridPixelSize 16`, `GridSizeZ 64`, `DepthDistributionScale 32`
  (`VolumetricFog.cpp:56-75`); XY = экран/16 с округлением вверх (`:1213 GetVolumetricFogGridSize`).
* **Распределение слайсов по глубине** — `RenderUtils.h CalculateGridZParams(Near, Far, S, GridSizeZ)`:
  `slice = log2(z·B + O)·S`, `NearOffset = 0.095·100` (см!), `N = Near + NearOffset`, `O = (F − N·2^(GridSizeZ/S)) / (F − N)`,
  `B = (1 − O)/N`; обратно `depth = (2^(slice/S) − O)/B` (`Common.ush:2379-2388`). Near = `max(near,
  VolumetricFogStartDistance)` (`VolumetricFog.cpp:1204-1209`), Far = `VolumetricFogDistance` компонента.
* **Позиция ячейки** (`VolumetricFog.usf:66-76`): `VolumeUV = (coord + offset)/GridSize`, NDC с
  флипом y, глубина слайса → device z → `UnjitteredClipToTranslatedWorld` (матрица БЕЗ джиттера).
* **MaterialSetupCS** (`:122-140`): плотность = exp-height-fog в позиции ячейки, `× 0.5`
  («Exponential height fog interprets density differently, match its behavior»), extinction =
  `density × GlobalExtinctionScale`, scattering = `Albedo × extinction`; пишет `VBufferA(rgb
  scattering, a extinction)`, `VBufferB` emissive.
* **LightScatteringCS** (`:766-1000`): per-ячейка, джиттер `FrameJitterOffsets[0..15]` = Halton(2,3,5)
  по номеру кадра (`VolumetricFog.cpp:196-207`), суперсэмплинг ×4 при промахе истории
  (`HistoryMissSupersampleCount 4`); солнце: `Color × Shadow(P) × HG(PhaseG, dot(L, −V))`
  (`:888`), тень — cascaded/VSM/RT + карта теней облаков (`:874-880`, `CloudShadowmapStrength`);
  skylight: `SkyLightColor × SH(−V·PhaseG)` × sky visibility (`:936-942`); локальные источники из
  light grid с `InverseSquaredLightDistanceBiasScale` против алиасинга у источника (`:958-963`), soft
  fading по размеру ячейки; результат `PreExposure × (L × scattering + emissive), extinction`;
  **история** `lerp(cur, hist, HistoryWeight 0.9)` с реэкспозицией (`:1027-1033`), UV истории по
  `UnjitteredPrevTranslatedWorldToClip`, вне экрана/за глубиной → вес 0 (`:812-818`, `FixupHistoryUV
  :699-765` по conservative depth).
* **FinalIntegrationCS** (`:1075-1120`): front-to-back по слайсам, `T = exp(−σ·step)`,
  **энергосохраняющая интеграция Frostbite** `S·(1 − T)/σ`, `NearFadeIn` по накопленной глубине,
  пишет `(накопленный свет, накопленная T)` в каждый слайс.
* **Применение** (`HeightFogCommon.ush:430-460 CombineVolumetricFog`): `lookup = Integrated.Sample(uv,
  slice(depth))`, снять pre-exposure, ступенька до `VolumetricFogStartDistance`, итог
  `rgb = Vol.rgb + GlobalFog.rgb × Vol.a; a = Vol.a × GlobalFog.a`, где GlobalFog — аналитический
  туман ЗА пределами объёма (exclude distance = VolumetricFogMaxDistance).
* **Хранение:** обе 3D-текстуры pre-exposed, RGBA16F; ресурс — размер scene textures / 16, чтобы не
  переаллоцировать при dynamic res (`:179-188`).

### 2.2. Sky atmosphere (`SkyAtmosphere.usf` 1885, `SkyAtmosphereCommon.ush` 351, `SkyAtmosphereRendering.cpp` 2245)
Hillaire 2020 «A Scalable and Production Ready Sky and Atmosphere Rendering Technique»: LUT
transmittance 256×64 (10 сэмплов, `:157-172`), multi-scattering 32×32 (15 сэмплов, `:179-194`),
fast SkyView LUT 192×104 (`:102-107`, сэмплы 4..32 по дистанции), aerial-perspective volume 32×32×16
слайсов на 96 км (`:121-137`), distant sky light LUT на высоте 6 км (`:200-206`) — это их skylight
для тумана/облаков. Шейдеры: `RenderTransmittanceLutCS :1100`, `RenderMultiScatteredLuminanceLutCS
:1156`, SkyView, CameraAerialPerspectiveVolume, `RenderSkyAtmosphereRayMarchingPS :861` (небо из LUT
+ солнечный диск `GetLightDiskLuminance :313`). Параметры планеты — Bruneton (Earth 6360/6420 км,
Rayleigh/Mie/озон), в км.

### 2.3. Volumetric clouds (`VolumetricCloud.usf` 2488, `VolumetricCloudRendering.cpp` 3164)
Слой между двумя высотами над планетой, raymarch по view-лучу: сэмплов 2..768 по дистанции
(`DistanceToSampleMaxCount 15 км`, `ViewRaySampleMaxCount 768`, отражения 80; `:45-66`), пропуск
пустоты по conservative density (`:70`, `:816-870`); фаза — две лопасти HG (`:329-335`), участвующая
среда с октавами multi-scattering (`SetupParticipatingMediaContext :376`, `MsScattFactor/MsExtinFactor`),
тень к солнцу — вторичный марш `Shadow.ViewRaySampleMaxCount 80` (`:1071`, `:1153`) ИЛИ карта теней
облаков (`GetCloudVolumetricShadow :1077`, `r.VolumetricCloud.ShadowMap` 512², snap, temporal
`:157-207`), sky AO (`:125-150`), ambient из distant sky light LUT (`:730`), аэроперспектива из AP LUT.
Материал облака (плотность из шумов) у UE — материал; у нас будет фиксированный шейдер по Schneider
2015 (Perlin-Worley 128³ + detail 32³ + weather 2D). Рендер в `VolumetricRenderTarget` (половина/четверть
разрешения, temporal reconstruction).

### 2.4. SSGI (`Shaders/Private/SSRT/`: `SSRTDiffuseIndirect.usf` 586, `SSRTRayCast.ush` 748, `SSRTPrevFrameReduction.usf` 353; `ScreenSpaceRayTracing.cpp`)
`r.SSGI.Quality 1..4` = лучей 4/8/16/32 × шагов 8 (12 при Q4), тайлы 8×8 (`SSRTDiffuseIndirect.usf:22-70`),
Hammersley16 по лучу + случайное семя (`:413-424`), марш против **furthest HZB** (`:454-457`,
`SSRTRayCast.ush`), цвет попадания — из РЕДУЦИРОВАННОГО цвета прошлого кадра (`SSRTPrevFrameReduction`,
leak-free репроекция `r.SSGI.LeakFreeReprojection 1`, `:55`), промах → небо на `SkyDistance`,
`RejectUncertainRays 1` (`:70`); выход — диффузный indirect + AO, затем денойзер SSD и композ
(`DiffuseIndirectComposite.usf`) в diffuse indirect term вместо skylight.

## 3. Наш дизайн — что берём, что нет и почему

| UE | Берём? | Почему / дельта |
|---|---|---|
| Froxel 16 px × 64 слайса, distribution 32, Halton-джиттер, история 0.9 | да | Размеры и константы безразмерные — как есть. Ресурс под размер рендера/16 (у нас нет dynamic res, DLSS меняет размер — учесть). |
| MaterialSetupCS отдельным пассом | **нет** | Локальных объёмов тумана нет; плотность аналитическая — считается прямо в scatter-пассе. Появятся local fog volumes — вернём. |
| `MatchHeightFogFactor 0.5` | **нет, (ln 2)²** | Аналитический туман UE (и наш порт) интегрирует профиль в base 2 И транслирует в base 2: для однородной среды `LineIntegral = density·ln 2` за метр (предел `(1−2^−x)/x` при x→0), `T = exp2(−integral)` → `e^(−(ln 2)²·density·l)`. Froxel-интеграция — base e, поэтому σ_e = density·(ln 2)² = 0.4805·density; 0.5 у UE — приближение этого числа. Первая версия брала ln 2 (только один из двух переходов) — объём был на 44 % плотнее аналитики, паритет A1 ловил. |
| Skylight через SH | наш IBL | Irradiance-кубмапа `ibl_common.hlsli` в направлении `−V·g` (их `GetSkySHDiffuseSimple(CameraVector·−PhaseG)`); после части B — distant sky light LUT. |
| Sky visibility (volumetric lightmap / distance field) | нет | Нет ни того, ни другого; после части D можно кормить SSGI AO. |
| Light grid для локальных источников | наш список | Источников десятки, не тысячи: цикл по буферу LightManager с тестом сферы влияния на ячейку. |
| Conservative depth (не светить ячейки за геометрией) | да, шаг A3 | Это 20–40 % стоимости scatter-пасса на закрытых видах. |
| Pre-exposed хранение | да | Как у них: экспозиция снимается при чтении. |
| Аналитический туман за пределами объёма | да | `CombineVolumetricFog` буквально; наш «небо = туман» и floor остаются на аналитической части. |
| Небо на LUT Hillaire, диск солнца, AP volume | да, часть B | HDRI остаётся режимом; процедурное — второй режим `sky.mode`. IBL из процедурного неба — динамическая свёртка при смене солнца. |
| Облака как материал | **нет, фиксированный шейдер** | Schneider-шумы в compute при загрузке; материальная система для облаков — не наша задача. |
| Карта теней облаков → освещение + туман | да, часть C | Именно она даёт «тень облака ползёт по острову» и дырявые лучи в тумане. |
| SSGI: Q1..Q4, prev-frame reduction, HZB-марш | да, часть D | Furthest HZB есть; closest — есть под флагом. Денойзер — по форме нашего GTAO temporal/bilateral, не SSD целиком. |
| Lumen / RT GI | нет | Отдельный план, если SSGI не хватит; DDGI на существующем TLAS — кандидат. |

**Порядок пассов после части A** (`SceneRenderer_Graph.cpp`):
```
… Main_Hzb → Main_Gtao
Main_FogScatter    (compute)  ячейки: плотность + солнце(тень) + небо + локалы + история → Scatter[f]
Main_FogIntegrate  (compute)  front-to-back → Integrated[f]
Main_Lighting … Main_Skybox → Main_Compose (читает Integrated[f] для ВСЕХ пикселей до far объёма,
                                             аналитику — за ним)  → Main_Transparent (океан, стекло,
                                             частицы читают Integrated[f] по своей глубине)
```
Оба fog-пасса зависят от теней (CSM/VSM после `Main_ShadowCull`/`Main_CSM`/`Main_VsmPageRender`) и
от глубины (HzbA/conservative depth) и НЕ зависят от освещения → кандидат на compute-очередь
параллельно с `Main_Lighting` (шаг A6, только после замера).

---

## A. Объёмный туман

### A0. Инструментирование и baseline — [полдня]
**Зависит от:** ничего. **Эффект:** есть чем мерить.
* Debug-view тумана расширяется: 3 = слайс-сетка (полосы по `frac(slice)`, как закомментировано у UE
  `HeightFogCommon.ush:459`), 4 = вес истории (красное = промах), 5 = только объёмный inscatter.
* `--set=fog.debug:N` (сейчас `fogDebugView` из UI), строка readout: `fog volumetric=%d grid=%ux%ux%u
  history=%.2f` в session-лог по смене состояния (как `camera hzb cull:`).
* Пол шума на роще под солнце с `--wind-freeze` ×3 (ожидаемо 0.03 %).
* Рецепт камеры «роща под солнце» посчитать (`--cam-rot`), записать в §1 F4.

### A1. Froxel-объём: плотность + интеграция, паритет с аналитикой — [день]
**Зависит от:** A0. **Эффект:** объём есть, картинка НЕ меняется (паритет). **Риск:** низкий.
1. `RenderTargetManager`: в `DeferredTargets` два 3D RGBA16F — `fogScatter` и `fogIntegrated`
   (ceil(W/16) × ceil(H/16) × 64; при 1920×1080 = 120×68×64 × 8 Б = 4.2 МБ каждая, ×3 слота), UAV + SRV,
   покой NON_PIXEL. Пересоздание при resize вместе с остальными.
2. `shaders/fog_common.hlsli`: транскрипция `ComputeDepthFromZSlice/ZSliceFromDepth`
   (`Common.ush:2379-2388`), `CalculateGridZParams` (CPU, `RenderUtils.h`, NearOffset в МЕТРАХ =
   0.095), `ComputeCellWorldPosition` (`VolumetricFog.usf:66-76`) с нашей `invViewProjNoJitter`,
   `ComputeVolumeUV` (`HeightFogCommon.ush:479-492`), HG-фаза.
3. `shaders/fog_scatter_cs.hlsl` (A1 — без света): σ = `density(y)·ln 2` по формуле
   `AtmosphereSharedIntegral`'а (та же `density·exp2(−falloff·(y−ref))`), scattering = albedo·σ,
   свет = 0, история выкл. → `Scatter[f] = (0, σ)`.
4. `shaders/fog_integrate_cs.hlsl`: `FinalIntegrationCS` буквально (`:1075-1120`), thread per (x,y),
   цикл по 64 слайсам, `NearFadeIn` от `fog.startDistance`.
5. `compose_cs.hlsl`: `CombineVolumetricFog` буквально: lookup по `(uv, slice(depth))`, снять
   pre-exposure, `rgb = Vol.rgb + Analytic.rgb·Vol.a`, `a = Vol.a·Analytic.a`, где аналитика считается
   ОТ `fog.volumetricDistance` (exclude distance) до поверхности. Небо: Vol на последнем слайсе
   (лучи над горизонтом), аналитика на небо — как сейчас, не применяется.
6. Ручки: `fog.volumetric 0|1` (дефолт 0 до A2), `fog.volumetricDistance` (м, дефолт 300),
   `fog.gridPixels 16`, `fog.gridZ 64`, `fog.depthScale 32`, `fog.albedo 1`, `fog.extinctionScale 1`.
**Критерий приёмки:** debug-view transmittance froxel vs аналитика на камере теней и острове —
разница ≤ 1.5 % пикселей (>8/255) при `sunScatterStrength 0`; ошибка объясняется дискретизацией
слайсов, не знаком/базой. GBV CLEAN (новые ресурсы/пассы), три конфига.
**Откат:** `fog.volumetric:0` — пассы не регистрируются, композ как сегодня.

### A2. Свет в тумане: солнце с тенью, небо, фаза — [день]
**Зависит от:** A1. **Эффект:** лучи сквозь кроны, тёмные столбы под пальмами.
1. `fog_scatter_cs`: за ячейку `L += sunColor·shadow(P)·HG(g, dot(L, −V))·fog.sunScatter` (`:888`);
   `shadow(P)` — `shaders/shadow_volume.hlsli`: CSM `CsmSampleShadow` с `N = −sunDir, ndl = 1`, без
   контактных; VSM `VsmClipmapShadow` с `rayCount 0` (однотап), без screen-ray. CB — зеркало
   освещения (`SceneResourceBootstrapper.h`: `LightingConstants`), заполняется тем же билдером;
   зеркало = одна структура, не копия полей.
2. Небо: `L += skyIrradiance(−V·g)·fog.skyScatter` (их `:936-942` с SH → наш irradiance-куб).
3. Джиттер Halton(2,3,5) по `frameNumber & 1023` (`VolumetricFog.cpp:196-207`), пока без истории
   (A3), поэтому в A2 джиттер = 0.5 (центр ячейки) — иначе шум без накопления.
4. Экспозиция: пишем `preExposure·L·scattering`, читаем ×`1/preExposure` (их `:1024`, `HeightFogCommon:448`).
5. Аналитический sun-lobe (`sunScatterStrength`) в диапазоне объёма ВЫКЛЮЧАЕТСЯ (иначе двойной
   счёт): аналитика за объёмом сохраняет свою лопасть.
**Критерий приёмки (пересмотрен 2026-09-05):** ~~лучи видны глазами при плотности уровня~~ — физически
недостижимо: при ясной погоде (density 0.0002–0.001, видимость 8–50 км) столб тени длиной 30 м меняет луч
на 0.3–1 %. Лучи «сквозь кроны» — это либо туман/пыль (density ≥ 0.01), либо экранные light shafts
(шаг A7). Принято: механизм тени в объёме проверен видом debug 4 при `sunVolScatter:20`
(crepuscular rays, тёмные столбы) в обоих режимах теней; стоимость ≤ 0.25 мс @1080p (профдамп ×2).
~~Паритет с off на камере теней ≤ 0.5 %~~ — критерий для transmittance (A1), для освещённого объёма
недостижим по построению. **Дефолт `volumetric` остаётся 0 (ключ уровня).**
**Откат:** `fog.volumetric:0`.

#### Что сделано (2026-09-05): A0 + A1 + A2 одним инкрементом, история из A3 — ГОТОВО, не закоммичено
**Файлы.** Новые: `shaders/fog_common.hlsli` (сетка Z, `FogCellWorldPosition`, HG, σ), `shaders/fog_scatter_cs.hlsl`
(4×4×4, b0 = cbuffer освещения через новый `shaders/lighting_cb.hlsli`, вынесенный из `lighting_cs.hlsl`; b1 = FogCB;
t0 атлас CSM, t1/t2 VSM table/pool, t3 irradiance-куб, t4 история; u0 scatter), `shaders/fog_integrate_cs.hlsl` (8×8,
FinalIntegrationCS буквально, цикл по литералу 64). Правки: `compose_cs.hlsl` (t13 = объём, `CombineVolumetricFog`,
exclude distance для аналитики, debug 3/4/5), `RenderTargetManager` (два R16G16B16A16F 3D-объёма ceil(W/16)×ceil(H/16)×64
на слот кольца, покой NON_PIXEL), `SceneRenderer` (`FrameDecisions.volumetricFog/fogHistoryValid`, `Main_VolumetricFog`
после Main_Gtao с prereq `pGbufDone` (+`pVsmPageRender` в VSM) и mtDep `pShadow`, три точки барьеров; `FillLightingConstants`
вынесен из Pass_Lighting и заполняет b0 ОБОИХ потребителей), `RenderGraph::AddPass2(DependencyList)` — перегрузка для
рёбер, существующих не каждый кадр (ребро туман→композ). Ручки: `AtmosphereSettings.volumetric/volumetricDistance/albedo/
extinctionScale/phaseG/sunScatter/skyScatter/historyWeight/temporal` (JSON уровня, Inspector «Volumetric fog»,
`--set=fog.*`, для солнца/неба в объёме — `sunVolScatter`/`skyVolScatter`). Лог: `volumetric fog: on= history= grid=`
по смене состояния.

**Что решено иначе, чем в плане.** (1) σ_e = density·(ln 2)², не ln 2 — см. §3 (паритет A1 ловил разницу 10 % T на
20× плотности). (2) `NearFadeIn` = дефолт UE (0 → `1/1e-5`, ступенька на near объёма), а не от `startDistance`: иначе
паритет с аналитикой (ступенька на `startDistance`) невозможен по построению. (3) Джиттер Halton(2,3,5) включается
только вместе с историей (UE `VolumetricFogTemporalRandom`), иначе центр ячейки. (4) Фаза: наш HG — учебная форма
`(1+g²−2g·cos)^−1.5`, аргумент `dot(toSun, V)` (V = камера→ячейка); у UE функция записана с `+2g·cos` и аргумент
`dot(L, −CameraVector)` — ОДНА пара (ловушка [[transcription-half-a-pair]]; первая версия взяла аргумент UE с нашей
функцией — лепесток смотрел от солнца).

**Два бага аналитической модели (P7), которые объём вскрыл и которые исправлены.**
* `AtmosphereSharedIntegral`: у UE `RayDirectionZ = CameraToReceiver.z` — ПОЛНЫЙ перепад высоты луча
  (`HeightFogCommon.ush:251/:283`), порт делил его на длину → член высоты сворачивался в предел Тейлора и весь луч
  интегрировался при плотности на высоте КАМЕРЫ. С 218 м остров читался как тонкий воздух. Плюс UE re-base'ят луч на
  start/exclude distance (`:270-289`: origin term на высоте точки исключения, остаток перепада и длины) — это и делает
  шов объём→аналитика точным. Эффект на картинку: остров (`atoll.json`, туман wind_test'а) 18.8 % px > 8/255,
  средняя +3.5 (ровная дымка над водой с высоты). Уровни настраивались на старой модели — юзеру смотреть.
* Debug-view тумана (1 = transmittance) писал серый КАК РАДИАНС × pre-exposure: единица при дневном EV → чёрный,
  ПЕРВЫЙ паритет «сошёлся» на двух чёрных картинках ([[verify-against-a-working-control]]). Теперь виды 1/3/5 —
  display-linear (без pre-exposure), 2/4 — радианс. Правило «неизмеренные пиксели чёрные» для видов 1/2 было мёртвым
  (перезаписывалось следом) — исправлено; с объёмом небо ИЗМЕРЕНО (луч до дальней плоскости) и показывается.

**Замеры (Release, 2560×1440, сетка 160×90×64, `--wind-freeze --dlss=off exposure.autoExposure:0 vsm.smrtRayCount:0`).**
* Паритет A1 (debug 1, sunVolScatter/skyVolScatter 0, temporal 0), пол 0.022–0.026 %: камера теней при 20× плотности
  (0.02) — поверхности (строки ≥ 110, без неба) **0.000 %** px > 8/255, ±1 уровень серого (дискретизация слайсов);
  остров (falloff 0.01, 0.01) **0.022 %**; однородная среда (falloff 0) **0.022 %**. Небо в кадре камеры теней
  различается ПО ПОСТРОЕНИЮ (аналитика неба не трогает, объём — да).
* A2, глаза: при плотности уровня (0.001) объём почти невидим (палмовая полоса −0.4 средн., 0.04 % px) — тонкая дымка;
  при `sunVolScatter:20 skyVolScatter:0 debugView:4` — отчётливые crepuscular rays сквозь кроны, тёмные столбы тени
  (`fogA_shaftdbg_sunA.png` в scratchpad) — механизм тени в объёме работает в Legacy и VSM. Камера теней, нормальный
  вид, on−off: 8.3 % px, средняя +1.4; +9 уровней в верхней полосе (дальний берег/небо) — солнечный in-scatter объёма
  почти изотропен (g 0.2), аналитический лепесток `pow(cos,8)` при взгляде от солнца = 0. Это ожидаемая смена вида,
  не ошибка; ручки `sunVolScatter/phaseG/skyVolScatter`.
* Стоимость (роща, ветер ВКЛ, 30 с; ×2, см. ниже): Pass_VolumetricFog **0.107 мс** VSM /
  **0.060 мс** Legacy GPU (@1440p; бюджет ≤ 0.30 @1080p), CPU 0.04 мс; GPU.Frame 2.218 → 2.301 мс VSM.
* Гейты: три конфига; `check_shaders` 61/61; `--log-stress` 0/0 Debug+Release; GBV `--scene-stress-gbv=20` Legacy и VSM
  CLEAN ×2 — но первые две пары валидировали ДЕФОЛТ (`on=0` весь прогон): стресс-харнесс применял `--set` один раз
  после boot, а блок atmosphere уровня затирал его при каждом reload/switch (уровни стресса не несут ключ
  `volumetric`). Исправлено: `SceneStressDriver::SetOnLevelLoaded` → `ApplyFixedSettings` после КАЖДОЙ загрузки
  (ловушка S3a во второй раз). Вердикт с включённым объёмом — см. строку ниже.
* Вторая причина «GBV без объёма»: `Scene::SetRenderSettings` зеркалит `atmosphere_` в `renderSettings_`, а кадр брал
  `renderSettings_.atmosphere` — без вызова SetRenderSettings между `AtmosphereRef()`-правкой и кадром (харнесс его не
  делает) объём не включался. Кадр теперь берёт `atmosphere_` напрямую (`Scene.cpp`, сборка frameData_).
* **Debug-ассерт в первом кадре с объёмом** (`RenderGraph.h:623 inGroup && "grouped pass (non-first) has a prereq
  from outside the group"`): Main_Compose — не-первый член CL-группы отражений, а я дал ему prereq туман→композ.
  Ребро перенесено на ПЕРВЫЙ член группы (`withFog(...)` в BuildReflections — та же схема, что у wetness). В Release
  ассерт вырезан, а порядок пассов при этом ребре был валиден — Release-замеры выше корректны. Заодно списки
  преемников RenderGraph (`kAdjacencyCapacity`) подняты с MaxPasses/4 = 11 до MaxPasses: G-buffer — хаб с ~10
  потребителями, бюджет был на грани (переполнение inl_vector в Release молчит). Debug-ассерты теперь пишутся в
  session-лог как `[FATAL] CRT assert: файл(строка): выражение`, окна нет, без отладчика процесс сам завершается с
  кодом 3 (`_CrtSetReportHookW2` + `_CrtSetReportMode(FILE)`, main.cpp) — headless-гейты читают лог.
* GBV с объёмом (`--set=fog.volumetric:1 fog.enabled:1 fog.density:0.004`, после починки ребра
  и харнесса): **Legacy CLEAN (169.7 с), VSM CLEAN (162.6 с)**, `volumetric fog: on=1` во ВСЕХ трёх уровнях и на каждом
  ресайзе (сетки 93×53 … 29×22 … 107×60, история сбрасывается на ресайз — `history=0` → `1`).
* Стоимость повторно на починенном бинаре (тег 2): Pass_VolumetricFog 0.107 мс VSM / 0.062 мс Legacy, GPU.Frame
  2.203 → 2.276 мс — совпадает с первым прогоном (0.107 / 0.060), два прогона каждого.

**Дефолт.** `volumetric = false` (ключ уровня). Включение меняет вид уже настроенных уровней (см. выше) — включать
в Inspector по уровню (wind_test — кандидат), не глобально. Плановый пункт «дефолт → 1 после приёмки» снят: приёмка
A2 по камере теней (≤ 0.5 %) в принципе недостижима для ОСВЕЩЁННОГО объёма — критерий был написан для transmittance.

**Осталось из A3:** conservative depth (min-глубина по тайлу 16), `HistoryMissSupersampleCount`, `FixupHistoryUV`,
раздельная ручка `jitter`. История + джиттер + реэкспозиция уже стоят (вес `historyWeight` 0.9, cut по
`GetHistoryRevision()`/resize → кадр без истории).

### A3. Temporal: история, джиттер, conservative depth — [день]
**Зависит от:** A2. **Эффект:** мягкие лучи без бэндинга и без мерцания при движении.
1. История: `Scatter` предыдущего слота (`GetDeferredForPrevFrame`), UV по `prevViewProjNoJitter`
   (`:775`), вес 0.9 (`HistoryWeight`), вне [0,1] → 0 (`:812-818`), реэкспозиция (`:1031`), cut по
   `GetHistoryRevision()` и resize → вес 0 на кадр (как `gtaoHistoryFrames_`).
2. Джиттер Halton по кадру + `HistoryMissSupersampleCount 4` при промахе (`:822-829`).
3. Conservative depth (`:785-800`): min-глубина по 16×16 тайлу (наш HZB mip 3 = /8 — не тот шаг; свой
   downsample по 16 из `D.depth` или mip 4 при 1080p → `ceil`); ячейка за геометрией → 0 и выход;
   `FixupHistoryUV` (`:699-765`) по conservative depth прошлого кадра (второй слот кольца).
4. `fog.temporal 0|1`, `fog.historyWeight`, `fog.jitter 0|1`, `fog.conservativeDepth 0|1`.
**Критерий приёмки:** статичная камера: покадровая разница on/on ≤ пол; полёт `--cam-fly` без
шлейфов глазами (debug 4 показывает промахи только по краям); scatter-пасс с conservative depth
дешевле на закрытых видах (стена) — записать; GBV CLEAN.
**Откат:** `fog.temporal:0`.

#### Что сделано (2026-09-05, A3 остаток)
* **Conservative depth** — из furthest-HZB (min reverse-Z): база пирамиды — половина рендера, mip 3 = тайл 16×16 = одна
  ячейка (`fogMisc.x`). Ячейка, чья ближняя грань (сдвиг −0.5 слайса к камере, как UE `FarDepthOffset`) дальше самой
  дальней поверхности тайла, пишет 0 и выходит — до сэмплов тени. За последним текселем `Load` даёт 0 = far, не режет.
  Пасс получает prereq `pHzb`, читает `D.hzb`/`P.hzb` (t5/t6). Ручка `conservativeDepth` (JSON/CLI/Inspector, дефолт 1).
* **FixupHistoryUV** в одном тапе: ячейка прошлого кадра проверяется по ПРОШЛОМУ HZB (второй слот кольца) — была за
  геометрией → истории нет (вес 0). Дельта от UE (4 билинейных тапа с весами): у нас один тап по центру ячейки.
* **HistoryMissSupersampleCount 4**: при весе истории 0 (cut, ресайз, край кадра, отфильтрованная ячейка) — четыре
  джиттер-сэмпла (Halton текущего кадра + последовательность R3) усредняются; литерал цикла 4, счётчик из CB только
  укорачивает. `fogFlags`: bit0 история, bit1 джиттер, bit2 conservative, bit3 temporal.
* Ручка `jitter` (дефолт 1): выключает только сдвиг ячейки, история остаётся (UE `r.VolumetricFog.Jitter`).
* Замеры: камера теней, density 0.004, объём вкл: два одинаковых запуска (история + джиттер) —
  **0.032 %** px > 8/255 (пол 0.02–0.03); conservative 0 vs 1 — **0.031 %** (паритет: режутся только невидимые ячейки).
  Стоимость Pass_VolumetricFog: стена (occlusion_test, закрытый вид) **0.097 → 0.080 мс** (−18 %), роща 0.107 → 0.104.
  Полёт `--cam-fly` глазами не снимался (шлейфы не проверены — TODO при первом полёте юзера).

### A4. Локальные источники в тумане — [полдня–день]
**Зависит от:** A2. **Эффект:** конусы спотов и ореолы поинтов в тумане (demo.json).
Цикл по буферам LightManager с тестом сферы влияния на ячейку, `InverseSquaredLightDistanceBiasScale`
(`:962-963`, bias на размер ячейки — против алиасинга у источника), тень: спот/поинт из атласа Legacy
или VSM (`VsmSpotShadow`/`VsmPointShadow`), soft fading конуса по радиусу ячейки (`:958-960`,
`LightSoftFading`). `fog.localLights 0|1`, per-light `volumetricScatteringIntensity` (дефолт 1, в
уровне — опционально).
**Критерий приёмки:** demo.json — конусы видны, стоимость ≤ +0.1 мс при 9 спотах + 8 поинтах; без
источников в кадре стоимость не растёт (ранний выход по сфере).

#### Что сделано (2026-09-05, A4)
* В `FogSampleCell` два цикла (литерал 256 с `break` по счётчику) по буферам LightManager (те же
  `SpotLightData`/`PointLightData`, `rt_lights.hlsli`): тест дальности = cull, конус как в `spotlight_cs`
  (`angleAtten²`), `LightDistanceAttenuation` (его `+1` в знаменателе = UE `InverseSquaredLightDistanceBiasScale`),
  фаза `HG(g, dot(toLight, V))`. Тени: Legacy — атлас спотов (`SampleCmp`, bias света) и куб поинтов (формула
  `pointlight_cs`), VSM — `VsmSpotShadow`/`VsmPointShadow` по тем же страницам. Вместо нормального смещения (у объёма
  нет поверхности) точка сдвигается ВДОЛЬ луча к источнику на 2 тексела уровня VSM.
* Дельты от UE: нет light grid (десятки источников, не тысячи), нет `LightSoftFading` по радиусу ячейки, одна общая
  `localLightScatter` вместо per-light `VolumetricScatteringIntensity`.
* Граф: prereq `pPointShadow`, mtDep `pSpotShadow` (как у Main_PointLights / Main_SpotLights), декларации атласов
  NON_PIXEL в Legacy. Ручки `localLights` (дефолт 1), `localLightScatter` (1.0). t7–t10 в scatter (RS 11 SRV).
* Замеры: demo.json (9 спотов + 8 поинтов, камера уровня, density 0.01), local 0 vs 1 — **10.7 %** px
  > 8/255, средняя +2.7: ореолы и конусы в воздухе видны (side-by-side `fogB_demo_side.png`); Legacy vs VSM тени —
  2.0 % px (разные карты, ожидаемо). Стоимость Pass_VolumetricFog 0.092 → 0.089 мс — прибавка в шуме (тест дальности
  режет почти все ячейки, тень берётся только внутри сфер влияния).

#### A4b. Качество конусов (2026-09-05, по жалобе юзера: «конусы пикселизированы и дрожат»)
Причины: ячейка 16 px при DLSS 0.58 = ~27 px экрана, кромка конуса решалась в ячейке как монетка (внутри/снаружи),
джиттер + история 0.9 не успевали сгладить бинарный край → ступени и мерцание. Сделано:
* **UE LightSoftFading** (`VolumetricFog.usf:951-956`, `DeferredLightingCommon.ush:583-601`): кромка конуса спота
  затухает на `localSoftFading` × 2D-радиус ячейки (расстояние до диагонального соседа в слайсе). У UE дефолт 0
  («1 — хорошая стартовая точка»), у нас дефолт **1**. Ручка `localSoftFading` (JSON/CLI/Inspector).
* **InverseSquaredLightDistanceBiasScale**: `1/(d² + max(radius·scale, 1)²)` вместо `+1` — ячейка, содержащая
  источник, не взрывается. Ручка `localDistanceBias` (дефолт 1, как UE).
* **Размер ячейки** `fog.gridPixels` 8 / 16 / 32 (с A4d: степени двойки 4..64 + `fog.gridZ`) (`render::g_fogGridPixels`, `graphics_settings.json`
  performance/fogGridPixels, комбо во вкладке Fog окна Developer Controls, `--set=fog.gridPixels`): смена = пересоздание deferred-кольца на
  границе кадра (`Renderer::SetFogGridPixels`, как смена режима DLSS). Mip conservative depth следует за размером
  (log2(px) − 1). 8 px = ×4 ячеек. Синхронизация ручки — в `Renderer::BeginFrame` (первый GBV-прогон с
  `fog.gridPixels:8` молча валидировал 16: синк стоял в цикле App::Run, а у стресс-харнесса свой цикл).
* Тестовая сцена `data/levels/fog_spot_test.json`: один спот 12 Mlm сверху, три колонны-окклюдера, туман 0.03
  однородный, солнце 400 лк, камера `10,3,10` на конус. Замеры: 
  * тестовая сцена, натив: конус гладкий, повтор кадра (история + джиттер) **0.023 %** px — статичная камера не мерцает;
    soft fading 0 vs 1 — **0.08 %** px: у спота с широким ramp inner→outer (22°→32°, у demo 30°→41°) кромка и так
    мягче ячейки, поправка UE почти невидима (оставлена, дефолт 1 — для узких конусов).
  * стоимость сетки: 16 px 0.085 мс → 8 px **0.136 мс** (×4 ячеек = +60 %, пасс латентный, не ALU).
  * demo с камерой юзера под DLSS balanced (рендер 0.58, ячейка 27 px экрана): «конусы» — вертикальные столбы света
    девяти спотов по 543 Mlm, смотрящих вниз; 8 px даёт заметно ровнее, `localLightScatter 0.2` убирает пересвет.
    **Мерцание при движении камеры воспроизвести кадровым снимком нельзя** (харнесс снимает один кадр): природа —
    репроекция 16-px сетки с трилинейной выборкой при быстром полёте, у UE поверх работает TAA/TSR; у нас DLSS должен
    сглаживать, но объём считается в рендер-разрешении. Рекомендация для demo: `fog.gridPixels 8` + `localLightScatter
    0.2–0.3`; споты по 543 Mlm — нефизичная яркость, отсюда клип и «рваные» края пятен.
  * Гейты раунда: три конфига, 61/61 шейдеров, log-stress 0/0, GBV Legacy `--set=fog.gridPixels:8` **CLEAN** — сетка
    186×105 с первого кадра (пересоздание кольца на границе кадра) и через все ресайзы.

#### A4c. Мерцание в fog_spot_test (2026-09-06, по отчёту `docs/bug_fog_spot_flicker.md`)
Коллега локализовал два дефекта на камере `11.37,3.75,-5.79 / 0.0232,-0.5323,0.0146,0.8461` (DLSS balanced): тёмное
мерцающее пятно над краем пола (conservative depth) и дрожание верхушки конуса (одна джиттер-выборка). Метрика —
σ по пикселю за 12 снимков одного процесса, регионы из отчёта; их числа воспроизведены (0.560/0.205).
* **Пятно**: столбец резался по СВОЕМУ тайлу, а трилинейная выборка объёма читает соседние — соседний столбец,
  остановившийся на полу, затемнял небо рядом. Теперь `FogTileFarthest`: минимум reverse-Z по 3×3 тайлам на mip'е
  сетки (UE `GenerateConservativeDepth.usf`: rect тайла с `FootprintMargin 0.5`; у нас целый тайл — надмножество,
  9 Load). То же для проверки истории по прошлому HZB. Край пола: σ 0.205 → **0.108**, размах 28 → 9 = уровень
  «cull выключен» (0.114/10).
* **Верхушка**: радиусы ячейки для bias/soft-fade — от НЕДЖИТТЕРНОГО центра; ручка `samplesPerCell` 1–4 (дефолт **2**:
  антитетическая пара offset / 1−offset, линейный градиент внутри ячейки гасится точно; 3–4 — R3). σ 0.560 → 1 выборка
  0.564 / 2 → **0.280** / 4 → 0.246 (пол «джиттер выкл» 0.090 — остаток от тени и накопления). Стоимость на сцене:
  0.099 / 0.103 / 0.115 мс. UE берут одну выборку и полагаются на TAA.
* Остаток и чем добить при желании: 4 выборки дают мало сверх 2 — остаток мерцания идёт от одиночного тапа тени спота
  в джиттерной точке; лечится PCF-тапом тени в объёме или отдельной стабилизацией тени (не делалось).
* Процессы серий завершились штатно (`session end: clean shutdown`) — зависание, отмеченное в отчёте, не
  воспроизвелось с `--wind-freeze` (у коллеги `--wind-freeze=0`).

#### A4d. Вес источника в объёме и шаг сетки (2026-09-06, по вопросам юзера)
Вопросы: «почему настройка фрокселей только для локалов», «это пер-лайт или глобальная», «у UE точно можно шаг сетки
задавать», «степень влияния источника в волюметрик». Ответы и что сделано:
* **Что пер-лайт, что глобально (как у UE).** Сетка (`r.VolumetricFog.GridPixelSize` 16, `GridSizeZ` 64 —
  `VolumetricFog.cpp:64-75`, глобальные cvar'ы) и температура/фаза среды — глобальные; у КАЖДОГО источника свой
  `VolumetricScatteringIntensity` (`VolumetricFog.usf:974-1024` локальные, `:845` направленный, `:934` skylight).
  Наши `sunVolScatter` («Volume Sun Intensity») и `skyVolScatter` — это и есть веса направленного и неба: у нас одно
  солнце, поэтому они лежат в блоке тумана уровня, а не в источнике. `localSoftFading`/`localDistanceBias` —
  конусные и позиционные поправки (UE держит их глобальными cvar'ами `r.VolumetricFog.LightSoftFading`,
  `InverseSquaredLightDistanceBiasScale`), у направленного света ни конуса, ни позиции нет — ему они не нужны.
* **Per-light `volumetricIntensity`** (спот и точечный; JSON, редактор через `EnvironmentRuntime`, Inspector
  «Volumetric Intensity» под Luminous Flux, дефолт 1): `SpotLightGpu.shadowParams2.y`, `PointLightGpu.volumetric.x`
  (новый float4 в буфере точечных — зеркала `PointLightData` в `rt_lights.hlsli`, `glass.hlsl`, `pointlight_cs.hlsl`
  обновлены), множитель в `FogSampleCell` поверх глобального `localLightScatter` — как `LightData.Color * ... *
  VolumetricScatteringIntensity` у UE. 0 = источник светит на поверхности, но не даёт конуса/гало в воздухе.
  Проверка на `fog_spot_test` (вариант уровня с ключом 0 и 0.5 против исходного): 0 — конус исчез, разница только
  внутри конуса (7.9 % px темнее, 0.01 % светлее), пятно на земле не изменилось (−1.7 % линейно = сдвиг авто-экспозиции,
  кадр без конуса темнее); 0.5 — 0.54–0.58 вклада конуса (Filmic-кривая, ожидаемо > 0.5).
* **Сетка**: `fog.gridPixels` расширен до степеней двойки **4..64** (`Renderer::SetFogGridPixels` округляет вниз до
  степени двойки — conservative depth читает HZB-mip «ячейка = тексель»), добавлен **`fog.gridZ` 16..128**
  (`render::g_fogGridZ`, `Renderer::SetFogGridZ`, `graphics_settings.json` performance/fogGridZ, комбо
  «Volumetric fog slices» в собственной вкладке **Fog** окна Developer Controls (с readout сетки) — до 2026-09-06 оба
  комбо по ошибке лежали во вкладке VSM и сбрасывались её Reset'ом; у вкладки свои `ApplyFog`/`ResetFog`; `--set=fog.gridZ`). Литерал цикла `fog_integrate_cs.hlsl` поднят до 128 (граница
  ТОЛЬКО литералом, счётчик из CB укорачивает). Смена глубины пересоздаёт кольцо и сбрасывает историю
  (`fogHistoryDepth_` в `sameHistory`). Обе ручки глобальные, как у UE.
  Замеры (`fog_spot_test`, натив 1440p, Legacy):
  | сетка | ячеек | Pass_VolumetricFog | картинка против 160×90×64 |
  |---|---|---|---|
  | 160×90×**32** | ½ | — | 1.7 % px темнее (конус тоньше слайса, ячейка длиннее — недобор) |
  | 160×90×64 (дефолт) | 1 | **0.101 мс** | — |
  | 160×90×**128** | ×2 | **0.171 мс** | 0.18 % px светлее (сходится вверх) |
  | **640×360**×64 (4 px) | ×16 | **0.769 мс** | 0.10 % px (тот же ответ, чётче кромка) |
  | **40×23**×64 (64 px) | 1/16 | — | 0.69 % px темнее, конус размыт |
  Гейты раунда: три конфига, 61/61 шейдеров, log-stress Debug/Release 0/0, GBV Legacy churn с `fog.gridZ:96` +
  `fog.gridPixels:8` (буфер точечных сменил layout).

### A5. Туман на прозрачном: океан, стекло, частицы — [полдня]
**Зависит от:** A1. **Эффект:** вода и стекло в тумане согласны с сушей (шов на береговой линии —
известная ловушка `height_fog.hlsli`).
`ocean_surface.hlsl` / `ocean_surface_legacy.hlsli`: `CombineVolumetricFog` по глубине поверхности
(та же формула, тот же lookup); `glass.hlsl`, `particles.hlsl`: lookup по глубине фрагмента.
**Критерий приёмки:** береговая линия при тумане без шва (zoom side-by-side), паритет off при
`fog.volumetric:0`.

#### Что сделано (2026-09-05, A5)
* `fog_common.hlsli`: `FogVolumeSampleAt` (lookup по экранному UV и view depth, реэкспозиция, идентичность при выкл.) и
  `FogAnalyticExclude` — одна функция для всех потребителей. Параметры `(on, far, 1/preExposure, gridZ)` и `(B,O,S)`
  решаются ОДИН раз в `DecideFrame` (`decisions_.fogVolumeParams/ZParams`) и раздаются композу, туману, океану
  (`SetFogVolumeParams`), стеклу и частицам (хвост `GlassView`).
* Океан (`ocean_surface.hlsl` + legacy): t20 = объём (RS 21), UV/глубина из `viewProjNoJitter`, аналитика от
  `FogAnalyticExclude`, композиция `(color·T + in·(1−T))·vol.a + vol.rgb` — та же формула, что на песке.
* Стекло: `GlassView` вынесен в `glass_view_cb.hlsli` (его читают и частицы — раньше они объявляли ПРЕФИКС того же
  буфера под другим именем), в хвост добавлены `fogVolumeParams/ZParams/fogParams0..2`; стекло получило и аналитику
  (раньше туман на стекло не ложился вовсе), t12 = объём. Частицы: только объём (аналитике нужен куб неба, не
  привязан к пассу), t4 = объём, `(rgb·lum·vol.a + vol.rgb)·a`.
* Состояния: Main_Transparent декларирует `fogIntegrated` PIXEL_SHADER_RESOURCE КАЖДЫЙ кадр (таблицы прозрачных всегда
  заполнены, шейдеры гейтятся по `fogVolumeParams.x`; дескриптор над NON_PIXEL-ресурсом — то, что GBV ловил на
  fallback-слотах океана). Цепочка на кадре с туманом: UAV → NPS (композ) → PIXEL (прозрачные) → UAV.
* Замеры: (1) объём ВЫКЛ, новый бинарь vs закоммиченный (`fogA_shpar_off`): **0.014 %** = пол — путь
  идентичности точен на воде/стекле/частицах; (2) камера теней, density 0.02, off/on: 51.6 % px (объём меняет всю
  картинку по построению), береговая линия в зуме (`fogB_shore_zoom.png`) — без шва, вода и мокрый песок в одной дымке.

### A6. Стоимость и очередь — [полдня]
Профдамп ×2: 1080p и DLSS Perf (размер рендера меньше — сетка меньше). Если scatter+integrate
> 0.3 мс: (а) `fog.gridZ 48`, (б) перенос на compute-очередь параллельно `Main_Lighting` (по правилам
async-плана: mtDep на тени и глубину, NON_PIXEL для чтений, hand-over Integrated в compose). Только
после замера — «контеншен не экстраполировать с одного воркоада» ([[async-compute-plan]]).

### A7. Light shafts (экранные лучи солнца, UE LightShaftBloom) — [полдня]
**Зависит от:** ничего (независим от объёма). **Эффект:** те самые god rays сквозь кроны при ясной погоде,
за ~0.05–0.1 мс. Юзер (2026-09-05): «раньше нахаляву радиальным блюром за 0 перфа делались» — это оно.
**Источник:** `Shaders/Private/LightShaftShader.usf` (148 строк, читать целиком), `Renderer/Private/
LightShaftRendering.cpp` (691). Транскрипция:
1. **Экранная позиция солнца** (`:78`): `View.WorldToScreen(LightPositionForLightShafts)` → для
   directional — точка `camPos + toSun·far`; `TextureSpaceBlurOrigin = ndc→uv · invAspect` (`:172`). Солнце
   за камерой (`clip.w ≤ 0`) → пасс не регистрируется. Вне экрана — работает (лучи входят с края), UE
   гасят только через `BlurOriginDistanceMask`.
2. **Downsample + маска** (`DownsampleLightShaftsPixelMain`, `#else`-ветка = bloom): половинное разрешение
   (`r.LightShaftDownsampleFactor 2`), на пиксель: `Luminance = dot(SceneColor, (.3,.59,.11))`;
   `AdjustedLum = clamp(Lum − BloomThreshold, 0, BloomMaxBrightness)`; `BloomColor = BloomScale ·
   SceneColor/Lum · AdjustedLum · 2`; маски: `BloomDistanceMask = saturate((depth − 0.5·range)/range)`
   (только ДАЛЬНЯЯ половина `OcclusionDepthRange`, т.е. небо и далёкое), `EdgeMask` (1 у краёв экрана,
   0 в центре, в 4-й степени), `BlurOriginDistanceMask = 1 − saturate(|origin − uv|·2)` — в квадрате.
   Дефолты UE (LightComponent.cpp:484-487, DirectionalLightComponent.cpp:1007): BloomScale 0.2,
   BloomThreshold 0, BloomMaxBrightness 100, BloomTint white, OcclusionDepthRange 1000 м (100000 см),
   OcclusionMaskDarkness 0.05. У нас SceneColor pre-exposed — порог и MaxBrightness в тех же
   единицах, что композ (снимать pre-exposure перед порогом, как композ делает для объёма).
3. **TAA пасс** (`AddTemporalAAPass`, `:345-363`) — у нас нет TAA-хелпера; шаг 1: без него (DLSS
   сглаживает мерцание, в нативе — оценить глазами), шаг 2 при мерцании — история по слоту как у GTAO.
4. **Радиальный блюр** (`BlurLightShaftsMain`): `r.LightShaftBlurPasses 3`, `NUM_SAMPLES 12`,
   `FirstPassDistance 0.1`; `PassScale = pow(0.4·12, passIndex)`, вектор к origin в aspect-corrected UV
   `· min(0.1·PassScale, 1)`, 12 сэмплов вдоль, среднее. Три пинг-понг пасса на половинном разрешении.
5. **Apply** (`ApplyLightShaftsPixelMain`, blend `BF_One, BF_One` `:530`): аддитив в scene colour ПОСЛЕ
   прозрачных (UE: после translucency, до пост-процесса; `RenderAfterDOF 0`). У нас: новый пасс
   `Main_LightShafts` между Main_Transparent и Main_Bloom/Tonemap, compute (8×8) с UAV на scene, четыре
   диспатча в одном пассе (маска+3 блюра) по форме Pass_Gtao, промежуточные R11G11B10 половинного размера
   ×2 (пинг-понг) в DeferredTargets.
6. Occlusion-вариант (`OCCLUSION_TERM`, `FinishOcclusionMain`: затемнение неба/дымки по маске глубины) —
   НЕ делаем: у нас нет отдельного fog/sky прохода, куда его умножать; записать как опцию.
7. Ручки (per-level, в блоке `directionalLight` уровня, как у UE — свойство света): `lightShafts.enabled`
   (дефолт **1** — нулевая цена при выключенном солнце в кадре не нужна: пасс не регистрируется, если
   солнце за камерой), `bloomScale 0.2`, `bloomThreshold 0`, `bloomMaxBrightness 100`, `bloomTint`,
   `occlusionDepthRange 1000`. Inspector: секция «Light shafts» в Directional light.
**Критерий приёмки:** камера `--cam-pos=-22.07,2.27,-94.31 --cam-rot=-0.0826,0.2498,0.0214,0.9645`
(юзерская, солнце в кроне) и `-37.61,2.50,-98.03 / -0.0997,0.2987,0.0314,0.9486`: лучи видны глазами
при плотности уровня 0.001 (side-by-side off/on); стволы окклюдят лучи (маска глубины); солнце вне кадра
слева/справа — лучи входят с края без скачка при пересечении границы (`--cam-fly` глазами); стоимость
≤ 0.1 мс @1440p (профдамп ×2); GBV CLEAN (новые ресурсы/пасс); три конфига.
**Откат:** `lightShafts.enabled:0`.

#### Что сделано (2026-09-06, A7)
Транскрипция целиком: `shaders/light_shafts_cs.hlsl` — три ядра одного файла над одним cbuffer: `CSDownsample`
(`LightShaftShader.usf:39-85`, ветка bloom: полуразмер, luminance/threshold/MaxBrightness, маски дальней половины
OcclusionDepthRange, краёв экрана и расстояния до солнца), `CSBlur` (`:93-121`, 12 сэмплов, `PassScale =
pow(4.8, pass)`, `FirstPassDistance 0.1`), `CSApply` (`:141-149` + blend BF_One/BF_One `LightShaftRendering.cpp:530`).
Параметры — `GetLightShaftParameters` (`:143-190`): aspect-corrected uv, `TextureSpaceBlurOrigin` из направления на
солнце через unjittered view-projection как гомогенного направления (UE `ViewOrigin − Direction·WORLD_MAX`,
`DirectionalLightComponent.cpp:449-453`); пасс регистрируется только при `clip.w > 0` (`:107-116`), вне кадра, но
спереди — работает (лучи входят с края). TAA-пасс UE (`:345-363`) не переносился.
* Пасс `Main_LightShafts` между `Main_Transparent` и `Main_DebugDraw` (UE: после translucency, до постпроцесса;
  debug-геометрия не блумится): пять точек и пять диспатчей — downsample+маска → A, блюры A→B→A→B, apply в scene UAV
  (read-modify-write, RGBA16F). Таргеты `lightShaftA/B` R11G11B10 (UE PF_FloatRGB) полуразмера (UE DownsampleFactor
  2), в Texture inspector [F4] → Lighting → «Light shafts A (mask) / B (final)». Решение в `DecideFrame`
  (`decisions_.lightShafts`, `lightShaftOrigin`), строка лога `light shafts: on=… origin=…`.
* Ручки — свойства СОЛНЦА (у UE это поля ULightComponent), в блоке `directionalLight` уровня: `lightShaftsEnabled`,
  `lightShaftBloomScale`, `lightShaftBloomThreshold`, `lightShaftBloomMaxBrightness`, `lightShaftBloomTint`,
  `lightShaftOcclusionDepthRange` (м); читаются `JsonLevel` и `EnvironmentRuntime`, Inspector → Directional Light →
  секция «Light Shafts»; headless `--set=lightShafts.enabled|bloomScale|bloomThreshold|bloomMaxBrightness|occlusionDepthRange`.
  Порог и потолок — в PRE-EXPOSED единицах сцены, как у UE («post exposure brightness», `usf:70`).
* **Два дефолта отличаются от UE, оба измерены**: `enabled` 1 (UE off — пасс бесплатен при солнце за камерой),
  **`bloomThreshold` 2** (UE 0). При 0 сеет КАЖДЫЙ пиксель неба: камера palm-ring (диск солнца в кадре) — небо вдали от
  солнца ×1.20–1.22 по всем строкам (row 100/300), т.е. равномерная дымка на всё небо, а не лучи. При 2 (дважды
  «display white») сеют только диск и его гало: небо ×1.00, лучи сквозь кроны на юзерской камере остаются (off→on
  23.4 % px, mean +2.5; при пороге 0 — 76 % px, mean +6 за счёт сдвига авто-экспозиции).
* Замеры (wind_test, натив 1440p, VSM, `--wind-freeze`): камера юзера `-22.07,2.27,-94.31 / -0.0826,0.2498,0.0214,0.9645`
  — веер лучей от солнца сквозь кроны, стволы режут лучи (маска глубины) — `a7_user_off/on/thr2.png`; стоимость
  **Pass_LightShafts 0.080 мс** (5 диспатчей, 1280×720 + apply 2560×1440; профдамп 30 с, off-арм не регистрирует пасс).
  Солнце уходит за край (`--cam-fly-yaw=12 --cam-fly-delay=8`, 10 кадров через 0.5 с): гало у угла гаснет плавно,
  скачка при пересечении границы нет (Δ яркости верхней-левой четверти по кадрам ≤ 0.03 линейно, знак меняется
  только с уходом диска и адаптацией экспозиции).
* Гейты: три конфига, `check_shaders` 64/64, `--log-stress` 0/0 Debug + Release, GBV `--scene-stress-gbv=20` VSM
  **CLEAN (313.9 с)** — пасс отработал в харнессе (лог `on=1 origin=(0.270, −0.551)`: солнце над кадром, спереди).
* Не сделано (как и планировалось): TAA-история пасса (под DLSS сглаживает апскейлер; в нативе на статике не мерцает —
  два одинаковых кадра различаются на уровне шума листвы), occlusion-вариант (`FinishOcclusionMain`), тюнинг
  уровней — за юзером (`Bloom Scale`, `Bloom Threshold` в Inspector).

#### Что сделано (2026-09-05, A6)
Роща, ветер вкл, 30 с: нативные 2560×1440 (сетка 160×90×64) — Pass_VolumetricFog **0.107 мс** VSM / 0.062 Legacy;
DLSS Performance (рендер 1280×720, сетка 80×45×64) — **0.087 мс**: в 4 раза меньше ячеек дают лишь −19 % времени, т.е.
пасс упирается в латентность выборок теней и малый размер диспатча, а не в число ячеек. Бюджет ≤ 0.3 мс соблюдён с
запасом; перенос на compute-очередь и `gridZ 48` не нужны — не делались (правило: контеншен не экстраполировать).

#### Гейты части A — итог 2026-09-05
Три конфига собраны; `check_shaders` 61/61; `--log-stress` 0/0 Debug + Release; GBV `--scene-stress-gbv=20` Legacy и
VSM с `fog.volumetric:1 enabled:1 density:0.004` (объём, conservative depth и локальные источники в трёх уровнях
и на ресайзах): **Legacy CLEAN (191.5 с), VSM CLEAN (192.0 с)** — в логах 25 кадров `on=1 conservative=1`, 8 кадров `local=1` на каждый прогон. Регресс-контроль: объём выкл = закоммиченный бинарь с точностью до пола (0.014 %).

### Гейты части A
`--log-stress` 0/0; `--scene-stress-gbv=20` Legacy и VSM с `fog.volumetric:1`; три конфига; паритет
камеры теней; профдампы роща/стена K=4 off/on; глаза юзера на лучах.

---

## B. Процедурное небо (Hillaire / UE SkyAtmosphere)

### B1. LUT: transmittance + multi-scattering — DONE (2026-09-07)
Транскрипция `RenderTransmittanceLutCS` (`SkyAtmosphere.usf:1100`, 256×64, 10 сэмплов) и
`RenderMultiScatteredLuminanceLutCS` (`:1156`, 32×32, 15 сэмплов) с параметрами Земли из UE
(`FAtmosphereSetup`: 6360/6420 км, Rayleigh β, Mie β/g, озон), `SkyAtmosphereCommon.ush` целиком
(`fromTransmittanceLutUVs`, `getTransmittanceLutUvs :213-221`). `shaders/sky_height_fog.hlsli` +
`sky_lut_transmittance_cs.hlsl`, `sky_lut_multiscatter_cs.hlsl`. Пересчёт только при смене параметров
атмосферы (не солнца). Проверка: transmittance к солнцу при зените ≈ 0.9 (визуально бело-жёлтое),
у горизонта — оранжевое (числа против UE-таблицы в комментарии).

**Реализация B1 (2026-09-07):** `SkyAtmosphere` владеет двумя фиксированными RGBA16F LUT,
SRV/UAV и readback-кольцом по frame slot. `Main_SkyAtmosphereLuts` выполняется при первом
включении и изменении `SkyAtmosphereParameters`; солнце, камера, экспозиция и размер экрана
в ключ не входят. Ключ и pending readback коммитятся в serial builder, запись CL их не меняет.
Ресурсы не входят в resize-зависимый Deferred ring; учёт выделений — `mem: ... sky.luts`.
При Clear уровня GPU уже осушен, LUT сбрасываются вместе со сценой.

Дельты и уточнения относительно UE-дропа:
* Единицы атмосферы и коэффициентов сохранены: км и км⁻¹, `kMetresPerKm=1000`. В LUT локальный
  Z-up планеты; камера Y-up и reverse-Z в B1 не участвуют. Earth defaults взяты из
  `SkyAtmosphereComponent.cpp:94-128`, ground albedo = linear sRGB(170) = 0.40197778.
* Транскрибированы mapping/medium и общая часть интегратора из `SkyAtmosphereCommon.ush` и
  `SkyAtmosphere.usf`; зависящие от UE View/AP/skylight обёртки не подключены — их потребители
  относятся к B2/B3/B5. Нет зависимости от нынешнего HDRI или аналитического тумана.
* Дроп использует **два вертикальных направления** для MS (не 64 sphere samples), isotropic
  phase, `DEFAULT_SAMPLE_OFFSET=0.3`, 15 шагов. `MULTI_SCATTERING_POWER_SERIE=0` в `usf:733`:
  `MultiScatAs1 += throughput * scattering * dt`, затем **пять членов** `1+r+r²+r³+r⁴` (`:1251`).
  Это не бесконечный ряд `1/(1-r)`. Ground bounce не входит в `MultiScatAs1`.
* RGBA16F вместо desktop R11G11B10F, alpha=0. LUT — transfer при unit-white illuminance,
  без pre-exposure; солнце применяется будущим потребителем. `Mie.g=0.8` хранится в параметрах,
  но B1 интегрирует изотропно, как UE.
* Диагностический просмотр выполняется отдельной композицией **после** forward/океана, перед
  tonemap: иначе океан перекрывал LUT. На нормальном кадре этого пасса нет.

Управление B1 (session overrides через `--set`/`--sweep`, по умолчанию выключено):
`sky.lutEnabled`, `sky.lutDebugView` (0 normal, 1 T, 2 MS×10), `sky.lutValidate`;
`sky.rayleighScale`, `sky.mieScale` (scattering+absorption), `sky.ozoneScale`,
`sky.groundAlbedo` (linear), `sky.multiScatteringFactor`. Debug-view сам запрашивает LUT.
`sky.mode`, SkyView, диск солнца и замена HDRI по-прежнему относятся к B2.
Источник настроек — `Scene::SkyAtmosphereRef()`, frame берёт его напрямую, как height fog:
иначе `--set` в scene-stress меняет AppController settings, которые harness не передаёт в Scene.

Численный эталон: scalar double-транскрипция формул UE; проверяются **все 17408 RGB-текселей**,
диапазон T [0,1], конечность/неотрицательность MS и максимальная абсолютная ошибка.
MS сверяется отдельно с использованием проверенной GPU T-таблицы (linear clamp), чтобы
локализовать ошибку стадии. Readback/CPU-эталон работают только с `sky.lutValidate:1`, после
fence текущего слота; диагностика и verdict — исключительно session log.

| Центр текселя T, высота 3.679 м | Double-эталон RGB | GPU RGBA16F RGB |
|---|---|---|
| (0,0), μ=0.9736727, около зенита | 0.932043 / 0.850490 / 0.729200 | 0.931641 / 0.850098 / 0.729004 |
| (255,0), μ=−0.0008248, около горизонта | 0.084015 / 0.006338 / 0.000022 | 0.083984 / 0.006332 / 0.000022 |

Проверка invalidation на одном Release-бинаре: `--sweep=sky.rayleighScale:1,1,2,2,0,1`
дала ровно **4 rebuild**, все четыре CPU/GPU PASS. `--sweep=light.exposure:0,1,2,0`
дала **1 rebuild**. Переключение debug 1→2 также не перестраивает LUT.

Паритет normal-view (VSM, камера теней из F4, 2560×1440, native, SMRT=0, wind frozen,
manual exposure, no HUD), один процесс `--sweep=sky.lutEnabled:0,0,0,0,1,1,1,1`:
три off/off пола по доле пикселей с max RGB delta > 1/255 = **0 / 0 / 0.027045 %**;
три on/on = **0 / 0.006293 / 0.014323 %**; off→on = **0.016412 %** (внутри пола).
MAE off→on = 0.003899 в 8-битных кодах. Обычные шейдеры неба/освещения/композа не менялись.
Просмотрены `logs/b1_lut_final_00.png` (T) и `_01.png` (MS×10): полные непрерывные таблицы.

На Earth defaults max abs CPU/GPU: **T 0.0005222, MS 0.0000422**; max MS 0.060974.
Фраза «≈0.9» выше относится к красному каналу около зенита, не ко всем трём каналам.
Таблица — центры текселей, не математически точный луч с поверхности (у UE интегратор
возвращает пустой результат на/внутри планеты). Числа эталона также стоят в шапке T-шейдера.

**Гейты B1 пройдены:** Debug / Release / Release_Editor; `check_shaders` **67/67**;
`check_logging` **0 findings**; `--log-stress` Debug + Release **0 failed checks**, exit 0.
`--scene-stress-gbv=20 --gbv-mode=unguarded --no-streamline` с
`--set=sky.lutEnabled:1 --set=sky.lutValidate:1` и debug T (Legacy) / MS (VSM):
**CLEAN**, exit 0, пустой GBV error-list, CPU/GPU PASS, clean-shutdown footer в обоих режимах.
Unguarded оставляет shader validation (out-of-bounds/uninitialized), убирая guard branches;
это не режим state-only. Проверены reload, resize/render scale, shadow mode и editor churn.
Session: `session_20260907_174726_35876_debug.log` (Legacy),
`session_20260907_175115_51184_debug.log` (VSM). Первый пробный GBV до исправления источника
настроек не запускал LUT и **не засчитывался**. Первый обычный Debug-shot завис в shutdown
NGX; все зачтённые headless-прогоны выполнены с `--no-streamline` и завершились чисто.

GPU цена: три Release trace по 40 кадров с `--shot-delay=0 --set=sky.lutEnabled:1`,
validation/debug off: **0.015 / 0.018 / 0.015 мс** (медиана 0.015), по **одному**
`Pass_SkyAtmosphereLuts` в каждой трассе, на неизменных кадрах dispatch отсутствует.
Размер LUT фиксированный, не зависит от 2560×1440 вывода; бюджет 0.10 мс соблюдён.
Трассы `trace_20260907_175148_release_000.json`, `_175551_...`, `_175553_...`.
Дополнительно все три коэффициента extinction=0: T **точно 1**, CPU/GPU PASS без NaN;
MS содержит положенный ground bounce (его будущий потребитель умножит на scattering=0).
Все затронутые текстовые файлы — CRLF без смешанных окончаний. Коммит — за пользователем.

### B2. SkyView LUT + skybox из LUT + диск солнца — DONE (2026-09-07)
`RenderSkyViewLutCS` (192×104, сэмплы 4..32), `skybox.hlsl` второй режим `sky.mode 1`: направление →
`SkyViewLutParamsToUv` (`:972`) → luminance; `GetLightDiskLuminance` (`:313`) с `sunAngularSize`
(уже есть в настройках); экспозиция та же. Каждый кадр (дёшево: 192×104). Небо ниже горизонта —
земля (`IntersectGround`). Ручки `sky.mode 0|1`, `sun.elevation/azimuth` (пишут `dirLight->direction`;
источник правды — уровень, слайдер — override сессии как `--shadow-mode`).
**Критерий приёмки:** закат/полдень/сумерки глазами; переключение mode 0↔1 без изменения
экспозиции сцены (яркость неба калибруется на HDRI: измерить среднюю яркость зенита обеих).

**Реализация B2 (2026-09-07):** `Main_SkyView` строит RGBA16F 192×104 каждый активный кадр,
после lighting и до skybox. `SkyAtmosphere` владеет ресурсом, SRV/UAV и PSO; fixed-size,
resize не переаллоцирует, Clear сбрасывает после drain, память входит в `sky.luts`.
T/MS по-прежнему пересчитываются только по ключу атмосферы. Зависимости и состояния объявлены
в serial builder; skybox читает SkyView и T в pixel-readable state. В режиме HDRI для новых
слотов используются type-correct null Texture2D SRV, включая editor preview.

Транскрипция и дельты относительно UE:
* `SkyAtmosphere.usf:228-267,1282-1338`, `SkyAtmosphereCommon.ush:194-225`: полный longitude,
  нелинейная вертикаль с концентрацией у горизонта и исходные unit/sub-UV remap. Вместо
  `acosFast4/atan2Fast` — HLSL `acos/atan2` с ограничением домена от округлений.
* `usf:519-595`: 4..32 шага, максимум после 150 км, квадратичное распределение и дробный
  последний интервал, sample offset 0.3. `ParticipatingMediaCommon.ush:91-104`: Rayleigh и HG
  с правильным знаком `HG(g,-dot(toSun,viewDir))`. MS lookup и аналитический segment integral —
  `usf:342-350,653-758`, тень планеты — `:669-671`.
* Наш мир Y-up в метрах → локальный Z-up `(x,z,y)`, высота = bottom + max(1 м, camera.y)/1000.
  Планета закреплена под локальным уровнем моря, горизонтальное перемещение не меняет её up;
  yaw/roll камеры не вращают LUT. Для камеры выше оболочки выполняется MoveToTopAtmosphere.
* У UE SkyView вызывает интегратор с `Ground=false`. По требованию B2 включён его Lambert
  ground term (`usf:771-783`) при IntersectGround. Opaque/cloud shadows и второе солнце не входят в B2.
* Диск — `Common.ush:256-279`, `Rendering.cpp:445-446`: L = RGB lux / solid angle, T вдоль
  луча и soft edge, диск закрывается планетой. Существующий `sunAngularSize` интерпретируется
  здесь как **угловой радиус в радианах** (для прежнего BRDF он остаётся прежним расширением alpha).
  0 отключает диск. Поток света берётся из `DirectionalLight::GetEffectiveColor()`; как вход
  атмосферы он трактуется как outer-space illuminance, без изменения освещения геометрии в B2.
* SkyView LUT хранит pre-exposed luminance. Skybox декодирует её перед записью в raw FP16
  LightTarget, общий для HDRI и освещения геометрии. Диск ограничивается 65504 до экспозиции
  и soft-edge coverage, итоговый raw sky также ограничен 65504. Compose применяет одну общую
  экспозицию; отдельного `skyPreExposed` пути нет. Это совместимость с текущим диапазоном
  HDRI/геометрии, не полная физическая HDR-цепочка. Velocity/depth skybox сохранены.
* IBL, отражения, дальний ambient и атмосферное ослабление прямого света по-прежнему используют
  прежний путь. Особенно заметно в сумерках: окружение/вода ещё освещаются HDRI. Это граница B4/B5.

Управление: **F1 → Sky → Sky mode → Procedural atmosphere**; `Sun elevation`, `Sun azimuth`,
`Sun angular radius (rad)`, `Sky luminance scale`. CLI: `--set=sky.mode:0|1`,
`sun.elevation` (−90..90°), `sun.azimuth` (0° = +Z, +90° = +X), `sun.angularSize` (0..0.25 rad),
`sky.luminanceScale` (0..10). Положение солнца пишет `DirectionalLight::SetDirection`, поэтому
инвалидация теней работает; исходник направления — уровень, новые ручки являются session override.
Режим по умолчанию **0**, до визуальной приёмки владельцем. Камерные параметры не меняются.

Калибровка `wind_test`, солнце уровня, zenith camera `(0,50,0)`, quaternion
`(-0.7071068,0,0,0.7071068)`, один Release-бинарь, native 2560×1440, frozen wind,
manual EV100=12, bloom/light shafts/froxel off, neutral grade/local exposure, toneCurve=0.
Среднее central 128×128: raw luminance восстановлена обратной Narkowicz-кривой из PNG
(pow 2.2, решение квадратного уравнения, деление на 1.44/2^12); это оценка с 8-bit квантизацией,
не прямой HDR-readback. Ни один канал не clipped. HDRI ≈ **1276.08 cd/m²**, SkyView scale=1
≈ **599.19**, отношение **2.1297**. Default sky-only scale **2.13** даёт ≈ **1276.27 cd/m²**
(разница 0.014%, ниже точности PNG-оценки), без изменения EV. Артефакты
`logs/b2_calibrated_00.png`, `_01.png`. Предварительная оценка с localContrast=0 не засчитывается:
0 выравнивает base luminance; нейтральное значение — 1.

Паритет mode 0 после включения/выключения, камера теней F4, VSM SMRT=0, wind frozen,
manual exposure, native 2560×1440, один процесс `--sweep=sky.mode:0,0,0,0,1,1,0,0,0,0`:
три пола до переключения **0.02216 / 0.02661 / 0.01736 %** пикселей с max RGB delta >1/255;
возврат 0→1→0 — **0.00000 %**, MAE 0.0000633 кода. Три пола после возврата
**0.01997 / 0.01270 / 0.02043 %**. Артефакты `logs/b2_parity_00..09.png`.

Проверены кадры солнца +60° / +2° / −6°, в том числе `logs/b2_final_00..02.png` при общем
EV100=12, и `logs/b2_time_00..02.png` при исходной дневной экспозиции. Диск в `b2_disk_00..02.png`:
радиусы 0 / 0.00465 / 0.01 rad дают 0 / 116 / 525 ярких пикселей, диаметр 0 / 12 / 26 px.
При радиусе 0 остаётся рассеянный ореол, диск исчезает. Визуальный вердикт — за пользователем.

**Гейты B2 пройдены:** Debug / Release / Release_Editor, shader matrix **70/70**,
`check_logging` **0 findings**, `--log-stress` Debug / Release — **0 failed checks**, exit 0.
`--scene-stress-gbv=20 --gbv-mode=unguarded --no-streamline --set=sky.mode:1 --set=sky.lutValidate:1`
в Legacy и VSM: **CLEAN**, exit 0, clean-shutdown footer, без validation errors.
Сессии `session_20260907_233806_44376_debug.log` (20 итераций / 279.3 с) и
`session_20260907_234323_58836_debug.log` (20 / 307.9 с). Проверены reload, resize,
render/reflection scale, переключение теней и editor churn. T/MS CPU/GPU reference PASS
после reload; SkyView и skybox действительно выполнялись. После этих прогонов изменён только
default calibration scale 1→2.13 и комментарии; финальные три бинаря пересобраны.

Стоимость на одном финальном Release-бинаре, камера теней F4, 2560×1440, native, SMRT=0,
manual exposure, wind frozen, warm-up 2 с, `--trace=40`: три медианы **Pass_SkyView
0.010 / 0.010 / 0.010 мс** (43/42/42 GPU events с учётом хвоста timestamp readback).
В трёх HDRI-контролях SkyView отсутствует; T/MS не перестраивались на неизменных кадрах.
Skybox: procedural 0.005 мс, HDRI 0.006 мс; compose соответственно 0.116–0.117 / 0.117–0.118 мс,
различия этого порядка не интерпретируются как ускорение. Бюджет LUT 0.10 мс соблюдён.
Трассы `traces/trace_20260907_234928_release_000.json`, `_234932_...`, `_234935_...` (on),
`_235031_...`, `_235035_...`, `_235038_...` (off). GBV-процессы к моменту замера завершились.
Все 23 затронутых текстовых файла — CRLF, без смешанных окончаний. Коммит — за пользователем.

**Исправление диапазона солнца B2 (2026-09-08, повторный отчёт владельца):**
Предыдущая попытка ослабляла общие LightShaftBloom / convolution bloom / ghosts и меняла
local exposure. Владелец отклонил её: она портила настройку HDRI и убирала bloom с блика
на сфере, не устраняя разрыв яркости источников. Все три настройки `wind_test.json`
(100 / 1 / 0.5), `exposure_baselum_cs`, `tonemap_cs` и `local_exposure.hlsli` восстановлены
из HEAD. `skyfix_*` — исторические кадры отклонённого решения, не актуальный результат.

Причина: B2 обходил raw FP16 LightTarget посредством отдельной pre-exposure ветки только
для procedural sky. HDRI и surface lighting оставались в прежнем ограниченном диапазоне.
Процедурный диск получал на несколько порядков больше доступной яркости. Декодирование
BC6H исходного rustig DDS показало пик 65504 в cube units; после physical scale 10985.2468
это 7.196e8. Но в рендере HDRI уже ограничен raw FP16 диапазоном 65504, как и блик на сфере.
Калибровка зенита сама по себе этот разрыв не обнаруживает.

Исправлено в `skybox.hlsl`: SkyView декодируется перед LightTarget, диск ограничивается
тем же raw диапазоном до exposure, soft edge применяется после ограничения радианса.
Исключение `skyPreExposed` из compose и его CPU binding удалено. Удаление ошибочного
sky-only luminance scale 2.13 из диска сохранено. Авторские bloom, shafts, экспозиция,
освещение поверхностей и HDRI shader path не перенастраиваются. Полный перенос ВСЕГО
освещения в pre-exposed HDR требует отдельной согласованной миграции; снимать ограничение
только с процедурного неба повторно нельзя.

Проверки, точная камера нового HUD:
`--cam-pos=-54.81,3.00,63.71 --cam-rot=-0.0571,0.4842,0.0317,0.8725`, wind frozen.
* `energy_range_00/01.png`: mode 0/1, EV100=16, manual compensation=0, neutral grade/local
  exposure, bloom/shafts/volumetric off. Солнце и блик сферы в ОБОИХ режимах имеют пик
  RGB 239/239/239. Инверсия 8-bit ACES даёт около 6.41e4 (квантованный замер, не прямой
  HDR readback). Исправный ключ: `fog.volumetric`; `fog.volumetric` не существует.
* `energy_fixed_00/01/02.png`: mode 0→1→0 с исходной auto exposure и восстановленными
  авторскими эффектами. HDRI против кадра до правки raw пути: MAE 0.00984/255,
  0.0874% пикселей отличаются больше 1 code value; round trip 0.0635% >1 code value.
* `energy_old_view_00/01.png`: первая камера владельца
  `--cam-pos=-88.57,5.11,-37.55 --cam-rot=-0.1980,0.3683,0.0806,0.9048`, bloom off/on.
  Слабая полоса сохраняется, прежний гигантский пересвет ядра/ghosts отсутствует.
* Debug / Release / Release_Editor собраны; 70/70 shader entries; check_logging 0.
  Кадры native 2560×1440 без DLSS (`--no-streamline --dlss=off`). Новых ресурсов/пассов нет;
  GBV для этой коррекции не повторялся.

### B3. Aerial perspective volume — DONE (2026-09-09)
`CameraAerialPerspectiveVolume` 32×32×16 на 96 км (`:1002-1009`, `SkyAtmosphereRendering.cpp:121-137`);
в композе для геометрии дальше `fog.volumetricDistance`: `color = color·T_ap + L_ap` ПОВЕРХ нашего
аналитического тумана (разные масштабы: км против м). На малых сценах почти невидимо — принимать по
острову с дальних камер. Старое имя `fog.volumetricDistance` в тексте было неточным: CLI использует
`fog.volumetricDistance`.

**Реализация и дельты UE:**
* `sky_lut_aerial_cs.hlsl`: транскрипция `SkyAtmosphere.usf:1466-1636`, без ground bounce,
  opaque/cloud shadows, второго солнца и capture-360. Позиция камеры и солнечный источник — B2,
  мир Y-up в метрах → локальный Z-up в км. Перспективная камера, inverse projection без jitter.
* Квадратичное распределение глубины: `((z+0.5)/16)^2 * 96 km` (`:1504-1514`),
  2*(z+1) равномерных сэмпла, sample offset 0.3 (`:1608`, `:590-604`). Конечный луч ограничен
  планетой/атмосферой, подземные/скрытые горизонтом ячейки перепроецируются как UE (`:1527-1570`,
  включая 20-метровую поправку камеры). Для камеры выше атмосферы вычитается путь до входа
  (`:1575-1601`); сегмент, не дошедший до неё, имеет L=0, T=1.
* RGB — pre-exposed рассеянный радианс (sky luminance scale 2.13 включён, как в SkyView),
  alpha — средняя RGB transmittance. Не содержит солнечный диск. Новый объём RGBA16F занимает
  128 KiB полезных данных; выделение учитывается существующим memory provider `sky.luts`.
* Общая graphics queue, постоянный ресурс без истории; rebuild каждый активный кадр.
  `SkyView → SkyAerial → Skybox → ... → Compose`, UAV/SRV-переходы объявлены в serial builder.
  Выключение AP / mode 0 не планирует AP dispatch. Размер окна не меняет размер объёма.
  Дельта dispatch: Y/Z развёрнуты в Y для существующего 2D compute helper; 32×512 потоков.
* Начало AP — именно ДАЛЬНЯЯ ПЛОСКОСТЬ froxel fog, по view Z, а не сфера вокруг камеры:
  `startKm = volumetricDistance / (1000 * viewDir.z)`. Сегмент начинается там даже при
  выключенном ближнем fog. Это требование B3; UE передаёт радиальную start distance.
  Пересечение земли до start plane даёт нейтральную ячейку.
* `sky_aerial_common.hlsli`: UE `SkyAtmosphereCommon.ush:62-81,103-115` — sqrt depth lookup,
  near fade с sqrt(0.5) и 1-сантиметровой зоной старта. Дальше 96 км используется край LUT.
  Compose восстанавливает позицию из jittered depth и проецирует в non-jittered UV объёма,
  снимает его exposure и применяет `color*T+L` после существующего fog, до общего exposure.
  Sky / ближняя геометрия не получают AP. HDRI, bloom, shafts, свет поверхности не изменены.
* Диагностический `Main_SkyAtmosphereDebug` работает ПОСЛЕ forward draws. Читает `depthCopy`
  (opaque snapshot до океана), иначе depth океана подменяет измеренную геометрию. Незатронутые
  пиксели чёрные; обычная вода не перекрывает диагностический вывод. Старые виды T/MS сохранены.
* B3 применяется только к opaque compose. Океан/стекло/частицы не получают собственного AP;
  океан видит уже обработанный opaque цвет через существующее преломление. IBL остаётся HDRI
  до B4. Согласование океана и его собственной AP обязательно закрывается в B6.
  Остаточные неиспользуемые CPU handles `skyPreExposed` от раннего B2 удалены.

**Управление:** F1 → Sky → Procedural atmosphere → Aerial perspective.
`--set=sky.aerialPerspective:1`, `--set=sky.aerialDebugView:0|1|2|3` (scene / T / L / depth slices).
Режимы диагностики доступны при включённом AP. По §0.5 AP пока выключен по умолчанию до
визуальной приёмки владельцем; это session setting, как sky mode, не авторская правка уровня.
Пользовательское изменение `graphics_settings.json` (sunAngularSize 0.0192) сохранено.

**Проверки:** native 2560×1440, `--no-streamline --dlss=off --wind-freeze`, shot delay 8 s,
VSM SMRT=0. Кватернион острова из §F4, дальняя камера — та же ориентация и позиция ×4:
`--cam-pos=-1249.08,872.56,476.92 --cam-rot=0.1445,0.8409,-0.2736,0.4440`.
* Три пары пола ДО правки (`b3_before_00..02`, manual EV12, камера острова): 0.538–0.658%
  пикселей >1 code value. Здесь вода заметно шумнее прежней камеры теней: нельзя сравнивать
  этот кадр с чужим 0.03%-полом. На том же бинаре HDRI AP off/on: 0.168%; старый HDRI против
  нового off: 0.538%; procedural off против B2: 0.168%. Регрессии сверх пола нет.
* `b3_far_00..04`: AP off/on и три пары on/on. Пол 0.00713–0.00770% >1 code value,
  изменение AP 0.2811%. В opaque-маске средняя абсолютная разница 1.682/255 против
  on/on 0.479–0.529/255. Эффект слабый на сотнях метров, с расстоянием видимо снижает
  контраст/добавляет голубоватое рассеяние. `b3_detail_review.jpg` — просмотренные кропы.
* `b3_debug_final_00..02`: T, L, slices по opaque depth. `b3_zero_sun_00/01`:
  с 85000 lx положительный AP L в 40588 пикселях, при 0 lx весь кадр RGB=0.
  `b3_trans_sun_00/01`: T при тех же 85000/0 lx совпадает побитово.
* Три конфига собраны, shader check 71/71, `check_logging` 0; `--log-stress` Debug/Release 0/0.
  Legacy GBV 20 циклов: CLEAN, exit 0 (session PID 41772). VSM 20: CLEAN, exit 0 (PID 66124).
  Финальный VSM после перехода диагностики на opaque depth: 20 циклов CLEAN, exit 0
  (PID 54020); ошибок в session log нет, clean shutdown. Старые T/MS debug views и
  Release_Editor с AP также прошли smoke run, exit 0.
* Ближний пользовательский ракурс `--cam-pos=-54.81,3.00,63.71`
  `--cam-rot=-0.0571,0.4842,0.0317,0.8725`, manual EV12: три пары пола
  0.383–0.678% >1 code value; AP off/on 0.502%, в пределах пола (`b3_near_00..03`).
* GPU timing: Release, дальняя камера, native 2560×1440, без параллельного GBV;
  три последовательные пары AP off/on, по 82 GPU samples в trace. SkyAerial median
  0.007 / 0.007 / 0.007 ms, mean 0.013659 / 0.007524 / 0.007732 ms (первый прогон
  содержит выброс). Compose median 0.030 ms во всех шести прогонах; AP dispatch при off
  отсутствует. Traces: `trace_20260908_200232` .. `trace_20260908_200315_release_000.json`.
  `--dlss=off` выставляет render scale 1.0: проверено HUD `b3_resolution.png`.
  Сохранённый пользовательский renderScale 0.580078 не изменялся.

### B4. IBL из процедурного неба — DONE (2026-09-09)
При изменении солнца/атмосферы: захват кубмапы 128² из SkyView LUT (6 дисп.) → irradiance (наш
существующий свёрточник, если он GPU; если CPU-бейк при загрузке — перенести в compute) → префильтр
specular по мипам. Заменяет `SkySpecular`/`SkyboxTex` в режиме 1; `skyboxIntensity` = 1 (единицы
физические: LUT в cd/m² через `sunIlluminanceLux`). Время суток → свет меняется везде согласованно
(вода, IBL, туман).
**Критерий приёмки:** камера теней при mode 1 и солнце уровня ≈ HDRI-картинка по экспозиции;
слайдер `sun.elevation` — непрерывно без скачков; стоимость перезахвата ≤ 0.3 мс и ТОЛЬКО при смене.

**Реализация и дельты UE:**
* GPU capture 128² × 6 из отдельного SkyView 192×104; фиксированный глобальный probe на
  1 м над уровнем моря. Камера и автоэкспозиция не входят в dirty key. Ключ: параметры
  атмосферы, нормализованное направление и RGB illuminance солнца, sky luminance scale.
  SkyView probe хранится с фиксированной preExposure 1/1024; кубы содержат raw radiance,
  skyboxIntensity=1. Авторский HDRI trim и lux calibration сохраняются для mode 0.
* `SkyAtmosphere.usf:956-974` — выборка SkyView; `:314-317` — БЕЗ солнечного диска в
  отражениях, чтобы не удваивать аналитический specular солнца. Изначально нижняя полусфера
  сохраняла ground term B2; после коррекции ниже capture имеет чёрную нижнюю полусферу.
  World Y-up → local Z-up только при выборке SkyView.
* `ReflectionEnvironmentShaders.usf:99-129` — ориентация шести D3D-граней; `:537-647`,
  `MonteCarlo.ush:58-63,248-261,347-363` — Hammersley, cosine hemisphere и GGX importance
  sampling. 64 samples; specular 128² / 8 mips, diffuse 32² с E/PI (наш формат вместо UE SH).
  Roughness/mip mapping — общий `IblRoughnessFromMip`, как у существующих потребителей.
  Mip 0 — sharp copy. Дельта: disk-free источник гладкий, свёртка читает source mip 0,
  без отдельной source mip pyramid и PDF-based LOD. Все шесть граней развёрнуты в dispatch Y,
  один dispatch на целевой mip вместо шести по граням.
* Постоянные GPU-ресурсы принадлежат SkyAtmosphere, память включена в `sky.luts`.
  Пересборка идёт на graphics queue до lighting и RT; dirty state коммитится serial builder.
  На статичных кадрах capture/filter не планируются. Prologue снимает PIXEL bit перед async RT,
  forward снова объявляет pixel reads; новые ресурсы проходят обычный render graph.
* Общий выбранный environment SRV используется в deferred lighting/compose, RT hit shading,
  обоих океанских путях и стекле. Asset/editor previews продолжают показывать исходный HDRI.
  BRDF LUT остаётся существующим, загружается также при отсутствии запечённых F7 derivatives.
  B4 переключает источник окружения океана; собственная AP воды и приёмка горизонта остаются B6.

**Управление:** F1 → Sky → Procedural atmosphere → Procedural environment lighting,
`--set=sky.environmentLighting:1`. По §0.5 выключено по умолчанию до визуальной приёмки.
HDRI mode всегда использует исходное окружение независимо от этой галочки.

**Проверки:**
* `b4_before_00..02`: три пары пола на камере теней, native 2560×1440, no Streamline/DLSS,
  wind-freeze, SMRT=0, manual EV12: 0.05084–0.06388% пикселей >1 code value.
  До/после в HDRI: 0.02214%; в одном новом бинаре HDRI с environment off/on: 0.05122%,
  в пределах пола. `b4_shadow_review.jpg` — HDRI / procedural+B3 lighting / procedural+B4.
* `b4_near_00..03`, точный пользовательский ракурс из B3, authored auto exposure: солнце 28°,
  environment off/on, затем 5° и 29°. `b4_near_review.jpg` просмотрен: цвет воды и заполнение
  теней следуют процедурному небу, энергия солнца/bloom не перенастраивалась.
* `--scene-stress-sky=64`: непрерывные sun edits, hold, exposure changes, environment/mode
  toggles при RT без GPU-idle между кадрами; существующий stress verdict/session log и trace.
  Первые 32 кадра меняют солнце, следующие 32 проверяют удержание/переключение cache.
* GPU timing: три Release-прогона `--scene-stress-sky=64`, native 2560×1440 (размер
  подтверждён session log), wind frozen, RT, SMRT=0, без параллельного GBV. В каждом trace
  ровно 32 capture/filter dispatch на кадрах смены солнца; на следующих 32 кадрах (hold,
  exposure, master/mode toggles) пересборок нет. Median 0.038 / 0.039 / 0.038 ms,
  mean 0.03881 / 0.03919 / 0.05859 ms; maxima 0.044 / 0.042 / 0.674 ms. Типичная стоимость
  ниже бюджета 0.3 ms; в третьем прогоне был единичный выброс, поэтому это не гарантия
  верхней границы каждого кадра. Traces: `trace_20260909_114220_release_000.json`,
  `trace_20260909_114223_release_000.json`, `trace_20260909_114225_release_000.json`.
* В SceneStress добавлен пропущенный `CollectGpuResults()` после BeginFrame, как в App:
  без него старые timestamp batches доживали до перезаписи readback ring. Ранние стресс-трейсы
  `013909..013913` для GPU timing непригодны и в итоговый замер не входят. Sky stress явно
  выставляет native scale / DLSS off / frozen wind: обычные CLI-парсеры этих ручек идут
  ПОСЛЕ раннего входа в stress. Прогрев sky stress — 32 кадра, без idle между sun edits.
* GBV Legacy 20: CLEAN, exit 0 (PID 65444); VSM 20: CLEAN, exit 0 (PID 10904).
  Дополнительно continuous sun/cache/mode stress с RT + unguarded GBV, 64 шага: CLEAN,
  exit 0 (PID 20676), без ERROR/FATAL, clean shutdown. Debug-лог: 1 начальная пересборка
  + 32 sun edits, затем cache не пересчитывается. Проверки GBV проводились до последней
  правки только условий замера/сбора profiler results в harness; рендер-путь не менялся.
* Debug / Release / Release_Editor собраны; shader check 73/73, check_logging 0;
  log-stress Debug/Release 0/0. Final Debug sky-stress, Release_Editor и classic ocean
  smoke — exit 0; `b4_final_editor.png` просмотрен.
* После возобновления работы сохранены параллельные пользовательские правки океана
  (`reflectDir.y = abs(reflectDir.y)` в обоих шейдерах и reflectionSkyHorizonPull=1 в уровне).
  Повторный HDRI off/on на текущих файлах, пользовательская камера, manual EV14:
  три пары пола 0.29910–0.36160%, environment toggle 0.25130%, в пределах пола
  (`b4_final_hdri_00..03`). Эти правки не входят в реализацию B4; исходный before/after
  выше снят до них. HDRI/bloom/экспозиция в рамках B4 не перенастраивались.

**Коррекция B4: рыжая обратная сторона при Ground Bounce Albedo = 0 (2026-09-09).**

* Пользовательский ракурс: camera `-56.69,3.66,39.71`, quaternion
  `-0.0921,0.2555,0.0244,0.9621`, все переключатели процедурного неба включены.
  Диагностическое выключение B5 и всего fog почти не убирало яркие стволы. Выключение
  отдельного ground bounce тоже оставляло проблему; источник — B4 environment.
* Причина: камера SkyView B2 рисует виртуальную поверхность планеты в нижней полусфере.
  B4 переносил эту поверхность в radiance cube, затем в diffuse/GGX. Её яркость задавали
  planet ground albedo=0.40197778 и sky luminanceScale=2.13; ручка материала земли
  `light.groundAlbedo` на этот свет не влияла. Поверх добавлялся отдельный GroundBounceOverPi.
* Прочитан UE `ReflectionEnvironmentShaders.usf:412-434` и
  `ReflectionEnvironmentRealTimeCapture.cpp:967-991`: замена нижней полусферы skylight
  перед свёрткой. В `sky_ibl_capture_cs.hlsl` направления local Z < 0 записывают чёрный RGB
  до diffuse/GGX фильтрации. Дельта: фиксированный black lower hemisphere для нашего
  sky-only probe; отражение от земли в diffuse остаётся под `light.groundAlbedo`, одинаково
  для deferred и RT hit lighting. Нижний environment-specular также больше не содержит
  фиктивной светящейся поверхности планеты; геометрия земли отражается через SSR/RT.
  Camera SkyView, физический ground contribution в MS, B5 и AP не изменены.
* Никакой перенастройки HDRI, экспозиции, bloom, солнца или уровня. Один Release-бинарь
  до/после изменения runtime shader; native, frozen wind, SMRT=0, EV14, ground bounce=0.
  По три снимка на каждую сторону: procedural noise до 1.0714–1.3268% px >1, после
  0.2207–0.3765%; изменение 73.6361%, MAE 5.4612%. `sky_ground_fix_review.jpg` просмотрен:
  пропало избыточное рыжее заполнение стволов/крон, освещённые поверхности и небо сохранены.
* HDRI mode 0: пол до 0.2762–0.6543%, после 0.3407–0.7043%; до/после 0.6187%, внутри пола.
  Снимки `sky_ground_before_<mode>_0..2`, `sky_ground_after_<mode>_0..2`.
  Три конфигурации собраны, 74/74 шейдеров компилируются. Новых ресурсов/пассов/барьеров нет:
  по §0.6 гейт этой коррекции — shader compile, паритет и визуальное сравнение, без повторного GBV.

* Дополнительно просмотрен `sky_ground_variants.jpg`: ground bounce=0 / 0.25,
  автоэкспозиция, солнце 5° при EV12. Ручка возвращает контролируемое заполнение,
  автоэкспозиция не возвращает прежнюю рыжую засветку. Debug sun/cache/mode stress
  64 шага с RT — exit 0, ERROR/FATAL=0, clean shutdown (PID 64780).

### B5. Distant sky light LUT → туман и облака — DONE (2026-09-09)

* Источник прочитан первым: `SkyAtmosphere.usf:1360-1449` (`RenderDistantSkyLightLutCS`),
  интегратор `:590-604,628-633,738-758`; высота 6 км — `SkyAtmosphereRendering.cpp:200-206`.
  64 направления по сфере, 10 равномерных шагов с offset 0.3, isotropic phase, T + MS,
  тень планеты, без солнечного диска и отражения от земли. Солнечный RGB и B2 luminanceScale
  применяются один раз. Результат — raw RGB, без pre-exposure; усреднение уже включает
  `4π / 64 × 1 / (4π)`, поэтому потребитель не умножает его повторно на фазу или `1/π`.
* Дельты: один источник солнца, без cloud/opaque shadows, отдельный UE
  SkyAndAerialPerspectiveLuminanceFactor белый (1), как в B2. Вместо structured buffer —
  одна RGBA32F-текстура 1×1; alpha=0. Направления из `SkyAtmosphereRendering.cpp:1102-1120`
  и `RandomStream.h:115-123,342` (seed `0xDE4DC0DE`) записаны константами вместо GPU-буфера.
  Все уровни shared reduction имеют полный group barrier, без предположения о lockstep последних lanes.
* `Main_SkyDistant` идёт после T/MS и B4, перед потребителями. Пересчёт только при смене
  параметров атмосферы, направления/цвета/энергии солнца или luminanceScale; камера, экспозиция
  и радиус диска не входят в ключ. Ресурс объявлен в render graph, между кадрами находится
  в NON_PIXEL_SHADER_RESOURCE; lifetime и память принадлежат `SkyAtmosphere` / `sky.luts`.
  `sky.lutValidate` копирует один texel в readback-кольцо по frame slot и пишет RGB/PASS
  в session log после fence. При ошибке создания сохраняется прежний IBL ambient.
* `fog_scatter_cs.hlsl` при `sky.mode=1 && sky.distantSkyLight` использует LUT × `skyScatter`
  вместо irradiance-куба. Это дельта проекта по плану: UE fog сам использует SH; UE clouds
  читают distant sky LUT (`VolumetricCloud.usf:729-743`). SRV открыт для будущего C3, самих
  облаков B5 не добавляет. Нет HDRI exposure/skyFill поверх LUT; fog применяет pre-exposure
  ровно один раз. При смене LUT или включении/выключении источника история fog сбрасывается;
  пересоздание сцены также сбрасывает её ключ.
* Master по умолчанию **off**: F1 → Sky → **Distant sky light (fog)**,
  CLI `--set=sky.distantSkyLight:1`. Для видимого эффекта нужны mode 1, объёмный туман
  и `fog.skyVolScatter > 0`. Независим от B4 `environmentLighting`.
  HDRI, bloom, калибровка солнца и аналитический height fog не менялись.
  Собственная AP океана и приёмка горизонта остаются обязательным B6.

Проверки B5 (2026-09-09):

* Readback, wind_test, sun azimuth 30°, 85000 lux, luminanceScale 2.13:
  raw RGB `[1477.431274, 2026.848877, 3039.851807]`.
  При 42500 lux — `[738.715637, 1013.424438, 1519.925903]` (ровно половина в точности FP32).
  Ноль lux и отдельно ноль luminanceScale дают `[0,0,0]`, alpha=0.
  Azimuth 120° при той же высоте: `[1476.675415,2025.824219,3039.086426]`;
  отклонение <0.052% соответствует фиксированному набору 64 направлений, а не зависимости от камеры.
* Debug / Release / Release_Editor собраны; 74/74 шейдеров; `--log-stress` Debug/Release 0/0;
  `check_logging.py` 0 findings. Изменённые текстовые файлы — CRLF без смешанных окончаний.

* Первоначальный Legacy+B5 на сетке 16 px дважды получил TDR при первом ReloadLevel
  (PID 68584, 69384), без page fault в DRED. Контроль B5 off — CLEAN (PID 69404),
  B5 on / 32 px — CLEAN (PID 60660), по 20 циклов. Чтение distant texel вынесено
  из `FogSampleCell` за цикл supersampling: один Load на ячейку вместо до четырёх,
  то же значение ambient и меньше descriptor checks в инструментированном шейдере.
  После правки **Legacy+B5 unguarded GBV / 16 px / 20 циклов — CLEAN**, exit 0,
  PID 65800, ERROR/FATAL=0, clean shutdown. **Повторный VSM / 16 px / 20 циклов также
  CLEAN**, exit 0, PID 62272, ERROR/FATAL=0, clean shutdown.
* После паузы сохранены пользовательские изменения `graphics_settings.json`:
  legacyCsm maxSlope=3, normalBiasTexels=1.5. Они не входят в B5.
* HDRI, native 2560×1440, frozen wind, SMRT=0, manual EV14, камера
  `-37.61,2.50,-98.03 / -0.0997,0.2987,0.0314,0.9486`, volumetric fog on:
  три пары пола до B5 — 0.06779–0.15112% пикселей >1 code value;
  три пары нового бинаря — 0.11136–0.14950%. HDRI distant off/on в одном бинаре —
  0.09551%, внутри пола. До/после: 0.15465%, MAE 0.00731% против MAE пола нового
  бинаря 0.00528–0.00775%; сопоставимо с шумом, но не заявляется побитовым паритетом.
  Снимки `b5_before_00..02`, `b5_after_00..02`, `b5_hdri_on`.
  Для старого before временно задано CLI csm.maxSlope=1 / csm.normalBias=0.7, файл настроек сохранён.
* `b5_fog_review.jpg` просмотрен: солнце 28°/EV14 и 5°/EV12, B4 включён в обеих сторонах,
  fog density=0.005, skyVolScatter=1, sunVolScatter=0, bloom off только в тесте.
  B5 off/on меняет именно ambient: дневная дымка холоднее, при низком солнце цвет и
  энергия следуют атмосфере. MAE 1.729% / 0.673%, существенно выше пола повторных кадров.
  Виды не являются приёмкой границы океана (B6).
* Три Release `--scene-stress-sky=64`, native 2560×1440, RT, frozen wind, SMRT=0,
  volumetric fog on: в каждом trace 32 `Pass_SkyDistant`, затем 32 кадра без пересчёта
  (hold/exposure/environment/mode toggles). Median 0.007 / 0.007 / 0.007 ms,
  mean 0.007156 / 0.006969 / 0.006906 ms, maxima 0.009 / 0.008 / 0.009 ms.
  Traces `trace_20260909_170302_release_000.json`, `_170305_...`, `_170307_...`.
  Это стоимость пересчёта LUT, не всего тумана; при смене освещения сброс history
  также запускает существующий history-miss supersampling в fog scatter.

* Финальный Debug `--scene-stress-sky=64` с B5, T/MS validation и объёмным туманом:
  CLEAN, exit 0, PID 54020. 33 пересчёта (начальный + 32 изменения солнца), 33 readback
  PASS, ERROR/FATAL=0, clean shutdown. Проверены обновление при непрерывных sun edits,
  кольцо readback и повторное использование cache при hold/exposure/mode toggles.

### B6. Согласование океана с небом и aerial perspective — обязательно до завершения части B
Требование владельца (2026-09-09): океан, туман и видимое небо должны давать согласованный,
плавный переход на горизонте. Текущий шов в procedural mode — незавершённая интеграция;
одних B4/B5 недостаточно для закрытия этого пункта. Зависимости: B3, B4, B5.

* B4: в обоих океанских путях (`ocean_surface.hlsl`, `ocean_surface_legacy.hlsli`) отражения,
  выборки неба для дальнего тумана и horizon fade используют текущее процедурное окружение
  в mode 1, согласованное с видимым небом по направлению, яркости и экспозиции.
* B5: освещение участвующей среды согласовано с тем же небом и солнцем.
* Подключить AP B3 к собственной поверхности океана в forward pass: тот же объём, глубина
  старта и единицы, что у opaque compose. Учесть порядок ближнего fog, дальней AP и рефракции:
  исключить повторное затуманивание уже обработанного фона и двойной horizon fade.
* Сохранить настроенный HDRI-путь (mode 0), bloom и экспозицию. Не скрывать шов их перенастройкой.

**Критерий приёмки:** при включённой AP переход океан → небо плавный, без искусственного
цветового/яркостного шва; ориентир по качеству перехода — нынешнее HDRI + aerial perspective.
Проверить оба океанских пути, береговой и дальний ракурсы, взгляд к солнцу и от него, день и
низкое солнце; изменение sun.elevation не разрывает переход. Сравнения off/on — с одинаковыми
камерой, экспозицией и замороженным wind/ocean clock; HDRI-паритет — относительно своего пола шума.
Часть B не считается визуально принятой, пока этот пункт не закрыт.

### B6.0. Роли текстур неба и общий источник тумана — СДЕЛАНО (2026-09-09), не закоммичено
Жалоба владельца: в процедурном режиме по горизонту чёрный обод, взгляд вниз из океана фетчит ноль.

**Разбор.** Коррекция B4 покрасила нижнюю полусферу в `sky_ibl_capture_cs.hlsl` чёрным. Это UE-точно
для ЗОНДА ОСВЕЩЕНИЯ (`ReflectionEnvironmentShaders.usf:78-81,205-211,429-434`, их же обоснование:
«no sky lighting is coming from below the horizon ... to avoid leaking from below since we are
integrating incoming lighting and shadowing separately»). Но той же текстурой кормился аналитический
туман: `viewRay = normalize(positionWS - cameraPos)` смотрит НА ПОВЕРХНОСТЬ, то есть вниз на каждом
пикселе земли и воды, где туман вообще считается. `skyAlongView` возвращал значение политики, и при
`maxOpacity 0.9` пиксель у горизонта — это `0.1*вода + 0.9*inscatter` без небесного члена.

UE такой ошибки не делает, потому что тычет вниз В ДРУГОЙ КУБ: `InscatteringColorCubemap` на
компоненте тумана (`HeightFogCommon.ush:144-181`, тултип `ExponentialHeightFogComponent.h:50` —
«useful to make distant, heavily fogged scene elements match the sky»), полносферный ассет без
политики горизонта. И вдобавок включает направленность ПО ДИСТАНЦИИ.

**Сделано — разведение ролей.**
* `sky_ibl_capture_cs.hlsl`: чёрная полусфера убрана, куб снова полносферный, и получил мип-цепочку
  0..7. Каждый мип снимается прямо со SkyView LUT с `128/size` суперсэмплами по оси (кап 8), поэтому
  ни один мип не читает другой — цепочка это один прогон диспатчей без барьеров между ними.
  `environment_[0]` = наш аналог `InscatteringColorCubemap`, генерируемый вместо авторского.
* `sky_ibl_filter_cs.hlsl`: политика нижней полусферы переехала СЮДА, в форме UE
  `lerp(sky, LowerHemisphereColor.rgb, LowerHemisphereColor.a)` — на каждый сэмпл свёртки и на
  sharp-копию mip 0. Дефолт `{0,0,0,1}` = непрозрачный чёрный, как `SkyLightComponent.cpp:304,312`.
  Зонды (`environment_[1]` specular, `[2]` irradiance) поэтому бит-в-бит те же, что при покрашенном
  захвате; разница только в том, что КАРТИНКА теперь выживает для потребителя, которому она нужна.
* `fog_common.hlsli`: `FogSkyAlongView(...)` — один источник для всех четырёх потребителей тумана
  (`compose_cs.hlsl:448`, `ocean_surface.hlsl:2248`, `ocean_surface_legacy.hlsli:1360`,
  `glass.hlsl:542`). Внутри — фейд UE по дистанции: ближе `nonDirectionalDistance` берётся верхний
  мип (среднее по сфере), дальше `fullyDirectionalDistance` — направленный сэмпл с нашим
  headroom-блюром. Направленная ветка сохраняет прежний `IblClampToSharp`; дистанционный фейд им
  НЕ ограничивается — вблизи среднее законно ярче mip 0 везде, кроме взгляда в солнце.
  Число мипов читается с самого куба (`GetDimensions`), поэтому ни один потребитель его не носит.
* Ручки: `fog.nonDirectionalDistance` и `fullyDirectionalDistance`, в уровне, в `--set`, и в
  Inspector → Fog → Sky sampling. Упаковка одна, в `PackAtmosphere` → `params2.zw` = (invRange, bias),
  как `FogRendering.cpp:273,284`. **Дефолт 0/0 = фейд ВЫКЛЮЧЕН**, см. ниже.
* Мип берётся тем же `IblMipFromRoughness(roughness, levels)`, что и раньше, против собственного
  числа уровней куба. То есть количество размытия у тумана не изменилось — изменилась только
  текстура, которая отвечает. Первая версия считала мип как `roughness * kSkyRoughMaxMip` (2.5 против
  прежних 4.8 при skyBlur 0.5), то есть вдвое резче, и печатала облака в ближний туман.

**Дельты UE.** (1) Полносферный куб у нас производный от процедурного захвата, а не авторский ассет.
(2) Политика применяется в свёртке, а не к самому захвату — у UE полносферного скайлайт-куба не
остаётся вовсе. (3) Наша формула inscatter (`AtmosphereInscatter`, солнечная доля + back-scatter) не
UE-шная и здесь не менялась. (4) `AtmosphereSkyRoughness` и `AtmosphereClampSkySample` — наши, у UE
блюр только по дистанции; оставлены поверх фейда.

**Почему дистанционный фейд едет ВЫКЛЮЧЕННЫМ (правка по отчёту владельца, 2026-09-09).** Первая
версия ставила дефолты UE (10 м / 1000 м) и забелила открытую воду молоком до горизонта в HDRI-режиме.
Структурная причина: у UE куб УМНОЖАЕТ авторский цвет тумана (`Inscattering = FogColor * lerp(NonDir,
Dir, t)`), а у нас сэмпл идёт прямо базой в `AtmosphereInscatter`. Их безопасное среднее по сфере —
с солнцем внутри — у нас становится вбросом энергии в ближний туман, и вдобавок недирекционная ветка
намеренно не ограничена `IblClampToSharp`. Ручка остаётся (рассуждение за ней верное: луч тумана
смотрит НА ПОВЕРХНОСТЬ, вблизи круто вниз), но включать её — это look change, который по §0.5 обязан
заработать свой дефолт через A/B. Обод она не убирала: контрольный прогон с выключённым фейдом
показал чистый горизонт и без неё.

**Проверки.** Шейдеры 74/74; Debug + Release + Release_Editor собраны; `check_logging` 0.
* Процедурный режим, камера владельца `-87.28,32.17,80.98` кват `0.0493,0.1042,-0.0052,0.9933`,
  `wind-freeze=0`, sky.mode 1, environmentLighting 1, luminanceScale 2.13, солнце 28.3/35: обод
  исчез, и остаётся исчезнувшим на финальном бинаре с выключённым фейдом.
* HDRI-паритет (режим, в котором чинить было нечего и ломать было нельзя), камера владельца
  `-91.18,26.43,73.77` кват `0.0046,-0.1184,0.0005,0.9930`, `wind-freeze=0`, чистый HEAD против
  дерева, оба Release: небо (строки 0-480) и полоса горизонта (480-620) — **0.000 %** пикселей
  >1 code value, сдвиг средней яркости 0.04 %. Различия только в ближней воде.
* Пол шума измерен ТРЕМЯ парами на одном бинаре (океан переджиттеривает блеск между прогонами):
  4.91 / 9.85 / 9.43 % пикселей >1 cv, MAE 0.24-0.37, max 38-44. A/B before/after: 13.62 %,
  MAE 0.445, max 47 — чуть выше пола, того же порядка. Не look change.

### B6.1. Один пасс тумана на непрозрачную геометрию и воду — СДЕЛАНО (2026-09-09, коммит `20e242f`)
Требование владельца (2026-09-09): «тумань воду и транспаренты общим кодом».

Порядок UE (`DeferredShadingRenderer.cpp`): `RenderSingleLayerWater` (3230) рисует воду, подменяет
`SceneTextures.Depth` на SLW depth prepass, и только потом `RenderLightShaftSkyFogAndCloud()` (3262)
делает sky atmosphere → height fog. То есть **поверхность воды туманится теми же экранными пассами,
что и непрозрачная геометрия**; в `SingleLayerWaterShading.ush` / `SingleLayerWaterComposite.usf`
кода тумана нет вообще. Прозрачность (translucency) рисуется ПОСЛЕ тумана и туманит себя сама.

Наш раскол ложится на это один в один: океан пишет глубину и не блендится
(`OceanRenderable.cpp:1035`, RT0 BlendEnable по умолчанию FALSE) = их SingleLayerWater; стекло
(`TransparentStaticMesh.cpp:295`, `DepthWriteMask ZERO` + альфа-бленд) и частицы = их translucency.

Состав:
* Порядок отрисовки: океан → пасс тумана → стекло → частицы. Сейчас океан и стекло в одном ведре
  `TransparentComplex` (`Pass_Transparent`, `SceneRenderer_Geometry.cpp:409`); нужен предикат
  «прозрачный, но пишет глубину» на `RenderableObjectBase` и разделение ведра на две выборки.
* `Transparent_Fog`: полноэкранный растровый пасс, блендинг `ONE / SRC_ALPHA`, выход
  `(inscatter, transmittance)` — ровно `TStaticBlendState<CW_RGB, BO_Add, BF_One, BF_SourceAlpha>`
  из UE `RenderViewFog`. Глубина как SRV: `depth` DEPTH_WRITE → PIXEL_SHADER_RESOURCE и обратно,
  два новых Use-точки в билдере `Main_Transparent`.
* Из `compose_cs.hlsl` аналитический туман + композит объёма уходят в общий код; туда же переезжают
  debug-виды 1..5. Из обоих океанских шейдеров блок тумана уходит целиком, остаётся только гейт
  «фог включён → собственный horizon fade не работает» (правило P7).
* `sceneOpaque` (источник рефракции) становится НЕзатуманенным — это дефолт UE:
  `r.Water.SingleLayer.UnderwaterFogWhenCameraIsAboveWater` = false, и их же комментарий говорит, что
  туманить фон за водой при камере над водой «causes artifacts when looking at the water surface
  from a distance». `FogAnalyticExclude` для рефракции при этом уходит — механики разные, складывать
  нельзя.
* Гейт по §0.6: правка пассов/барьеров/ресурсов → полный набор, GBV guarded (Legacy + VSM),
  `--scene-stress`, три конфига, log-stress.

### B6.2. Горизонт: линия и вертикальные штрихи — СДЕЛАНО (2026-09-09), не закоммичено
Владелец: «я вижу что на небо ложится какая-то туманная линия», «голубая херня что тянется вниз это
AP такое дает». **AP оправдан замером**, глубина поверх неба не пишется.

Замер на `wind_test`, процедурное небо, камера `-88.55,22.79,-51.37` rot `0.0395,0.9219,-0.0975,0.3730`:
AP вкл/выкл над этой областью — `max|d| = 1` код, `mean 0.000` (в compose ветка AP стоит под
`z > kEps`, а там ноль); световые шахты вкл/выкл — амплитуда штрихов 7.247 → 7.247; туман вкл/выкл —
`(120,131,162) → (186,168,139)`, то есть туман её КРАСИЛ, но не создавал.

Причина — **два независимых клампа**, каждый схлопывал всю нижнюю полусферу в ОДНУ строку: по высоте
не менялось ничего, менялись только 192 азимутальных текселя LUT — отсюда плоское плато с
вертикальными штрихами и бритвенная граница там, где начинался кламп.
1. `sky_view_mapping.hlsli` зажимал UV на строке горизонта;
2. `fog_common.hlsli` зажимал `viewRay.y` в `+1e-4` — **держащий кламп**: зеркалирование неба само по
   себе не меняло область ни на единицу (7.247 → 7.247), пока он стоял.

Правка: `SkyViewDirToUvHorizonClamped` → `SkyViewDirToUvNoPlanet`, зеркало через **горизонт** (не
через уровень глаз: между ними 0.15° на 22 м и 0.62° на 377 м, и там лежат самые сжатые строки LUT;
зеркало по уровню глаз не достаёт до последних 3 % и возвращает прежнюю ступеньку 4.3/18.3 уровня).
`FogSkyAlongView` берёт луч как есть, с гардом на нулевую длину. Строки 444→700:
`118,133,163 → 117,131,163` (плоско) стало `124,139,168 → 50,100,165`.

Сознательное отличие от UE: их нижние строки LUT честные — луч упирается в планету
(`SkyAtmosphere.usf:451-470` ограничивают `tMax` нижней сферой) — и на высоте 22 м честно = **чёрный**
(замерено: `23,34,53` под горизонтом → `0,1,1` ниже). UE этого не видит, потому что у них там
ландшафт; у нас на уровне без океана это половина кадра.

Заодно приведено к UE:
* **Небо туманится.** `HeightFogPixelShader.usf` читает буфер глубины, и единственное, что щадит
  нерисованный пиксель, — `bOnlyOnRenderedOpaque`, инициализированный `false` в
  `SceneRendering.cpp:909` и включаемый только scene capture'ом (`SceneCaptureRendering.cpp:1397`).
  У аналитической ветки compose убран гейт `z > kEps`; пиксель неба шейдится на `kSkyFogDistance`
  (100 км), укорачиваемых по лучу так, чтобы перепад высоты не выходил за собственный кламп UE `-127`.
* **Порядок композиции**: AP первым, туман ПОВЕРХ него (`SkyAtmosphereCommon.ush:148-151`).
* `sky_lut_aerial_cs.hlsl`: убран early-out `groundHit <= startKm`, которого у UE нет.

Отладка: `--set=fog.debugView:6` — маска геометрии, `:7` — `log2(z)`. Оба **строго серые**, с
калибровочной полосой известного градиента в верхних 8 строках: дисплейная цепочка нелинейна
(png 0.5 = записанные 0.154), а цветной зонд ещё и мешает каналы — числа с него врут.

Осталось: комментарий у `HeightFogMinTransmittance` в `height_fog.hlsli` обосновывает «отпускание»
потолка тем, что «НЕБО НИКОГДА НЕ ТУМАНИТСЯ» — это перестало быть правдой; у UE потолок жёсткий
(`max(saturate(exp2(-integral)), MinFogOpacity)`), вернуть их форму и посмотреть.

### B6.3. Тинт солнечного диска: форма ручки — СДЕЛАНО (2026-09-10), не закоммичено
Владелец: «а что делать с тем что блум высветляет тинт солнечного диска?»

**Блум оказался не причиной, а усилителем.** Замер на `wind_test` (солнце 7.8°, камера
`-101.79,23.65,42.56` rot `0.0234,0.3121,-0.0077,0.9497`, `--dlss=off`, `exposure.autoExposure:0`),
кольца вокруг диска, значения png:

| арм | ядро R−B | ядро sat | клип (≥253 из 69 px) |
|---|---|---|---|
| как в уровне | −4.8 | 0.023 | 0 |
| `bloom.enabled:0` | +2.1 | **0.011** | 0 |
| `lightShafts.enabled:0` | +2.2 | 0.011 | 0 |
| оба выключены | +2.2 | 0.011 | 0 |
| нейтральный `kernelTint` | +0.7 | 0.003 | 10 |

Диск был белым ДО блума (sat 0.011). Блум добавлял −6.9 к R−B, потому что авторский
`convKernelTint` уровня синий `[0.28, 0.32, 1.00]`; нейтральное ядро не помогает — оно ярче и
пережигает 672/2372 px ореола в чистый 255. Шахты дают у диска +0.4 напрямую и +6.9 через блум
(их bloom попадает точно в солнце и умножается на `intensity × kBloomMaxMips` = 12).

Причина белизны — **форма `kSunDiscTint`**. Было `lerp(1, tr, 0.35)`: под каждым каналом остаётся
пол в 0.65 белого, поэтому чем краснее настоящее солнце, тем СИЛЬНЕЕ доминирует пол. Транскрипция
`SkyTransmittanceTowardSun` на параметрах уровня (rayleigh ×0.70):

| высота | физический tr (UE) | что показывал `lerp` |
|---|---|---|
| 7.8° | 1.00 : 0.63 : 0.31 | 1.00 : 0.90 : 0.82 |
| 2° | 1.00 : 0.34 : 0.05 | 1.00 : 0.88 : 0.83 |
| 0° | 1.00 : 0.16 : 0.00 | 1.00 : 0.93 : 0.91 |

То есть на закате диск БЕЛЕЛ. Заменено на экспоненту по нормированному на пик трансмиттансу:
`pow(tr / max(tr), k)`, `k = 0.25`. Только оттенок — величину всё равно стирает кламп по пику
(диск приходит ~7e7 при потолке 65504, `disk * 65504/diskPeak` перенормирует). `k = 1` — точно UE
(`SkyAtmosphereCommon.ush:271`), `k = 0` — белый диск.

Замер A/B одним бинарём, старый шейдер через `git checkout`:

| высота солнца | ядро R−B стар → нов | ядро sat | ореол R−B |
|---|---|---|---|
| 7.8° (уровень) | −4.0 → −3.8 | 0.017 → 0.017 | −10.4 → −9.9 |
| 1° | −1.5 → **+17.7** | 0.019 → **0.072** | +33.1 → **+95.1** |

Принятый владельцем вид при высоком солнце не поменялся; на закате диск перестал быть
бело-сиреневым пятном в оранжевом небе. Белая клякса блума ушла сама — у неё пропал белый источник.

**Следствие: чем возвращать анаморф и госты после потемневшего диска.** Владелец: «а что делать
если я хочу от пригашенного анаморфик, госты и все такое, понижать трешхолды?». Замер там же,
солнце 1°, `bloom.anamorphicIntensity:0.5` (в уровне он 0.0, то есть анаморф просто выключен).
Полоса меряется как ЛОКАЛЬНЫЙ ПОДЪЁМ над тем же диапазоном x на 70 px выше: абсолютное среднее по
бэнду даёт 2 кода там, где глаз видит появление и исчезновение полосы — первый замер именно так и
соврал, вердикт спасли глаза.

| арм | подъём полосы | ядро, яркость | ядро B/max | небо вдали | океан | пережжено |
|---|---|---|---|---|---|---|
| старый диск | 13.00 | 246.2 | 1.00 | 104.50 | 28.86 | 0 |
| новый диск | 9.57 | 238.5 | 0.92 | 104.50 | 28.85 | 0 |
| `anamorphicThreshold` 3.5→1.5 | **11.14** | 238.6 | **0.92** | 104.49 | 28.87 | 0 |
| `kernelTint` → 1,1,1 | 10.88 | 242.9 | **0.91** | 104.50 | 28.86 | 0 |
| `ghostThreshold` 9→4 | 9.57 | 238.6 | 0.92 | 104.51 | 28.86 | 0 |
| `bloom.threshold` 3.5→1.5 | 15.73 | 248.8 | **0.99** | 104.50 | 28.86 | 0 |
| все три | 17.12 | 248.9 | 0.99 | 104.49 | 28.88 | 0 |
| `preFilterMult:2` | 5.35 | 255.0 | 1.00 | 199.45 | 101.64 | **576** |

* `bloom.anamorphicThreshold` — правильная ручка: её порог сравнивается с `max(r,g,b)`, диск не
  трогает вовсе.
* `bloom.threshold` даёт больше всех, но она же ОТМЕНЯЕТ B6.3: основной блум кладёт бледное свечение
  неба обратно поверх солнца, B/max 0.92 → 0.99.
* `bloom.ghostThreshold` — ровно ноль, и не из-за порога: цепочка гостов масштабируется относительно
  ЦЕНТРА ЭКРАНА, а солнце в 100 px от него (1246,624 при центре 1280,720), так что вся цепочка
  складывается сама на себя поверх солнца. Госты видны только с солнцем не в центре.
* `preFilterMult` — НЕ усилитель: он ЗАМЕНЯЕТ порог (`lit = color`), блумит весь кадр, локальный
  контраст умирает.
* Настоящая причина слабых бликов — не порог, а ТИНТЫ: `convAnamorphicTint` [0.38, 0.44, 1.00] и
  `convKernelTint` [0.28, 0.32, 1.00] синие, а диск теперь красный. У `convAnamorphicTint` нет
  `--set`, поэтому проверен только kernel.

**ПОПРАВКА к таблице выше: она мерила не ту цепочку.** Владелец: «у меня в кернеле анаморфик полоса
есть, я спецом доп анаморфики не юзаю». Стрик приходит из САМОГО ядра свёртки
(`convKernel = DefaultBloomKernelAnamorphic.dds`), а стадия `convAnamorphicIntensity` в уровне стоит
на 0.0. Значит `anamorphicThreshold` к его картинке отношения не имеет вовсе, а единственный порог
в его пути — `bloom.threshold`. Перемер на авторской цепочке (солнце 1°, доп. анаморфик выключен),
подъём полосы в 60..320 px от солнца:

| арм | подъём полосы | ядро, яркость | ядро B/max |
|---|---|---|---|
| старый диск | 12.38 | 246.2 | 1.00 |
| новый диск | 9.11 | 238.5 | 0.92 |
| `bloom.threshold` 3.5→1.5 | **15.32** | 248.8 | **0.99** |
| `convKernelTint` → 1,1,1 | 10.43 | 242.9 | **0.91** |
| `bloom.intensity` 1.5→2.5 | 9.71 | 240.7 | 0.95 |

* Свободного обеда нет: яркость блика у солнца и тинт диска — одна и та же величина, «сколько
  бледного света блум кладёт поверх солнца».
* `convKernelTint` — единственная ручка, которая возвращает часть стрика (+15 %) и НЕ отбеливает
  диск. Синее ядро × красный диск ≈ ноль; это физически честно (анаморфный блик синий потому, что
  просветление отражает синее, и на красном закате он и должен быть тусклым).
* `bloom.intensity` почти инертна: +67 % усиления дали +7 % подъёма — область вокруг солнца уже в
  плече тон-кривой.
* Прямая ручка размена — сам `kSunDiscTint`. Сейчас это константа в `skybox.hlsl`; кандидат на вывод
  в объект Sky Atmosphere как `sunDiscTint` рядом с `mieAnisotropy`.

**ВЕРДИКТ: делать ничего не нужно, совет «покрасить ядро в белый» отозван.** Владелец: «но блять
физично анаморфик синий» — и он прав, синева анаморфного блика физична (просветление отражает синее),
так что нейтральное ядро сломало бы ровно тот вид, ради которого оно синее. Замер на СОБСТВЕННОМ
солнце уровня (7.8°, без `--set=sun.elevation`), старый диск против нового:

| арм | полоса вблизи | полоса вдали | ядро RGB |
|---|---|---|---|
| старый диск | 13.87 | 10.91 | 250.4 249.9 254.3 |
| новый диск | 13.74 | 10.88 | 250.3 249.8 254.1 |

Разница −0.9 % по полосе, ядро совпадает до 0.2 кода, по всему кадру `|d| > 2` у **0.050 %**
пикселей. То есть на авторском солнце правка B6.3 не стоит ничего, синий стрик цел.

Проседание, которое я мерил, существует только на солнце ~1°, и там оно ФИЗИЧНО: трансмиттанс на 1°
= (0.301, 0.077, 0.006), синего 2 % от красного. Синее покрытие отражает синее, которого в источнике
нет — настоящая анаморфная оптика на таком закате даёт тусклый ТЁПЛЫЙ стрик. Синие стрики берутся от
источников с синим в спектре (фары, практикалы, верхнее небо), а не от красного диска. Восстанавливать
там синеву — значит врать.

### B6.4. Паритет с UE по окраске низким солнцем — ПРОВЕРЕНО (2026-09-10)
Вопрос владельца: «а у нас солнце на низком угле корректно окрашивает всё? у эпиков также?»

**Кто у нас получает трансмиттанс** (всё через `GetEffectiveColor()`): непрозрачное освещение
(`SceneRenderer_Lighting.cpp:288`), плоский ambient-фолбэк (`:289`), объёмный туман (`:1025` и
`SceneRenderer_Geometry.cpp:582`), SSR и RT-отражения (`SceneRenderer_Reflections.cpp:273,511`),
океан (`OceanRenderable.cpp:1697`), view-константы (`SceneRenderInternal.h:327`). Собственные LUT
неба берут `GetOuterSpaceIlluminance()` (`SceneRenderer_Graph.cpp:77`) — иначе экстинкция учлась бы
дважды. Обходных путей нет: грепа по `GetColor()` в рендере не даёт ни одного потребителя.

**Кто у UE** (`DirectionalLightComponent.cpp:582-627` + рендер): `LightGridInjection.cpp:1064`,
`SceneRendering.cpp:1477`, `Lumen/LumenSceneDirectLighting.cpp:2027`,
`RayTracing/RayTracingLighting.cpp:396`, `LocalFogVolumeRendering.cpp:615`,
`ShadowRendering.cpp:2501`, `VolumetricFog.cpp:1766`, `MobileBasePassRendering.cpp:421`. API той же
формы: `GetOuterSpaceIlluminance() == GetColor()`, `GetSunIlluminanceOnGroundPostTransmittance() ==
outer * transmittance`. Наша пара — 1:1.

**Чего у нас нет:**
* `bPerPixelAtmosphereTransmittance` — трансмиттанс попиксельно по непрозрачной геометрии вместо
  одного глобального значения. **Дефолт 0** (`DirectionalLightComponent.cpp:1056`), то есть UE из
  коробки ведут себя ровно как мы. Разница видна только на большом перепаде высот.
* `AtmosphereSunDiskColorScale` — отдельный авторский цвет-множитель на диск. У нас вместо него
  `kSunDiscTint` (экспонента по оттенку), другая форма.

**Замер**, один кадр на трёх высотах, экспозиция зафиксирована, DLSS off; песок разделён по яркости
внутри одного бокса (верхние 15 % = освещён солнцем, нижние 15 % = в тени, то есть светит только небо):

| высота | предсказанный оттенок солнца | песок на солнце | песок в тени | океан | небо |
|---|---|---|---|---|---|
| 20° | 1.00 : 0.82 : 0.61 | 1.00 : 0.96 : 0.92 | 0.48 : **1.00** : 0.63 | 0.40 : 0.79 : 1.00 | 0.72 : 0.87 : 1.00 |
| 6° | 1.00 : 0.57 : 0.23 | 1.00 : 0.83 : 0.71 | 0.57 : **1.00** : 0.39 | 0.58 : 0.94 : 1.00 | 0.78 : 0.86 : 1.00 |
| 1° | 1.00 : 0.26 : 0.02 | 1.00 : 0.58 : 0.43 | **1.00** : 0.88 : 0.23 | 1.00 : 0.94 : 0.70 | 1.00 : 0.86 : 0.96 |

Главная проверка — **солнечный и теневой песок НЕ движутся вместе**: на 20° и 6° тень остаётся
холодной (её максимальный канал — зелёный), пока освещённый песок уже тёплый, и переворачивается
только на 1°, когда само небо становится оранжевым. Это правильное разделение (тени светит НЕБО, не
солнце) и одновременно доказательство, что ambient берётся из кубмапы неба, а не из подкрашенного
солнцем фолбэка.

Освещённая поверхность менее насыщена, чем сырой трансмиттанс (1.00:0.58:0.43 против 1.00:0.26:0.02
на 1°) — это добавка неба сверху плюс тон-кривая, а не потерянный множитель; направление и порядок
монотонны на всех трёх высотах.

### Ревью части B и правки по нему — СДЕЛАНО (2026-09-11)
Ревью на HEAD `d37ebd5`: два агента сверили транскрипцию функция за функцией с `SkyAtmosphere.usf`,
`SkyAtmosphereCommon.ush`, `SkyAtmosphereRendering.cpp`, `SkyAtmosphereComponent.cpp`, `SkyAtmosphereCommonData.cpp`
(LUT-цепочка: маппинги, медиум, MS два луча + 5 членов, SkyView, distant с численно сверенной таблицей 64 направлений,
CPU-трансмиттанс солнца 500 м / 15 сэмплов; apply: диск, AP-объём, композиция AP-под-туманом, GGX-фильтр зондов,
нижняя полусфера). **Багов нет.** Стоимость пассов за кадр @1440p: SkyView 0.011 / Skybox 0.010 / SkyAerial 0.009 мс;
Environment 0.072 и LUT 0.016 только при перестройке. Гейты до правок: 76/76, `check_logging` 0, GBV churn 20 CLEAN
(194.7 с, небесные LUT и окружение отработали в кадре 0), `--scene-stress-sky=64` + `lutValidate` PASS
(T 0.0005145 / MS 0.0000411). Открытые дельты от UE и их правки, все применены:

1. **Два luminance-фактора, как у UE.** `luminanceScale` шёл во ВСЕ LUT (SkyView, AP, distant, захват) — это
   `SkyAndAerialPerspectiveLuminanceFactor`, а не «sky only», как утверждали комментарий и подсказка (в `wind_test`
   стоит 2.6, т.е. дымка AP на геометрии и ambient от неба были в 2.6 раза ярче физики относительно солнца).
   Добавлен `skyLuminanceFactor` (UE `SkyLuminanceFactor`, дефолт 1): `SkyExposure.y` умножает пиксель неба при
   отрисовке (`skybox.hlsl`, `usf:919-922`), захват (`captureMip.y`, их захват рендерит sky-pass) и distant LUT
   (`usf:1397`); диск и AP не трогает. Семантика и картинка `luminanceScale` не изменились, тексты честные;
   Inspector → Sky Atmosphere → «Sky-only Luminance», `--set=sky.skyLuminanceFactor`. Перенос 2.6 в sky-only —
   решение владельца (это меняет ambient).
2. **Угловой размер солнца — свойство солнца.** Был проектной graphics-настройкой `sunAngularSize` 0.01 рад радиуса
   (вкладки Reflections и Sky, дубль), т.е. в 2.1 раза шире реального солнца (4.4× по площади). Теперь
   `directionalLight.lightSourceAngle` уровня в градусах (UE `LightSourceAngle` 0.5357, `DirectionalLightComponent.cpp:1024`),
   `DirectionalLight::GetSunHalfApexRadians()` кормит и диск (`skybox.hlsl`), и пол спекуляра (`lighting_cb`), как у UE;
   Inspector → Directional Light → «Sun Disc»; `--set=sun.angularSize` (радианы полу-апекса) переехал на солнце;
   `GraphicsControl::SunAngularSize` и ключ `reflections.sunAngularSize` удалены (старый файл настроек читается,
   ключ игнорируется). Замер: ядро диска на закатной камере 52×58 → 13×14 px (2345 → 130 px при lum ≥ 200).
3. **Старт AP — своя ручка.** Был `heightFog.volumetricDistance` (300 м по умолчанию; выключенный туман его не
   гейтил) в трёх местах. Теперь `skyAtmosphere.aerialStartDepthMetres` (UE `AerialPerspectiveStartDepth` 0.1 км),
   compose / `BuildAerial` / debug-view читают одно значение; Inspector «Aerial Start Depth (m)», `--set=sky.aerialStartDepth`.
4. **AP на воде.** `Main_TransparentFog` (`fog_apply.hlsl`) сэмплит `SkyAerialVolume` (t4) по тому же unjittered
   clip, что compose, и складывает как compose: AP первым, туман поверх, `dst·(overA·ap.a) + (overRgb + ap.rgb·overA)`
   в форме ONE/SRC_ALPHA; `FogApplyConstants::aerialParams` — те же значения, что у compose; объём объявляется
   PIXEL_SHADER_RESOURCE на кадрах, где построен, иначе dummy в t4 (шейдер гейтит по `aerialParams.x`). Стекло и
   частицы — по-прежнему без AP (у UE translucency делает это per-material; отдельная работа).
5. **Пол тумана в форме UE.** `HeightFogMinTransmittance` = `1 − maxOpacity`, жёстко
   (`max(saturate(exp2(-integral)), MinFogOpacity)`); «отпускание» на два стопа держалось на «небо никогда не
   туманится», что перестало быть правдой с B6.2. Замер на камере горизонта `-87.28,32.17,80.98`: строки 590..636
   монотонны, max step 0.0483 → 0.0465, ступеньки нет; небо 0.3402 → 0.3395, вода 0.1444 → 0.1377 (−4.6 %, это AP
   на воде + пол); закатная камера: небо 0.2840 → 0.2823, вода 0.1401 → 0.1349.
6. **Подсказка Inspector про пустой `skybox.texture` исправлена**: загрузчик создаёт Skybox только с текстурой
   (`JsonLevel.cpp`), без неё небо чёрное — так теперь и написано.
7. **Debug + свёрнутое окно = ассерт.** `SubmitEditorDockSpace` строил док из `viewport->WorkSize` 0×0 →
   `IM_ASSERT` в `DockBuilderSetNodeSize`, модалка у владельца, гейт стоял 14 мин. Гард: при нулевой рабочей области
   докспейс не сабмитится вовсе (иначе `DockSpaceOverViewport` создал бы узел сам и кастомный лейаут не построился бы).

Не трогал (мелочь, зафиксировано): кламп `min(L, 64000)` на SkyView/AP после pre-exposure (у UE нет, кусается только
при ручной EV), нет ручек `AerialPespectiveViewDistanceScale` / `SunDiskColorScale` (дефолт-эквивалент, `kSunDiscTint`
константой), мёртвые ключи `wind_test.json` (`skyBackScatter`, `ambientColor`, `ambientTintedBySun`), строка
`[ibl] physical scale` печатается и на процедурном уровне, где калибровка обходится (`Skybox::GetExposure()` = 1).

**Гейты после правок (2026-09-11):** три конфига; `check_shaders` 76/76; `check_logging` 0; GBV churn 20 (Debug, VSM,
обычное окно) **CLEAN, 177.0 с**; `--scene-stress-sky=64` + `lutValidate` PASS (T 0.0005145 / MS 0.0000411), exit 0.
A/B одним бинарём до/после на двух камерах: `sky_sunset` / `sky_horizon` против `*_after` — сверху. Не закоммичено.

**Дополнение (2026-09-11, вопросы владельца после ревью).** Замер на закате с зафиксированной экспозицией (база: обе
luminance-ручки = 1) показал, что «Sky Luminance Scale» и «Sky-only» были ОДНОЙ ручкой: небо ×1.87 / ×1.87, песок
×1.11 / ×1.11, кроны ×1.37 / ×1.37, разница только в AP на дальней воде (×1.371 / ×1.364) — потому что у UE
`SkyLuminanceFactor` уходит и в захват скайлайта. Сделано три правки:
1. **`skyLuminanceFactor` = только КАРТИНКА неба** (пиксель неба в `skybox.hlsl`); захват (`captureMip.y`) и distant LUT
   его больше не получают, ключ перестройки окружения не включает (`view.exposure[1] = 1` в `BuildEnvironment`/`BuildDistant`).
   Сознательное отклонение от UE, задокументировано в `SkyAtmosphereSettings.h` и подсказке. Перемер: Sky-only 2 → небо
   ×1.87, песок ×0.998, кроны ×0.998, ближняя вода ×0.999.
2. **`aerialViewDistanceScale`** (UE `AerialPespectiveViewDistanceScale`, `usf:601-606`: множитель оптической глубины
   на сэмпл AP-объёма, `AerialStart.y`): дефолт 1 = Земля, невидимо на километре (AP выкл/вкл — 0.8 % на дальней воде,
   0 на острове); при 10 — дальняя вода ×1.059, остров ×1.008. Inspector «Aerial View Distance Scale»,
   `--set=sky.aerialViewDistanceScale`.
3. **Галка «Distant sky light (fog)» удалена**: её единственный потребитель — небесный член объёмного тумана, и она
   переключала два источника ОДНОГО света (distant LUT на 6 км против зонда того же неба на уровне моря): 0.5 % пикселей,
   ±0.0002 по областям. Теперь distant LUT всегда активен в процедурном режиме (`SkyAtmosphere.cpp`); ключ `distantSkyLight`
   в уровнях игнорируется, `--set=sky.distantSkyLight` пишет UNKNOWN SETTING.

**Полоса под горизонтом при Sky-only ≠ 1 — ПРИЧИНА НАЙДЕНА (2026-09-11): локальное экспонирование P3B.** Симптом:
при Sky-only 2 первые ~25 строк воды под горизонтом темнеют (×0.877 у горизонта → ×1.000 к строке 688 без тумана), при
0.5 — светлеют (×1.12); гладкий градиент, нейтральный по каналам, одинаковый по ширине кадра. Исключены: блум, шахты, FXAA,
туман, техника SSR, сам ocean-шейдер (все члены положительны по небу). Ключ — второй знак: в пробнике с ЧЁРНОЙ водой
строки неба над горизонтом стали ярче (218/225 → 232/248), т.е. взаимное ОТРИЦАТЕЛЬНОЕ влияние через кромку на ~25 px —
сигнатура оператора локального контраста. База `exposure_baselum_cs` — размытая лог-яркость 256×144 (10×10 px на тексель
при 1440p) плюс широкий бокс; яркое небо поднимает базу под горизонтом, `LocalExposureMultiplier` сжимает эти пиксели воды
вниз (уровень: localHighlightContrast 0.7, localShadowContrast 0.9). `local_exposure.hlsli:17-20` сам предупреждает: «a plain
blur bleeds across a high-contrast edge, so a bright sky…», UE гасят это билатеральной сеткой (`BlurredLuminanceBlend`), у нас
она «reserved». Доказательство: `--set=exposure.localHighlightContrast:1 localShadowContrast:1 localDetailStrength:1`
(оператор нейтрален, блок пропускается) — Sky-only 2 даёт ×1.000 на КАЖДОЙ строке воды, небо ×1.27 как и было. Это не баг
неба и не океана: гало blur-based локального тонмаппинга на самой контрастной кромке кадра; видно оно на любом изменении
яркости неба (при `luminanceScale` его маскировало одновременное осветление воды через IBL). Лечение по UE — билатеральная
база (отдельная работа); паллиатив — localHighlightContrast ближе к 1 в уровне.
**ЗАКРЫТО 2026-09-12: билатеральная база транскрибирована** (`exposure_bilateral_cs.hlsl`, ручка
`cameraExposure.localBlurredBlend`, UE-дефолт 0.6; замеры и свойства сетки — в photographic plan, P3B). На этой камере при
blend 0.6 глубина полосы 0.956 → 0.974 (вдвое мельче, 60 % базы по UE — всё ещё размытие), при 0 — 0.9997 (нет), но
чистая сетка сдвигает всю воду ×0.989 (общие бины с небом в тайле) и превращает оператор почти в глобальную кривую.

**Темпоральный резолв отражения океана (по просьбе владельца).** `oceanReflection` шёл в forward-пасс сырым. Добавлена
история `oceanReflectionHistory` (размер и формат отражения океана, per-frame set, rest PIXEL), точка `oceanTemporal`
в `Main_Transparent` (raw NPS, история UAV, прошлая история NPS, velocity NPS) и диспатч того же `ssr_temporal_cs` сразу
после `RecordOceanReflection`, с теми же ручками `ssr.temporal*`. Репроекция — по ТОЧКЕ ПЛОСКОСТИ ВОДЫ (`planeReproject`,
`planeParams`, `invViewProj`, `prevViewProj` в `SsrTemporalConstants`): резолв бежит до отрисовки воды, и `gbVelocity`
в этот момент несёт движение непрозрачного фона (неба), не плоскости. Океан биндит историю вместо сырого буфера по
`render::g_oceanReflectionTemporal`, который ставит serial-билдер. Стоимость **0.012 мс** (океан на весь кадр, 1280×720).
Первый замер кипения (три кадра подряд под DLSS quality, `--wind-freeze` фризит и океанские часы) НЕ показал разницы
(0.0035/0.0025 → 0.0031/0.0034 на дальней воде): в `ssr_temporal_cs.hlsl` не было `#pragma pack_matrix(row_major)`, матрицы
репроекции читались транспонированными, prevUv улетал за экран и история сеялась каждый кадр. Прагма добавлена.
Перемер: **контроль без джиттера** (`--dlss=off`, статичная камера) — резолв вкл/выкл совпадают на 0.00 % пикселей
(история при неподвижной камере обязана равняться сырому буферу — репроекция зарегистрирована точно). Под DLSS quality на
установившихся кадрах средние off/on совпадают (открытое море 0.3643 / 0.3651: там SSR-луч уходит в небо и отсекается,
отражение — куб, резолву нечего усреднять; замеченный ранее «+6 %» был переходным процессом первого кадра арма off).
Межкадровая разница f1→f2: открытое море 0.0060 → 0.0038, ближняя вода 0.0055 → 0.0025, **вода у берега под островом
(где SSR отражает пальмы) 0.0030 → 0.0017**; оговорка — off-прогон был чуть менее сошедшимся (небо 0.0003 vs 0.0002).
Окончательный вердикт — глазами. GBV не гонялся (по указанию).

**Два SSR-пасса (геометрия и океан) — оверхед?** Разные отражатели: deferred-SSR трассирует G-buffer, океан — плоскость
воды, которой в G-buffer нет (forward после compose); у UE SingleLayerWater тоже трассирует своё. Цена второго:
`Pass_OceanReflection` 0.029 мс на камере уровня, 0.064 мс с океаном на весь кадр (1280×720), плюс резолв 0.012; для
сравнения deferred-цепочка 0.074 (SSR) / 0.80 (RTTrace) + 0.03 + 0.03. Итого ≈ 2–3 % кадра в 2.8 мс.
Правка по просьбе владельца (2026-09-11): в `ocean_reflection_cs.hlsl` ранний выход для пикселей, где точка плоскости
воды скрыта непрозрачной геометрией ВЫШЕ уровня воды (`DepthToViewZ(opaque) < Pv.z && opaqueWS.y > waterHeight + 0.5`):
остров, песок, стволы не трассируются; дно (ниже уровня) и небо (far plane) трассируются как раньше. Паритет одним
состоянием на закатной камере: 0.5 % пикселей >8, все на кронах пальм (шум прогона), береговая линия и вода без изменений;
`Pass_OceanReflection` на камере уровня 0.029 → 0.029 мс (пасс и так внутри шума профилировщика). Замечание про общий
SSR-пасс: у UE один шейдер (`SSRTReflections.usf`) и два вызова — непрозрачный и водяной с `ShouldReflectOnlyWater` по
собственному G-buffer-слою воды и тайловой классификацией (`SingleLayerWaterRendering.cpp:735,1551-1567`); у нас плоскость
аналитическая, так что единый пасс «трассировать видимую поверхность» возможен и убрал бы `ocean_reflection_cs`, его таргеты
и отдельный резолв (оценка — день; blur должен брать roughness воды, темпоральная репроекция — попиксельный выбор).

### Гейты части B
Паритет `sky.mode 0` = сегодняшняя картинка (0.03 %); mode 1 — глаза; GBV; три конфига.

---

## C. Объёмные облака (UE VolumetricCloud, шумы Schneider)

### C1. Шумы и слой — [день]
Compute при загрузке: Perlin-Worley 128³ RGBA8 (base), Worley 32³ (detail), curl 128² (3 канала),
weather 512² (coverage, type, wetness) из seed'а; `cloud.bottomKm 1.5`, `cloud.topKm 4.0`,
`cloud.coverage`, `cloud.density`, `cloud.windKmH` (сдвиг по `frame.wind` направление). Debug-view:
weather map на небе.

### C2. Марш и реконструкция — [2 дня]
`shaders/cloud_trace_cs.hlsl`: half-res, луч на пиксель, вход/выход из слоя (сферические оболочки,
`RaySphereIntersectNearest :140`), сэмплов по дистанции 2..768/15 км (`:45-66`), пропуск пустоты по
conservative density (`:816-870`), Beer-Lambert + «powder», две лопасти HG (`:329-335`), октавы
multi-scattering (`SetupParticipatingMediaContext :376`, факторы 0.5/0.5 как у UE по умолчанию),
тень к солнцу вторичным маршем 6 сэмплов (их `Shadow.ViewRaySampleMaxCount 80` — ДОРОГО, начать с 6
и мерить), ambient — distant sky light (B5) × sky AO-фактор от высоты в слое; аэроперспектива из
B3 на дистанцию облака. Выход: `(luminance pre-exposed, transmittance, depth)`. Temporal: history по
`prevViewProjNoJitter` + вес 0.9 (форма `ssr_temporal_cs`), чекерборд 2×2 по кадрам (их
`VolumetricRenderTarget`: полное покрытие за 4 кадра). Композ перед `Main_Skybox`-цветом: небо =
`sky·T_cloud + L_cloud`, геометрия дальше облаков — не бывает (слой на 1.5 км+; исключение — камера
выше слоя: тогда порядок по глубине).
**Критерий приёмки:** глаза (форма, освещение по солнцу, серебряная кромка, закат снизу); стоимость
≤ 1.0 мс на половине 1080p; статичный кадр без мерцания (on/on ≤ пол + чекерборд-сходимость за 4
кадра).

### C3. Карта теней облаков → освещение + туман — [день]
`r.VolumetricCloud.ShadowMap`: орто-вид от солнца над камерой 512², марш плотности по столбу
(`CloudShadowTraceContext :2053-2086`), snap к текселю, temporal 0.8; `lighting_cs` умножает солнце
на `GetCloudVolumetricShadow(P)` (`:1077`), `fog_scatter_cs` — то же (`VolumetricFog.usf:874-880`),
океан — то же. Это самый дешёвый по цене-на-эффект пункт всей части C: тени облаков ползут по
острову даже без самих облаков в кадре.
**Критерий приёмки:** движение тени по песку согласовано с облаком над ним (камера остров, глаза);
стоимость ≤ 0.1 мс.

### C4. Облака в отражениях и IBL — [полдня]
RT-отражения при промахе TLAS сэмплируют небо: добавить облака по лучу с `ReflectionRaySampleMaxCount
80/24` (дёшево — мало лучей уходит в небо); IBL-захват B4 включает облака (низкая частота обновления:
раз в N кадров или при смене солнца).

### Гейты части C
`cloud.enabled 0` = паритет; GBV (новые 3D-ресурсы); стоимость; глаза.

---

## D. SSGI (UE SSRT diffuse indirect)

### D1. Редукция цвета прошлого кадра — [полдня]
`SSRTPrevFrameReduction.usf`: репроекция `Deferred[prev].scene` (уже pre-exposed, есть velocity) в
half-res пирамиду (mips до 8×8) с leak-free правилом (`r.SSGI.LeakFreeReprojection`: отбраковка по
глубине). Хранится в кольце (`ssgiColorMips`).

### D2. Трассировка — [день]
`shaders/ssgi_trace_cs.hlsl`: транскрипция `SSRTDiffuseIndirect.usf` MainCS (`:234-570`): тайлы 8×8,
Q1..Q4 = 4/8/16/32 лучей × 8 (12) шагов, Hammersley16 + семя по кадру (`:413-424`), косинусное
распределение по нормали G-buffer'а, марш `CastScreenSpaceRay` (`SSRTRayCast.ush`) против furthest
HZB (`D.hzb`), попадание → цвет из D1 по мипу (конус луча), промах → небо (irradiance по направлению
луча, `SkyDistance`), `RejectUncertainRays` (`:70`); выход `(diffuse indirect, AO)` половинного
разрешения. `ssgi.quality 1..4` (дефолт 2 = 8 лучей), `ssgi.enabled`.

### D3. Денойз — [день]
По форме GTAO-цепочки (`gtao_filter_cs` bilateral по глубине/нормали, `gtao_temporal_cs` история с
clamp, `gtao_upsample_cs`), не SSD целиком: SSD — 3 пасса и harmonics, нам достаточно
bilateral + temporal + upsample. История в кольце; cut/resize → сброс (как GTAO).

### D4. Композ в освещение — [полдня]
`lighting_cs.hlsl`: diffuse indirect = `lerp(irradianceIBL·AO_gtao, ssgi.rgb, ssgi.confidence)` ×
albedo·(1−metallic)·(1−F) — их `DiffuseIndirectComposite.usf`; specular occlusion — оставить GTAO.
Debug-view SSGI (raw / denoised / confidence).
**Критерий приёмки:** стена C: тень стены на песке получает розовый отскок от стены и песка (глаза,
side-by-side), под кронами тени теплеют; паритет `ssgi.enabled:0`; стоимость ≤ 0.8 мс на половине
1080p (Q2); статичный кадр без шума (on/on ≤ пол после temporal); полёт без шлейфов.

### D5. Границы — [полдня]
Только opaque в композе освещения; вода/стекло/частицы — без SSGI (как у UE без translucency volume);
RT-отражения — без SSGI (они шейдят off-screen сами); экспозиция — вход pre-exposed, выход pre-exposed.

### Гейты части D
`--log-stress`; GBV Legacy/VSM с `ssgi.enabled:1`; три конфига; профдамп стена K=4 и остров.

---

## 4. Порядок и связи

**Правка 2026-09-05:** после A2 → **A7 light shafts** (экранные лучи — единственный способ получить god rays при
ясной погоде, см. пересмотренный критерий A2), затем A3–A6, потом D.

```
A0 → A1 → A2 → A3 → A4 → A5 → A6          [туман: сразу; дефолт ON после A2]
D1 → D2 → D3 → D4 → D5                    [SSGI: независим от A/B/C, нужен HZB (есть)]
B1 → B2 → B3 → B4 → B5                    [небо: B5 переключает A2.skyScatter на LUT]
C1 → C2 → C3 → C4                         [облака: C2 нужна B2/B5 (ambient, AP); C3 кормит A2 и lighting]
```
Жёсткие зависимости: C ← B; A2.sky и C3.ambient ← B5 (до этого — наш irradiance-куб); C3 → A2
(карта теней облаков в scatter) и → `lighting_cs`. D независим. Самое рискованное: C2 (цена и
мерцание реконструкции) и B4 (динамический IBL против запечённого — калибровка яркости, чтобы
переключение режимов не меняло экспозицию). Самое дешёвое-на-эффект: A2 (лучи) и C3 (тени облаков).

## 5. Ловушки, известные до начала
* Froxel и DLSS: размер рендера меньше вывода — сетка от размера РЕНДЕРА (как у UE от scene textures).
* Reverse-Z камеры: `ComputeDepthFromZSlice` даёт линейную глубину; device z через нашу
  `projMatrix` (не UE `ConvertToDeviceZ`).
* Exp2 против exp: наш аналитический туман — база 2 (`height_fog.hlsli`), интегратор — база e;
  ln 2 в σ, иначе паритет A1 не сойдётся на 30 %.
* Pre-exposure: froxel-текстуры хранят pre-exposed, история реэкспонируется (`:1031`); забыть — лучи
  мигают при автоэкспозиции.
* Небо не туманится аналитикой (наше правило), но ОБЪЁМНЫЙ туман на небо ложится (лучи над
  горизонтом) — иначе лучи обрываются на силуэте пальмы.
* VSM в тумане: однотап без SMRT (марш на 64×120×68 ячеек — не по бюджету).
* Cloud shadow map и CSM/VSM — разные проекции, snap к текселю обязателен (`ShadowMapSnapLength`),
  иначе тень облака дрожит при движении камеры.

---

## B6.5. Тёмный синий купол на противосолнечной стороне — ФИЗИЧЕН (проверено 2026-09-11)

Вопрос владельца: «при угле солнца −3.2 сзади выростает синий купол тёмный, так и должно быть?»

Это **земная тень** (тёмный сегмент), а розово-фиолетовая полоса над ней — **Пояс Венеры**.

**Транскрипция совпадает с UE построчно.** `sky_lut_view_cs.hlsl`:
`float planet = SkyRaySphereNearest(q, SkySunDirection.xyz, PlanetRadiusOffset * up, AtmosphereRadii.x);`
и далее множитель `(planet >= 0 ? 0 : 1)` на ОДНОКРАТНОМ рассеянии. UE `SkyAtmosphere.usf:664-697`:
`tPlanet0 = RaySphereIntersectNearest(P, Light0Dir, PlanetO + PLANET_RADIUS_OFFSET * UpVector,
Atmosphere.BottomRadiusKm); PlanetShadow0 = tPlanet0 >= 0.0f ? 0.0f : 1.0f;` — и многократное
рассеяние у них тоже ВНЕ маски, с комментарием на `:696`: «Multi-scattering is also not affected by
PlanetShadow or TransmittanceToLight because it contains diffuse light after single scattering».
Именно это и даёт тени остаточную синеву вместо чёрного. Чего у нас нет из их строк — домножение
`PlanetShadow0` на volumetric/VSM/облачное затенение; у нас в шапке LUT так и записано «no
opaque/cloud shadows».

**Замер**: камера `-91.80,5.26,34.73` rot `-0.0046,0.9498,-0.0139,-0.3125` (противосолнечная),
авто-экспозиция, DLSS off, hfov 90° → vfov 58.7°, горизонт взят ОДИН на все армы (камера не
двигается; подетекторный горизонт, дающий три разные строки, — это сломанный детектор).

| высота солнца | верх тёмной полосы над горизонтом | отношение к погружению |
|---|---|---|
| −1.0° | +1.61° | 1.6 |
| −2.0° | +3.35° | 1.7 |
| −3.2° | +6.48° | 2.0 |
| −4.5° | полосы нет (контраст 3.5 кода) | — |
| −6.0° | кадр чёрный (max lum 4.4) | — |

**Тень поднимается примерно вдвое быстрее, чем садится солнце** — это и есть наблюдаемое поведение
земной тени, и диапазон видимости (примерно от −0.5° до −4°) тоже совпадает с натурой.

Поперёк кадра верх полосы идёт от +7.40° в центре до +11.2° на ±40° по азимуту. Это НЕ дуга купола,
а проекция: линия постоянной высоты в прямолинейной проекции при hfov 90° сама выгибается вверх к
краям, `atan(tan(8°)/cos(40°)) = 10.4°`, то есть +2.4° из измеренных +3.8°. Разделить остаток от
кривизны самой тени этим кадром нельзя, и не нужно — вердикт держат пункты выше.

**Ниже −4.5° смотреть нечего**, и это тоже не баг: в модели нет ночных источников (звёзд, Луны,
собственного свечения атмосферы), а единственное рассеяние к этому моменту затенено планетой почти
до самого верха атмосферы. У UE SkyAtmosphere ровно так же; играбельная ночь — отдельный слой.
