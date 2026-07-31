# План улучшения каскадных теней (CSM) в `D:\programming\test_cube`

Документ для ИИ-исполнителя. **Самодостаточен** — доступ к исходникам Unreal Engine не требуется.
Весь код приведён уже переведённым на типы и конвенции `test_cube` (`mat4`/`float3`, прямой Z
в shadow-атласе, имена полей CB проекта). Алгоритмы восстановлены по реализации CSM в UE 5.6;
названия UE-функций/cvar'ов оставлены как ориентиры, но копий исходников Epic в документе нет.

Каждый шаг — независимо внедряемый и независимо проверяемый.

**Цель.** CSM здесь — не легаси, а осознанно поддерживаемый **быстрый** путь: визуально
конкурентная альтернатива VSM при заметно меньшей стоимости кадра. Отсюда два сквозных следствия:

* каждый шаг оценивается по паре «качество ↔ `Pass_CSM` + `Pass_Lighting`», а не по одному качеству.
  Шаги, покупающие качество временем (пресеты `Balanced`/`Quality` в S4, ядро 6×6 в S8), обязаны
  быть **пресетами/тумблерами**, а не новым дефолтом; дефолтный пресет должен остаться дешевле VSM;
* VSM живёт в том же билде и **не должен пострадать** — ни визуально, ни по времени (правила 2 и 3).
  Часть шагов трогает общий с VSM код, и там цена измеряется в обоих режимах.

---

## 0. Правила работы (обязательно к прочтению)

1. **Line endings.** Репозиторий требует CRLF во всех C++/HLSL/project-файлах
   (см. `D:\programming\test_cube\AGENTS.md`). После правок проверить отсутствие смешанных переводов строк
   скриптом из `AGENTS.md`. Это требование репозитория, не рекомендация.
2. **Не ломать VSM-режим.** В проекте два режима направленных теней:
   * Legacy CSM (`Pass_CSM`, атлас `D.shadow`) — предмет этого документа;
   * VSM clipmap (`VirtualShadowMap.cpp`, `vsm_sample.hlsli`) — **не трогать**.
   Переключение — `render::VsmActive()`; в лайтинге ветка `useVsm != 0` (`shaders/lighting_cs.hlsl:189`).

   **⚠ Дефолт билда — VSM**, а не CSM: `sources/rendering/renderables/InstanceTypes.h:133`
   (`inline ShadowMode g_shadowMode = ShadowMode::VSM;`). В VSM-режиме пас `Main_CSM` вообще
   **не попадает в граф** (`SceneRenderer.cpp:1374`), поэтому **любая проверка любого шага этого
   документа начинается с переключения в Legacy**: Ctrl+V (`AppController.cpp:90-96`, действие
   `ToggleVsmPageRequest`) или тумблер в `DeveloperWindow.cpp:518`. Забыть это — значит «проверить»
   шаг на коде, который не исполнялся.

   Обратная сторона: шаги S6/S7 меняют код, который в VSM-режиме исполняется, поэтому у них
   в приёмке стоит отдельная проверка VSM.
3. **⚠ Три общих ресурса CSM↔VSM.** Это главный источник риска в шагах S6/S7:
   * `shaders/shadow_indirect_csm.hlsl` — **один и тот же шейдер** для CSM-каскадов и для VSM-страниц;
   * его PSO **тоже один**: `VirtualShadowMap.cpp:755` вызывает `shadowGpu->IndirectShadowMaterial()`
     (`ShadowGpuData.cpp:331-334`). **Любое изменение rasterizer/depth state этого PSO меняет и VSM.**
   * его `cbuffer PerView (b1)`: для CSM заполняется `SceneRenderer::BuildShadowViewCB`
     (`SceneRenderer.cpp:219-226`), для VSM — вычислительным шейдером `vsm_page_setup_cs.hlsl`
     в слот `PageProj` со шагом **256 байт** (`vsm_page_setup_cs.hlsl:122-131`: viewProj в 0..63,
     ветровой хвост в 192..223, байты 64..191 и 224..255 **не пишутся**).
   Следствие: любое новое поле CB, которое читает этот шейдер, обязано записываться **и** в
   `vsm_page_setup_cs.hlsl` (хотя бы нулями), иначе VSM получит мусор.
4. **Два потребителя CSM в шейдерах.** Каскады (`lightViewProj[4]` / `cascadeScaleBias[4]`) читают
   **ровно два** шейдера — `shaders/lighting_cs.hlsl` и `shaders/glass.hlsl` — и они содержат
   **две независимые копии** логики, уже разошедшиеся по поведению (в `glass.hlsl`
   нет ни fallback-цепочки, ни blend-полосы). Шаг **S3** обязателен перед S8/S9/S10/S12.
   `shaders/debug_texture.hlsl` только визуализирует атлас (`SampleLevel`), логики каскадов не
   содержит; `shaders/spotlight_cs.hlsl` работает с отдельным `SpotShadowAtlas` (Texture2DArray).

   **Важно для S11:** `glass.hlsl` шейдит преломление/отражение, то есть **приёмник там может
   лежать вне фрустума камеры**. Любая оптимизация, сужающая отрисованную область атласа
   (scissor в S11, кэш в S13), обязана это учитывать.
5. **Один шаг = один коммит.** Не переходить к следующему шагу, пока «Критерий приёмки» текущего
   не выполнен.
6. **Шейдеры компилируются в рантайме** через DXC (`sources/materials/Material.cpp:102-140`),
   профиль автоматически поднимается до максимально поддерживаемого (до `SM 6.7`,
   `Material.cpp:33`), есть hot-reload по времени файла (`Material.cpp:305-330`). `Gather()`
   доступен. Производные (`ddx`/`ddy`) в compute-шейдере — **нет** (требуют SM 6.6), см. S9.
7. **Числа** посчитаны для дефолтной камеры (`sources/app/camera/Camera.h:21-23`: `hfov = 90°`,
   `zNear = 0.01`, `zFar = 10000`) при aspect 16:9 (`vfov ≈ 58.7°`, `tan(vfov/2) = 0.5625`).

---

## 1. Текущее состояние (baseline)

### 1.1. CPU

| Что | Где | Значение |
|---|---|---|
| Число каскадов | `sources/app/scene/SceneFrameData.h:81` | `kCascades = 4` (жёстко) |
| Атлас | `sources/rendering/core/RenderTargetManager.cpp:404`, `.h:87` | `4096 × 4096` |
| Формат | `RenderTargetManager.cpp:491,507,513` | ресурс `R16_TYPELESS`, DSV `D16_UNORM`, **SRV `R16_UNORM`** (важно для S8: `Gather` вернёт нормализованные глубины) |
| Раскладка тайлов | `Scene.cpp:144,258-261` **и** `Renderer.cpp:1474-1485` | жёсткая сетка 2×2, тайл `shadowRes/2 = 2048` — **захардкожено в двух местах** |
| Очистка | `Renderer.cpp:1489` | `ClearDepthStencilView` на **весь** атлас |
| Сплиты | `sources/app/scene/SceneRenderConfig.h:8` | `{10, 35, 100, 300}` м, абсолютные |
| Fit каскада | `Scene.cpp:183-195` | bounding-сфера по **центроиду** 8 углов слайса |
| Padding | `SceneRenderConfig.h:11` | `overlap = 2.0f` **мировых единиц** |
| Стабилизация | `Scene.cpp:200-220` | texel snap в фиксированном light-фрейме — **корректно, не менять** |
| «Глаз» света | `Scene.cpp:198` | `lightDistance = max(1, maxDistance) = 300` м для **всех** каскадов |
| Расширение near | `SceneRenderConfig.h:19` | `casterReachWS = 150` м |
| z-padding | `SceneRenderConfig.h:12` | `zPadding = 25` м |
| Bias | `Scene.cpp:253-256` | normal offset `0.75` текселя + depth bias `2.0` текселя |
| Depth-функция shadow-PSO | `RenderableObject.cpp:458`, `ShadowGpuData.cpp:1091` | `LESS_EQUAL`, clear `1.0` → **прямой Z** (0 = у света, 1 = далеко) |
| Настройка shadow-PSO (не-индирект) | `RenderableObject.cpp:445-461` | **единственное** место; overrides отсутствуют |
| Настройка shadow-PSO (индирект) | `ShadowGpuData.cpp:1080-1110` | **общий с VSM** (см. правило 3) |
| Rasterizer по умолчанию | `sources/materials/Material.h:66-79` | `DepthClipEnable = TRUE`, `DepthBias = 0`, `SlopeScaledDepthBias = 0` |
| CB лайтинга | `SceneResourceBootstrapper.h:147-178`, `.cpp:15-50`, `.cpp:469-515` | добавление поля = HLSL + struct + handle + write |
| PerView CB shadow-паса | `SceneRenderer.cpp:120-138` (`static_assert(sizeof == 224)`) | HLSL: `gbuffer_common.hlsli:68-83` + **копия** в `shadow_indirect_csm.hlsl:63-75` |
| Input layouts индиректа | `InputLayoutManager.cpp:80-96` | `PosOnly_InstCasterId`, `PosUV_InstCasterId` — **без NORMAL** (см. S6) |
| Shadow-depth VS | `gbuffer_csm.hlsl`, `gbuffer_inst_csm.hlsl`, `gbuffer_instcb_csm.hlsl`, `glass_csm.hlsl`, `shadow_indirect_csm.hlsl` | depth-only, никакого bias |

### 1.2. Шейдер (`shaders/lighting_cs.hlsl`)

| Что | Строки | Поведение |
|---|---|---|
| Выбор каскада | `80-85` | по `dot(P - camPos, camDir)`, жёсткие сплиты |
| Фильтр | `87-101` | `SampleCmpLevelZero` × **9 тапов** с шагом 1 тексель, без вращения/джиттера. **Не box:** `gSmpLinear` = `D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT` (`SamplerManager.cpp:152`), т.е. каждый тап — аппаратный 2×2 PCF, и сетка 3×3 даёт **пересэмплированный 4×4 tent**. Это надо помнить в S8: там улучшение приходит от рампы, а не от формы ядра |
| Радиус PCF | `151` | `pow(normalBiasWS[0]/normalBiasWS[c], 0.25)` |
| Граница тайла | `135-139` | **margin + `continue`** → провал в следующий каскад (видимое кольцо) |
| Blend каскадов | `167-178` | полоса 10 % от **абсолютной** дистанции сплита |
| За последним каскадом | `155` | `return 1.0` — **жёсткий терминатор на 300 м** |
| Сэмплеры | `29-31` | `s0` = PointClamp (`gSmpPoint`), `s1` = ComparisonLinearClamp (`gSmpLinear`), `s2` = LinearWrap |

### 1.3. Посчитанный baseline каскада 0

```
слайс [0.01, 10];  углы far = (±10, ±5.625, 10)
центроид 8 углов = (0, 0, 5.005);  радиус по центроиду = 12.51 м
+ overlap 2.0 м                   = 14.51 м
unitsPerTexel = 2*14.51/2048      = 14.17 мм/тексель        <-- текущая плотность c0

nearLS ≈ 300 - 14.5 - 150 = 135.5;  farLS ≈ 300 + 14.5 + 25 = 339.5
диапазон = 204 м на D16 -> шаг квантования 3.11 мм
depth bias = 2 текселя = 28 мм peter-panning
```

---

## 2. Что взято из UE 5.6 (сводка идей)

| Идея | Где в UE (ориентир) | Шаг |
|---|---|---|
| Минимальная объемлющая сфера слайса с центром на оси взгляда | `DirectionalLightComponent.cpp` → `GetShadowSplitBoundsDepthRange` | S1 |
| Распределение сплитов по экспоненте (`CascadeDistributionExponent = 3`) | там же → `ComputeAccumulatedScale`, `GetSplitDistance` | справка |
| Индивидуальное разрешение каскада (`r.Shadow.MaxCSMResolution`) + аллокатор атласа | `ShadowSetup.cpp` | S4 |
| `SHADOW_BORDER = 4` текселя + border-scale матрица проекции + clamp UV | `ShadowSetup.cpp`, `ShadowRendering.cpp` → `ShadowmapMinMax` | S5 |
| Slope-scaled + constant bias при **записи** глубины, по нормали вершины (`r.Shadow.CSMDepthBias = 10`, `CSMSlopeScaleDepthBias = 3`) | `ShadowDepthVertexShader.usf`, `ShadowRendering.cpp` → `UpdateShaderDepthBias` | S6 |
| Pancaking: кламп вершины к near-плоскости вместо отсечения | `ShadowDepthVertexShader.usf` → `bClampToNearPlane` | S7 |
| Soft-occlusion (линейная рампа) вместо бинарного сравнения | `ShadowFilteringCommon.ush` → `CalculateOcclusion` | S8 |
| Gather4-based tent PCF (4 тапа = ядро 4×4; 9 тапов = 6×6) | `ShadowFilteringCommon.ush` → `PCF3x3gather`, `Manual5x5PCF` | S8 |
| Ослабление рампы по NoL (`r.Shadow.CSMReceiverBias = 0.9`) | `ShadowProjectionPixelShader.usf` | S8 |
| Коррекция переблюра PCF (`Square`) и `ShadowSharpen` | `ShadowProjectionPixelShader.usf` | S8 |
| Receiver-plane depth bias с клампом наклона на 5° | `ShadowPercentageCloserFiltering.ush` | S9 |
| Fade-плоскость (`CascadeTransitionFraction = 0.1`) и затухание последнего каскада (`ShadowDistanceFadeoutFraction = 0.1`) | `DirectionalLightComponent.cpp` → `GetShadowSplitBounds` | S10 |
| Scissor-оптимизация каскада (`r.Shadow.CSMScissorOptim`) | `ShadowSetup.cpp` → `ComputeScissorRectOptim` | S11 |
| Кэш CSM со скроллингом (`r.Shadow.CSMCaching`, `SDCM_CSMScrolling`) | `ShadowSetup.cpp` | S13 |

**Важное наблюдение.** Текущие сплиты `{10, 35, 100, 300}` — это практически ровно UE-распределение
с экспонентой 3. Проверка: UE считает долю дистанции как `sum(exp^i, i<k) / sum(exp^i, i<N)`; для
`exp = 3, N = 4` веса `1,3,9,27` (сумма 40) дают доли `1/40, 4/40, 13/40, 1` → сплиты
`{7.5, 30, 97.5, 300}` при дистанции 300 м. **Схема разбиения уже правильная**, трогать её в первую
очередь не надо — основные потери не в сплитах.

---

## S0. Enabler: рантайм-конфиг и отладочная визуализация

**Делать первым.** Без этого шаги S1/S2/S4/S7 нечем измерить, а S5/S10/S11 нечем проверить.

### S0.1. Диагностический readout по каскадам

Добавить в `sources/app/scene/SceneFrameData.h` в `CascadeData` (рядом с существующими массивами):

```cpp
    // S0: диагностика для ImGui (заполняется в Scene::UpdateCascades, ничем не потребляется на GPU).
    float dbgSphereRadius[kCascades] = {};   // радиус до padding
    float dbgRadius[kCascades]       = {};   // радиус после padding
    float dbgUnitsPerTexel[kCascades]= {};   // мировых единиц на тексель
    float dbgNearLS[kCascades]       = {};
    float dbgFarLS[kCascades]        = {};
    uint32_t dbgTileSize[kCascades]  = {};
```

Заполнять в конце тела цикла `Scene::UpdateCascades` (`Scene.cpp`, после строки 261). В
`sources/app/ui/DeveloperWindow.cpp` вывести таблицу; ключевые производные величины считать на месте:

```cpp
// В секции "CSM" DeveloperWindow
for (int c = 0; c < 4; ++c)
{
    const float range = cascades.dbgFarLS[c] - cascades.dbgNearLS[c];
    ImGui::Text("c%d  tile=%u  texel=%.2f mm  R=%.2f/%.2f m  zRange=%.1f m  D16 step=%.2f mm  bias=%.1f mm",
        c, cascades.dbgTileSize[c],
        cascades.dbgUnitsPerTexel[c] * 1000.0f,
        cascades.dbgSphereRadius[c], cascades.dbgRadius[c],
        range,
        (range / 65535.0f) * 1000.0f,
        cascades.depthBiasNDC[c] * range * 1000.0f);
}
```

**Это те числа, по которым принимаются приёмочные решения в S1, S2, S4, S7.**

### S0.2. Рантайм-конфиг

`CascadeShadowConfig` (`SceneRenderConfig.h:5-22`) — compile-time дефолты. **Секции CSM в
`DeveloperWindow.cpp` сегодня нет вообще** (есть только VSM + shadow-LOD), так что это работа
с нуля, а не «раскрыть существующее». Доступ дешёвый: аксессор `Scene::CascadeConfig()`
(`sources/app/scene/Scene.h:58-59`) уже есть и возвращает неконстантную ссылку; сам
`cascadeConfig_` больше нигде, кроме `Scene::UpdateCascades`, не читается.

Вывести слайдерами (образец — существующая VSM-секция, `DeveloperWindow.cpp:562`):
`maxDistance`, `sliceDistances[4]`, `normalBiasInTexels`, `depthBiasInTexels`, `overlapInTexels`,
`zPadding`, `casterReachWS` + тумблеры шагов S7/S8/S11/S12/S13 и пресет атласа (S4).

Смена размера атласа требует пересоздания GPU-ресурсов на GPU-idle — в проекте такой паттерн уже
есть (переключение режима теней в `Scene::Render`, см. комментарий `Scene.cpp:105-108`).

### S0.3. Debug-режим «cascade tint»

В `LightingPassConstants` (`SceneResourceBootstrapper.h:147`) добавить `uint32_t csmDebugMode = 0;`
(handle + write по образцу `useVsm`), в шейдере — тонировка вклада каскада:

```hlsl
// В csm_sample.hlsli (после S3) вернуть индекс каскада наружу и в lighting_cs.hlsl:
if (csmDebugMode == 1u)
{
    const float3 kTint[4] = { float3(1,0.3,0.3), float3(0.3,1,0.3), float3(0.3,0.5,1), float3(1,1,0.3) };
    outColor.rgb *= kTint[csmCascadeIndex];
}
```

### S0.4. Перф-baseline — ЗАМЕРЕН

Цель документа — «CSM как быстрая альтернатива VSM», поэтому точка отсчёта фиксируется до первой
правки. Замер headless, обе строки — один и тот же уровень, одна и та же камера, один прогон подряд:

```
x64\Release\test_cube.exe --level=data/levels/wind_test.json --shadow-mode=legacy --profdump=<out> --shot-delay=30
x64\Release\test_cube.exe --level=data/levels/wind_test.json --shadow-mode=vsm    --profdump=<out> --shot-delay=30
```

**wind_test, Release, 2560×1440, DLSS Perf (scale 0.58), SSR on, камера `-30.74 4.75 70.70`,
30 с прогрева, живой ветер:**

| | shadow pass GPU (ms) | `Pass_Lighting` GPU | `GPU.Frame` | `CPU.Frame` | FPS |
|---|---|---|---|---|---|
| **Legacy CSM** | `Pass_CSM` **0.456** | 0.027 | **2.011** | 2.032 | **496** |
| **VSM** | `Pass_VsmPageRender` 1.205 + `Pass_VsmPageRequest` 0.062 = **1.267** | 0.032 | **2.800** | 2.817 | **356** |

**Вывод, на который опирается весь документ: CSM уже в 2.8 раза дешевле VSM на самом теневом пасе
и на 0.79 мс дешевле по кадру (+140 FPS).** Это и есть бюджет: у CSM есть **≈0.8 мс GPU** запаса,
прежде чем он перестанет быть «быстрой альтернативой». Все дорогие шаги тратят именно его:

* S4 `Balanced` (c0 2048→3072) — ×2.25 площади c0, самая крупная статья;
* S4 `Quality` (c0 4096) — ×4 площади c0, почти наверняка съедает бюджет целиком;
* S8/S9/S12 — тратят `Pass_Lighting`, который сейчас смехотворные 0.027 мс, т.е. там запас огромен
  (даже +0.3 мс на contact shadows укладывается);
* S11 — единственный шаг, который бюджет возвращает.

Обратите внимание, где узкое место: `Pass_Lighting` = 1.3 % кадра, `Pass_CSM` = 23 %. **Качество
фильтра почти бесплатно, а разрешение атласа — нет.** Это прямо противоречит интуиции «сделаем
атлас побольше» и задаёт приоритет: сначала бесплатное уплотнение (S1/S2) и качество выборки
(S6/S8/S9/S10), и только потом — и только как пресет — разрешение (S4).

Методические заметки:
* `--wind-freeze=3.0` замер **не искажает** (проверено: Legacy 0.467 против 0.456, VSM 1.194 против
  1.205 — в пределах шума), так что его можно свободно добавлять для попиксельно сравнимых `--shot`.
* `VsmPageRender.Scatter` (0.108) — вложенный скоуп внутри `Pass_VsmPageRender`, не прибавлять.
* Прогрев 30 с обязателен: `avg` — это EMA, на коротком прогоне её пробивают спайки загрузки.
  Ориентир — `frame=` в шапке дампа должен быть в тысячах.

### S0.5. Headless-переключатель режима (без него S0.4 не воспроизводится)

Билд стартует в VSM (`g_shadowMode = ShadowMode::VSM`), поэтому без флага любой Legacy-замер
требовал бы Ctrl+V руками — то есть headless A/B двух методов был бы невозможен. Добавлено в
`sources/app/main.cpp` рядом с прочими `--vsm-*`:

* `--shadow-mode=legacy|vsm` — выбирает метод направленных теней на старте;
* `--csm-tint` — включает визуализацию S0.3 из командной строки, чтобы её можно было снять `--shot`.

### Критерий приёмки
Все величины видны в рантайме и меняются при движении камеры; cascade-tint включается и показывает
различимые зоны; изменение слайдеров конфига мгновенно влияет на тени; таблица S0.4 заполнена.

**Что должен показать readout на дефолтном конфиге** (камера `hfov 90°`, aspect 16:9, атлас 4096,
тайл 2048) — это заодно проверка, что цифры действительно берутся из `UpdateCascades`, а не
пересчитываются в UI:

| c | slice | tile | texel | R fit/pad |
|---|---|---|---|---|
| 0 | 0.01–10 | 2048 | **≈14.17 мм** | **12.51 / 14.51 м** |

Совпало — пломбировка верна, и §1.3 документа подтверждён на живом коде.

---

## S1. Минимальная объемлющая сфера каскада

**Зависит от:** ничего. **Эффект:** −8 % размера текселя во всех каскадах. **Риск:** минимальный.

### Почему
`Scene.cpp:183-185` берёт центр сферы как **центроид** 8 углов слайса — это не минимальная сфера.
Минимальная сфера слайса пирамиды всегда имеет центр **на оси взгляда**, и его смещение находится
в закрытой форме из условия равенства расстояний до near- и far-углов:

```
пусть  a = |диагональ far| ,  b = |диагональ near| ,  L = splitFar - splitNear
центр на оси в точке z = c, тогда
   |P_far - C|²  = a² + (splitFar - c)²
   |P_near - C|² = b² + (c - splitNear)²
равенство ->  c = splitFar - [ (b² - a²)/(2L) + L/2 ]
```
Если слайс «широкий» относительно своей длины (большой FOV / короткий слайс), решение уходит
за far-плоскость — тогда центр кладётся **на** far-плоскость, и это по-прежнему минимальная сфера
для такого случая.

Для c0: центроид даёт радиус **12.51 м**, минимальная сфера — **11.474 м** (смещение уходит за far,
центр садится на far-плоскость).

### Код

Вставить в `sources/app/scene/Scene.cpp` рядом с `BuildFrustumSliceCornersWS` (после строки 86):

```cpp
struct CascadeSphere
{
    float3 center{};
    float  radius = 0.0f;
};

// Минимальная объемлющая сфера слайса фрустума, в мировых координатах.
// Центр лежит на оси камеры; его смещение от far-плоскости имеет закрытую форму (см. документ),
// что даёт строго меньший радиус, чем центроид 8 углов. Центр/радиус зависят ТОЛЬКО от
// (splitNear, splitFar, FOV) -> инвариантны к повороту камеры и солнца, на что опирается texel
// snap в UpdateCascades. Радиус всё равно замеряется по реальным углам: это гарантирует, что
// assert «ortho radius under-covers slice corners» продолжает держаться.
static CascadeSphere ComputeCascadeSphere(const Camera& camera,
                                          const std::array<float3, 8>& cornersWS,
                                          float splitNear, float splitFar)
{
    // tan(halfFov) прямо из матрицы проекции: для LH-perspective
    //   m._11 = 1/(aspect*tan(vfov/2))  ->  tan(hfov/2) = 1/m._11
    //   m._22 = 1/tan(vfov/2)           ->  tan(vfov/2) = 1/m._22
    // Берём НЕджиттернутую матрицу: джиттер трогает только _31/_32, но так интент явный.
    const mat4& proj = camera.GetProjMatrixNoJitter();
    const float tanHalfX = 1.0f / std::max(1e-6f, proj.m._11);
    const float tanHalfY = 1.0f / std::max(1e-6f, proj.m._22);

    const float farX  = tanHalfX * splitFar;
    const float farY  = tanHalfY * splitFar;
    const float nearX = tanHalfX * splitNear;
    const float nearY = tanHalfY * splitNear;

    const float diagFarSq  = farX  * farX  + farY  * farY;
    const float diagNearSq = nearX * nearX + nearY * nearY;
    const float sliceLen   = std::max(1e-4f, splitFar - splitNear);

    const float offset  = (diagNearSq - diagFarSq) / (2.0f * sliceLen) + sliceLen * 0.5f;
    const float centreZ = Clamp(splitFar - offset, splitNear, splitFar);

    CascadeSphere out{};
    out.center = camera.GetPosition() + camera.GetDirection() * centreZ;

    float rSq = 0.0f;
    for (const float3& c : cornersWS)
    {
        const float3 d = c - out.center;
        rSq = std::max(rSq, d.Dot(d));
    }
    out.radius = std::max(std::sqrt(rSq), 1.0f); // никогда 0: вырожденная ortho даёт INF-матрицы
    return out;
}
```

Заменить в `Scene::UpdateCascades` блок `Scene.cpp:183-192` (вычисление `sphereCenter`/`sphereRadius`) на:

```cpp
        const CascadeSphere sphere = ComputeCascadeSphere(camera, cornersWS, sliceNear, sliceFar);
        const float3 sphereCenter = sphere.center;
        const float  sphereRadius = sphere.radius;
```

Остальное (`radius`, `unitsPerTexel`, снап, z-диапазон) **не менять** — это S2/S7.

### Критерий приёмки
* `texel` каскада 0 в readout (S0): **≈ 13.16 мм** вместо 14.17 мм.
* Assert `Scene.cpp:237` не срабатывает при вращении камеры и солнца, включая крайние углы
  (солнце в зените, солнце у горизонта, камера смотрит вверх/вниз до упора).
* Визуально нет edge crawl / мерцания при движении и вращении камеры.

### Откат
Вернуть центроид (10 строк).

---

## S2. `overlap` — в текселях, а не в метрах

**Зависит от:** S1 (желательно). **Эффект:** −14 % размера текселя. **Риск:** минимальный.

### Почему
`SceneRenderConfig.h:11` задаёт `overlap = 2.0f` **мировых единиц**, `Scene.cpp:194` делает
`radius = sphereRadius + overlap`. Комментарий `Scene.cpp:235-237` объясняет, что это padding под
сдвиг от texel snap — а `std::floor` сдвигает центр не более чем на **1 тексель** по каждой оси
(≈ 14 мм для c0). 2 метра padding на радиус 11.5 м — ~15 % выброшенной плотности.

### Код

`sources/app/scene/SceneRenderConfig.h` — заменить поле:

```cpp
    // S2: padding под сдвиг texel snap, в ТЕКСЕЛЯХ каскада (не в мировых единицах).
    // std::floor в UpdateCascades сдвигает центр максимум на 1 тексель по каждой оси; 2 — запас.
    float overlapInTexels = 2.0f;
```

`sources/app/scene/Scene.cpp` — заменить строки 194-195:

```cpp
        // Два прохода: overlap задан в текселях, а размер текселя зависит от radius, который
        // зависит от overlap. Поправка второго порядка ~0.2 %, одной итерации достаточно.
        const float texelEstimate = (2.0f * sphereRadius) / static_cast<float>(tileRes);
        const float radius        = sphereRadius + cascadeConfig_.overlapInTexels * texelEstimate;
        const float unitsPerTexel = (2.0f * radius) / static_cast<float>(tileRes);
```

Других обращений к `cascadeConfig_.overlap` в проекте **нет** (проверено: `cascadeConfig_` читается
только в `Scene::UpdateCascades`), так что правка локальна — плюс слайдер в S0.2.

### Критерий приёмки
* `texel` каскада 0: **≈ 11.2 мм** (с S1) вместо 14.17 мм → **−21 % суммарно**.
* Никакого edge crawl при медленном движении камеры — главный риск шага, проверять внимательно
  на длинной прямой тени от вертикального объекта.
* Assert `Scene.cpp:237` не срабатывает.

### Откат
Вернуть константу 2.0f в мировых единицах.

---

## S3. Вынести сэмплирование CSM в общий `shaders/csm_sample.hlsli`

**Зависит от:** ничего. **Эффект:** 0 визуально в lighting-пути; **обязательный enabler** для S8/S9/S10/S12. **Риск:** средний (рефакторинг).

### Почему
Логика существует в двух копиях: `lighting_cs.hlsl:80-181` и `glass.hlsl:176-233`. Вторая уже
разошлась (нет fallback-цепочки и blend-полосы). VSM-путь вынесен правильно (`vsm_sample.hlsli`) —
сделать симметрично.

### Код

**Новый файл `shaders/csm_sample.hlsli`** (побитово воспроизводит текущее поведение
`lighting_cs.hlsl`; расширения добавляются в S8/S9/S10):

```hlsl
// Сэмплирование каскадных теней (Legacy CSM). Вынесено из lighting_cs.hlsl, чтобы glass.hlsl
// не держал вторую расходящуюся копию (VSM-путь так же вынесен в vsm_sample.hlsli).
// ВСЁ передаётся аргументами: у lighting_cs.hlsl и glass.hlsl РАЗНЫЕ cbuffer-layout'ы, поэтому
// этот заголовок не имеет права обращаться ни к одному глобальному полю.
#ifndef CSM_SAMPLE_HLSLI
#define CSM_SAMPLE_HLSLI

struct CsmParams
{
    float4x4 lightViewProj[4];
    float4   scaleBias[4];    // xy = atlas scale, zw = atlas bias
    float4   splitsVS;        // x = near (не используется), yzw = far каскадов 0..2
    float4   depthBiasNDC;
    float4   normalBiasWS;
    float2   atlasSize;       // (W, H) в текселях; после S4 НЕ обязательно квадрат
    float3   camPosWS;
    float3   camDirWS;        // нормализован
    float    pcfRadius;       // радиус фильтра в текселях (сейчас 1.0)
};

// Доля сплита, на которой каскад c кросс-фейдится в каскад c+1.
static const float CSM_BLEND_FRACTION = 0.1f;

int CsmChooseCascade(CsmParams p, float3 Pws)
{
    const float z = dot(Pws - p.camPosWS, p.camDirWS);
    const float3 gt = saturate(sign(z.xxx - p.splitsVS.yzw));
    return (int)(gt.x + gt.y + gt.z);
}

float CsmPcf3x3(Texture2D atlas, SamplerComparisonState cmp,
                float2 uv, float zRef, float2 texel, float radiusPx)
{
    float s = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            s += atlas.SampleCmpLevelZero(cmp, uv + float2(x, y) * texel * radiusPx, zRef).r;
        }
    }
    return s / 9.0f;
}

// Сэмплирует начиная с каскада `start`, откатываясь на более грубый, если точка (после normal
// offset) вышла за его тайл. Тест идёт по КАСКАД-ЛОКАЛЬНОМУ UV (до atlas scale+bias), поэтому
// соседний тайл никогда не сэмплируется; отступ на радиус PCF не даёт 3x3 залезть за границу
// (в S5 этот отступ заменяется на настоящий gutter + clamp).
// Возвращает 1.0 (освещено) только за последним каскадом.
float CsmSampleChain(CsmParams p, Texture2D atlas, SamplerComparisonState cmp,
                     int start, float3 Pws, float3 Nws, float NdotL, out int outCascade)
{
    const float2 texel = 1.0f / p.atlasSize;
    outCascade = 3;

    [unroll] for (int c = 0; c < 4; ++c)
    {
        if (c < start) { continue; }

        const float2 scale  = p.scaleBias[c].xy;
        const float2 biasUV = p.scaleBias[c].zw;

        // Пересчёт смещения на каждый каскад: у каждого свой размер текселя.
        const float3 Poff = Pws + Nws * p.normalBiasWS[c];
        const float4 lc = mul(float4(Poff, 1.0f), p.lightViewProj[c]);
        const float2 uvLocal = (lc.xy / max(1e-6f, lc.w)) * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
        const float  z = lc.z / max(1e-6f, lc.w);

        const float2 margin = (p.pcfRadius * texel) / max(1e-6f, scale);
        if (any(uvLocal < margin) || any(uvLocal > 1.0f - margin)) { continue; }

        const float2 uv = uvLocal * scale + biasUV;
        const float bBase = p.depthBiasNDC[c];
        const float b = bBase + (1.0f - saturate(NdotL)) * bBase;

        // Радиус PCF масштабируется по каскаду так, чтобы МИРОВАЯ полутень была привязана к c0,
        // а не росла вместе с каскадом. normalBiasWS[c] пропорционален мировому текселю каскада c,
        // поэтому его отношение к c0 и есть нужный масштаб (множитель normalBiasInTexels сокращается).
        const float pcfR = p.pcfRadius *
            pow(p.normalBiasWS[0] / max(1e-6f, p.normalBiasWS[c]), 0.25f);

        outCascade = c;
        return CsmPcf3x3(atlas, cmp, uv, z - b, texel, pcfR);
    }
    return 1.0f;
}

float CsmSampleShadow(CsmParams p, Texture2D atlas, SamplerComparisonState cmp,
                      float3 Pws, float3 Nws, float NdotL, out int outCascade)
{
    const int idx = CsmChooseCascade(p, Pws);
    float shadow = CsmSampleChain(p, atlas, cmp, idx, Pws, Nws, NdotL, outCascade);

    // Blend-полоса перед far-сплитом: жёсткое переключение каскада (и разрыв bias / плотности
    // текселя / радиуса PCF) превращается в градиент. У каскада 3 более грубого соседа нет.
    if (idx < 3)
    {
        const float zView = dot(Pws - p.camPosWS, p.camDirWS);
        const float splitNext = idx == 0 ? p.splitsVS.y : (idx == 1 ? p.splitsVS.z : p.splitsVS.w);
        const float band = splitNext * CSM_BLEND_FRACTION;
        const float t = saturate((zView - (splitNext - band)) / max(1e-4f, band));
        if (t > 0.0f)
        {
            int dummy;
            const float shadowNext = CsmSampleChain(p, atlas, cmp, idx + 1, Pws, Nws, NdotL, dummy);
            shadow = lerp(shadow, shadowNext, t);
        }
    }
    return shadow;
}

#endif // CSM_SAMPLE_HLSLI
```

**`shaders/lighting_cs.hlsl`** — удалить строки `80-181` (`ChooseCascadeIndex`, `ShadowPCF3x3`,
`kBlendFraction`, `SampleCascadeChain`, `SampleShadowCSM`) и `pcfRadius` на `:78`; добавить
`#include "csm_sample.hlsli"` рядом с `#include "vsm_sample.hlsli"` (`:14`) и вставить:

```hlsl
CsmParams MakeCsmParams()
{
    CsmParams p;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        p.lightViewProj[i] = lightViewProj[i];
        p.scaleBias[i]     = cascadeScaleBias[i];
    }
    p.splitsVS     = cascadeSplitsVS;
    p.depthBiasNDC = shadowBiasNDC;
    p.normalBiasWS = normalBiasWS;
    p.atlasSize    = shadowAtlasSize;
    p.camPosWS     = camPosWS;
    p.camDirWS     = camDirWS;
    p.pcfRadius    = 1.0f;
    return p;
}
```

и переписать `SampleSunShadow` (`:187-195`):

```hlsl
float SampleSunShadow(float3 P, float3 N, float ndl, out int outCascade)
{
    outCascade = 0;
    if (useVsm != 0u)
    {
        return VsmClipmapShadow(P, N, camPosWS, clipmapBaseExtent, clipmapNormalBias, vsmDepthBias,
                                clipmapViewProj, VsmPageTable, VsmPool, gSmpLinear);
    }
    return CsmSampleShadow(MakeCsmParams(), ShadowAtlas, gSmpLinear, P, N, ndl, outCascade);
}
```

Обновить оба вызова `SampleSunShadow` в `CSMain` (основной и transmission-лоб с `-N`) —
для transmission передавать отдельный `int` и игнорировать.

**`shaders/glass.hlsl`** — удалить `ChooseCascadeIndex` (`:176-183`), `ShadowPCF3x3Texture`
(`:185-199`) и тело `SampleShadowCSM` (`:201-233`); добавить `#include "csm_sample.hlsli"` после
`:7` и заменить на:

```hlsl
float SampleShadowCSM(float3 Pws, float3 Nws, float NdotL)
{
    if (vsmParams.x != 0.0f)
    {
        return VsmClipmapShadow(Pws, Nws, camPosSky.xyz, clipmapParams.x, clipmapParams.y,
                                clipmapParams.z, clipmapViewProj, VsmPageTable, VsmPool, ShadowSampler);
    }
    CsmParams p;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        p.lightViewProj[i] = lightViewProj[i];
        p.scaleBias[i]     = cascadeScaleBias[i];
    }
    p.splitsVS     = cascadeSplitsVS;
    p.depthBiasNDC = shadowBiasNDC;
    p.normalBiasWS = normalBiasWS;
    p.atlasSize    = shadowAtlasSizeInv.xy;      // xy = размер атласа (см. GlassView, glass.hlsl:55)
    p.camPosWS     = camPosSky.xyz;
    p.camDirWS     = normalize(camDirWS.xyz);
    p.pcfRadius    = 1.0f;

    int cascade;
    return CsmSampleShadow(p, ShadowAtlas, ShadowSampler, Pws, Nws, NdotL, cascade);
}
```

Третьей копии логики каскадов нет (проверено): `shaders/debug_texture.hlsl` только визуализирует
атлас, `shaders/spotlight_cs.hlsl` работает с отдельным `SpotShadowAtlas`.

**⚠ Перф-риск рефакторинга.** `CsmParams` передаётся **по значению** и несёт 4×`float4x4` +
4×`float4` + хвост скаляров (≈ 350 Б). После инлайна DXC это обычно разбирает (SROA), но
`lighting_cs.hlsl` чувствителен к occupancy, а критерий «скриншот идентичен» просадку не поймает.
Если `Pass_Lighting` вырастет — передавать `CsmParams` через `inout`/`in` ссылку либо разбить
на «дешёвую часть + индекс каскада», не таща все 4 матрицы в каждый вызов.

### Критерий приёмки
* Скриншот сцены до/после для lighting-пути — **идентичен** (`sources/rendering/core/Screenshot.cpp`).
* **`Pass_Lighting` не выросло** относительно S0.4 (порог: +2 %). Это отдельный критерий, потому
  что визуально шаг no-op и регрессию видно только в профайлере.
* Стекло теперь сэмплирует каскады так же, как остальная геометрия (проверить на границе каскадов
  и на границе тайла) — намеренное исправление, зафиксировать в сообщении коммита.
* Оба шейдера компилируются (hot-reload покажет ошибки сразу).

### Откат
Удалить файл, вернуть две копии.

---

## S4. Индивидуальное разрешение каскадов + rect-таблица атласа

**Зависит от:** ничего (делать после S1/S2). **Эффект:** ×1.5…×2 плотности c0 — это и есть «лютый нулевой каскад». **Риск:** средний.

### Почему
UE не раскладывает каскады сеткой: каждый каскад — самостоятельная запись со своим разрешением,
которую аллокатор кладёт в атлас. Именно поэтому нулевой каскад можно поднять независимо.

Шейдер **уже resolution-agnostic** — работает через `cascadeScaleBias[c]` и считает margin как
`texel / scale`. Менять надо только CPU-раскладку.

### Код

**1) `sources/rendering/core/RenderConstants.h`** — добавить (в `namespace render`, рядом с
`kMaxShadowViews`; стиль `inline`-глобала соответствует `render::g_indirectShadowsEnabled` и
`vsm::g_clipmapDepthBias`):

```cpp
    // S4: раскладка CSM-атласа. ЕДИНСТВЕННЫЙ источник правды — до этого сетка 2x2 была
    // захардкожена в двух местах (Scene::UpdateCascades и Renderer::BindShadowTarget),
    // что уже было готовым источником рассинхрона.
    struct CascadeTile
    {
        std::uint32_t x = 0, y = 0, size = 0;
    };

    struct CascadeAtlasLayout
    {
        std::uint32_t width  = 4096;
        std::uint32_t height = 4096;
        std::uint32_t border = 0;                 // gutter-тексели внутри тайла (задействуется в S5)
        std::array<CascadeTile, 4> tiles{};

        // Размер области, в которую реально рисуется каскад (S5). До S5 border == 0.
        std::uint32_t ContentSize(std::size_t c) const { return tiles[c].size - 2u * border; }
    };

    enum class CascadeAtlasPreset : std::uint32_t { Legacy = 0, Balanced = 1, Quality = 2 };

    inline CascadeAtlasLayout MakeCascadeAtlasLayout(CascadeAtlasPreset preset, std::uint32_t border)
    {
        CascadeAtlasLayout L{};
        L.border = border;
        switch (preset)
        {
        case CascadeAtlasPreset::Legacy:   // ровно текущее поведение: сетка 2x2 по 2048, 32 МБ
            L.width = 4096; L.height = 4096;
            L.tiles = { { {0, 0, 2048}, {2048, 0, 2048}, {0, 2048, 2048}, {2048, 2048, 2048} } };
            break;
        case CascadeAtlasPreset::Balanced: // та же память (32 МБ), c0 получает x1.5 плотности
            L.width = 4096; L.height = 4096;
            L.tiles = { { {0, 0, 3072}, {3072, 0, 1024}, {3072, 1024, 1024}, {3072, 2048, 1024} } };
            break;
        case CascadeAtlasPreset::Quality:  // 64 МБ на кадр, c0 получает x2 плотности
            L.width = 8192; L.height = 4096;
            L.tiles = { { {0, 0, 4096}, {4096, 0, 2048}, {6144, 0, 2048}, {4096, 2048, 2048} } };
            break;
        }
        return L;
    }

    // Тумблер из ImGui (S0.2). Смена требует пересоздания ресурсов на GPU-idle.
    inline CascadeAtlasPreset g_cascadeAtlasPreset = CascadeAtlasPreset::Legacy;
    inline std::uint32_t      g_cascadeAtlasBorder = 0;   // 4 после S5
```

Степени двойки для размера тайла не требуются: viewport/scissor и rect-таблица работают с любым
размером.

**⚠ Память.** Атлас 4096×4096 @ R16 = **33.5 МБ**, и существует в `render::kFrameCount` копиях
(`RenderTargetManager.cpp:384-405`).
* `Balanced` не использует нижнюю полосу `4096×1024` = **8.4 МБ** (25 % атласа), а не 4 МБ.
  Если это мешает — переложить c1..c3 в эту полосу и уменьшить высоту атласа до 3072
  (25 МБ вместо 33.5); раскладка на то и вынесена в таблицу, чтобы это была правка одной строки.
* `Quality` = 8192×4096 = 67 МБ на кадр, **~201 МБ VRAM** на 3 кадра (не 192). Залогировать
  при создании.

**⚠ Перф-бюджет — ключевой для этого шага.** Цель документа — CSM быстрее VSM, а этот шаг
единственный, который платит временем напрямую: c0 растёт 2048² → 3072² (**×2.25** площади
растеризации самого нагруженного каскада), `Quality` — 2048² → 4096² (**×4**). Поэтому:
* дефолт остаётся **`Legacy`**; `Balanced`/`Quality` включаются осознанно из S0.2;
* в приёмке — не «рост соответствует площади», а **записанные числа** против таблицы S0.4,
  и явный вывод, остаётся ли `Balanced` дешевле VSM. Если `Pass_CSM` в `Balanced` подходит к
  VSM-строке — весь смысл теряется, и правильный ответ не «принять», а «сократить `maxDistance`
  / радиус c0 через S0.2 и добрать плотность там» (см. последнюю строку таблицы в §3).

**2) `sources/rendering/core/RenderTargetManager.h`** — заменить `UINT shadowRes = 4096;` (`:87`) на:

```cpp
        UINT shadowAtlasW = 4096;
        UINT shadowAtlasH = 4096;
        render::CascadeAtlasLayout cascadeLayout{};   // раскладка, с которой атлас был создан
```

**3) `RenderTargetManager.cpp`** — в `CreateShadowResource` (`:474-520`) принимать `width`/`height`
вместо одного `resolution` (`rd.Width = width; rd.Height = height;`), а на вызове (`:404-405`):

```cpp
        D.cascadeLayout = render::MakeCascadeAtlasLayout(render::g_cascadeAtlasPreset,
                                                        render::g_cascadeAtlasBorder);
        D.shadowAtlasW = D.cascadeLayout.width;
        D.shadowAtlasH = D.cascadeLayout.height;
        CreateShadowResource(dev, tracker, f, D.shadowAtlasW, D.shadowAtlasH);
```

Путь «сжать до 1×1 в VSM-режиме» (`:662`) — `CreateShadowResource(dev, tracker, f, full ? D.shadowAtlasW : 1u, full ? D.shadowAtlasH : 1u)`.

**⚠ Не «чинить» здесь рассинхрон описания и ресурса.** Сегодня этот путь ужимает только сам
ресурс, а `D.shadowRes` остаётся 4096; `D.cascadeLayout` вести себя должен **точно так же** —
оставаться полным. Соблазн записать вырожденную раскладку (`width = height = 1`) и «честно»
поймать её guard'ом `atlasReady` **вреден**:

* существующая ветка обнуления ставит `view.frustum = Frustum{}`, а невалидный `Frustum` в
  `Intersects()` возвращает **`true` (принять всё)**, а не reject-all;
* `cascadeViews_[i].frustum` безусловно уходит в `shadowGpu_.UpdateViewFrustums` слотами 0..3
  (`Scene.cpp:1107`) и в `enqueueView` (`Scene.cpp:1019`) — **в обоих режимах**.

То есть вырожденная раскладка в VSM-режиме превратила бы каскадный куллинг из «отбраковывает» в
«пропускает всё» и добавила бы работы там, где её сейчас нет. Guard `atlasReady` остаётся ровно
тем, чем был guard `tileRes == 0`: защитой от «атлас ещё не создан», и в VSM-режиме не срабатывает.

*(Отдельное наблюдение, вне рамок этого документа: каскадные view куллятся каждый кадр и в
VSM-режиме, хотя `Main_CSM` в графе отсутствует. Слоты 0..3 в `UpdateViewFrustums` можно было бы
занулять при `VsmActive()` тем же приёмом, каким Legacy зануляет клипмап-слоты, `Scene.cpp:1119-1124`.
Это чистая экономия в VSM-пути, к CSM отношения не имеет — отдельная задача.)*

**4) `sources/app/scene/Scene.cpp`** — в `UpdateCascades` заменить `tileRes` (`:144`) и блок
atlas scale/bias (`:258-261`):

```cpp
    const auto& deferred = renderer->GetDeferredForFrame();
    const render::CascadeAtlasLayout& layout = deferred.cascadeLayout;
    const bool atlasReady = layout.width > 1u && layout.height > 1u && layout.tiles[0].size > 0u;
    if (!atlasReady)
    {
        /* ... существующая ветка обнуления cascadeViews_ ... */
        return;
    }
```
внутри цикла по каскадам, вместо `tileRes`:
```cpp
        const uint32_t tileRes = layout.ContentSize(idx);   // до S5 == tiles[idx].size
```
и вместо `:258-261`:
```cpp
        const float atlasW = static_cast<float>(layout.width);
        const float atlasH = static_cast<float>(layout.height);
        const render::CascadeTile& tile = layout.tiles[idx];
        cascades.atlasScale[idx] = float2(static_cast<float>(tile.size) / atlasW,
                                          static_cast<float>(tile.size) / atlasH);
        cascades.atlasBias[idx]  = float2(static_cast<float>(tile.x) / atlasW,
                                          static_cast<float>(tile.y) / atlasH);
```

**5) `sources/rendering/core/Renderer.cpp`** — заменить `BindShadowTarget` (`:1465-1491`) целиком:

```cpp
void Renderer::BindShadowTarget(ID3D12GraphicsCommandList* cl, int cascadeIndex, bool clearDepth)
{
    auto& D = rtManager_.Deferred(currentFrameIndex_);

    // Один DSV на весь атлас.
    cl->OMSetRenderTargets(0, nullptr, FALSE, &D.shadowDSV);

    if (clearDepth)
    {
        cl->ClearDepthStencilView(D.shadowDSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        return;
    }

    // S4: раскладка берётся из ЕДИНОЙ таблицы (render::CascadeAtlasLayout), а не дублируется здесь.
    const render::CascadeAtlasLayout& layout = D.cascadeLayout;
    const int idx = Clamp(cascadeIndex, 0, static_cast<int>(layout.tiles.size()) - 1);
    const render::CascadeTile& tile = layout.tiles[idx];

    const D3D12_VIEWPORT vp{
        static_cast<float>(tile.x), static_cast<float>(tile.y),
        static_cast<float>(tile.size), static_cast<float>(tile.size), 0.0f, 1.0f };
    const D3D12_RECT sc{
        static_cast<LONG>(tile.x), static_cast<LONG>(tile.y),
        static_cast<LONG>(tile.x + tile.size), static_cast<LONG>(tile.y + tile.size) };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
}
```

**6) `sources/app/scene/SceneRenderer.cpp`** — `shadowAtlasSizeInv` (`:261-263`) и
`shadowAtlasSize` (`:1894-1895`) становятся несимметричными:

```cpp
        const float shadowW = static_cast<float>(std::max(deferred.shadowAtlasW, 1u));
        const float shadowH = static_cast<float>(std::max(deferred.shadowAtlasH, 1u));
        vc.shadowAtlasSizeInv = float4(shadowW, shadowH, 1.0f / shadowW, 1.0f / shadowH);
```
```cpp
        constants.shadowAtlasSize = float2(
            static_cast<float>(std::max(renderer->GetDeferredForFrame().shadowAtlasW, 1u)),
            static_cast<float>(std::max(renderer->GetDeferredForFrame().shadowAtlasH, 1u)));
```

**7) `shaders/csm_sample.hlsli`** — `pow(normalBiasWS[0]/normalBiasWS[c], 0.25)` формально
продолжает работать (в `normalBiasWS` уже заложен `unitsPerTexel`), но становится неочевидным.
Добавить явное поле в `CsmParams` и в CB:

```cpp
// LightingPassConstants (SceneResourceBootstrapper.h:147) + handle + write:
    float4 cascadeTexelWorld{};   // мировых единиц на тексель, по каскадам (S4)
```
```hlsl
// csm_sample.hlsli: в CsmParams
    float4 texelWorld;
// и в CsmSampleChain вместо normalBias-отношения:
    const float pcfR = p.pcfRadius * pow(p.texelWorld[0] / max(1e-6f, p.texelWorld[c]), 0.25f);
```
Заполнять `cascades.dbgUnitsPerTexel[idx]` (S0) → `constants.cascadeTexelWorld`. Для `glass.hlsl`
добавить `float4 cascadeTexelWorld;` в `cbuffer GlassView` (`glass.hlsl:41-66`) и в
`GlassViewCB` (`SceneRenderer.cpp:141+`) — **в хвост**, чтобы не сдвинуть существующие смещения.

### Критерий приёмки
* `TextureDebugViewer` (`sources/rendering/debug/TextureDebugViewer.cpp`) показывает новую раскладку:
  c0 занимает большой тайл, c1..c3 — маленькие; ни один каскад не рисует за пределы своего тайла.
* `Legacy`-пресет даёт **идентичный** baseline-скриншот (проверка, что рефакторинг корректен).
* На `Balanced`/`Quality` тени в c0 заметно резче, на границах c0↔c1 нет разрывов.
* VSM-режим (Ctrl+V) переключается без крэша, атлас сжимается до 1×1.
* **Перф — числа, а не «соответствует».** Заполнить и приложить к коммиту:

  | Пресет | `Pass_CSM` GPU | Δ к baseline S0.4 | дешевле VSM? |
  |---|---|---|---|
  | `Legacy` | | должно быть **0 %** (проверка корректности рефакторинга) | |
  | `Balanced` | | | |
  | `Quality` | | | |

  `Legacy` обязан совпасть с baseline и по времени, и по скриншоту — иначе рефакторинг раскладки
  что-то сломал. Если `Balanced` подходит к VSM-строке по `Pass_CSM` — шаг **не принимать как
  дефолт**: цель документа (CSM дешевле VSM) при этом теряется; вместо повышения разрешения
  сокращать покрытие c0 через S0.2 (последняя строка таблицы §3 даёт −76 % текселя без роста атласа).

### Откат
`g_cascadeAtlasPreset = Legacy`.

---

## S5. Настоящий gutter + clamp UV вместо fallback по границе тайла

**Зависит от:** S3, S4. **Эффект:** убирает видимое «кольцо» смены резкости; **обязателен** перед S8. **Риск:** низкий.

### Почему
Сейчас при попадании в margin шириной в радиус PCF происходит `continue` — пиксель молча берётся
из следующего, более грубого каскада. Это видимое кольцо.

UE резервирует 4 текселя внутри тайла, домножая проекцию на border-scale матрицу (контент рисуется
во внутренний прямоугольник), и при сэмплировании клампит UV границами контента.

**Почему border нужен, если UV клампится:** `Gather()` (S8) выбирает 2×2 квад по округлению UV
аппаратно, и float-погрешность точно на границе может втянуть тексель соседнего тайла. 4 текселя
гарантированно очищенного (значение clear = 1.0) поля делают это безвредным.

### Код

**1) Включить border:** `render::g_cascadeAtlasBorder = 4;` (S4, п.1). `ContentSize(c)` уже
учитывает это, и `tileRes` в `UpdateCascades` автоматически становится content-размером.

**2) Border-scale проекции.** В `Scene::UpdateCascades` после построения `lightProj` (`Scene.cpp:251`):

```cpp
        // S5: контент каскада должен занять внутренние ContentSize текселей тайла, оставив
        // border-полосу со значением clear (1.0 = «далеко» = «не в тени»). Ortho отображает
        // [-radius, radius] в NDC [-1,1]; масштаб NDC на k = content/tile сжимает изображение
        // в центральную k-долю тайла. Пост-умножение (row-vector конвенция: clip = pos*view*proj*S).
        const float borderK = static_cast<float>(layout.ContentSize(idx)) /
                              static_cast<float>(layout.tiles[idx].size);
        const mat4 lightProjBordered = lightProj * mat4::Scaling(borderK, borderK, 1.0f);
```
Дальше использовать `lightProjBordered` **везде**, где раньше стоял `lightProj`
(`cascades.lightProj[idx]`, `cascadeView.proj`, `cascadeView.invProj`). Frustum для куллинга
(`Frustum::FromOrthoBounds`, `Scene.cpp:270`) оставить по **немасштабированным** `minX..maxY`:
мировая область каскада не изменилась, изменилось только её место в тайле.

`unitsPerTexel` уже считается по content-размеру (п.1) — bias автоматически корректен.

**3) UV-границы контента в CB.** `LightingPassConstants` + `GlassViewCB` (в хвост) + handles + write:

```cpp
    std::array<float4, 4> cascadeUvMinMax{};   // S5: (uMin, vMin, uMax, vMax) контента в UV атласа
```
Заполнение в `SceneRenderer.cpp` рядом с `cascadeScaleBias` (`:1888-1892`):
```cpp
        const render::CascadeAtlasLayout& layout = renderer->GetDeferredForFrame().cascadeLayout;
        const float atlasW = static_cast<float>(std::max(layout.width, 1u));
        const float atlasH = static_cast<float>(std::max(layout.height, 1u));
        for (size_t i = 0; i < constants.cascadeUvMinMax.size(); ++i)
        {
            const render::CascadeTile& t = layout.tiles[i];
            const float b = static_cast<float>(layout.border);
            // Полтекселя внутрь: билинейный след тапа целиком остаётся в контенте.
            constants.cascadeUvMinMax[i] = float4(
                (t.x + b + 0.5f) / atlasW,
                (t.y + b + 0.5f) / atlasH,
                (t.x + t.size - b - 0.5f) / atlasW,
                (t.y + t.size - b - 0.5f) / atlasH);
        }
```

**4) `shaders/csm_sample.hlsli`** — добавить в `CsmParams` поле `float4 uvMinMax[4];` и заменить
блок margin (`CsmSampleChain`) на:

```hlsl
        // S5: провал на более грубый каскад ТОЛЬКО когда точка реально вне мировой области
        // каскада, а не когда она рядом с границей тайла. Тапы фильтра клампятся к области
        // контента, поэтому ни соседний тайл, ни border не сэмплируются.
        if (any(uvLocal < 0.0f) || any(uvLocal > 1.0f)) { continue; }

        const float2 uv = clamp(uvLocal * scale + biasUV, p.uvMinMax[c].xy, p.uvMinMax[c].zw);
```
и в `CsmPcf3x3` добавить параметр `float4 uvClamp` с клампом каждого тапа:
```hlsl
float CsmPcf3x3(Texture2D atlas, SamplerComparisonState cmp,
                float2 uv, float zRef, float2 texel, float radiusPx, float4 uvClamp)
{
    float s = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const float2 t = clamp(uv + float2(x, y) * texel * radiusPx, uvClamp.xy, uvClamp.zw);
            s += atlas.SampleCmpLevelZero(cmp, t, zRef).r;
        }
    }
    return s / 9.0f;
}
```

### Критерий приёмки
* Медленное панорамирование: на границах тайлов нет кольца/скачка резкости и нет «просвечивания»
  соседнего каскада (характерный артефакт — тень от другого места сцены).
* В `TextureDebugViewer` border-полоски внутри тайлов остались белыми (= 1.0).
* Cascade-tint (S0.3) показывает, что переход между каскадами теперь идёт по blend-полосе,
  а не по границе тайла.

### Откат
`g_cascadeAtlasBorder = 0` (тогда `borderK == 1`, clamp становится no-op'ом по построению).

---

## S6. Slope-scaled и constant depth bias в shadow-depth vertex shader

**Зависит от:** ничего. **Эффект:** снижение acne на скошенных поверхностях без роста общего bias. **Риск:** средний — 5 шейдеров, 2 input layout'а, PerView CB, общий с VSM.

### ⚠ Этот шаг стоит времени в VSM-режиме — гейтить обязательно

Правило 3 в шапке говорит про общий **PSO** и общий **CB**. Здесь их мало: общий ещё и
**input layout**, а VSM per-page draw loop — самый горячий вершинный путь движка (тысяча страниц
за кадр). После этого шага VSM платит:

* **+12 Б выборки на вершину** — новый элемент `NORMAL` в `PosOnly_InstCasterId` /
  `PosUV_InstCasterId` (п. 8), которые используются и CSM-каскадами, и VSM-страницами;
* **~15 ALU на вершину** — `normalize` + `dot` + `sqrt` + `clamp` в `ApplyShadowDepthBias`,
  причём в VSM-режиме все четыре параметра равны нулю, то есть это чистые потери.

Поэтому ALU **обязательно** гейтится ранним выходом (п. 6), а после шага замеряется
`VsmPageRender` — см. приёмку. Если гейта окажется мало (ветвление не спасает от выборки нормали),
следующая ступень — отдельная перестановка PSO `SHADOW_DEPTH_BIAS=0/1` с собственным input
layout'ом: механика перестановок в этом шейдере уже есть (`SHADOW_MASKED`, `shadow_indirect_csm.hlsl:12-19`).

### Почему
Bias применяется только при **сэмплировании**. UE применяет его при **записи** глубины, причём
slope-компоненту — по нормали вершины: `slope = tan(угол между поверхностью и светом) =
sqrt(1-NoL²)/NoL`, с клампом (иначе поверхность, стоящая почти ребром к свету, требует бесконечный
bias). Дефолты UE: константа 10, slope-scale 3, обе нормированы на диапазон глубины и на мировой
размер текселя. Нормировка у нас уже правильная (`Scene.cpp:253-256`) — не хватает slope-члена
и переноса bias в depth-пас.

### Код

**1) `sources/app/scene/SceneRenderConfig.h`** — добавить:

```cpp
    // S6: slope-scaled bias при записи глубины. slopeScale множит константный NDC-bias;
    // maxSlope ограничивает tan(угла) — иначе поверхность ребром к свету требует бесконечный bias.
    float slopeScale = 3.0f;
    float maxSlope   = 3.0f;
    // S7: pancaking. 1 = вершину перед near-плоскостью прижать к ней вместо отсечения.
    float clampToNear = 0.0f;
```

**2) `sources/app/scene/SceneRenderer.cpp`** — расширить `PerViewCB` (`:120-138`), **строго в хвост**:

```cpp
        float windPrevGustMul = 1.0f;
        // S6/S7: параметры bias'а shadow-depth паса. Смещение 224 в 256-байтном слоте PageProj,
        // который vsm_page_setup_cs.hlsl заполняет для VSM-страниц — там пишутся нули, поэтому
        // ApplyShadowDepthBias в VSM-режиме превращается в no-op (см. правило 3 в шапке).
        float shadowConstBias = 0.0f;   // 224
        float shadowSlopeBias = 0.0f;   // 228
        float shadowMaxSlope  = 0.0f;   // 232
        float shadowClampNear = 0.0f;   // 236
    };
    static_assert(sizeof(PerViewCB) == 240, "PerViewCB must match the gbuffer/shadow HLSL layout");
```

**3) `BuildShadowViewCB`** (`SceneRenderer.cpp:219-226`) — принимать индекс каскада и заполнять:

```cpp
    D3D12_GPU_VIRTUAL_ADDRESS BuildShadowViewCB(Renderer* renderer, const mat4& lightView,
                                                const mat4& lightProj, const vfx::WindState* wind,
                                                float constBias, float slopeBias,
                                                float maxSlope, float clampNear)
    {
        PerViewCB vc{};
        vc.viewProj = lightView * lightProj;
        ApplyWind(vc, wind);
        vc.shadowConstBias = constBias;
        vc.shadowSlopeBias = slopeBias;
        vc.shadowMaxSlope  = maxSlope;
        vc.shadowClampNear = clampNear;
        return UploadFrameCB(renderer, vc);
    }
```
В `Pass_CSM` (`:1409` и `:1472`) передавать `cascades.depthBiasNDC[cascadeIndex]`,
`cascades.depthBiasNDC[cascadeIndex] * cfg.slopeScale`, `cfg.maxSlope`, `cfg.clampToNear`.
Для spot/point (`:1339`) передавать нули — их bias-путь этот шаг не затрагивает.

**4) HLSL PerView — в ДВУХ местах, байт-в-байт одинаково.**
`shaders/gbuffer_common.hlsli` (после `:82`) и `shaders/shadow_indirect_csm.hlsl` (после `:74`):

```hlsl
    float  shadowConstBias;   // 224 (S6)
    float  shadowSlopeBias;   // 228
    float  shadowMaxSlope;    // 232
    float  shadowClampNear;   // 236 (S7)
```

**5) `shaders/vsm_page_setup_cs.hlsl`** — после `:130` добавить:

```hlsl
    // S6/S7: хвост shadow-bias параметров PerView. VSM-страницы получают НУЛИ: ApplyShadowDepthBias
    // тогда прибавляет 0 и не клампит, т.е. VSM-путь остаётся бит-в-бит прежним. Без этой записи
    // байты 224..239 слота остались бы мусором (см. правило 3).
    PageProj.Store4(po + 224u, asuint(float4(0.0f, 0.0f, 0.0f, 0.0f)));
```

**6) Новый файл `shaders/shadow_depth_common.hlsli`:**

```hlsl
// Bias, применяемый там, где глубина тени ЗАПИСЫВАЕТСЯ (S6), и pancaking (S7).
// Общий для всех вариантов shadow-depth VS. Не включает gbuffer_common.hlsli: shadow_indirect_csm
// объявляет собственный, более узкий PerObject/PerView, поэтому всё передаётся аргументами.
#ifndef SHADOW_DEPTH_COMMON_HLSLI
#define SHADOW_DEPTH_COMMON_HLSLI

// H       — clip-позиция после mul(worldPos, lightViewProj). Для ortho H.w == 1.
// normalWS— мировая нормаль вершины.
// lightVP — та же матрица: при row-vector математике clip.z = dot(pos, (_13,_23,_33)) + _43,
//           то есть колонка (_13,_23,_33) и есть направление света, помноженное на z-масштаб
//           проекции. Нормализация даёт ось света (знак не важен: берём abs).
// params  — (constBias, slopeBias, maxSlope, clampToNear).
//
// Атлас использует ПРЯМОЙ Z (clear 1.0, DepthFunc LESS_EQUAL), поэтому bias ПРИБАВЛЯЕТСЯ:
// это отодвигает записанную глубину от света.
float4 ApplyShadowDepthBias(float4 H, float3 normalWS, float4x4 lightVP, float4 params)
{
    // Гейт вокруг ВСЕЙ нормальной математики — НЕ микрооптимизация. Этот VS исполняется и на
    // VSM-страницах, где все четыре параметра нули (vsm_page_setup_cs пишет их нулями, п. 5),
    // а VSM per-page loop — самый горячий вершинный путь движка. Без гейта VSM платил бы
    // normalize + dot + sqrt на вершину ни за что. Pancaking проверяется отдельно ниже:
    // он не нуждается ни в нормали, ни в матрице.
    if (params.x != 0.0f || params.y != 0.0f)
    {
        const float3 lightAxis = normalize(float3(lightVP._13, lightVP._23, lightVP._33));
        const float  NoL = abs(dot(lightAxis, normalize(normalWS)));
        const float  maxSlope = params.z;
        // slope = tan(угол между поверхностью и светом). Кламп обязателен: при NoL -> 0 tan -> inf.
        const float  slope = clamp(NoL > 1e-4f ? sqrt(saturate(1.0f - NoL * NoL)) / NoL : maxSlope,
                                   0.0f, maxSlope);
        H.z += params.x + params.y * slope;
    }

    // S7 pancaking: кастер перед near-плоскостью прижимается к ней, а не отсекается. Кламп в VS
    // возвращает вершину внутрь фрустума, поэтому DepthClipEnable менять НЕ нужно (что критично:
    // PSO индирект-теней общий с VSM). Побочный эффект тот же, что и в UE: если у треугольника
    // часть вершин клампится, а часть нет — треугольник деформируется. Это принятый компромисс.
    if (params.w > 0.0f) { H.z = max(H.z, 0.0f); }
    return H;
}

#endif // SHADOW_DEPTH_COMMON_HLSLI
```

**7) Применить во всех пяти shadow-depth VS.** Пример для `shaders/gbuffer_csm.hlsl` — заменить
тело `VSMain` (`:9-19`):

```hlsl
#include "shadow_depth_common.hlsli"

[RootSignature(GBUFFER_CSM_RS)]
VSOutD VSMain(VSIn i)
{
    VSOutD o;
    float4 wp = mul(float4(i.P, 1.0f), world);
    wp.xyz += ApplyWindWS(i.P, wp.xyz, world, windStrength, i.WIND, windFoliage,
                          windTrunkStiff, windLeafScale, windGustMul, windTime);
    const float3 nWS = mul(i.N, (float3x3)world);   // масштаб влияет только на bias — допустимо
    o.H = ApplyShadowDepthBias(mul(wp, viewProj), nWS, viewProj,
                               float4(shadowConstBias, shadowSlopeBias, shadowMaxSlope, shadowClampNear));
    return o;
}
```
Аналогично в `gbuffer_inst_csm.hlsl`, `gbuffer_instcb_csm.hlsl`, `glass_csm.hlsl` (там world берётся
из `inst[...]`/`PerObject` соответственно).

Для `shaders/shadow_indirect_csm.hlsl` — изменить `WindTransformH` (`:79-88`), добавив нормаль:

```hlsl
inline float4 WindTransformH(float3 objPos, float3 objNormal, float4x4 world, float4 windWeights,
                             float windStrengthValue, float foliageValue, float trunkStiffValue,
                             float leafScaleValue)
{
    float4 wp = mul(float4(objPos, 1.0f), world);
    wp.xyz += WindOffset(objPos, wp.xyz, float3(world._41, world._42, world._43), windStrengthValue,
                         windWeights, foliageValue, trunkStiffValue, leafScaleValue,
                         windDirXZ, windSwayAmp, windSwayFreq, windGustMul, windTime);
    const float3 nWS = mul(objNormal, (float3x3)world);
    return ApplyShadowDepthBias(mul(wp, viewProj), nWS, viewProj,
                                float4(shadowConstBias, shadowSlopeBias, shadowMaxSlope, shadowClampNear));
}
```
и добавить `float3 N : NORMAL;` в `VSInIndirect` (`:136-141`) и `VSInMasked` (`:97-103`),
плюс `#include "shadow_depth_common.hlsli"` после `:10`.

**8) Input layouts.** `sources/rendering/descriptors/InputLayoutManager.cpp` — добавить NORMAL
в оба индирект-layout'а (`:80-96`). Нормаль лежит по смещению **12** формата `VertexPNTUV`;
эти layout'ы **уже** предполагают PNTUV (COLOR по смещению 48, UV по 40), так что допущение не новое.

Заодно **поправить комментарий** над `PosOnly_InstCasterId`: сейчас там написано *«POSITION is at
offset 0 in every mesh vertex format, so one layout serves all shadow-caster meshes»*. Это уже
неправда с W7.1 (COLOR@48), а с NORMAL@12 — тем более: layout жёстко требует `VertexPNTUV`.
Написать это явно, чтобы следующий формат вершин не сломал shadow-путь молча.

Эти layout'ы общие с VSM — см. предупреждение в начале шага:

```cpp
    Builder()
        .Add("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0)
        .Add("NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12) // S6: slope-scaled shadow bias
        .Add("COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 48)
        .Add("CASTERID", 0, DXGI_FORMAT_R32_UINT, 1, D3D12_APPEND_ALIGNED_ELEMENT,
             D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1)
        .Build(*this, "PosOnly_InstCasterId");
```
То же для `PosUV_InstCasterId` (`:90-96`).

**9) После этого уменьшить sample-time bias:** `SceneRenderConfig.h:10` `depthBiasInTexels`
с `2.0` до `0.5…1.0` (подобрать на глаз через S0.2) — его роль теперь берёт depth-пас.

### Критерий приёмки
* На наклонной геометрии (склон, пандус, скат крыши) при низком солнце acne исчезло **без**
  увеличения общего bias.
* Контакт объекта с землёй не «отъехал» дальше baseline (peter-panning не хуже).
* **VSM-режим (Ctrl+V) визуально идентичен baseline** — главная проверка корректности этого шага.
* **VSM-режим не подорожал:** `VsmPageRender` GPU в пределах +2 % от S0.4 — главная проверка
  *цены* этого шага (см. предупреждение в начале). Если вырос — ранний выход в
  `ApplyShadowDepthBias` не сработал (проверить дизассемблер/`--profdump`), следующий ход —
  перестановка PSO.
* Все 5 shadow-шейдеров компилируются; `static_assert(sizeof(PerViewCB) == 240)` проходит.
* Индирект-путь (`render::g_indirectShadowsEnabled`) и CPU-fallback дают одинаковые тени.

### Откат
`slopeScale = 0` → формула становится no-op'ом. Полный откат — вернуть `static_assert` на 224
и убрать хвост CB.

---

## S7. Pancaking: сжатие диапазона глубины каскада

**Зависит от:** S6 (использует его инфраструктуру). **Эффект:** шаг квантования D16 в c0 с 3.11 мм до <1 мм. **Риск:** средний.

### Почему
`SceneRenderConfig.h:13-19` сам описывает проблему и правильное решение как отложенное: чтобы
кастеры между солнцем и слайсом не отсекались, near-плоскость отодвигается на `casterReachWS = 150` м.
Для c0 это раздувает ортодиапазон до ~204 м на D16.

Механизм клампа уже добавлен в S6 (`params.w`). **PSO менять не нужно** — кламп в VS возвращает
вершину внутрь фрустума, так что `DepthClipEnable` остаётся `TRUE`. Это существенно: PSO
индирект-теней общий с VSM (`VirtualShadowMap.cpp:755`), и его изменение затронуло бы VSM.

### ⚠ Ловушка, без которой шаг ломает сам себя: near-плоскость КУЛЛИНГА

Наивная реализация («сжать `nearLS` и включить кламп») **не работает** и даёт ровно тот баг,
который `casterReachWS` и был призван закрыть. Причина — `Scene.cpp:270`:

```cpp
cascadeView.frustum = Frustum::FromOrthoBounds(cascadeView.invView, minX, maxX, minY, maxY, nearLS, farLS);
```

`FromOrthoBounds` строит ortho-**бокс с настоящей near-плоскостью** (`Frustum::BuildOrthoLs`), и
его 6 планов уходят не только в CPU-куллинг, но и в GPU-куллинг: `Frustum::Planes()`
(комментарий в `Frustum.h`: *«Exposed for GPU shadow culling»*) → `shadowGpu_.UpdateViewFrustums`,
слоты 0..3 (`Scene.cpp:1101-1110`).

Если сжать `nearLS`, то кастеры между солнцем и слайсом **отбраковываются на стадии куллинга** —
до вершинного шейдера они не доезжают, и клампить `max(H.z, 0)` становится нечего. Крона дерева,
верх столба, стрела крана просто исчезнут.

**Правильная формулировка: `nearLS` — это ДВЕ разные величины.**

| | значение | куда идёт | зачем |
|---|---|---|---|
| `nearProjLS` | тугой, `minZ` | `mat4::OrthoOffCenterLH` | сжать диапазон D16 — цель шага |
| `nearCullLS` | широкий, `minZ - casterReachWS` | `Frustum::FromOrthoBounds` | сохранить кастеров, которых спасёт pancaking |

`casterReachWS` **остаётся рабочим полем** и после этого шага (меняется только его смысл: не
«отодвинуть проекцию», а «насколько далеко к свету искать кастеров»). Удалять его нельзя.

### Код

**1) Включить кламп:** `clampToNear = 1.0f` в `CascadeShadowConfig` (S6, п.1) → приходит в
`shadowClampNear`.

**2) Отодвинуть «глаз» света ровно настолько, чтобы ortho-z оставался положительным.**
Заменить `lightDistance` (`Scene.cpp:198`):

```cpp
        // S7: «глаз» ставится за сферой каскада + casterReachWS, а не на фиксированные
        // maxDistance для всех каскадов. Это НЕ влияет на диапазон глубины (его задаёт
        // near/far ниже) — только гарантирует, что любой кастер в пределах casterReachWS
        // к свету имеет z_ls > 0, т.е. лежит внутри ortho-бокса куллинга.
        // (Старое max(1, maxDistance) для c3 давало lightDistance < radius: глаз оказывался
        // ВНУТРИ объёма каскада, и near упирался в кламп 0.001.)
        const float lightDistance = radius + cascadeConfig_.casterReachWS + 1.0f;
```

**3) Развести near проекции и near куллинга.** Заменить `Scene.cpp:245-251`:

```cpp
        // z-диапазон пропорционален экстенту каскада — так один NDC-bias работает на всех
        // каскадах (тот же приём уже применён для VSM-клипмапа, Scene.cpp:329-330).
        const float zPad = cascadeConfig_.zPaddingInRadii * radius;   // старт: 1.0

        // S7: ТУГАЯ near для ПРОЕКЦИИ — это и есть весь выигрыш по точности D16.
        // Кастеры перед ней не отсекаются: pancaking-кламп в shadow-depth VS
        // (ApplyShadowDepthBias, shadow_depth_common.hlsli) прижимает их к NDC z = 0.
        const float nearProjLS = std::max(0.001f, minZ);
        const float farLS      = maxZ + zPad;

        // S7: ШИРОКАЯ near для КУЛЛИНГА — иначе кастеров, которых спасает pancaking,
        // отбракуют раньше, чем они дойдут до VS (см. «Ловушка» выше). Эти планы уходят
        // и в CPU-куллинг (SceneView::frustum), и в GPU-куллинг (ShadowGpuData::UpdateViewFrustums).
        const float nearCullLS = std::max(0.001f, minZ - cascadeConfig_.casterReachWS);

        const mat4 lightProj = mat4::OrthoOffCenterLH(minX, maxX, minY, maxY, nearProjLS, farLS);
```
и ниже, в построении фрустума (`Scene.cpp:270`), поставить `nearCullLS`:
```cpp
        cascadeView.frustum = Frustum::FromOrthoBounds(cascadeView.invView, minX, maxX, minY, maxY,
                                                       nearCullLS, farLS);
```
**Все остальные потребители** (`cascades.depthBiasNDC[idx]`, `cascadeView.proj`,
`cascadeView.invProj`) используют `nearProjLS`/`lightProj` — то есть проекционную пару. Единственное
место с `nearCullLS` — `FromOrthoBounds`.

**4)** Заменить в `SceneRenderConfig.h` `float zPadding = 25.0f;` на `float zPaddingInRadii = 1.0f;`.
`casterReachWS` **оставить** (см. таблицу выше) и вывести слайдером в S0.2: теперь это прямой
рычаг «дальность поиска кастеров ↔ стоимость куллинга».

Для c0 (`radius = 11.5`) это даёт `nearProjLS ≈ 151`, `farLS ≈ 185`, диапазон
`2*radius + zPad ≈ 34.5` м вместо 204 м → шаг D16 ≈ **0.53 мм**. Абсолютное удаление «глаза»
на точность не влияет: ortho-глубина линейна, квантуется равномерно по `[near, far]`.

### Критерий приёмки
* Readout (S0): `zRange` каскада 0 упал с ~204 м до ≲ 60 м, `D16 step` < 1 мм.
* **Высокие объекты (деревья, столбы, краны), верх которых выше слайса, продолжают отбрасывать
  тень и она не обрезана** — это то, что раньше спасал `casterReachWS`. Проверять специально:
  встать под пальму, посмотреть, что тень ствола+кроны цельная. **Это проверка на ловушку с
  near-плоскостью куллинга — если развести `nearProjLS`/`nearCullLS` забыли, тень обрежется
  ровно здесь.**
* **Отдельно проверить, что кламп реально нужен, а не маскирует ошибку:** временно
  `clampToNear = 0` при новых near/far — верх высоких кастеров должен «срезаться» плоскостью.
  Если картинка не меняется, значит кастеры отсекаются куллингом и pancaking не работает.
* Acne не появилось; peter-panning уменьшился (можно ещё снизить `depthBiasInTexels`).
* VSM-режим идентичен baseline.
* Профайлер: `Pass_CSM` без изменений (кламп в VS бесплатный, `lightDistance` на растеризацию
  не влияет). `Pass_ShadowCull` тоже без изменений — объём ortho-бокса куллинга по построению
  тот же, что был (`minZ - casterReachWS .. maxZ + zPad`).

### Известный побочный эффект
Если у треугольника часть вершин клампится, а часть нет — треугольник деформируется (это природа
pancaking'а, UE документирует то же). Проявляется только на кастерах, пересекающих near-плоскость
**проекции**. Если станет заметно — отодвинуть `nearProjLS` от `minZ` на запас
(`minZ - slack`, `slack` в метрах, слайдером): это прямой компромисс «точность D16 ↔ частота
клампа», при `slack = casterReachWS` шаг вырождается в baseline.

### Откат
`clampToNear = 0` + вернуть `nearProjLS = nearCullLS` (одна величина, как было) и `zPadding = 25`.
`casterReachWS = 150` не трогается — он остаётся живым в обоих состояниях.

---

## S8. Soft-occlusion + Gather4 tent PCF вместо `SampleCmp` box 3×3

**Зависит от:** S3, S5, **S6 (жёстко, см. ниже)**. **Эффект:** мягкий градиент вместо бинарного сравнения; тот же футпринт фильтра за меньшее число текстурных операций. **Риск:** средний (меняется внешний вид теней).

### ⚠ Без S6 этот шаг ломает картинку

В Gather-ветке приёмнику передаётся **`z`, а не `z - b`**: sample-time bias выбрасывается целиком,
и его роль полностью переходит к write-time bias из **S6**. Если S6 не сделан (или
`slopeScale`/`constBias` нулевые), то на любой освещённой поверхности `stored ≈ receiver` с
точностью до квантования D16 → рампа даёт ≈ 0.5 → **вся сцена покрывается равномерной серой
самозатенённостью**. Это не «настроить потом», это неработающий шаг.

Порядок обязателен: S6 → (S7) → S8. И `depthBiasInTexels` к этому моменту уже должен быть
опущен по п. 9 шага S6.

### Почему
UE **не использует** hardware-сравнение для 2D-теней. Он берёт сырые глубины через `Gather()`
и заменяет бинарное сравнение **линейной рампой**:

```
lit = saturate((storedDepth - receiverDepth) * transitionScale + 1)
```
При `storedDepth ≥ receiverDepth` → 1 (освещено); по мере ухода `storedDepth` ниже receiver'а
на `1/transitionScale` результат линейно падает до 0. Ориентация **совпадает** с конвенцией проекта
(прямой Z: меньше = ближе к свету, occluded ⟺ stored < receiverZ), формула переносится дословно.

Что это даёт:
* «bias» превращается в **переходную зону** — основная причина, почему у UE нет acne при малом
  константном bias. **Это ~весь выигрыш этого шага по качеству.**
* `Gather()` берёт 4 текселя за один тап → ядро **4×4 стоит 4 тапа** вместо 9, а ядро **6×6 —
  9 тапов**, с корректными билинейными весами (tent).

**Чего этот шаг НЕ даёт — честная оговорка.** Текущий фильтр — не box и не «лестница из
3 текселей»: `gSmpLinear` объявлен как `D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT`
(`SamplerManager.cpp:152`), то есть **каждый** из 9 существующих `SampleCmpLevelZero` уже
аппаратный 2×2 PCF, и сетка 3×3 с шагом в тексель даёт пересэмплированный **4×4 tent**.
Форма и ширина ядра после S8 практически те же. Меняется:
1. бинарное сравнение → линейная рампа (качество, и оно того стоит);
2. 9 текстурных операций → 4 (перф);
3. появляется точка входа для S9 (receiver-plane bias) и для ядра 6×6, чего с `SampleCmp` нет.

Соответственно и перф-ожидание умеренное: 4 `Gather` + ~40 ALU против 9 hw-PCF-тапов — это
**скорее паритет**, чем ускорение. Обещать падение `Pass_Lighting` не надо; надо проверить,
что оно не выросло.

SRV атласа — `R16_UNORM` (`RenderTargetManager.cpp:513`), поэтому `Gather` вернёт нормализованные
глубины в `[0,1]` — ту же величину, что и NDC-z ортопроекции. Дополнительный сэмплер не нужен:
`Gather` использует SamplerState только для адресации, годится существующий `gSmpPoint`
(`lighting_cs.hlsl:29`) / `LinearSampler` (`glass.hlsl:80`).

**Важно:** офсеты задаются **не** через `int2`-параметр `Gather`, а сложением UV — тогда каждый
тап можно заклампить границами контента (S5). С `int2`-офсетами clamp обходится аппаратно.

### Код

Добавить в `shaders/csm_sample.hlsli`:

```hlsl
// --- S8: soft-occlusion + Gather4 tent PCF -------------------------------------------------
// Линейная рампа вместо бинарного сравнения: «bias» становится переходной зоной шириной
// 1/transitionScale по глубине. 1 = освещено, 0 = в тени.
float4 CsmOcclusion4(float4 storedDepth, float receiverDepth, float transitionScale)
{
    // Разложено так, чтобы per-pixel-константа вынеслась из per-sample математики.
    const float constantFactor = receiverDepth * transitionScale - 1.0f;
    return saturate(storedDepth * transitionScale - constantFactor);
}

// Gather возвращает 2x2 в порядке (-,+), (+,+), (+,-), (-,-) относительно центра квада, то есть
// .xyzw = (левый-низ, правый-низ, правый-верх, левый-верх) в UV-координатах DirectX.
// Билинейные веса ядра 4x4 (tent) по дробной части позиции внутри текселя.
float CsmTent4x4Gather(Texture2D atlas, SamplerState smp, float2 uv, float receiverDepth,
                       float transitionScale, float2 atlasSize, float2 texel, float4 uvClamp)
{
    const float2 texelPos = uv * atlasSize - 0.5f;
    const float2 frac2    = frac(texelPos);
    // Центр 2x2-квада: Gather сэмплит на пол-текселя вокруг переданной точки.
    const float2 quadUV   = (floor(texelPos) + 1.0f) * texel;

    // 4 квада: смещения на ±1 тексель. Сложением UV (не int2-офсетами), чтобы clamp работал.
    const float2 o = texel;
    const float4 v00 = CsmOcclusion4(atlas.Gather(smp, clamp(quadUV + float2(-o.x, -o.y), uvClamp.xy, uvClamp.zw)), receiverDepth, transitionScale);
    const float4 v10 = CsmOcclusion4(atlas.Gather(smp, clamp(quadUV + float2( o.x, -o.y), uvClamp.xy, uvClamp.zw)), receiverDepth, transitionScale);
    const float4 v01 = CsmOcclusion4(atlas.Gather(smp, clamp(quadUV + float2(-o.x,  o.y), uvClamp.xy, uvClamp.zw)), receiverDepth, transitionScale);
    const float4 v11 = CsmOcclusion4(atlas.Gather(smp, clamp(quadUV + float2( o.x,  o.y), uvClamp.xy, uvClamp.zw)), receiverDepth, transitionScale);

    // Свёртка 4x4 tent: крайние столбцы/строки весят (1-frac) и frac, внутренние — 1.
    float4 rows;
    rows.x = v00.w * (1.0f - frac2.x) + v00.z + v10.w + v10.z * frac2.x;
    rows.y = v00.x * (1.0f - frac2.x) + v00.y + v10.x + v10.y * frac2.x;
    rows.z = v01.w * (1.0f - frac2.x) + v01.z + v11.w + v11.z * frac2.x;
    rows.w = v01.x * (1.0f - frac2.x) + v01.y + v11.x + v11.y * frac2.x;

    return saturate(dot(rows, float4(1.0f - frac2.y, 1.0f, 1.0f, frac2.y)) * (1.0f / 9.0f));
}

// PCF пересветляет край; возведение в квадрат возвращает художественно ожидаемый профиль.
float CsmCorrectOverBlur(float shadow) { return shadow * shadow; }

// Резкость края: 1 = без изменений, >1 сужает переход.
float CsmSharpen(float shadow, float sharpen)
{
    return saturate((shadow - 0.5f) * sharpen + 0.5f);
}
```

Расширить `CsmParams`:
```hlsl
    float4 transitionScale;   // S8: 1/ширина переходной зоны по глубине, на каскад
    float  receiverBiasMin;   // S8: множитель transitionScale при NoL == 0 (старт 0.1)
    float  sharpen;           // S8: 1.0 = выключено
    uint   useGatherPcf;      // S8: 0 = старый SampleCmp путь (аварийный откат)
```
и в `CsmSampleChain` заменить возврат:
```hlsl
        outCascade = c;
        if (p.useGatherPcf != 0u)
        {
            // Ослабление рампы по NoL работает как receiver bias: чем косее свет падает,
            // тем шире переходная зона и тем труднее получить self-shadowing.
            const float ts = p.transitionScale[c] * lerp(p.receiverBiasMin, 1.0f, saturate(NdotL));
            float sh = CsmTent4x4Gather(atlas, smp, uv, z, ts, p.atlasSize, texel, p.uvMinMax[c]);
            sh = CsmCorrectOverBlur(sh);
            return CsmSharpen(sh, p.sharpen);
        }
        return CsmPcf3x3(atlas, cmp, uv, z - b, texel, pcfR, p.uvMinMax[c]);
```
Сигнатуры `CsmSampleChain`/`CsmSampleShadow` получают дополнительный параметр
`SamplerState smp` (в `lighting_cs.hlsl` — `gSmpPoint`, в `glass.hlsl` — `LinearSampler`).

**CPU-часть.** `LightingPassConstants` + `GlassViewCB` + handles + write:
```cpp
    float4 cascadeTransitionScale{};   // S8
    float  csmReceiverBiasMin = 0.1f;  // S8
    float  csmSharpen = 1.0f;          // S8
    uint32_t csmUseGatherPcf = 1;      // S8
```
Заполнение в `SceneRenderer.cpp` (рядом с `shadowBiasNDC`, `:1896`):
```cpp
        // transitionScale = 1 / (ширина переходной зоны в NDC). Берём ту же величину, которой
        // раньше служил жёсткий NDC-bias: переход шириной в один прежний bias.
        for (int c = 0; c < 4; ++c)
        {
            const float w = std::max(1e-6f, cascades.depthBiasNDC[c]);
            reinterpret_cast<float*>(&constants.cascadeTransitionScale)[c] = 1.0f / w;
        }
```

**Опция высокого качества (по желанию):** ядро 6×6 = 9 гатеров (квады на смещениях
`{-2,0,+2}×{-2,0,+2}` с той же tent-свёрткой, нормировка `1/25`). Держать за `#define`
или полем CB; включать только для c0. **Это пресет, а не дефолт** (см. «Цель» в шапке): 9 гатеров
против 4 — ×2.25 текстурных операций в `Pass_Lighting`, и без S9 расширенное ядро всё равно даст
self-shadowing на наклонных поверхностях. Правильный порядок — сначала S9, потом решать про 6×6.

### Критерий приёмки
* Край тени в каскаде 0 — плавный градиент, а не ступенька «свет/тень» на границе полутени.
  Сравнивать надо с `csmUseGatherPcf = 0`, а не с памятью: старый путь тоже мягкий (см. оговорку
  выше), разница именно в профиле перехода.
* `Pass_Lighting` **не выросло** относительно S0.4 (порог +2 %). Падение — приятный бонус, а не
  ожидание.
* **Нет равномерной серой пелены** на освещённых поверхностях — если есть, S6 не сделан или его
  bias нулевой (см. предупреждение в начале шага).
* Acne не появилось при уменьшенном `depthBiasInTexels`.
* `csmUseGatherPcf = 0` возвращает baseline-вид (проверка, что откат работает).
* Стекло (`glass.hlsl`) визуально согласовано с остальной геометрией.
* Сравнительные скриншоты «до/после» в трёх точках: контактная тень, край тени на плоскости,
  граница каскадов.

### Откат
`csmUseGatherPcf = 0`.

---

## S9. Аналитический receiver-plane depth bias

**Зависит от:** S3, S8. **Эффект:** позволяет расширить ядро PCF без self-shadowing на скошенных поверхностях. **Риск:** низкий.

### Почему
Все тапы фильтра сравниваются с **одной** глубиной приёмника. На поверхности под углом к свету это
даёт self-shadowing на дальних тапах — поэтому радиус и держится на 1 текселе. UE считает наклон
плоскости приёмника в shadow-space через `cross(ddx(shadowPos), ddy(shadowPos))` и добавляет к
каждому тапу `max(dot(biasFactors, tapOffsetUV), 0)`.

**`ddx`/`ddy` копировать нельзя**: лайтинг — compute shader (`lighting_cs.hlsl:225`), а производные
в CS требуют SM 6.6. Считаем аналитически из нормали G-буфера, которая уже есть.

Вывод: ортопроекция света — аффинное отображение, поэтому плоскость приёмника с нормалью `N`
переходит в плоскость в координатах `(u, v, z)` shadow-space с нормалью `N_L`. Из условия
`dot(N_L, (du, dv, dz)) = 0`:
```
dz/du = -N_L.x / N_L.z ,   dz/dv = -N_L.y / N_L.z
```
`N_L` берётся из той же матрицы `lightViewProj`, что уже передана: её колонки — это оси света,
помноженные на масштабы проекции, а UV = NDC*0.5 (+0.5) — то есть нужный пересчёт делается прямо
в шейдере без новых полей CB.

### Код

Добавить в `shaders/csm_sample.hlsli`:

```hlsl
// Наклон плоскости приёмника в координатах (u, v, z) тайла каскада: dz/du и dz/dv.
// Аналитически из мировой нормали (производные в compute-шейдере недоступны до SM 6.6).
// lightVP: clip.xyz = mul(float4(P,1), lightVP).xyz; uv = clip.xy * (0.5,-0.5) + 0.5.
// Поэтому нормаль в (u,v,z): N_uv.x = N_clip.x / 0.5, N_uv.y = N_clip.y / (-0.5), N_uv.z = N_clip.z,
// где N_clip получается умножением N на транспонированную обратную 3x3 часть lightVP. Для ortho
// (ортонормальные оси * диагональные масштабы) это сводится к покомпонентному делению на масштабы.
float2 CsmReceiverPlaneFactors(float3 Nws, float4x4 lightVP)
{
    // Масштабы ортопроекции вдоль её осей: длина колонок 3x3 части.
    const float3 cx = float3(lightVP._11, lightVP._21, lightVP._31);
    const float3 cy = float3(lightVP._12, lightVP._22, lightVP._32);
    const float3 cz = float3(lightVP._13, lightVP._23, lightVP._33);
    const float sx = length(cx), sy = length(cy), sz = length(cz);

    const float3 n = normalize(Nws);
    // Компоненты нормали в базисе света (оси ортонормальны -> просто проекции).
    const float3 nL = float3(dot(n, cx / max(sx, 1e-8f)),
                             dot(n, cy / max(sy, 1e-8f)),
                             dot(n, cz / max(sz, 1e-8f)));
    // Перевод из мировых длин в единицы (u, v, z): u = x*sx*0.5, v = -y*sy*0.5, z = z*sz.
    float3 nUV = float3(nL.x / (sx * 0.5f), nL.y / (-sy * 0.5f), nL.z / sz);

    // Кламп наклона на ~5 градусов (sin(5deg) = 0.0872665): поверхность, стоящая почти ребром
    // к свету, иначе требует неограниченного bias'а.
    const float denom = max(abs(nUV.z), length(nUV) * 0.0872665f);
    return -nUV.xy / max(denom, 1e-8f) * sign(nUV.z + 1e-8f);
}
```
и в `CsmTent4x4Gather` добавить параметр `float2 planeFactors`, прибавляя к receiver-глубине
на каждом кваде смещение по его UV-офсету:
```hlsl
    // На каждый квад: поправка глубины по наклону плоскости приёмника.
    // (Смещения квадов известны, поэтому поправка считается 4 раза, а не 16.)
    const float d00 = max(dot(planeFactors, float2(-o.x, -o.y)), 0.0f);
    // ... и так для d10/d01/d11; передавать receiverDepth + dNN в CsmOcclusion4.
```
Вызов в `CsmSampleChain`: `CsmReceiverPlaneFactors(Nws, p.lightViewProj[c])`.

После этого включить более широкое ядро (6×6, S8) для c0 и проверить.

### Критерий приёмки
* Ядро PCF расширено — acne на наклонных поверхностях **не появился**.
* На поверхности почти параллельной свету (низкое солнце, скат) клампа на 5° достаточно: нет
  «взрыва» bias'а и провалов тени.
* `planeFactors = 0` возвращает поведение S8 (проверка отката).

### Откат
Вернуть `planeFactors = float2(0,0)`.

---

## S10. Затухание последнего каскада + blend-полоса от длины слайса

**Зависит от:** S3. **Эффект:** убирает жёсткий терминатор теней на 300 м. **Риск:** минимальный.

### Почему
`CsmSampleChain` возвращает `1.0` за каскадом 3 — на 300 м тени исчезают линией. UE переносит
fade-плоскость **внутрь** последнего каскада на `(splitFar - splitNear) * fadeFraction` и гасит
тень к 1.0 (освещено).

Плюс: текущая blend-полоса берёт `band = splitNext * 0.1` — долю от **абсолютной** дистанции.
UE берёт долю от **длины слайса**. Для c0 разница мала, для c3 (100…300) существенна: 30 м против 20 м.

### Код

`SceneRenderConfig.h`:
```cpp
    // S10: доля длины слайса, на которой каскад кросс-фейдится в следующий (UE: 0.1).
    float blendFraction = 0.1f;
    // S10: доля длины последнего слайса, на которой тень гаснет в «нет тени» (UE: 0.1).
    float distanceFadeFraction = 0.1f;
```
Передать в CB как `float2 csmFadeParams` (blendFraction, distanceFadeFraction).

`shaders/csm_sample.hlsli` — заменить blend-блок в `CsmSampleShadow`:

```hlsl
float CsmSampleShadow(CsmParams p, Texture2D atlas, SamplerComparisonState cmp, SamplerState smp,
                      float3 Pws, float3 Nws, float NdotL, out int outCascade)
{
    const int idx = CsmChooseCascade(p, Pws);
    float shadow = CsmSampleChain(p, atlas, cmp, smp, idx, Pws, Nws, NdotL, outCascade);
    const float zView = dot(Pws - p.camPosWS, p.camDirWS);

    // Границы слайса каскада idx: near = предыдущий сплит (для 0 — камера), far = свой сплит.
    const float4 splitFar  = float4(p.splitsVS.y, p.splitsVS.z, p.splitsVS.w, p.maxDistance);
    const float4 splitNear = float4(p.splitsVS.x, p.splitsVS.y, p.splitsVS.z, p.splitsVS.w);
    const float sFar  = splitFar[idx];
    const float sNear = splitNear[idx];
    const float band  = max(1e-4f, (sFar - sNear) * p.blendFraction);   // доля ДЛИНЫ слайса (S10)

    if (idx < 3)
    {
        const float t = saturate((zView - (sFar - band)) / band);
        if (t > 0.0f)
        {
            int dummy;
            const float shadowNext = CsmSampleChain(p, atlas, cmp, smp, idx + 1, Pws, Nws, NdotL, dummy);
            shadow = lerp(shadow, shadowNext, t);
        }
    }
    else
    {
        // Последний каскад: более грубого соседа нет, поэтому fade-плоскость сдвигается ВНУТРЬ
        // и тень гаснет в 1.0 (освещено) — иначе на границе дальности получается линия-терминатор.
        const float fade = max(1e-4f, (sFar - sNear) * p.distanceFadeFraction);
        const float t = saturate((zView - (sFar - fade)) / fade);
        shadow = lerp(shadow, 1.0f, t);
    }
    return shadow;
}
```
Добавить в `CsmParams`: `float maxDistance; float blendFraction; float distanceFadeFraction;`
(`maxDistance` = `cascadeConfig_.maxDistance`, чтобы у c3 была известна far-граница; можно взять
из `cascades.splitsVS[4]`, который уже вычисляется в `BuildSplitScheme`, — тогда передать
`splitsVS` пятым компонентом или отдельным полем).

### Критерий приёмки
* На дистанции 270…300 м тени плавно исчезают, линии-терминатора нет.
* Ширина blend-полос пропорциональна длине слайса; на границах c0↔c1 и c2↔c3 нет швов.
* `distanceFadeFraction = 0` → возврат к жёсткой границе (проверка отката).

### Откат
`distanceFadeFraction = 0`, `blendFraction` вернуть к прежней семантике.

---

## S11. Scissor-оптимизация каскада (производительность)

**Зависит от:** S4. **Эффект:** меньше растеризации в shadow-пасе, особенно в c0 при `hfov 90°`. **Риск:** средний — легко «отрезать» нужных кастеров.

### Почему
Fit по сфере покрывает область в ~2.5× глубже слайса (для `hfov 90°` радиус ≈ 1.25·splitFar), но
камера видит только конус внутри неё. UE считает scissor-прямоугольник как проекцию углов фрустума
камеры в тайл каскада (cvar `r.Shadow.CSMScissorOptim`, по умолчанию выключен).

Корректность держится на том, что кастер, находящийся между солнцем и слайсом, проецируется вдоль
света **в те же тексели**, что и точка, которую он затеняет. Раз затеняемая точка внутри
прямоугольника — кастер тоже внутри. Обрезать нечего.

### ⚠ Но это верно только для приёмников, видимых камерой

Прямоугольник выводится из фрустума **камеры**, а `ShadowAtlas` сэмплит не только `lighting_cs`:
`glass.hlsl` шейдит преломление и отражение, и **приёмник там может лежать вне фрустума камеры**
(см. правило 4 в шапке). После S5 fallback по границе тайла убран, поэтому такой пиксель прочитает
не отрисованную часть тайла — то есть clear-значение `1.0` = «освещён» — вместо провала в грубый
каскад. Симптом: у стекла/воды пропадает тень на отражённой/преломлённой геометрии сбоку от кадра.

Следствия для реализации:
* тумблер по умолчанию **off** (как в UE) — и это не формальность, а страховка;
* критерий «на статичной камере тени идентичны» **обязательно проверять на сцене со стеклом и
  водой**, иначе он ничего не докажет;
* если артефакт подтвердится — минимальный безопасный вариант: применять scissor **только к c2/c3**
  (там доля площади, которую он отрезает, максимальна, а стекло в дальних каскадах почти не
  различимо), либо расширять прямоугольник на экранный радиус преломления.

### Код

Добавить в `sources/app/scene/SceneFrameData.h` в `CascadeData`:
```cpp
    // S11: scissor-прямоугольник каскада в текселях АТЛАСА (не тайла). Пустой = весь тайл.
    struct ScissorRect { std::int32_t x0, y0, x1, y1; };
    ScissorRect scissor[kCascades] = {};
```

В `Scene.cpp` рядом с `ComputeCascadeSphere`:

```cpp
// S11: минимальный прямоугольник тайла каскада, покрывающий проекцию видимой части слайса.
// Достаточно 4 far-угла + позиция камеры: слайс — выпуклая пирамида, её проекция вдоль света
// лежит в выпуклой оболочке этих точек. Прямоугольник расширяется на `padTexels` (радиус
// фильтра + border), иначе на срезе появятся артефакты.
static SceneFrameData::CascadeData::ScissorRect ComputeCascadeScissor(
    const Camera& camera, const mat4& lightViewProj, const render::CascadeTile& tile,
    float sliceFar, int padTexels)
{
    const mat4& proj = camera.GetProjMatrixNoJitter();
    const float tanHalfX = 1.0f / std::max(1e-6f, proj.m._11);
    const float tanHalfY = 1.0f / std::max(1e-6f, proj.m._22);

    const mat4& invView = camera.GetInvViewMatrix();
    const float3 right = float3(invView.m._11, invView.m._12, invView.m._13);
    const float3 up    = float3(invView.m._21, invView.m._22, invView.m._23);
    const float3 fwd   = camera.GetDirection();
    const float3 eye   = camera.GetPosition();

    const float hx = tanHalfX * sliceFar;
    const float hy = tanHalfY * sliceFar;
    const float3 c = eye + fwd * sliceFar;

    const float3 pts[5] = {
        c + right * hx + up * hy,
        c + right * hx - up * hy,
        c - right * hx + up * hy,
        c - right * hx - up * hy,
        eye,
    };

    float uMin = 1e9f, vMin = 1e9f, uMax = -1e9f, vMax = -1e9f;
    for (const float3& p : pts)
    {
        const float4 clip = lightViewProj.Transform(float4(p, 1.0f));
        const float w = std::max(1e-6f, clip.w);
        const float u = (clip.x / w) * 0.5f + 0.5f;   // [0,1] по тайлу
        const float v = (clip.y / w) * -0.5f + 0.5f;
        uMin = std::min(uMin, u); uMax = std::max(uMax, u);
        vMin = std::min(vMin, v); vMax = std::max(vMax, v);
    }

    const float ts = static_cast<float>(tile.size);
    SceneFrameData::CascadeData::ScissorRect r{};
    r.x0 = static_cast<std::int32_t>(tile.x) + Clamp(static_cast<std::int32_t>(std::floor(uMin * ts)) - padTexels, 0, static_cast<std::int32_t>(tile.size));
    r.y0 = static_cast<std::int32_t>(tile.y) + Clamp(static_cast<std::int32_t>(std::floor(vMin * ts)) - padTexels, 0, static_cast<std::int32_t>(tile.size));
    r.x1 = static_cast<std::int32_t>(tile.x) + Clamp(static_cast<std::int32_t>(std::ceil (uMax * ts)) + padTexels, 0, static_cast<std::int32_t>(tile.size));
    r.y1 = static_cast<std::int32_t>(tile.y) + Clamp(static_cast<std::int32_t>(std::ceil (vMax * ts)) + padTexels, 0, static_cast<std::int32_t>(tile.size));
    // Вырожденный результат (камера смотрит вдоль света и т.п.) -> весь тайл.
    if (r.x1 <= r.x0 || r.y1 <= r.y0)
    {
        r = { static_cast<std::int32_t>(tile.x), static_cast<std::int32_t>(tile.y),
              static_cast<std::int32_t>(tile.x + tile.size), static_cast<std::int32_t>(tile.y + tile.size) };
    }
    return r;
}
```
Вызывать в конце тела цикла `UpdateCascades`, **после** построения итоговой проекции — то есть
передавать `lightView * lightProjBordered` (S5), а не `lightProj`, иначе прямоугольник разъедется
с содержимым тайла на величину border-масштаба. Писать в `cascades.scissor[idx]`; в
`Renderer::BindShadowTarget` (S4, п.5) — если тумблер включён, брать `RSSetScissorRects` из
переданного прямоугольника (потребует протащить его через `BindShadowTarget` или прочитать из
`SceneFrameData`). **Viewport оставить полным тайлом** — иначе поедут матрицы.

Держать за тумблером, по умолчанию **off** (как в UE).

### Критерий приёмки
* Время `Pass_CSM` снизилось (профайлер, `ProfilerScopes::kPassCSM`) — записать Δ к S0.4.
* **На статичной камере тени при включённом и выключенном тумблере идентичны, и проверка сделана
  на сцене со стеклом и водой** (см. предупреждение выше). Если есть разница на обычной геометрии —
  scissor режет нужное, увеличить `padTexels`; если разница только в стекле/отражениях — это
  системное ограничение шага, а не настройка: ограничить scissor дальними каскадами.
* Быстрое вращение камеры не даёт мигающих обрезанных теней.
* Камера смотрит прямо вдоль/против света — не крэшится, откатывается на полный тайл.

### Откат
Тумблер в off.

---

## S12. Contact shadows

**Зависит от:** S3 (желательно). **Эффект:** мелкий контактный детейл, который тексель CSM физически не передаёт. **Риск:** низкий.

### Почему
Самый дешёвый способ «догнать» ощущение высокого разрешения. Всё нужное уже забиндено:
`DepthT` (t4), G-буфер, `invProj`/`invView`, compute-проход.

### Код

⚠ **Ниже — набросок, а не готовый к компиляции код.** Он опирается на поле CB (`viewProj`),
которого сегодня нет, и на хелпер (`LinearizeDepth`), наличие которого надо проверить — см.
«Что доделать» сразу после блока. Правильный порядок: сначала завести поле и хелпер, потом писать
марш.

Добавить в `shaders/lighting_cs.hlsl` (после `#include`-ов):

```hlsl
// S12: короткий screen-space raymarch к солнцу. Закрывает масштаб, который тексель CSM не
// разрешает (стыки, мелкие детали, контакт объекта с поверхностью).
// P/N — мировые позиция и нормаль приёмника; L — направление НА солнце.
float ContactShadow(float3 P, float3 N, float3 L, float lengthWS, uint steps,
                    float thicknessWS, float ditherOffset)
{
    if (lengthWS <= 0.0f || steps == 0u) { return 1.0f; }

    // Марш ведётся в МИРОВЫХ координатах с репроекцией каждого шага через viewProj камеры —
    // так не нужна view-матрица (в CB лайтинга лежат только invView/invProj).
    const float stepWS = lengthWS / (float)steps;
    float occluded = 0.0f;

    [loop] for (uint i = 1u; i <= steps; ++i)
    {
        // Дизеринг стартовой фазы убирает полосатость (TAA/DLSS дорезолвит остаток).
        const float t = (float(i) - 1.0f + ditherOffset) * stepWS;
        const float3 samplePws = P + N * 0.01f + L * t;

        // Мир -> экран через матрицу вида-проекции камеры. В CB лайтинга её нет, но есть
        // invView/invProj: добавить поле `viewProj` в LightingPassConstants (handle + write).
        const float4 clip = mul(float4(samplePws, 1.0f), viewProj);
        if (clip.w <= 0.0f) { break; }
        const float2 sUV = (clip.xy / clip.w) * float2(0.5f, -0.5f) + 0.5f;
        if (any(sUV < 0.0f) || any(sUV > 1.0f)) { break; }

        // Глубина сцены в этой точке против глубины луча. ОСНОВНАЯ глубина камеры — reverse-Z
        // (больше = ближе): Renderer.cpp:1460 чистит D.dsv значением 0.0. Это ДРУГАЯ конвенция,
        // чем в shadow-атласе (там прямой Z, clear 1.0) — не перепутать при правках.
        const float sceneZ = DepthT.SampleLevel(gSmpPoint, sUV, 0).r;
        const float rayZ   = clip.z / clip.w;

        // Луч оказался ЗА геометрией -> перекрыт. thicknessWS отсекает случаи, когда
        // геометрия на самом деле далеко впереди (ложное перекрытие через тонкий объект).
        if (sceneZ > rayZ)
        {
            const float dz = LinearizeDepth(sceneZ) - LinearizeDepth(rayZ); // helper из utils.hlsli
            if (dz < thicknessWS) { occluded = 1.0f; break; }
        }
    }
    // Плавное затухание по длине луча, чтобы контактная тень не обрывалась.
    return 1.0f - occluded;
}
```

**Что доделать при реализации:**
* Добавить `mat4 viewProj` в `LightingPassConstants` (+ handle + write + HLSL-поле) — сейчас в CB
  только `invView`/`invProj`.
* `LinearizeDepth` — проверить, есть ли готовый helper в `shaders/utils.hlsli`; если нет, вывести
  из `invProj` (`_43`/`_33`).
* `ditherOffset` — брать из Bayer/blue-noise по `dispatchThreadId` (в проекте уже есть
  DLSS/TAA: `DlssHandler.cpp`, так что стохастика дорезолвится).
* Результат комбинировать как `sunShadow = min(sunShadow, contact)`.
* Параметры (`lengthWS` 0.1…0.5 м, `steps` 8…16, `thicknessWS`) вывести в ImGui (S0.2).

### Критерий приёмки
* Появился контактный детейл под мелкими объектами и в стыках, которого нет в CSM.
* Нет полосатых артефактов (при необходимости — усилить дизеринг).
* `Pass_Lighting` выросло приемлемо (< 0.3 мс на целевом разрешении) — и **суммарно
  `Pass_CSM + Pass_Lighting` остаётся ниже VSM-строки S0.4**. Этот шаг — самый прямой способ
  потерять смысл всего документа: он покупает качество чистым временем в лайтинге.
* `lengthWS = 0` полностью отключает эффект.

### Замечание про приоритет
Из всех «качественных» шагов этот даёт максимум ощущения детализации на вложенное время, и он
**полностью независим от режима теней** — работает и в VSM. Если он окажется дорогим для CSM,
это не повод его выбрасывать: он просто переезжает в общий пост-шейдинг и делится с VSM.

### Откат
`lengthWS = 0`.

---

## S13. Амортизация / кэширование каскадов (опционально, крупная работа)

**Зависит от:** S4. **Эффект:** дальние каскады перестают перерисовываться каждый кадр. **Риск:** высокий.

### Почему
Все 4 каскада перерисовываются каждый кадр (`SceneRenderer.cpp:1392-1458`). UE 5.6 умеет кэшировать
CSM: если центр каскада не изменился — перерисовать только движущиеся примитивы поверх кэша; если
сдвинулся, но перекрытие с кэшем велико — **проскроллить** карту и дорисовать только открывшуюся
L-образную полосу. Ключевое: скроллинг возможен только при texel-snapped каскадах, а в проекте снап
уже сделан корректно (`Scene.cpp:200-220`), так что предпосылка выполнена.

### Два блокера, снять первыми

**1) Очистка всего атласа.** `Renderer.cpp:1489` чистит **весь** DSV. Нужна очистка по тайлу:

```cpp
// В Renderer::BindShadowTarget: чистить только тайлы, помеченные к перерисовке.
void Renderer::ClearShadowTile(ID3D12GraphicsCommandList* cl, int cascadeIndex)
{
    auto& D = rtManager_.Deferred(currentFrameIndex_);
    const render::CascadeAtlasLayout& layout = D.cascadeLayout;
    const int idx = Clamp(cascadeIndex, 0, static_cast<int>(layout.tiles.size()) - 1);
    const render::CascadeTile& t = layout.tiles[idx];
    const D3D12_RECT rect{ static_cast<LONG>(t.x), static_cast<LONG>(t.y),
                           static_cast<LONG>(t.x + t.size), static_cast<LONG>(t.y + t.size) };
    cl->OMSetRenderTargets(0, nullptr, FALSE, &D.shadowDSV);
    cl->ClearDepthStencilView(D.shadowDSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 1, &rect);
}
```
и в `Pass_CSM` заменить единый clear-CL (`SceneRenderer.cpp:1376-1383`) на per-tile очистку внутри
`renderCascade` для тех каскадов, которые обновляются.

**2) Атлас существует в `kFrameCount` копиях** (`RenderTargetManager.cpp:384-405`). Кэш требует
либо одного общего (не per-frame) атласа, либо копирования между кадрами. Первый вариант чище, но
требует ревизии барьеров (`sources/rendering/core/ResourceStateTracker.cpp`).

### Рекомендуемый первый вариант: амортизация (не полный скроллинг)

Обновлять c2 каждый 2-й кадр, c3 каждый 4-й; форсировать обновление, если снапнутый центр каскада
сдвинулся хотя бы на 1 тексель или изменилось направление солнца:

```cpp
// Scene: рядом с cascadeViews_
struct CascadeCacheState
{
    float3 lastCenter{};
    float3 lastSunDir{};
    std::uint32_t lastFrame = 0;
    bool  valid = false;
};
std::array<CascadeCacheState, kCascades> cascadeCache_{};

// В UpdateCascades, после вычисления снапнутого `center`:
bool needsRedraw = true;
if (render::g_csmCachingEnabled && idx >= 2)
{
    CascadeCacheState& st = cascadeCache_[idx];
    const std::uint32_t period = (idx == 2) ? 2u : 4u;
    const bool centerMoved = !st.valid ||
        (center - st.lastCenter).Length() > 0.5f * unitsPerTexel ||
        (sunDirWS - st.lastSunDir).Length() > 1e-4f;
    needsRedraw = centerMoved || ((frameCounter_ - st.lastFrame) >= period);
    if (needsRedraw) { st = { center, sunDirWS, frameCounter_, true }; }
}
cascades.needsRedraw[idx] = needsRedraw;   // новое поле в CascadeData
```
и в `Pass_CSM` пропускать каскады с `needsRedraw == false` (не чистить и не рисовать их тайл).

**Важно:** движущиеся объекты в закэшированных каскадах «залипнут». Для дальних каскадов (100…300 м)
это почти всегда незаметно; если заметно — потребуется разделение кастеров на статических и
движущихся (у проекта уже есть `Scene::BumpStaticSetVersion`, `Scene.cpp:104`, и per-caster буферы
`ShadowGpuData::Rebuild`) и рисование движущихся каждый кадр поверх кэша.

Полный скроллинг (переиспользование перекрывающейся области при сдвиге центра) реализовывать
только если амортизации недостаточно.

### Критерий приёмки
* Время `Pass_CSM` снизилось пропорционально доле пропущенных каскадов.
* Нет визуального «залипания»: при движении камеры дальние тени не отстают и не мигают.
* Резкое движение камеры и вращение солнца корректно инвалидируют кэш.
* Тумблер off даёт baseline.

### Откат
`g_csmCachingEnabled = false`.

---

## 3. Сводка ожидаемого эффекта на каскад 0

| Состояние | Радиус c0 | Тайл | Тексель | Δ | Цена |
|---|---|---|---|---|---|
| baseline | 14.51 м | 2048 | 14.17 мм | — | — |
| +S1 (минимальная сфера) | 13.47 м | 2048 | 13.16 мм | −7 % | **0** |
| +S2 (overlap в текселях) | 11.50 м | 2048 | 11.23 мм | −21 % | **0** |
| +S4 пресет `Balanced` | 11.50 м | 3072 | 7.49 мм | −47 % | ×2.25 растеризации c0 |
| +S4 пресет `Quality` | 11.50 м | 4096 | 5.61 мм | −60 % | ×4 растеризации c0, +34 МБ/кадр |
| +сокращение c0 до 6 м (тюнинг через S0.2) | 6.90 м | 4096 | 3.37 мм | −76 % | c1 берёт на себя 6…35 м |

**Читать эту таблицу надо по последней колонке.** Первые две строки — бесплатное уплотнение
на 21 %, и именно они соответствуют цели «CSM как быстрая альтернатива VSM». Строки S4 — это
покупка качества за время кадра; последняя строка показывает, что тот же результат достижим
перераспределением покрытия, а не памятью и не растеризацией. Дефолт должен собираться из
бесплатных шагов, дорогие остаются пресетами.

Независимо от плотности: **S7** даёт шаг квантования D16 ≈ 0.5 мм вместо 3.11 мм (бесплатно);
**S6 + S8 + S9** позволяют снизить bias (сейчас ≈ 28 мм peter-panning) и заменить бинарное
сравнение мягкой рампой — примерно за те же деньги, при условии, что гейт в S6 и паритет
`Gather` в S8 подтверждены замером.

---

## 4. Рекомендованный порядок

```
S0                                  enabler + перф-baseline, обязательно первым
S1  ->  S2                          дешёвые чистые выигрыши плотности (бесплатно)
S3                                  рефакторинг-enabler для шейдеров
S4  ->  S5                          per-cascade разрешение + gutter   [S4 = пресеты, не дефолт]
S6  ->  S7                          bias в depth-пасе + pancaking
S8  ->  S9                          качество фильтра                  [S8 ТРЕБУЕТ S6]
S10                                 затухание дальней границы
S11                                 перф (тумблер, off по умолчанию)
S12                                 опционально, высокая отдача
S13                                 опционально, крупная работа
```

Жёсткие зависимости, нарушать которые нельзя:
* **S8 после S6.** S8 выбрасывает sample-time bias целиком и опирается на write-time bias из S6;
  без него вся сцена получает серую пелену (см. предупреждение в S8).
* **S7 включает развязку `nearProjLS` / `nearCullLS`.** Без неё pancaking не работает вообще:
  кастеров отбраковывает куллинг (см. «Ловушка» в S7).
* **S5 после S4.** `border` живёт в раскладке атласа, которую вводит S4.

Прочее:
* Минимальный набор для эффекта «как в Days Gone»: **S0 → S1 → S2 → S4 → S5 → S7**.
* Максимальная отдача на вложенное время: **S1 + S2** (две небольшие правки, −21 % размера
  текселя, ноль стоимости) — и это же самый безопасный первый коммит.
* Самые рискованные шаги — **S6** и **S7** (общие с VSM шейдер, PSO, input layout и CB). После
  каждого обязательна проверка VSM-режима (Ctrl+V) и **на идентичность картинки, и на время**
  `VsmPageRender`.
* Каждый шаг после S0 дописывает свою строку в таблицу S0.4. Документ считается выполненным не
  когда сделаны 13 шагов, а когда в этой таблице CSM-строка **и красивее, и дешевле** VSM-строки.

---

## 5. Что ещё можно достать из UE, если понадобится

Документ самодостаточен, исходники Epic для его выполнения не нужны. Но если по ходу вскроется
расхождение и захочется свериться с первоисточником, полезны именно эти файлы (порядок по
убыванию пользы):

| Файл UE 5.6 | Зачем | Для шага |
|---|---|---|
| `Engine/Shaders/Private/ShadowFilteringCommon.ush` | `CalculateOcclusion`, `PCF3x3gather`, `Manual5x5PCF` — точные веса tent-свёртки и форма рампы | S8 |
| `Engine/Shaders/Private/ShadowPercentageCloserFiltering.ush` | receiver-plane depth bias, кламп наклона | S9 |
| `Engine/Shaders/Private/ShadowProjectionPixelShader.usf` | `CSMReceiverBias`, `ShadowSharpen`, коррекция переблюра | S8 |
| `Engine/Shaders/Private/ShadowDepthVertexShader.usf` | `bClampToNearPlane` (pancaking) и применение slope-bias по нормали | S6, S7 |
| `Engine/Source/Runtime/Renderer/Private/ShadowRendering.cpp` | `UpdateShaderDepthBias` — нормировка констант bias'а на диапазон и на тексель | S6 |
| `Engine/Source/Runtime/Engine/Private/Components/DirectionalLightComponent.cpp` | `GetShadowSplitBounds`, `GetShadowSplitBoundsDepthRange` — сфера слайса и fade-плоскости | S1, S10 |
| `Engine/Source/Runtime/Renderer/Private/ShadowSetup.cpp` | аллокатор атласа, `SHADOW_BORDER`, `ComputeScissorRectOptim`, CSM caching/scrolling | S4, S5, S11, S13 |

Первые четыре — единственные, где формулы имеет смысл сверять дословно; остальные нужны только
если возникнет спор о том, как UE что-то решает, а не как считает.
