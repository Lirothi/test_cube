# План: океан в G-buffer и единый SSR-проход (схема UE SingleLayerWater)

Дата: 2026-09-12. Статус: **PLANNED, не начат.** Родился из разговора 11–12.09 про два SSR-пасса,
`kShadingModelWater` и «вертексный шейдер воды один раз». Референс — UE `SingleLayerWaterRendering.cpp`
(есть в дропе, `D:\Programming\ue_strip\Source\Runtime\Renderer\Private\`) и
`Shaders/Private/SingleLayerWaterComposite.usf`, `SSRT/SSRTReflections.usf`.

## 0. Зачем

* **Два трейсера отражений — копипаста.** `ssr_cs.hlsl` (294 строки) ходит по G-buffer до композа;
  `ocean_reflection_cs.hlsl` (204 строки) ходит по АНАЛИТИЧЕСКОЙ плоскости y = 0 до отрисовки воды,
  со своей копией `SSRHit`, своим ранним выходом «суша над водой», своим подводным биасом
  (`kUnderwaterSsrHitBias`) и своим темпоральным резолвом с репроекцией по плоскости
  (`SsrTemporalConstants::planeReproject/planeParams/invViewProj/prevViewProj`, 2026-09-11).
  Цена второго прохода ~2–3 % кадра (0.03–0.06 + 0.012 мс) — терпимо; нетерпимо, что любая правка
  трейсера/резолва делается дважды и они уже разъехались.
* **Плоскость ≠ волна.** Луч стартует с плоскости, а не со смещённой FFT-поверхности: параллакс
  отражения по волне неверен, берег и суша — костыльный ранний выход, подводные хиты — биас.
* **После воды её никто не видит.** Океан уже пишет depth и velocity (RT1 = `gbVelocity`,
  `OceanRenderable.cpp:1038-1050`), но не нормаль/ID: RT-отражения, будущий SSGI, дебаг-вью
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

Т.е. у UE **VS воды один раз, PS один раз**, отражение — отдельный маленький композит после SSR.

## 2. Инвентарь (что уже есть у нас)

* `Main_Transparent` (`SceneRenderer_Graph.cpp:1585`), точки `copy` → `oceanRead` → `oceanTemporal` →
  `pixel` → `rebind` (`SceneRenderer.h:473-481`); тело `Pass_Transparent`
  (`SceneRenderer_Geometry.cpp:347`): `RecordOceanReflection` (`:389`) → draw океана.
* Океан: depth write ALL, RT1 = velocity, RT2 = objectID с нулевой маской (`OceanRenderable.cpp:1038-1050`).
  Половина «G-buffer воды» уже пишется.
* ID материала: `gbAux.b`, 4 бита, `utils.hlsli:17-20` (0 DefaultLit / 1 TwoSidedFoliage / 2 Terrain),
  зеркало `MaterialData.h:21`; 3..15 свободны. **`kShadingModelWater = 3`** — та же таблица, на которую
  ляжет и унификация RT-шейдинга.
* SSR: `ssr_cs.hlsl` читает LightTarget t0, GB1 t1, Depth t2, OriginDepth t3, HzbFurthest t4,
  PrevSceneColor t5, Velocity t6, GB0 t7; `ssr_temporal_cs.hlsl` (252) с океанской добавкой
  `planeReproject`; цели `oceanReflection` + `oceanReflectionHistory` (`RenderTargetManager.h:71,74`);
  бит `render::g_oceanReflectionTemporal`, ставится в serial-билдере.
* `ocean_surface.hlsl` (2591): `OceanReflectionTexture` t15 (`:130`), семпл `:2042`, композит
  `color = specular + lerp(refracted, reflected, fresnel)` (`:2204`) — отражение уже отделено одним
  lerp, поэтому композит UE ложится тривиально. Рефракция читает `sceneOpaque`/`depthCopy`
  (`OceanRenderable.cpp:819,869,893`) — копии ДО воды, как `SceneWithoutWaterTextures`. Не меняется.

## 3. Целевой порядок кадра

```
opaque G-buffer → Main_Hzb → lighting → SSR(opaque) → compose
  → [copy scene/depth]                                        (как сейчас)
  → WATER DRAW: VS FFT один раз; PS = шейдинг воды БЕЗ отражённой части,
       MRT: scene, gbVelocity (как сейчас), + gbNormal (волновая нормаль), + gbAux (roughness, ID=3, foam)
       depth write (как сейчас)
  → WATER TILES: классификация по gbAux.b == 3 → список тайлов + indirect args
  → SSR(water): ssr_cs с reflectOnlyWater=1, origin = вода (depth ПОСЛЕ draw), HZB = pHzb (opaque, НЕ перестраивать),
       цвет = scene (композ до воды); выход в oceanReflection
  → ssr_temporal: ОБЩАЯ репроекция по gbVelocity (вода уже записала свою velocity) → oceanReflectionHistory
  → WATER COMPOSITE (по тайлам): scene += (ssr.rgb + cube(R)·(1 − ssr.a)) · fresnel · (1 − foam)
  → glass / particles / fog / shafts                           (как сейчас)
```

Что переезжает: **трейс отражения — после отрисовки воды** (сейчас до). Что остаётся на месте:
пиксельный шейдер воды (после композа — рефракции нужна освещённая сцена, она в копии), velocity,
depth, стекло/частицы.

## 4. Шаги

**S0 — Инвентарь и базовые замеры (0.5 дня).** Профдамп на трёх камерах (берег
`--cam-pos=-54.81,3.00,63.71`, горизонт, close-up волн): `Pass_Transparent`, `OceanReflection`,
`OceanReflectionTemporal`, `Pass_ReflectionSource`. Скриншоты-эталоны тех же камер при SSR on/off
(`--dlss=off --wind-freeze`, фикс. экспозиция) — паритетный гейт S4 сравнивает с ними.

**S1 — ID воды и G-buffer-цели у водного draw (0.5 дня).** `kShadingModelWater = 3` в
`utils.hlsli` + `MaterialData.h` (+ строковые таблицы). PSO океана: RT3 = `gbNormal`, RT4 = `gbAux`
(форматы из RTM; write mask ALL, blend off — как RT1). PS пишет волновую макро-нормаль, roughness из
sigma Брюнетона, ID 3, пену (см. риск про канал). Билдер `Main_Transparent`: `gbNormal`/`gbAux` →
RENDER_TARGET на точке `pixel`, назад в NPS на `rebind`. Визуально ничего не меняется.
Гейт: полный (новые цели в пассе — GBV Debug без Minimized, Release scene-stress, компаратор барьеров).

**S2 — Классификация тайлов (0.5 дня).** CS 8×8: маска тайлов с ID 3 → список + `DispatchIndirect`
args (UE: Mark + BuildLists). Первый срез допустим без неё — full-screen dispatch с ранним выходом
по ID, замерить; UE тайлят, и при камере над сушей это отличает 0.00 от 0.05 мс.

**S3 — SSR(water) = вторая инвокация `ssr_cs` (1 день).** Флаг `reflectOnlyWater` в
`SsrPassConstants` (CB-структура — зеркало шейдера): при 1 пиксели с ID ≠ 3 пишут 0 и выходят;
origin/normal/roughness — из G-buffer как у остальных; `OriginDepth` = depth после воды, `Depth`/HZB
— непрозрачные (`pHzb`, без перестройки); цвет — `scene` после композа. Выход — в существующий
`oceanReflection`. Резолв — тот же `ssr_temporal_cs` с `planeReproject = 0` (velocity воды уже в
`gbVelocity` в момент резолва; точка `oceanTemporal` переезжает после draw). **Удалить:**
`ocean_reflection_cs.hlsl`, `RecordOceanReflection`, `planeReproject/planeParams/invViewProj/
prevViewProj`, ранний выход по суше (он был про плоскость), `kUnderwaterSsrHitBias`, запись в
check_shaders/vcxproj/filters. Граф: после draw воды новая точка scene → NPS (трейс) → UAV (композит)
→ RENDER_TARGET (glass); три барьера на `scene` — см. риск.

**S4 — Композит (0.5–1 день).** `water_composite_cs` по тайлам (UE `SingleLayerWaterComposite`):
для ID 3 и `depthCopy` глубже depth (DeltaDepth > 0; камера под водой — пропуск): `fresnel` =
`EffectiveFresnel` по нормали из G-buffer, `reflected = ssr.rgb + cube(R)·(1 − ssr.a)` (тот же кубик,
что сейчас в `ocean_surface.hlsl` как фолбэк), `scene += reflected · fresnel · (1 − foam)`.
В `ocean_surface.hlsl`: убрать t15 и `lerp(refracted, reflected, fresnel)` (`:2204`) → писать
`specular + refracted · (1 − fresnel)`; композит добавляет вторую половину. **Ключевой гейт паритета:**
при SSR off «кубик через композит» == «кубик в PS» пиксель-в-пиксель (вне шума пола); при SSR on —
против эталонов S0, ожидаемые отличия только по параллаксу волн/берегу (глазами).

**S5 — Уборка (0.5 дня).** `g_oceanReflectionTemporal` уходит (PS воды больше не биндит отражение),
TextureDebugViewer (oceanReflection остаётся как выход SSR(water)), профайлер-скоупы, документация,
`ocean-reflection-temporal` в памяти → устарело.

**S6 — Бонусы, отдельными шагами.** RT-отражения на воде (`rt_trace` по ID 3 — та же «вторая
инвокация»); SSGI (Part D) игнорирует ID 3; дебаг-вью нормалей/roughness воды; единый
`ShadeSurface(id, …)` (разговор 11.09) — вода встаёт в ту же таблицу ID.

## 5. Риски и открытые вопросы

* **Всё, что читает G-buffer/depth ПОСЛЕ воды, теперь видит воду.** Depth и velocity — уже так
  (DLSS, шахты, `fog_apply`); новое — `gbNormal`/`gbAux`: проверить читателей после
  `Main_Transparent` (DLSS — нет; `Main_LightShafts` — depth; дебаг-вью; история для SSR(opaque)
  следующего кадра — `PrevSceneColor` и так с водой).
* **Канал пены.** `gbAux` = AO / indirect specular scale / ID (`RenderTargetManager.h:53`) — свободного
  канала под foam может не быть; варианты: пена в `gbAux.r` (AO воды = 1 всё равно) с оговоркой в
  утилитах, либо отдельная R8-цель (UE пишут water-параметры в custom data G-buffer).
* **Roughness воды** для размытия SSR: sigma → roughness (GB1-канал); mirror-случай < 0.1 = один луч,
  как сейчас у плоскости. Порог `ssr.maxRoughness` работает и на воду.
* **Три барьера на `scene`** после draw воды (NPS → UAV → RT). Альтернатива: композит пишет в
  `scene` как UAV, glass рисуется после — это уже так (glass после океана), значит RT нужен только
  для glass/частиц: NPS → UAV → RENDER_TARGET, две транзакции сверх нынешних.
* **Под водой.** Композит гейтится DeltaDepth/камерой (UE так же); ныне подводный биас исчезает
  вместе с плоскостью — проверить камерой под поверхностью.
* **Стоимость.** Экономим `ocean_reflection_cs` (0.03–0.06) + plane-резолв; платим классификацию
  (~0.01) + композит (~0.02–0.03 по тайлам) + два барьера. Ожидание ±0; выигрыш — корректность
  (параллакс волн, берег без костыля) и один трейсер/один резолв на всё.
* **Не путать с «одним пассом на всё».** Сливать SSR(opaque) и SSR(water) в один диспатч нельзя:
  SSR(opaque) нужен композу ДО воды (рефракция читает освещённую сцену с отражениями на дне). Один
  ШЕЙДЕР, две инвокации — ровно как у UE.

## 6. Оценка

S1 0.5 + S2 0.5 + S3 1 + S4 0.5–1 + S5 0.5 = **3–3.5 дня с гейтами** (в разговоре 11.09 «~1 день» была
оценка слитого трейса по видимой поверхности БЕЗ G-buffer — другой объём).

## 7. Гейты

* Паритет S4 при SSR off — 0 % px вне шума пола (пол мерить ДО: два одинаковых прогона).
* Визуально: пальмы в воде у берега, горизонт, close-up волн (параллакс), DLSS-boil по трём кадрам
  с пропуском первой пары (рецепт 11.09), камера под водой.
* Полный стресс-набор на S1/S3/S4 (новые цели, точки, диспатчи): три конфига, Release
  `--scene-stress`, Debug `--gbv` (окно НЕ свёрнуто), компаратор барьеров; на S2/S5 — dxc-чек + глаза.
