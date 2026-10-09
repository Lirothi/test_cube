# План: океан в G-buffer и единый SSR-проход (схема UE SingleLayerWater)

Дата: 2026-09-12, сверено с кодом 2026-10-09 (что поправлено — §8). Статус: **S0 + S1 DONE (2026-10-09, не
закоммичено), дальше S2.**
Родился из разговора 11–12.09 про два SSR-пасса, `kShadingModelWater` и «вертексный шейдер воды один раз».
Референс — UE `SingleLayerWaterRendering.cpp` (есть в дропе, `D:\Programming\ue_strip\Source\Runtime\Renderer\Private\`)
и `Shaders/Private/SingleLayerWaterComposite.usf`, `SSRT/SSRTReflections.usf`.

## 0. Зачем

* **Два трейсера отражений — копипаста.** `ssr_cs.hlsl` (294 строки) ходит по G-buffer до композа;
  `ocean_reflection_cs.hlsl` (204 строки) ходит по АНАЛИТИЧЕСКОЙ плоскости y = 0 до отрисовки воды,
  со своим использованием `SSRHit`, своим ранним выходом «суша над водой» (`kCoveredByLandMargin` 0.5 м),
  своим подводным биасом (`kUnderwaterSsrHitBias`) и своим темпоральным резолвом с репроекцией по плоскости
  (`SsrTemporalConstants::planeReproject/planeParams/invViewProj/prevViewProj`, 2026-09-11; `viewProj` добавлена
  2026-09-12 — motion берётся как разность двух проекций точки плоскости, а не `uv − prevProj(P)`: обратный путь
  через `invViewProj` у неподвижной камеры не тождество, при длинной истории даёт решётку плиток, см. план облаков).
  Цена второго прохода ~2–3 % кадра (0.03–0.06 + 0.012 мс, замер 11.09) — терпимо; нетерпимо, что любая правка
  трейсера/резолва делается дважды и они уже разъехались.
* **Плоскость ≠ волна.** Луч стартует с плоскости, а не со смещённой FFT-поверхности: параллакс
  отражения по волне неверен, берег и суша — костыльный ранний выход, подводные хиты — биас.
* **После воды её никто не видит.** Океан уже пишет depth и velocity (RT1 = `gbVelocity`,
  `OceanRenderable.cpp:1105-1117`), но не нормаль/ID: RT-отражения, будущий SSGI, дебаг-вью
  нормалей, единый шейдинг по ID (см. разговор 11.09 про RT-копипасту) воды не знают.

## 1. Как это сделано в UE (что транскрибируем)

Порядок `FDeferredShadingSceneRenderer::RenderSingleLayerWater` (`SingleLayerWaterRendering.cpp:1676`):

1. `AddCopySceneWithoutWaterPass` — копии scene color + depth ДО воды (`SceneWithoutWaterTextures`) —
   это наши `sceneOpaque`/`depthCopy`, ничего нового.
2. `RenderSingleLayerWaterInner` (`:1743`, «SLW::Draw»): **один draw** воды пишет scene color + main
   depth (write) + **G-buffer targets** SLW-раскладки (`GetGBufferRenderTargets(..., GBufferLayout)`),
   в т.ч. velocity. Шейдинг воды (преломление/поглощение из копии, солнце, тени) — прямо в этом PS.
   Отражений в PS НЕТ (в forward-режиме есть, комментарий `:1716`).
3. `RenderSingleLayerWaterReflections` (`:1328`): тайл-классификация по ID воды
   (`WaterTileCatergorisationMarkCS` / `WaterTileClassificationBuildListsCS`, indirect args), **вторая
   инвокация того же SSR-шейдера** с `ShouldReflectOnlyWater` (`SSRTReflections.usf:138`:
   `bNoMaterial = !IsSingleLayerWaterMaterial(...)` — не-вода выходит), HZB — **FurthestHZB,
   построенный ДО воды** (`ScreenSpaceRayTracing.cpp:1151`; `RenderHzb` идёт после base pass, `:457`):
   луч от воды марширует по НЕПРОЗРАЧНОЙ пирамиде, сама вода в неё не попадает — воде нечего отражать
   в себе. Затем денойз/TAA по cvar и **композит** `SingleLayerWaterCompositePS`
   (`SingleLayerWaterComposite.usf:61`): гейт `ShadingModelID == SINGLELAYERWATER && DeltaDepth > 0`
   (вода над непрозрачной глубиной из копии), `Reflection = SSR.rgb + Env*(1 − SSR.a)`, умножить на
   `EnvBRDF(F0, roughness, NoV)` и прибавить к scene color по тайлам воды.
4. Туман на воду — уже ПОСЛЕ этого (`DeferredShadingRenderer.cpp:3230` вода, `:3262` небо и туман), т.е.
   отражение туманится вместе с водой.

Т.е. у UE **VS воды один раз, PS один раз**, отражение — отдельный маленький композит после SSR.
Цвет хитов UE берёт из scene color ПОСЛЕ воды (`SingleLayerWaterRendering.cpp:1566-1567`); мы оставляем
`sceneOpaque`, как сейчас у `ocean_reflection_cs` (паритет; разница — только хиты по суше, видимой сквозь воду).

## 2. Инвентарь (что уже есть у нас)

* **Проходы (B6.1, 2026-09-09; `RenderPass.h:94-101`).** `Main_Transparent` (`SceneRenderer_Graph.cpp:1855`)
  рисует ТОЛЬКО воду (depth-writing transparents). За ним `Main_TransparentFog` (`:2007`, `fog_apply.hlsl`:
  только пиксели воды — `discard` где depth == depthCopy; height fog + aerial perspective неба поверх воды,
  `dst · (overA·ap.a) + …`) и `Main_Translucent` (стекло, частицы; рибиндит цели сам,
  `SceneRenderer_Geometry.cpp:469-470`).
* Точки `Main_Transparent`: `copy` → `oceanRead` → `oceanTemporal` → `pixel` → `rebind` (`SceneRenderer.h:522-532`);
  тело `Pass_Transparent` (`SceneRenderer_Geometry.cpp:353`): `Transparent_Driver` (копии, `RecordOceanReflection`
  `:402`, rebind `:405-406`) + `Transparent_Water` fan-out чанками. `RecordOceanReflection` —
  `SceneRenderer_Reflections.cpp:845`: t0 `sceneOpaque`, t1 `depthCopy`, t2 furthest HZB; резолв `:919-944`.
* **Океан, PSO** (`OceanRenderable.cpp:1071-1117`): RT0 scene, RT1 velocity (write ALL), RT2 objectID — только
  `WITH_EDITOR`, с нулевой маской; depth write ALL. Цели ставит `Renderer::BindSceneColorWithVelocity`
  (`Renderer.cpp:3084`); после B6.1 в `Main_Transparent` им пользуется только вода. Половина «G-buffer воды»
  уже пишется.
* **Раскладка G-buffer** (`RenderTargetManager.h:49-53`, `gbuffer_common.hlsli:156-166`): RT0 `gb0` = albedo.rgb +
  A = `PackRM(rough, metal)` (A8: 5 бит roughness, 3 бита metal; `utils.hlsli:103-118`); RT1 `gb1` = нормаль RGB10
  + пустые A2; RT2 `gb2` = эмиссив / пэйлоад листвы; RT3 velocity; [RT4 objectID в редакторе]; `gbAux` = AO,
  indirect specular scale, ID/15, transmission normal weight. **Roughness — в `gb0.a`, не в `gb1`; цели
  «gbNormal» нет.**
* **ID материала:** `gbAux.b`, 4 бита, `utils.hlsli:17-20` (0 DefaultLit / 1 TwoSidedFoliage / 2 Terrain),
  зеркало `MaterialData.h:21-30` (enum + `static_assert`'ы); 3..15 свободны. **`kShadingModelWater = 3`** — та же
  таблица, на которую ляжет и унификация RT-шейдинга.
* **SSR:** `ssr_cs.hlsl` читает t0 LightTarget, t1 GB1 (нормаль, `:172`), t2 Depth, t3 OriginDepth, t4 HzbFurthest,
  t5 PrevSceneColor, t6 Velocity, t7 GB0 (roughness = `UnpackRM(GB0.a).x`, `:192`). **`gbAux` не биндится** —
  ID для `reflectOnlyWater` потребует t8. `ssr_temporal_cs.hlsl` (267) с океанской добавкой
  `planeReproject/planeParams/invViewProj/prevViewProj/viewProj` (`:62-67`, ветка `:163-216`); хендлы
  `SceneResourceBootstrapper.cpp:1323-1346` / `.h:410-421`. Цели `oceanReflection` + `oceanReflectionHistory`
  (`RenderTargetManager.h:71,74`), свой размер (`GetOceanReflectionTextureWidth/Height`). Бит
  `render::g_oceanReflectionTemporal` (`RenderConstants.h:71`) ставится в serial-билдере
  (`SceneRenderer_Graph.cpp:1906-1920`, там же `oceanHistoryValid_/Frames_/Width_/Height_`).
* **Поверхность воды — ДВЕ** (с 2026-09-12 16:09; `ocean_surface.hlsl` теперь 19-строчный диспетчер по
  `OCEAN_SHORE_RUNUP`):
  `ocean_surface_surf_sim.hlsli` — по умолчанию (`--ocean-surf-sim-shore`): t15 `:128`, семпл `:753`, композит `:878`;
  `ocean_surface_runup.hlsli` (`--ocean-runup-shore`): t15 `:124`, семпл `:1603`, композит `:1743`.
  В обеих `color = specular + lerp(refracted, reflected, fresnel)` — отражение уже отделено одним lerp, поэтому
  композит UE ложится тривиально. Рефракция читает `sceneOpaque`/`depthCopy` (`OceanRenderable.cpp:883,933,957`) —
  копии ДО воды, как `SceneWithoutWaterTextures`. Не меняется.
* **Indirect compute** в движке нет: `ExecuteIndirect` есть только для draw (CSM/VSM), `RecordComputeDispatch`
  (`ComputeDispatch.h:22,69`) — прямой диспатч с фиксированной формой.

## 3. Целевой порядок кадра

```
opaque G-buffer → Main_Hzb → lighting → SSR(opaque) → compose
  → Main_Transparent:
       [copy scene/depth]                                      (как сейчас; RecordOceanReflection уходит)
       WATER DRAW: VS FFT один раз; PS = шейдинг воды БЕЗ отражённой части,
         MRT: scene, gbVelocity (как сейчас), + gb1 (волновая нормаль), + gb0 (только A: PackRM(rough, 0)),
              + gbAux (ID=3, пена), [objectID]; depth write (как сейчас)
  → Main_WaterReflection (НОВЫЙ внешний проход):
       WATER TILES: классификация по gbAux.b == 3 → список тайлов + indirect args
       SSR(water): ssr_cs с reflectOnlyWater=1, OriginDepth = depth ПОСЛЕ воды, Depth = depthCopy,
         HZB = pHzb (opaque, НЕ перестраивать), цвет = sceneOpaque; выход в oceanReflection
       ssr_temporal: ОБЩАЯ репроекция по gbVelocity (вода уже записала свою velocity) → oceanReflectionHistory
       WATER COMPOSITE (по тайлам): scene += (ssr.rgb + cube(R)·(1 − ssr.a)) · fresnel · (1 − foam)
  → Main_TransparentFog: height fog + AP неба на пиксели воды   (как сейчас — теперь туманит и отражение)
  → Main_Translucent: стекло / частицы                           (как сейчас)
  → shafts …                                                     (как сейчас)
```

Что переезжает: **трейс отражения — после отрисовки воды, в свой проход** (сейчас до неё, в драйвере
`Main_Transparent`); смешивание отражения — из PS воды в CS-композит. Что остаётся на месте: пиксельный шейдер
воды (после композа — рефракции нужна освещённая сцена, она в копии), velocity, depth, туман, стекло/частицы.

**Почему отдельный внешний проход, а не новые точки в `Transparent_Driver`.** SSR(water) читает depth после воды
как SRV: DEPTH_WRITE → NPS после fan-out-чанков `Transparent_Water`. Такую смену состояния барьерный компилятор
выражает между внешними проходами, но не между группами fan-out-чанков внутри одного
(`SceneRenderer_Graph.cpp:2002-2006` — по той же причине `Main_TransparentFog` отдельный проход).

**Почему композит ДО тумана.** `fog_apply` на пикселях воды умножает dst на пропускание и прибавляет in-scatter
(`fog_apply.hlsl:174-180`). Отражение, прибавленное после, останется без тумана и без AP — светится в дымке и у
горизонта. У UE тот же порядок (§1, п. 4).

## 4. Шаги

**S0 — Инвентарь и базовые замеры (0.5 дня).** Профдамп на трёх камерах (берег
`--cam-pos=-54.81,3.00,63.71`, горизонт, close-up волн): `Transparent`, `OceanReflection`,
`OceanReflectionTemporal`, `TransparentFog`, `Pass_ReflectionSource`. Скриншоты-эталоны тех же камер при SSR on/off
(`--dlss=off --wind-freeze`, фикс. экспозиция) на ОБЕИХ поверхностях (`--ocean-surf-sim-shore`,
`--ocean-runup-shore`) и один кадр с туманом — паритетный гейт S4 сравнивает с ними.

> **S0 DONE 2026-10-09** (Release на HEAD 247969a, wind_test, `--wind-freeze`, DLSS Balanced 2560×1440, кап FPS
> выключен, по одному профдампу на камеру; скрипт `s0_series.ps1` в scratchpad сессии). Камеры: `level` — авторская
> камера уровня (лагуна с отражениями пальм), `shore` — `--cam-pos=-54.81,3.00,63.71 --cam-rot=-0.0571,0.4842,0.0317,0.8725`
> (солнечная дорожка, горизонт), `horizon` — `--cam-pos=-59.50,6.00,52.73 --cam-rot=0.001889,-0.97626,0.008520,0.21643`
> (море по обе стороны косы, воды в кадре больше всего). GPU, мс:
>
> | скоуп | level | shore | horizon |
> |---|---:|---:|---:|
> | `GPU.Frame` | 3.403 | 3.185 | 3.403 |
> | `Transparent.Driver` (копии + трейс + резолв) | 0.054 | 0.054 | 0.053 |
> | `Pass_OceanReflection` | 0.029 | 0.031 | 0.030 |
> | `Pass_OceanReflection.Temporal` | 0.013 | 0.014 | 0.013 |
> | `Ocean.Surface` (draw воды) | 0.067 | 0.061 | 0.083 |
> | `Pass_Transparent` (это скоуп `Pass_TransparentFog`) | 0.006 | 0.006 | 0.007 |
>
> **Эталоны-скриншоты для пиксельного паритета НЕ сняты сознательно:** на wind_test пол `--shot` между прогонами
> 10–13 % пикселей (кадр берётся по времени, surf-sim/пена/темпоральный SSR уезжают на разное число кадров), так
> что «0 % px против эталонов S0» на этом уровне не измерить ни при каком пороге. Паритет S4 переделан в A/B в
> ОДНОМ бинарнике (§7).

**S1 — ID воды и G-buffer-цели у водного draw (0.75 дня).** `kShadingModelWater = 3` в `utils.hlsli` +
`MaterialData.h` (enum + `static_assert`) (+ строковые таблицы). PSO океана: + `gb1`, `gb0`, `gbAux` (форматы из RTM;
blend off; `gb1`/`gbAux` — write mask ALL, `gb0` — ТОЛЬКО ALPHA: albedo.rgb вода не пишет, A = `PackRM(rough, 0)`).
**Слоты:** objectID есть только в редакторе — новые цели ставить ДО него (RT2-4, objectID RT5 в редакторе),
одно правило на PSO, PS обеих поверхностей и привязку. **Привязка:** вместо
`BindSceneColorWithVelocity` в `Transparent_Driver` (`:406`) — своя для воды (`Main_Translucent` рибиндит сам,
стекло/частицы не затронуты). PS обеих поверхностей пишет волновую макро-нормаль, roughness из sigma Брюнетона,
ID 3, пену (см. риск про канал). Билдер `Main_Transparent`: `gb0`/`gb1`/`gbAux` → RENDER_TARGET на точке `rebind`;
назад в NPS — в `Main_WaterReflection`. Визуально ничего не меняется.
Гейт: полный (новые цели в пассе — GBV Debug без Minimized, Release scene-stress, компаратор барьеров).

> **S1 DONE 2026-10-09.** Что легло в код и чем отличается от текста выше:
> * `kShadingModelWater = 3` (`utils.hlsli`), `ShadingModel::Water = 3` + `static_assert` (`MaterialData.h`);
>   `TryParseShadingModel` его НЕ принимает — материал не может выбрать воду; `ShadingModelToString` → `"water"`.
> * Слой воды в PS обеих поверхностей — `OceanGBuffer` в `ocean_surface_common.hlsli`:
>   **GB0.a** = `PackRM(roughness отражения, 0)` (surf_sim — `oceanRoughness` из slope variance, runup —
>   `OceanSurfaceRoughness`); **GB1** = нормаль, ОТНОСИТЕЛЬНО КОТОРОЙ берётся отражение (`OceanReflectionNormal`:
>   шейдинговая нормаль, наклонённая к +Y на reflection normal strength; у runup — от МАКРО-нормали), а не шейдинговая;
>   **GBAux** = (1, 1, ID 3, **вес отражения**). Пена отдельного канала не получила — она внутри веса (риск снят, §5).
> * **Вес отражения W** = всё, на что умножается `reflected` по дороге в scene colour, кроме pre-exposure кадра:
>   `fresnel · (1 − foam) · (1 − horizon.a, только при выключенном тумане) · refractionEdgeWeight · (1 − debug)`.
>   Каждый из этих шагов — `lerp` к чему-то другому, т.е. линеен по цвету, поэтому
>   `final = остальное + reflected · W · skyParams.z` ТОЧНО. S4 берёт W из GBAux.a, а не пересчитывает Fresnel.
> * PSO: RT2 `gb0` (mask ALPHA), RT3 `gb1`, RT4 `gbAux`, RT5 objectID (редактор, mask 0, PS его не пишет — один PS на
>   обе сборки; у surf_sim убран фиктивный `bias : SV_Target2`). `Renderer::BindWaterTargets`; `RenderObjectBatch`
>   получил `BatchTargets { GBuffer, SceneVelocity, Water }` вместо двух bool (мёртвая ветка `BindSceneColor` ушла).
> * Барьеры: `WaterDrawsThisFrame()` (= `mainView->transparentWater` не пуст) решает и объявление RT на `rebind`, и
>   **возврат в NPS в `Main_TransparentFog`** — канон этих целей NPS, а внеграфовые подглядывания (ImGui-инспектор,
>   debug preview) берут их из канона (`Renderer::RenderImGui`). В S3 возврат переезжает в `Main_WaterReflection`.
> * Дебаг-блит (`debug_texture.hlsl`) получил выбор канала и gain: `--set=debug.texMode:1 debug.tex:9` — GB1 цветом,
>   `:10` — ID (GBAux.b ×5, вода белая), `:11` — W (GBAux.a). Инспектор: «Show fullscreen» для GBuffer1 и ShadingModel.
> * Проверено: dxc 112/112 + варианты `OCEAN_FOAM_DEBUG`/`SHORE_SINK`/`VS_DEPTH_PROBE`; три конфига собраны; Debug
>   `--scene-stress-gbv=20 --barrier-cmp` — CLEAN, `barrier_diag.log` нет; Release `--scene-stress=150` — CLEAN;
>   кадры дебаг-вью на камере `horizon`: вода белая в ID, нормаль вверх с волновой фактурой, W растёт к горизонту.
>   Стоимость: `Ocean.Surface` на `horizon` 0.083 → 0.078 мс (одна пара, в шуме — три лишние MRT не видны).
>   Пиксельный паритет кадра не мерился (см. S0); на глаз `horizon` до/после не отличается.

**S2 — Классификация тайлов (0.5 дня).** CS 8×8: маска тайлов с ID 3 → список + indirect args (UE: Mark +
BuildLists). Indirect-диспатча для CS в движке нет (§2) — нужна своя command signature. Первый срез поэтому
тем более без неё: full-screen dispatch с ранним выходом по ID, замерить; UE тайлят, и при камере над сушей это
отличает 0.00 от 0.05 мс.

**S3 — `Main_WaterReflection` + SSR(water) = вторая инвокация `ssr_cs` (1.5 дня).** Новый внешний проход между
`Main_Transparent` и `Main_TransparentFog` (`RenderPass` + строка имени + профайлер-скоуп). Объявляет: depth NPS
(OriginDepth), `depthCopy`/`sceneOpaque`/HZB/`gb0`/`gb1`/`gbAux`/`gbVelocity` NPS, `oceanReflection` UAV → NPS,
история UAV, прошлая история NPS, `scene` UAV (композит, S4). `Main_TransparentFog` уже объявляет `scene`
RENDER_TARGET и depth PSR — хвост выходит сам.
Флаг `reflectOnlyWater` в `SsrPassConstants` (CB-структура — зеркало шейдера): при 1 пиксели с ID ≠ 3 пишут 0 и
выходят; t8 = `gbAux` (таблица позиционная — SSR(opaque) тоже биндит t8); origin/normal/roughness — из G-buffer
как у остальных; `OriginDepth` = depth после воды, `Depth`/HZB — непрозрачные (`depthCopy`, `pHzb` без
перестройки); LightTarget = `sceneOpaque`. Выход — в существующий `oceanReflection`. Резолв — тот же
`ssr_temporal_cs` с `planeReproject = 0` (velocity воды уже в `gbVelocity`); учёт истории
`oceanHistoryValid_/Frames_/Width_/Height_` переезжает в билдер нового прохода.
**Удалить:** `ocean_reflection_cs.hlsl` (с ним `kCoveredByLandMargin`, `kUnderwaterSsrHitBias`),
`RecordOceanReflection`, `OceanReflectionConstants`, материал/CB-размер ocean reflection в bootstrapper'е,
точки `oceanRead`/`oceanTemporal` и `TransparentPoints::oceanReflect/oceanTemporalRun`,
`planeReproject/planeParams/invViewProj/prevViewProj/viewProj` (шейдер, `SsrTemporalConstants`, хендлы
`SceneResourceBootstrapper.cpp:1323-1346` / `.h:410-421`), запись в check_shaders/vcxproj/filters.

**S4 — Композит (0.75–1.25 дня).** `water_composite_cs` по тайлам (UE `SingleLayerWaterComposite`) в
`Main_WaterReflection` после резолва: для ID 3 и `depthCopy` глубже depth (DeltaDepth > 0; камера под водой —
пропуск): `reflected = ssr.rgb + cube(R)·(1 − ssr.a)` (тот же кубик, что сейчас фолбэк в PS),
`scene += reflected · W · skyParams.z`, где W = GBAux.a (S1; Fresnel по G-buffer НЕ пересчитывать). В ОБЕИХ поверхностях
(`ocean_surface_surf_sim.hlsli:878`, `ocean_surface_runup.hlsli:1743`): убрать t15 и
`lerp(refracted, reflected, fresnel)` → писать `specular + refracted · (1 − fresnel)`; композит добавляет вторую
половину. **Ключевой гейт паритета:** при SSR off «кубик через композит» == «кубик в PS» на обеих поверхностях и
с туманом — A/B в ОДНОМ бинарнике (рантайм-переключатель «отражение в PS / в композите» на время S4, через
`--sweep` в одном процессе, чтобы кадры шли подряд; см. §7); при SSR on — глазами, ожидаемые отличия только по
параллаксу волн/берегу.

**S5 — Уборка (0.5 дня).** `g_oceanReflectionTemporal` уходит (`RenderConstants.h:71`, `SceneRenderer.cpp:94`,
`OceanRenderable.cpp:984-987` — PS воды больше не биндит отражение), TextureDebugViewer (`oceanReflection`
остаётся как выход SSR(water)), профайлер-скоупы (`kPassOceanReflection`, `kOceanReflectionTemporal`),
документация, память `ocean-reflection-temporal` → устарело.

**S6 — Бонусы, отдельными шагами.** RT-отражения на воде (`rt_trace` по ID 3 — та же «вторая
инвокация»); SSGI (Part D, не начат) игнорирует ID 3; дебаг-вью нормалей/roughness воды; единый
`ShadeSurface(id, …)` (разговор 11.09) — вода встаёт в ту же таблицу ID.

## 5. Риски и открытые вопросы

* **Всё, что читает G-buffer/depth ПОСЛЕ воды, теперь видит воду.** Depth и velocity — уже так
  (DLSS, шахты, `fog_apply`); новое — `gb0`/`gb1`/`gbAux`. Lighting/compose/SSR(opaque) идут до воды
  (`SceneRenderer_Lighting.cpp`, `SceneRenderer_Reflections.cpp:164`) — им всё равно. Проверено в S1: в графе после
  `Main_Transparent` эти три цели не объявляет никто, кроме `Main_Debug` (дебаг-блит) и возврата в каноне в
  `Main_TransparentFog`; вне графа — ImGui-инспектор и debug preview, оба читают из канона (NPS).
* ~~**Канал пены**~~ и ~~**Fresnel композита ≠ fresnel PS**~~ — СНЯТЫ в S1: `gbAux.a` (transmission weight значим
  только для ID 1) несёт вес отражения W, в котором уже есть и Fresnel, и пена, и горизонт, и мягкий край
  рефракции. Композит ничего не пересчитывает.
* **Точность W.** `gbAux` — 8 бит на канал: ошибка W ≤ 1/510, т.е. до 0.2 % от `reflected`. На паритете S4 это
  может оказаться видимым на ярком небе в отражении у горизонта (W ~ 1); если так — отдельная R16-цель под W.
* **Roughness воды** для размытия SSR: sigma → roughness → `gb0.a` (5 бит, шаг 1/31); mirror-случай < 0.1 = один
  луч, как сейчас у плоскости. Порог `ssr.maxRoughness` работает и на воду.
* **Барьеры.** `scene`: RENDER_TARGET → UAV (композит) → RENDER_TARGET (`Main_TransparentFog`) — две транзакции
  сверх нынешних. depth: DEPTH_WRITE → NPS (трейс) → PSR (туман, уже объявлен) — одна лишняя, если барьерная
  система не примет NPS|PSR одним объявлением.
* **Порядок композит → туман** обязателен (§3); гейт с туманом в S0/S4.
* **Две поверхности воды.** Каждая правка PS — дважды; по умолчанию surf_sim, но паритет на обеих.
* **Под водой.** Композит гейтится DeltaDepth/камерой (UE так же); ныне подводный биас исчезает
  вместе с плоскостью — проверить камерой под поверхностью.
* **Стоимость.** Экономим `ocean_reflection_cs` (0.03–0.06) + plane-резолв; платим классификацию
  (~0.01) + композит (~0.02–0.03 по тайлам) + барьеры + ещё один внешний проход. Ожидание ±0; выигрыш —
  корректность (параллакс волн, берег без костыля) и один трейсер/один резолв на всё.
* **Не путать с «одним пассом на всё».** Сливать SSR(opaque) и SSR(water) в один диспатч нельзя:
  SSR(opaque) нужен композу ДО воды (рефракция читает освещённую сцену с отражениями на дне). Один
  ШЕЙДЕР, две инвокации — ровно как у UE.

## 6. Оценка

S1 0.75 + S2 0.5 + S3 1.5 + S4 0.75–1.25 + S5 0.5 = **4–4.5 дня с гейтами** (12.09 было 3–3.5: не учтены две
поверхности воды и отдельный внешний проход; в разговоре 11.09 «~1 день» была оценка слитого трейса по видимой
поверхности БЕЗ G-buffer — другой объём).

## 7. Гейты

* Паритет S4 при SSR off — на обеих поверхностях (`--ocean-surf-sim-shore`, `--ocean-runup-shore`) и с туманом,
  **A/B в одном процессе** (`--sweep` по рантайм-переключателю): между двумя прогонами на wind_test пол `--shot`
  10–13 % пикселей (S0), и пиксельная метрика там не различает ничего. Пол мерить тем же `--sweep` с двумя
  одинаковыми значениями.
* Визуально: пальмы в воде у берега, горизонт, close-up волн (параллакс), DLSS-boil по трём кадрам
  с пропуском первой пары (рецепт 11.09), камера под водой.
* Полный стресс-набор на S1/S3/S4 (новые цели, проход, точки, диспатчи): три конфига, Release
  `--scene-stress`, Debug `--gbv` (окно НЕ свёрнуто), компаратор барьеров; на S2/S5 — dxc-чек + глаза.

## 8. Сверка с кодом 2026-10-09

На момент сверки шаги не начаты: `kShadingModelWater` нет, `ocean_reflection_cs.hlsl`, `RecordOceanReflection`,
`planeReproject` и `g_oceanReflectionTemporal` на месте (S0 и S1 сделаны позже в тот же день — см. их блоки в §4).
Поправлено против версии 12.09:

* **Две поверхности воды** вместо одного `ocean_surface.hlsl` (2591 строка) — S1/S4 правят обе, паритет на обеих.
* **Раскладка G-buffer:** roughness в `gb0.a` (`PackRM`), не в `gb1`; цели «gbNormal» нет. Вода пишет три цели
  (`gb1`, `gb0` по альфе, `gbAux`), а не две; `ssr_cs` не биндит `gbAux` → t8; слоты RT учитывают objectID только в
  редакторе.
* **Порядок кадра:** после воды идут `Main_TransparentFog`, затем `Main_Translucent` (B6.1 от 09.09 — строка
  «glass / particles / fog» была неверна уже 12.09). Композит обязан встать до тумана; трейс + резолв + композит —
  новый внешний проход `Main_WaterReflection`, а не точки в драйвере. Риск «три барьера на `scene`» сведён к двум.
* В список удаления S3 добавлены `viewProj`, `kCoveredByLandMargin`, хендлы bootstrapper'а, `TransparentPoints`.
* Ссылки на строки обновлены (`SceneRenderer_Graph.cpp`, `SceneRenderer.h`, `SceneRenderer_Geometry.cpp`,
  `OceanRenderable.cpp`, `ssr_temporal_cs.hlsl` 252 → 267, `MaterialData.h`).
* Оценка 3–3.5 → 4–4.5 дня.
