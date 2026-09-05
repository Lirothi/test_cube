# Объёмный туман, процедурное небо, объёмные облака, SSGI — план

Написан 2026-09-05. Четыре части, каждая — своя цепочка шагов с замером, коммит за юзером после
каждого шага. Порядок: **A** объёмный туман (froxel, лучи света сквозь кроны) → **D** SSGI (отскок от
песка в тенях, независим от A) → **B** процедурное небо (Hillaire/UE SkyAtmosphere, время суток) →
**C** объёмные облака (UE VolumetricCloud, нужны LUT неба). Связи между частями — §4.

## 0. Правила работы (обязательно к прочтению)

1. **Читать UE-дроп первым, транскрибировать, не выводить.** Все четыре фичи у Epic есть в
   `D:/Programming/ue_strip/Shaders/Private/` и `Source/Runtime/Renderer/Private/`; файлы и строки
   ниже. Каждая формула, взятая из UE, помечается `file:line`; каждая ДЕЛЬТА (единицы, reverse-Z,
   наш IBL вместо их skylight) записывается в разделе шага явно. Урок `atmosphere.hlsli`: первая
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
* **Туман:** `shaders/atmosphere.hlsli` — аналитический exponential height fog, транскрипция
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
`--set=atmosphere.*`, для солнца/неба в объёме — `sunVolScatter`/`skyVolScatter`). Лог: `volumetric fog: on= history= grid=`
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
* GBV с объёмом (`--set=atmosphere.volumetric:1 atmosphere.enabled:1 atmosphere.density:0.004`, после починки ребра
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
* **Размер ячейки** `fog.gridPixels` 8 / 16 / 32 (`render::g_fogGridPixels`, `graphics_settings.json`
  performance/fogGridPixels, комбо в Render-табе, `--set=fog.gridPixels`): смена = пересоздание deferred-кольца на
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

### A5. Туман на прозрачном: океан, стекло, частицы — [полдня]
**Зависит от:** A1. **Эффект:** вода и стекло в тумане согласны с сушей (шов на береговой линии —
известная ловушка `atmosphere.hlsli`).
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

#### Что сделано (2026-09-05, A6)
Роща, ветер вкл, 30 с: нативные 2560×1440 (сетка 160×90×64) — Pass_VolumetricFog **0.107 мс** VSM / 0.062 Legacy;
DLSS Performance (рендер 1280×720, сетка 80×45×64) — **0.087 мс**: в 4 раза меньше ячеек дают лишь −19 % времени, т.е.
пасс упирается в латентность выборок теней и малый размер диспатча, а не в число ячеек. Бюджет ≤ 0.3 мс соблюдён с
запасом; перенос на compute-очередь и `gridZ 48` не нужны — не делались (правило: контеншен не экстраполировать).

#### Гейты части A — итог 2026-09-05
Три конфига собраны; `check_shaders` 61/61; `--log-stress` 0/0 Debug + Release; GBV `--scene-stress-gbv=20` Legacy и
VSM с `atmosphere.volumetric:1 enabled:1 density:0.004` (объём, conservative depth и локальные источники в трёх уровнях
и на ресайзах): **Legacy CLEAN (191.5 с), VSM CLEAN (192.0 с)** — в логах 25 кадров `on=1 conservative=1`, 8 кадров `local=1` на каждый прогон. Регресс-контроль: объём выкл = закоммиченный бинарь с точностью до пола (0.014 %).

### Гейты части A
`--log-stress` 0/0; `--scene-stress-gbv=20` Legacy и VSM с `fog.volumetric:1`; три конфига; паритет
камеры теней; профдампы роща/стена K=4 off/on; глаза юзера на лучах.

---

## B. Процедурное небо (Hillaire / UE SkyAtmosphere)

### B1. LUT: transmittance + multi-scattering — [день]
Транскрипция `RenderTransmittanceLutCS` (`SkyAtmosphere.usf:1100`, 256×64, 10 сэмплов) и
`RenderMultiScatteredLuminanceLutCS` (`:1156`, 32×32, 15 сэмплов) с параметрами Земли из UE
(`FAtmosphereSetup`: 6360/6420 км, Rayleigh β, Mie β/g, озон), `SkyAtmosphereCommon.ush` целиком
(`fromTransmittanceLutUVs`, `getTransmittanceLutUvs :213-221`). `shaders/sky_atmosphere.hlsli` +
`sky_lut_transmittance_cs.hlsl`, `sky_lut_multiscatter_cs.hlsl`. Пересчёт только при смене параметров
атмосферы (не солнца). Проверка: transmittance к солнцу при зените ≈ 0.9 (визуально бело-жёлтое),
у горизонта — оранжевое (числа против UE-таблицы в комментарии).

### B2. SkyView LUT + skybox из LUT + диск солнца — [день]
`RenderSkyViewLutCS` (192×104, сэмплы 4..32), `skybox.hlsl` второй режим `sky.mode 1`: направление →
`SkyViewLutParamsToUv` (`:972`) → luminance; `GetLightDiskLuminance` (`:313`) с `sunAngularSize`
(уже есть в настройках); экспозиция та же. Каждый кадр (дёшево: 192×104). Небо ниже горизонта —
земля (`IntersectGround`). Ручки `sky.mode 0|1`, `sun.elevation/azimuth` (пишут `dirLight->direction`;
источник правды — уровень, слайдер — override сессии как `--shadow-mode`).
**Критерий приёмки:** закат/полдень/сумерки глазами; переключение mode 0↔1 без изменения
экспозиции сцены (яркость неба калибруется на HDRI: измерить среднюю яркость зенита обеих).

### B3. Aerial perspective volume — [день]
`CameraAerialPerspectiveVolume` 32×32×16 на 96 км (`:1002-1009`, `SkyAtmosphereRendering.cpp:121-137`);
в композе для геометрии дальше `fog.volumetricDistance`: `color = color·T_ap + L_ap` ПОВЕРХ нашего
аналитического тумана (разные масштабы: км против м). На малых сценах почти невидимо — принимать по
острову с дальних камер.

### B4. IBL из процедурного неба — [день]
При изменении солнца/атмосферы: захват кубмапы 128² из SkyView LUT (6 дисп.) → irradiance (наш
существующий свёрточник, если он GPU; если CPU-бейк при загрузке — перенести в compute) → префильтр
specular по мипам. Заменяет `SkySpecular`/`SkyboxTex` в режиме 1; `skyboxIntensity` = 1 (единицы
физические: LUT в cd/m² через `sunIlluminanceLux`). Время суток → свет меняется везде согласованно
(вода, IBL, туман).
**Критерий приёмки:** камера теней при mode 1 и солнце уровня ≈ HDRI-картинка по экспозиции;
слайдер `sun.elevation` — непрерывно без скачков; стоимость перезахвата ≤ 0.3 мс и ТОЛЬКО при смене.

### B5. Distant sky light LUT → туман и облака — [полдня]
`:200-206`: ambient на высоте 6 км как у UE — кормит `fog.skyScatter` (A2 читает его вместо
irradiance-куба в mode 1) и облака (C3).

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
* Exp2 против exp: наш аналитический туман — база 2 (`atmosphere.hlsli`), интегратор — база e;
  ln 2 в σ, иначе паритет A1 не сойдётся на 30 %.
* Pre-exposure: froxel-текстуры хранят pre-exposed, история реэкспонируется (`:1031`); забыть — лучи
  мигают при автоэкспозиции.
* Небо не туманится аналитикой (наше правило), но ОБЪЁМНЫЙ туман на небо ложится (лучи над
  горизонтом) — иначе лучи обрываются на силуэте пальмы.
* VSM в тумане: однотап без SMRT (марш на 64×120×68 ячеек — не по бюджету).
* Cloud shadow map и CSM/VSM — разные проекции, snap к текселю обязателен (`ShadowMapSnapLength`),
  иначе тень облака дрожит при движении камеры.
