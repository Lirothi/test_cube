# План улучшения каскадных теней (CSM) в `D:\programming\test_cube`

Документ для ИИ-исполнителя. **Исходники UE лежат локально** — `D:/Programming/ue_strip`
(две ветки: `Shaders/` и `Source/`, карта в `ue_strip/README.md`). Читать оригинал ПЕРЕД тем,
как выводить что-то из первых принципов: сверка 2026-08-31 подтвердила четыре транскрипции
байт-в-байт и вскрыла **три неверных числа** — см. «§5. Сверка с оригиналом UE».
Весь код приведён уже переведённым на типы и конвенции `test_cube` (`mat4`/`float3`, прямой Z
в shadow-атласе, имена полей CB проекта). Алгоритмы восстановлены по реализации CSM в UE 5.6;
названия UE-функций/cvar'ов оставлены как ориентиры, но копий исходников Epic в документе нет.

Каждый шаг — независимо внедряемый и независимо проверяемый.

**Цель.** CSM здесь — не легаси, а осознанно поддерживаемый **быстрый** путь: визуально
конкурентная альтернатива VSM при заметно меньшей стоимости кадра. Отсюда два сквозных следствия:

* каждый шаг оценивается по паре «качество ↔ `Pass_CSM` + `Pass_Lighting`», а не по одному качеству.
  Шаги, покупающие качество временем (пресет `Quality` в S4, ядро 6×6 в S8), обязаны
  быть **пресетами/тумблерами**, а не новым дефолтом; дефолтный пресет должен остаться дешевле VSM;
* VSM живёт в том же билде и **не должен пострадать** — ни визуально, ни по времени (правила 2 и 3).
  Часть шагов трогает общий с VSM код, и там цена измеряется в обоих режимах.

---

## 0-. Сверка с кодом — 2026-08-31, HEAD `f486bdb`

Документ писался на состоянии ~2026-08-03. С тех пор в `master` легло **135 коммитов**
(async compute, VSM single-draw, распил SceneRenderer, RT-пассы, bloom/tonemap, GTAO).
Сверено заново; ниже — только то, что реально разошлось. **Шаги S0/S1/S2 закоммичены и живы**
(`ca887ed`, `4e42c40`, `f8726b7`), их код на месте и работает.

| Что | Было в документе | Сейчас |
|---|---|---|
| `SceneRenderer.cpp:NNNN` | все ссылки | **МЕРТВЫ.** Файл распилен 6703 → 483 строки на `SceneRenderer_{Geometry,Graph,Lighting,Post,Reflections,Shadows}.cpp` + `SceneRenderInternal.h` |
| Очередей команд | одна | **две**: графическая + `computeQueue_` (`GraphicsDevice.cpp:373/386`). На async сидят `Main_BuildAS`, `Main_ObjectCompute`, `Main_RTTrace` |
| `ResourceStateTracker` | файл существовал | **выпилен** (barrier plan). Барьеры = прекомпилированные декларации `AddPass2` |
| PSO индирект-теней | «один, общий с VSM» | **шесть** из одного шейдера: D16/D32 × opaque/masked + `VSM_PAGE` (см. правило 3) |
| Байты 224..239 слота `PageProj` | свободны, S6 хотел их занять | **заняты** `gWindFade`/`w2` — S6 переезжает на 240..255 |
| `normalBiasInTexels` / `depthBiasInTexels` | 0.75 / 2.0 | **1.0 / 1.5** (перетюнено юзером) |
| Baseline S0.4 | CSM 0.456 / VSM 1.267 мс | **устарел, перемерен** — см. S0.4 |

**Что НЕ изменилось** (проверено поимённо): дефолт `g_shadowMode = VSM`; `Main_CSM` отсутствует
в графе в VSM-режиме; `Renderer::BindShadowTarget` по-прежнему хардкодит сетку 2×2
(`Renderer.cpp:2568`); атлас по-прежнему `R16_TYPELESS` / DSV `D16_UNORM` / SRV `R16_UNORM`
и создаётся в per-frame цикле (`RenderTargetManager.cpp:396-417`); `gSmpLinear` по-прежнему
`COMPARISON_MIN_MAG_LINEAR` (`SamplerManager.cpp:156`); `Frustum::FromOrthoBounds` по-прежнему
строит ortho-БОКС с near-плоскостью и кормит GPU-куллинг (`Scene.cpp:346`, `Scene.cpp:1322`) —
**ловушка S7 в силе**; `glass.hlsl` по-прежнему держит вторую копию сэмплирования каскадов
(`glass.hlsl:184-247`) — **S3 в силе**; основная глубина по-прежнему reverse-Z (clear 0.0).

### Актуальная карта символов (взамен мёртвых ссылок)

| Символ | Где теперь |
|---|---|
| `PerViewCB` + `static_assert(... == 224)` | `SceneRenderInternal.h:105-122` |
| `GlassViewCB` | `SceneRenderInternal.h:125` |
| `BuildShadowViewCB` | `SceneRenderInternal.h:231` |
| `shadowAtlasSizeInv` заполнение | `SceneRenderInternal.h:283` |
| `SceneRenderer::Pass_CSM` | `SceneRenderer_Shadows.cpp:253` |
| `IndirectShadowDrawsActive` | `SceneRenderer_Shadows.cpp:171` |
| `LightingPassConstants` каскадный блок | `SceneRenderer_Lighting.cpp:320-333` |
| Регистрация `Main_CSM` в графе | `SceneRenderer_Graph.cpp:326` |
| Декларации `D.shadow` в графе | `SceneRenderer_Graph.cpp:327` (DEPTH_WRITE), `:760`, `:776` (SRV, обе — `Main_Lighting`) |
| Создание PSO индирект-теней | `ShadowGpuData.cpp:1451-1535` |
| `IndirectShadowMaterial` / `...PoolMaterial` | `ShadowGpuData.cpp:366` / `:372` |

**Правило на будущее:** номера строк здесь — на `f486bdb`. Ищи по имени символа, а не по строке.

---

## 0. Правила работы (обязательно к прочтению)

1. **Line endings.** Репозиторий требует CRLF во всех C++/HLSL/project-файлах
   (см. `D:\programming\test_cube\AGENTS.md`). После правок проверить отсутствие смешанных переводов строк
   скриптом из `AGENTS.md`. Это требование репозитория, не рекомендация.
2. **Не ломать VSM-режим.** В проекте два режима направленных теней:
   * Legacy CSM (`Pass_CSM`, атлас `D.shadow`) — предмет этого документа;
   * VSM clipmap (`VirtualShadowMap.cpp`, `vsm_sample.hlsli`) — **не трогать**.
   Переключение — `render::VsmActive()`; в лайтинге ветка `useVsm != 0` (`shaders/lighting_cs.hlsl:338`).

   **⚠ Дефолт билда — VSM**, а не CSM: `sources/rendering/renderables/InstanceTypes.h:133`
   (`inline ShadowMode g_shadowMode = ShadowMode::VSM;`). В VSM-режиме пас `Main_CSM` вообще
   **не попадает в граф** (`SceneRenderer_Shadows.cpp:257`), поэтому **любая проверка любого шага этого
   документа начинается с переключения в Legacy**: Ctrl+V (`AppController.cpp:90-96`, действие
   `ToggleVsmPageRequest`) или тумблер в `DeveloperWindow.cpp:518`. Забыть это — значит «проверить»
   шаг на коде, который не исполнялся.

   Обратная сторона: шаги S6/S7 меняют код, который в VSM-режиме исполняется, поэтому у них
   в приёмке стоит отдельная проверка VSM.
3. **⚠ Что CSM и VSM делят на самом деле** (перепроверено 2026-08-31 — картина сильно изменилась):

   * **Шейдер один** — `shaders/shadow_indirect_csm.hlsl` обслуживает и CSM-каскады, и VSM-страницы. Это по-прежнему так.
   * **Но PSO уже не один, а шесть** (`ShadowGpuData.cpp:1451-1535`), из одного шаблона `GraphicsDesc`:

     | Вариант | DSV | Кто использует |
     |---|---|---|
     | `indirectShadowMat_` / `...MaskedMat_` | **D16** | Legacy CSM-атлас |
     | `indirectShadowPoolMat_` / `...PoolMaskedMat_` | **D32** | VSM per-page цикл-фолбэк |
     | `VSM_PAGE=1` пермутации | D32 | VSM single-draw (основной путь) |

     Они строятся из **одного и того же `gd`**, последовательно мутируемого, — то есть правка raster/depth
     в начале блока всё равно уезжает **во все шесть**. Старое предупреждение в силе, просто механизм другой:
     не «общий объект PSO», а «общий шаблон, из которого штампуют шесть».
   * **Input layout общий** — `PosOnly_InstCasterId` / `PosUV_InstCasterId`
     (`InputLayoutManager.cpp:78-96`) используют все шесть. Предупреждение S6 про `NORMAL` в силе.
   * **А вот CB больше НЕ общий.** В пермутации `VSM_PAGE=1` **`b1` вообще убран из root signature**
     (`shadow_indirect_csm.hlsl:20-48`): один ExecuteIndirect на все страницы не может нести пер-страничные root-аргументы,
     поэтому VS читает проекцию и ветер из SRV через `LoadPageVP(page, w0, w1, w2)`
     (`shadow_indirect_csm.hlsl:138`). `cbuffer PerView` остался только у Legacy CSM и у VSM-цикл-фолбэка.
   * **Слот `PageProj` почти заполнен.** `vsm_page_setup_cs.hlsl:216-229` пишет в 256-байтный слот:
     viewProj в 0..63, ветровой хвост в 192..207 и 208..223, и **теперь ещё `gWindFade` в 224..239**
     (`w2` = camPos.xyz + windFadeEnd). Байты 64..191 не пишутся и не читаются.
     **Свободны только 240..255** — ровно 4 float’а. Это прямо ограничивает S6 (см. там).

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
6. **Шейдеры компилируются в рантайме** через DXC (`sources/materials/Material.cpp` (`CompileDXC`)),
   профиль автоматически поднимается до максимально поддерживаемого (до `SM 6.7`,
   `Material.cpp`), есть hot-reload по времени файла. `Gather()`
   доступен. Производные (`ddx`/`ddy`) в compute-шейдере — **нет** (требуют SM 6.6), см. S9.
7. **Числа** посчитаны для дефолтной камеры (`sources/app/camera/Camera.h` (конструктор): `hfov = 90°`,
   `zNear = 0.01`, `zFar = 10000`) при aspect 16:9 (`vfov ≈ 58.7°`, `tan(vfov/2) = 0.5625`).

---

## 1. Текущее состояние (baseline)

### 1.1. CPU

| Что | Где | Значение |
|---|---|---|
| Число каскадов | `sources/app/scene/SceneFrameData.h:81` | `kCascades = 4` (жёстко) |
| Атлас | `sources/rendering/core/RenderTargetManager.cpp:404`, `.h:87` | `4096 × 4096` |
| Формат | `RenderTargetManager.cpp:491,507,513` | ресурс `R16_TYPELESS`, DSV `D16_UNORM`, **SRV `R16_UNORM`** (важно для S8: `Gather` вернёт нормализованные глубины) |
| Раскладка тайлов | `Scene.cpp:208,335-340` **и** `Renderer.cpp:2576-2588` | жёсткая сетка 2×2, тайл `shadowRes/2 = 2048` — **захардкожено в двух местах** |
| Очистка | `Renderer.cpp:2592` | `ClearDepthStencilView` на **весь** атлас |
| Сплиты | `sources/app/scene/SceneRenderConfig.h:8` | `{10, 35, 100, 300}` м, абсолютные |
| Fit каскада | `Scene.cpp:259` (`ComputeCascadeSphere`) | **S1: минимальная объемлющая сфера** (было — центроид 8 углов) |
| Padding | `SceneRenderConfig.h:11` | `overlap = 2.0f` **мировых единиц** |
| Стабилизация | `Scene.cpp:276-296` | texel snap в фиксированном light-фрейме — **корректно, не менять** |
| «Глаз» света | `Scene.cpp:274` | `lightDistance = max(1, maxDistance) = 300` м для **всех** каскадов |
| Расширение near | `SceneRenderConfig.h:19` | `casterReachWS = 150` м |
| z-padding | `SceneRenderConfig.h:12` | `zPadding = 25` м |
| Bias | `Scene.cpp` (после S1/S2) | normal offset **1.0** текселя + depth bias **1.5** текселя — перетюнено юзером, было 0.75 / 2.0 |
| Depth-функция shadow-PSO | `RenderableObject.cpp:458`, `ShadowGpuData.cpp:1091` | `LESS_EQUAL`, clear `1.0` → **прямой Z** (0 = у света, 1 = далеко) |
| Настройка shadow-PSO (не-индирект) | `RenderableObject.cpp:445-461` | **единственное** место; overrides отсутствуют |
| Настройка shadow-PSO (индирект) | `ShadowGpuData.cpp:1080-1110` | **общий с VSM** (см. правило 3) |
| Rasterizer по умолчанию | `sources/materials/Material.h:66-79` | `DepthClipEnable = TRUE`, `DepthBias = 0`, `SlopeScaledDepthBias = 0` |
| CB лайтинга | `SceneResourceBootstrapper.h:494` (`LightingPassConstants`) + handles/Populate/Update в `.cpp` | добавление поля = HLSL + struct + handle + write |
| PerView CB shadow-паса | `SceneRenderInternal.h:105-122` (`static_assert(sizeof == 224)`) | HLSL: `gbuffer_common.hlsli:72` + **копия** в `shadow_indirect_csm.hlsl` (только при `VSM_PAGE=0`) |
| Input layouts индиректа | `InputLayoutManager.cpp:78-96` | `PosOnly_InstCasterId`, `PosUV_InstCasterId` — **без NORMAL** (см. S6) |
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
depth bias = 1.5 текселя = 16.8 мм peter-panning (на текущем текселе 11.23 мм после S1+S2)
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

**Реализовано (2026-08-31): распределение сплитов по UE доступно как опция.**
`CascadeShadowConfig::useUeSplitDistribution` + `cascadeDistributionExponent` (дефолт 3, кламп
0.1..10) и `ComputeUeSplitDistances()` — дословная транскрипция `ComputeAccumulatedScale` +
`GetSplitDistance` (`DirectionalLightComponent.cpp:753/784`). Включённая опция берёт из конфига
**только `maxDistance`**, промежуточные считает сама и **никогда не пишет в `sliceDistances`** —
ручные значения остаются нетронутыми, тумблер обратим. В dev-окне обе схемы показаны рядом:
неактивная строка гасится, а не прячется, плюс отдельный readout «UE auto splits (m)», который
считается всегда — чтобы сравнить ДО переключения. Проверка: экспонента 3 при 300 м даёт
`{7.51, 30.01, 97.51, 300}`, экспонента 1 — равномерные `{75, 150, 225, 300}`.

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
есть (переключение режима теней в `Scene::Render`).

### S0.3. Debug-режим «cascade tint»

В `LightingPassConstants` (`SceneResourceBootstrapper.h:494`) добавить `uint32_t csmDebugMode = 0;`
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

**ЗАМЕР 2026-08-31, HEAD `f486bdb`** — заменяет августовский: за 135 коммитов кадр потяжелел,
старые числа больше не годятся для решений. wind_test, Release, 2560x1440, DLSS Perf (scale 0.58),
SSR on, камера `-30.74 4.75 70.70`, 30 с прогрева, живой ветер:

| | shadow pass GPU (ms) | `Pass_Lighting` GPU | `GPU.Frame` | `CPU.Frame` | FPS |
|---|---|---|---|---|---|
| **Legacy CSM** | `Pass_CSM` **0.512** | 0.136 | **3.277** | 3.295 | **303** |
| **VSM** | `Pass_VsmPageRender` 2.032 + `Pass_VsmPageRequest` 0.061 = **2.093** | 0.075 | **4.051** | 4.067 | **246** |

Для сравнения, замер 2026-08-03: CSM 0.456 / 2.011 / 496 FPS; VSM 1.267 / 2.800 / 356 FPS.

**Вывод, на котором стоит весь документ, стал СИЛЬНЕЕ: CSM теперь в 4.1 раза дешевле VSM на
теневом пасе** (было 2.8x) **и на 0.77 мс дешевле по кадру (+57 FPS).** Бюджет прежний:
**~0.77 мс GPU** запаса, прежде чем CSM перестанет быть «быстрой альтернативой».

Что поменялось в раскладе:

* `Pass_Lighting` вырос 0.027 -> **0.136 мс** в Legacy. Это всё ещё лишь 4 % кадра, так что вывод
  «качество фильтра почти бесплатно» в силе, но запас уже не «огромен»: S8/S9/S12 суммарно должны
  укладываться в пару десятых миллисекунды, а не в любую.
* **Новое наблюдение:** `Pass_Lighting` в Legacy (0.136) вдвое дороже, чем в VSM (0.075) — цепочка
  каскадов сама по себе дороже выборки клипмапа. Это дополнительный аргумент за S8 (4 `Gather`
  вместо 9 `SampleCmp`): там теперь есть что экономить, чего в августе не было.
* `Pass_CSM` = 16 % кадра (было 23 %), `Pass_Lighting` = 4 % (было 1.3 %). Приоритет прежний:
  сначала бесплатное уплотнение (S1/S2, сделано) и качество выборки, разрешение атласа (S4) —
  последним и только пресетом.

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

## S1. Минимальная объемлющая сфера каскада — ✅ СДЕЛАНО

**Зависит от:** ничего. **Эффект:** −7.2 % текселя c0 (замерено). **Риск:** минимальный. **Цена: 0.**

### ⚠ Найдено при реализации: фит строился по ДЖИТТЕРНУТОМУ фрустуму

`Scene::UpdateCascades` брала углы слайса через `camera.GetInvProjMatrix()` — а это инверсия
**DLSS-джиттернутой** проекции (`Camera.cpp:87-95`: `_31 += jitter.x*2`, `_32 -= jitter.y*2`).
Значит углы слайса, а за ними `sphereRadius`, `unitsPerTexel` и шаг сетки снапа **дрожали каждый
кадр** вместе с субпиксельным джиттером.

Масштаб дрожания: `jitter = jitterPixels / renderWidth` (`Renderer.cpp:1270-1278`), джиттер
±0.5 px ⇒ сдвиг NDC ±1/renderWidth. На far-плоскости c0 (10 м, полуширина 10 м при `hfov 90°`)
и renderWidth 1484 это ≈ ±6.7 мм, т.е. **до ~1 текселя каскада 0 peak-to-peak** — ровно тот
масштаб, который texel snap и обязан прибивать. Для дальних каскадов относительная величина та же
(сдвиг и тексель растут вместе, ~0.5–1 тексель).

Исправлено вместе с S1: фит теперь берёт `camera.GetInvProjMatrixNoJitter()` (аксессор уже
существовал, `Camera.h:60`). Теневая карта не имеет никакого отношения к субпиксельному джиттеру
камеры. **Это предусловие S2:** S2 ужимает padding до ~2 текселей, и с дрожанием в 1 тексель
запаса бы не осталось.

### Почему
(на момент написания) `Scene.cpp` брала центр сферы как **центроид** 8 углов слайса — это не минимальная сфера.
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

Заменить в `Scene::UpdateCascades` блок вычисления `sphereCenter`/`sphereRadius` на:

```cpp
        const CascadeSphere sphere = ComputeCascadeSphere(camera, cornersWS, sliceNear, sliceFar);
        const float3 sphereCenter = sphere.center;
        const float  sphereRadius = sphere.radius;
```

Остальное (`radius`, `unitsPerTexel`, снап, z-диапазон) **не менять** — это S2/S7.

### Критерий приёмки — ВЫПОЛНЕН

| Критерий | Результат |
|---|---|
| `texel` c0 ≈ 13.16 мм вместо 14.17 | **13.16 мм (−7.2 %)** ✅ |
| Замкнутая форма = истинная минимальная сфера | ошибка **0** на 8 канонических случаях, **1.6e-8** на свипе 315 случаев (7 hfov × 5 aspect × 9 слайсов); свободный 3-D поиск подтверждает, что центр лежит на оси (x,y ~ 1e-16); **обе ветки покрыты** — 88 внутренних центров, 227 клампов на плоскость |
| Assert не срабатывает на крайних углах | запас **+1.69 м** минимум по свипу 17 yaw × 13 pitch × 158 направлений солнца × 4 каскада — **больше**, чем у центроида (+1.67). Debug-билд (ассерты активны) отработал headless без срабатывания |
| Нет edge crawl | диф к baseline сидит **только на краях теней** (пятнистая тень от фрондов, кромки листьев); ничего структурного |
| Цена | `Pass_CSM` GPU **0.467 → 0.447 мс**, `GPU.Frame` 1.976 → 1.987 — в пределах шума, шаг бесплатный ✅ |

Замеренные размеры текселя по каскадам (тайл 2048, `overlap` 2.0 м):

| c | слайс | R центроид → S1 | тексель |
|---|---|---|---|
| 0 | 0.01–10 | 12.514 → **11.473** м | 14.17 → **13.16** мм (−7.2 %) |
| 1 | 10–35 | 42.058 → **40.157** м | 43.03 → **41.17** мм (−4.3 %) |
| 2 | 35–100 | 119.249 → **114.735** м | 118.41 → **114.00** мм (−3.7 %) |
| 3 | 100–300 | 358.436 → **344.204** м | 351.99 → **338.09** мм (−3.9 %) |

Выигрыш максимален у c0 — того каскада, ради которого документ и пишется. Это не совпадение:
чем «шире» слайс относительно своей длины, тем дальше центроид от истинного центра.

### Методика проверки (переиспользуется во всех визуальных шагах)
Диф двух захватов сам по себе ничего не доказывает — DLSS темпорален, и два прогона **одного и
того же** билда расходятся. Поэтому измеряется **шумовой пол**: прогон A vs прогон B того же билда
дал 0.58 % пикселей с разницей > 8, а before-vs-after — 1.40 %. Реальный эффект = 1.26 % пикселей,
максимум разницы тот же (194 против 197), т.е. ничего не появилось и не исчезло.
Захваты сравнимы только с `--wind-freeze=<то же значение>`.

**Дополнено на S3.** Во-первых, для ШЕЙДЕРНЫХ шагов «до» снимается на ТОМ ЖЕ бинаре: шейдеры
компилируются в рантайме, поэтому достаточно `git stash push -- shaders/<файлы>`, прогон, `git stash pop`.
Это убирает из сравнения различия сборки целиком.
Во-вторых, **одной пары для шумового пола мало**: на S3 пара `pre vs a` дала 2.63 %, а `pre vs b` —
2.18 % при поле 2.02 %, то есть разброс метрики между прогонами того же билда сопоставим с
измеряемым эффектом. Нужны минимум две пары с каждой стороны.
В-третьих, **у времени должен быть контроль** — скоуп, который шаг заведомо не трогает
(для S3 это `Pass_CSM`). Он «сдвинулся» на +0.6 %, что и есть разрешение замера: любую дельту
меньше этого объявлять эффектом нельзя.

### Замечание (вне рамок S1)
Солнце **точно** в зените/надире (`sunDir` строго вертикален) вырождает
`mat4::LookAtLH(..., up=(0,1,0))` — направление взгляда совпадает с up. `right` в коде имеет
fallback, а `LookAtLH` — нет. Дефект существовал до S1 и им не затрагивается (S1 не трогает
`lightView`), но если понадобится солнце в зените — чинить там.

### Откат
Вернуть центроид (`ComputeCascadeSphere` удалить, 4 строки на месте вызова) и
`GetInvProjMatrix()` вместо `GetInvProjMatrixNoJitter()`.

---

## S2. `overlap` — в текселях, а не в метрах — ✅ СДЕЛАНО

**Зависит от:** S1 (**обязательно**, см. ниже). **Эффект:** −14.7 % текселя c0 поверх S1 (замерено). **Риск:** минимальный. **Цена: 0.**

**Зависимость от S1 оказалась жёсткой, а не «желательной».** S1 убрал дрожание фита от DLSS-джиттера
величиной ~1 тексель. S2 ужимает padding до 2 текселей, оставляя ровно ~1.2 текселя запаса — с
дрожанием в тексель запас бы обнулился. Порядок S1 → S2 обязателен.

### Почему
`SceneRenderConfig.h` задавал `overlap = 2.0f` **мировых единиц**, а `Scene.cpp` делал
`radius = sphereRadius + overlap`. Комментарий у ассерта объясняет, что это padding под
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

### Критерий приёмки — ВЫПОЛНЕН

| Критерий | Результат |
|---|---|
| `texel` c0 ≈ 11.2 мм, −21 % суммарно | **11.23 мм, −20.8 % от baseline** ✅ |
| Assert не срабатывает | запас **+1.20 текселя** (+13.5 мм) по свипу 17 yaw × 13 pitch × 156 солнц × 4 каскада; Debug-билд отработал headless |
| Нет edge crawl | `unitsPerTexel` теперь **строго константа** на каскад (зависит только от FOV/aspect/сплитов — всё инвариантно к повороту, а джиттер убран в S1), т.е. сетка снапа неподвижна по построению. Диф к S1 сидит на краях теней |
| Цена | `Pass_CSM` GPU **0.447 → 0.447 мс**, `GPU.Frame` 1.987 → 1.970 — бесплатно ✅ |

| c | тексель baseline → S1 → S2 | итог |
|---|---|---|
| 0 | 14.17 → 13.16 → **11.23 мм** | **−20.8 %** |
| 1 | 43.03 → 41.17 → **39.29 мм** | −8.7 % |
| 2 | 118.41 → 114.00 → **112.26 мм** | −5.2 % |
| 3 | 351.99 → 338.09 → **336.79 мм** | −4.3 % |

Выигрыш S2 почти весь достаётся c0 — там 2 метра padding были ~15 % радиуса, а у c3 — 0.6 %.
Вместе S1+S2 дают **−20.8 % текселя c0 за ноль миллисекунд**, что и было заявлено как «максимальная
отдача на вложенное время».

### Запас в assert — измерен, не предположен

Свип по ориентациям при разных значениях `overlapInTexels` (worst case по всем каскадам):

| overlap | запас |
|---|---|
| 2.0 м (S1) | +5.00 текселя |
| **2 текселя (дефолт S2)** | **+1.20 текселя** |
| 1.5 текселя | +0.63 текселя |
| 1.0 текселя | +0.21 текселя |
| 0.5 текселя | **−0.42 текселя — assert падает** |

Механика работает как задумано: ниже ~1 текселя ortho перестаёт покрывать снапнутый слайс.
Дефолт 2 оставляет ровно текстель с небольшим запасом — то, что и обещал комментарий в коде.

### Как проверялось, что единицы реально сменились
Прямой тест «поставить 0.5 и ждать assert» на статической сцене **не дискриминирует**: худший
ракурс свипа тестовая камера не достигает, и Debug спокойно отработал. Рабочий аргумент другой:
будь единицы прежними (метры), билды S1 и S2 считали бы **одинаковый** `radius` (11.4735 + 2.0) →
одинаковый `unitsPerTexel` → одинаковый снап → диф на уровне шума. Наблюдается 2.58 % пикселей
с разницей > 8 против шумового пола 0.54 % (среднее 1.43 против 0.77), т.е. картинка реально
изменилась. Отдельно: смена `overlapInTexels` 2 → 0.5 даёт 1.14 % — заметно, хотя тексель меняется
лишь на 0.1 %; это ожидаемо и безвредно, потому что `floor(c/u)*u` при другом `u` попадает на
другое кратное и разово сдвигает всю карту на ~тексель. Внутри кадра `u` константен, сетка не ездит.

### Откат
Вернуть `float overlap = 2.0f;` (мировые единицы) и `radius = sphereRadius + cascadeConfig_.overlap;`.

---

## S3. Вынести сэмплирование CSM в общий `shaders/csm_sample.hlsli` — ✅ СДЕЛАНО

**Зависит от:** ничего. **Эффект:** 0 в lighting-пути (подтверждено замером); **обязательный enabler** для S8/S9/S10/S12. **Риск:** средний (рефакторинг). **Цена: не разрешается замером.**

### Почему
Логика существует в двух копиях: `lighting_cs.hlsl:219-334` и `glass.hlsl:184-247`. Вторая уже
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
    p.atlasSize    = shadowAtlasSizeInv.xy;      // xy = размер атласа (см. GlassView, glass.hlsl:56)
    p.camPosWS     = camPosSky.xyz;
    p.camDirWS     = normalize(camDirWS.xyz);
    p.pcfRadius    = 1.0f;

    int cascade;
    return CsmSampleShadow(p, ShadowAtlas, ShadowSampler, Pws, Nws, NdotL, cascade);
}
```

Третьей копии логики каскадов нет (проверено): `shaders/debug_texture.hlsl` только визуализирует
атлас, `shaders/spotlight_cs.hlsl` работает с отдельным `SpotShadowAtlas`.

**Новый файл — не забыть про проект:** `shaders/csm_sample.hlsli` добавлен и в `test_cube.vcxproj`,
и в `test_cube.vcxproj.filters` (обратные слэши, CRLF), рядом с `caustics.hlsli`.

**⚠ Перф-риск рефакторинга.** `CsmParams` передаётся **по значению** и несёт 4×`float4x4` +
4×`float4` + хвост скаляров (≈ 350 Б). После инлайна DXC это обычно разбирает (SROA), но
`lighting_cs.hlsl` чувствителен к occupancy, а критерий «скриншот идентичен» просадку не поймает.
Если `Pass_Lighting` вырастет — передавать `CsmParams` через `inout`/`in` ссылку либо разбить
на «дешёвую часть + индекс каскада», не таща все 4 матрицы в каждый вызов.

### Критерий приёмки — ВЫПОЛНЕН

| Критерий | Результат |
|---|---|
| Скриншот lighting-пути идентичен | **не отличается от шума.** A/B на ОДНОМ бинаре (шейдеры компилируются в рантайме, поэтому «до» снималось через `git stash` двух шейдеров — тот же .exe): шумовой пол (два прогона одного билда) 2.024 % пикселей с разницей > 8; `pre vs post` дал 2.631 % и 2.178 % — то есть **разброс самой метрики больше, чем зазор до пола**, а у пары `pre vs b` среднее даже НИЖЕ, чем у пары одного билда |
| `Pass_Lighting` не выросло | медиана 3 прогонов: 0.1410 → 0.1430 (**+1.4 %**). **Контроль: `Pass_CSM`, который S3 не трогает вообще, «сдвинулся» на +0.6 %** — значит разрешение замера ~1 %, и +1.4 % не разрешается. Риск `CsmParams` по значению **не материализовался**: DXC инлайнит и разбирает структуру |
| Оба шейдера компилируются | `lighting_cs` cs_6_5 + cs_6_7, `glass` vs/ps_6_5 и пермутация `NORMALMAP_IS_RG` — все автономным `dxc`, до запуска движка |
| Стекло согласовано с геометрией | **сделано в коде, но НЕ продемонстрировано** — см. ниже |

**Про стекло — честно.** Замер на `demo.json` (единственный уровень с `transparentMesh`) дал разницу
**0.158 %** при шумовом поле **0.636 %**, то есть ниже шума. Причина не в том, что правка не
приехала, а в том, что стекло там — зеркальная плита на переднем плане, в глубине каскада 0, где
старый и новый пути обязаны совпадать: они расходятся **только** в margin у границы тайла и в
blend-полосе. Так что доказано «нет регрессии», а не «стало лучше». Улучшение следует из
конструкции (обе площадки зовут одну функцию), но увидеть его можно лишь на сцене, где стекло
попадает на стык каскадов. Проверять глазами при случае.

`wind_test` для проверки стекла **не годится** — там 618 `staticMesh` и ни одного прозрачного
(совпадения по строке «blend» в его JSON — это настройки каустики океана).

### Что реально изменилось в коде
* новый `shaders/csm_sample.hlsli`: `CsmParams`, `CsmChooseCascade`, `CsmPcf3x3`, `CsmSampleChain`,
  `CsmSampleShadow` — перенесены из `lighting_cs` дословно, включая `outCascade` из S0.3 и
  константу `kCsmNoCascade = 4`;
* `lighting_cs.hlsl`: локальные копии удалены, осталась только `MakeCsmParams()` — маппинг имён
  полей ЭТОГО cbuffer на `CsmParams`;
* `glass.hlsl`: собственные `ChooseCascadeIndex` / `ShadowPCF3x3Texture` удалены, `SampleShadowCSM`
  собирает `CsmParams` из своих имён (`shadowAtlasSizeInv.xy`, `camPosSky.xyz`,
  `normalize(camDirWS.xyz)`) и зовёт общую функцию.

Единственные намеренные различия между площадками: имена полей CB, сэмплер
(`gSmpLinear` / `ShadowSampler`) и то, что стекло игнорирует `outCascade`.

### Откат
Удалить файл (и обе строки из vcxproj/filters), вернуть две копии.

---

## S3.5. Один CSM-атлас вместо `kFrameCount` копий — ✅ СДЕЛАНО

**Зависит от:** ничего. **Эффект:** **−67 МБ VRAM в Legacy-режиме**; на пресете `Quality` из S4 это будет −134 МБ. **Риск:** низкий, но это барьеры. **Делать до S4**, иначе S4 меряется в утроенной памяти.

**⚠ Уточнение к первой редакции:** «−67 МБ сейчас» было неточно. Дефолт билда — VSM, а там
`SetLocalShadowResidency(false)` и так ужимает атлас до 1×1 во всех трёх кадрах
(`Scene::ReconcileShadowMode` → `RenderTargetManager::SetLocalShadowResidency`). Так что 100 МБ
существовали **только в Legacy** — то есть ровно в том режиме, ради которого пишется документ,
но говорить «сейчас» про дефолтную конфигурацию было нельзя.

### Почему

CSM-атлас создаётся внутри общего per-frame цикла `RenderTargetManager`
(`RenderTargetManager.cpp:396-417`), поэтому живёт в `render::kFrameCount` = **3 копиях**
по 33.5 МБ = ~100 МБ. VSM-пул (`VirtualShadowMap::EnsureResources`, `VirtualShadowMap.cpp:43`)
создаётся **одним** `CreateCommittedResource` вне всякого цикла по кадрам — одна копия.

**Это не преимущество VSM и не требование CSM.** VSM ничего специального не делает: его пул просто
не прогнали через общий цикл `RenderTargetManager`, где по инерции лежат все deferred-таргеты.

Дублирование по кадрам нужно ресурсу ровно в двух случаях:
* его **пишет CPU**, пока GPU читает предыдущую копию (upload/constant-буферы);
* он **несёт историю между кадрами** (DLSS/TAA history, `physOwnerPrev` у VSM).

CSM-атлас — ни то, ни другое:
* пишется `Pass_CSM` и читается `lighting_cs` + `glass.hlsl` **в том же кадре**; ни один потребитель
  `D.shadowSRV` не смотрит на атлас прошлого кадра (проверено: SRV-таблица лайтинга
  `SceneRenderer_Lighting.cpp`, `TransparentStaticMesh.cpp`, `TextureDebugViewer`);
* **атлас не пересекает границу очередей.** ⚠ Аргумент «в движке одна очередь» УСТАРЕЛ: с
  `async_compute_plan` появилась вторая, `computeQueue_` (`GraphicsDevice.cpp:373/386`), и на ней
  живут `Main_BuildAS`, `Main_ObjectCompute`, `Main_RTTrace`. Но **ни один из них не объявляет
  `D.shadow`**: единственные декларации — `SceneRenderer_Graph.cpp:327` (DEPTH_WRITE, `Main_CSM`)
  и `:760`/`:776` (SRV, обе — `Main_Lighting`), и все три паса на графической очереди. Значит
  внутри графической очереди порядок submit = порядок исполнения, и `Pass_CSM` кадра N+1 не может
  начать писать раньше, чем лайтинг кадра N дочитал.
  **Это условие надо перепроверять**, если теневой или читающий тени пас переедет на async:
  тогда одной копии станет мало без фенса, и вывод шага изменится.

### Код

Вынести создание `D.shadow` из цикла `for (UINT f = 0; f < render::kFrameCount; ++f)` (`:396`) в один
общий слот. `shadowDSV` / `shadowSRV` каждого кадра указывают на один и тот же ресурс — дескрипторы
дублировать можно и нужно (они лежат в per-frame кучах), дублировать не нужно **ресурс**.

Тот же приём, что у VSM: `pagePool_` один, и его состояние транзишенится каждый кадр
(`VirtualShadowMap.cpp:979`). Паттерн в движке уже есть, изобретать нечего.

### ⚠ Что проверить внимательно
⚠ `ResourceStateTracker` **больше не существует** — barrier plan его выпилил, барьеры теперь
компилируются из деклараций `AddPass2`. Поэтому «три состояния схлопнутся в одно» касается не
трекера, а **графа**: три копии `D.shadow` были тремя разными указателями ресурса, после
схлопывания декларации разных кадров указывают на один. Точки, которые надо пересмотреть:
* путь пересоздания атласа в `RenderTargetManager.cpp` (сброс состояния/дескрипторов): теперь
  сработает трижды для одного ресурса;
* `collect(D.shadow)` в списке ресурсов на удержание (`RenderTargetManager.cpp`);
* объявленные состояния в графе (`SceneRenderer_Graph.cpp:327/760/776`) — они по указателю
  ресурса, так что декларации разных кадров схлопнутся; убедиться, что граф это переваривает;
* путь «сжать до 1×1 в VSM-режиме» (`RenderTargetManager.cpp:971`) — теперь одно пересоздание,
  а не три.

Включить GBV на прогон: рассинхрон барьеров — ровно тот класс бага, который он ловит.

### Критерий приёмки — ВЫПОЛНЕН

| Критерий | Результат |
|---|---|
| Скриншот идентичен | **да, в пределах шума.** Кросс-билд диф (S3 → S3.5) 2.407 % и 2.470 % пикселей > 8, при шумовых полах **2.024 %** (пара S3) и **3.043 %** (пара S3.5) — то есть эффект лежит МЕЖДУ двумя полами |
| VRAM | 3 × 33.55 МБ → **1 × 33.55 МБ**, освобождено **67.1 МБ** в Legacy. Косвенно подтверждено: `--canonical-check` рапортует **238** объявленных ресурсов вместо 240 |
| GBV чистый | `--scene-stress=6 --scene-stress-gbv` (Debug): **verdict CLEAN**, барьеров 3243 enhanced / 0 legacy. Прогон покрывает ReloadLevel, **SwitchLevel**, **ResizeWindow**, DlssMode, RenderScale, ReflectionScale — resize и reload как раз пересоздают все таргеты, то есть новый путь создания |
| Атлас корректно завершает кадр | `--canonical-check`: **`CascadeShadow` НИ РАЗУ не появился в off-canonical**. В списке только `Ocean.SurfSim{Wave,Foam}{A,B}` — пинг-понг по построению, к шагу отношения не имеет. Это прицельная проверка главного риска: три ресурса схлопнулись в один, и состояние теперь переносится между кадрами по-настоящему |
| Барьеры совпадают с декларациями | `--barrier-cmp`: **0 mismatch** за прогон |
| Переключение режима не крэшит | оба режима грузятся и рисуют; VSM-загрузка прогоняет путь ужатия до 1×1 (теперь один раз вместо трёх) |
| Цена | `Pass_CSM` 0.509 → 0.510, `GPU.Frame` 3.283 → 3.245 — без изменений |

### Что реально изменилось в коде
* `RenderTargetManager::shadowAtlas_` — **единственный владелец** (`GpuResource`);
* `DeferredTargets::shadow` из `GpuResource` стал **невладеющим** `ID3D12Resource*`-алиасом на него;
* `CreateShadowResource` потерял параметр `f`: создаёт ресурс один раз и внутри цикла строит
  **пер-кадровые DSV/SRV** (дескрипторы живут в пер-кадровых кучах, общий только ресурс);
* вызов вынесен из пер-кадрового цикла `Create()`; в `SetLocalShadowResidency` — тоже один вызов;
* `Destroy` освобождает `shadowAtlas_`, а алиасы просто обнуляет;
* call-sites: `SceneRenderer_Graph.cpp` ×3, `SceneRenderer_Post.cpp`, `TextureDebugViewer.cpp` —
  `D.shadow.Get()` → `D.shadow`.

### Почему это безопасно (модель барьеров, проверено по коду)
`CanonicalStateRegistry::Entry::predicted` хранит **одно** состояние **на ресурс** и передаёт его
«из этого кадра в следующий» (`RenderGraph.h`, комментарий у `CompileBarriers`), а НЕ пересеивает
из canonical каждый кадр. С тремя копиями у каждой была своя запись; с одной запись общая и
состояние течёт в порядке сабмита — для модели это честнее, а не рискованнее. Fixed-point-тест
(«ресурс обязан закончить там же, где начал») держится: атлас идёт NON_PIXEL → DEPTH_WRITE
(`Main_CSM`) → NON_PIXEL (`Main_Lighting`), то есть кэш барьеров по-прежнему разрешён.

### Инструмент, который пришлось сделать
`--canonical-check` и `--barrier-cmp` пишут в `OutputDebugString`, а готового захвата DBWIN
в проекте не было. Скрипт лежит в скрэтчпаде (`dbwin.py`). Грабли: `MapViewOfFile` без
явного `restype` усекается ctypes до 32 бит на x64, и всё чтение из буфера — мусор.

### Откат
Вернуть `GpuResource shadow` в `DeferredTargets`, параметр `f` в `CreateShadowResource`, вызов —
внутрь пер-кадрового цикла.

---

## S3.6. Контракт «кастер = LOD приёмника» для CSM (как в UE) — ✅ СДЕЛАНО

**Зависит от:** ничего. **Эффект:** убирает самозатенение от рассогласования LOD кастера и приёмника; приводит CSM к поведению UE. **Риск:** средний — трогает общий cull/args-путь (CSM + spot + point).

### Как это устроено в UE (сверено по дропу 2026-08-31)

`FProjectedShadowInfo::CalcAndUpdateLODToRender` (`ShadowSetup.cpp:1770`):

```cpp
FLODMask ShadowLODToRender = CurrentView.PrimitivesLODMask[PrimitiveId];
if (ForcedLOD > -1 || !ShadowLODToRender.IsValid() || bShadowLODDistanceFactorEnabled)
{
    ... ComputeLODForMeshes(...) ...
    if (!bShadowLODDistanceFactorEnabled) CurrentView.PrimitivesLODMask[PrimitiveId] = ShadowLODToRender;
}
```

**`CurrentView` — это КАМЕРНОЕ вью**, не теневое: вызывающая сторона (`ShadowSetup.cpp:2079`) —
`for (FViewInfo& CurrentView : Views)`. LOD является свойством пары (примитив, камера), кэшируется
на вью, и теневой пас — его **потребитель**. Если камера примитив не оценивала (офскринный кастер),
тень считает сама, но по той же камерной метрике, и кладёт в тот же кэш.

**Все дефолты выключены, и все ручки идут только в сторону «грубее»:**

| Ручка | Дефолт | Направление |
|---|---|---|
| `r.Shadow.LODDistanceFactor` | **1** | пересчёта нет, берётся кэш камеры |
| `r.Shadow.LODDistanceFactor.CascadeScale` | **0** | **пер-каскадного масштаба нет** |
| `GFarShadowStaticMeshLODBias` | **0** | грубее, только для отдельного far-каскада |
| `r.Shadow.PreshadowsForceLowestDetailLevel` | **0** | грубее |

То есть **UE не применяет к обычным каскадам никакого пер-каскадного огрубления.** Дальние каскады
получают грубую геометрию не по назначению, а потому что камера сама огрубила объект по дистанции.
Наш `ShadowTierBaseLod(cascade) + g_shadowLodBias` аналога в UE не имеет.

*(Побочно: `ModifyViewForShadow` первым делом зовёт `HackRemoveTemporalAAProjectionJitter()` —
третье независимое подтверждение фикса джиттера из S1.)*

### Что у нас сейчас

Пер-инстансный контракт **есть, но только в VSM**: `casterLod_` (байт на слот кастера, бит 7 =
chunk EXACT), заполняется `RefreshCasterLods` и читается **только** `vsm_page_scatter_cs` /
`vsm_page_setup_cs`. CSM во всех трёх подпутях берёт LOD **из индекса каскада**:

| Путь | Источник |
|---|---|
| индиректный | `viewLod_[v] = ShadowTierBaseLod(v) + g_shadowLodBias` |
| CPU-фолбэк | `shadowLod = cascadeIndex` |
| GI-хвост | `giLod = cascadeIndex` |

При дефолтах это LOD 0/1/2/2 по каскадам. Направление «кастер грубее приёмника» закрыто случайно
(каскад 0 = тир 0 = LOD0), а «кастер финее приёмника» — открыто, и это семейство «чёрных квадратов».

**Хорошая новость:** `RefreshCasterLods` зовётся в `Scene::Render` **безусловно**, не под VSM-гейтом.
Данные уже есть каждый кадр; не хватает только потребителя на стороне CSM.

### ⚠ Найденный по дороге живой баг (исправлен отдельно, до основной работы)

`shadow_cull_clear_cs.hlsl` читал `PerGroup[g]`, тогда как привязан к нему `perViewGroup_` с
раскладкой `(v * numGroups + g) * 4` — **пер-(вью, группа)**. Индексация по `g` всегда доставала
запись **вью 0**. Итог: `IndexCountPerInstance` / `StartIndexLocation` во всех вью несли диапазоны
LOD каскада 0, а `RecordIndirectShadowDraws` биндил индексный буфер по `ViewLodAt(viewSlot)` —
то есть для каскадов 1–3 диапазон был от чужого LOD.

Исправление — `PerGroup[idx]`. Замерено: **6.65 % и 6.35 %** различающихся пикселей против шумовых
полов **3.04 %** и **2.58 %**, среднее выросло вдвое; диф локализован ровно в средней и дальней зоне,
передний план (каскад 0, где вью 0 == свой вью) не изменился. Это тот случай, когда «свой» LOD
каскада вообще не доезжал до аргументов.

### Конструкция

Ключевое наблюдение: **как только тир-кривая уходит из CSM, LOD кастера перестаёт зависеть от вью** —
он одинаков для всех каскадов, ровно как `PrimitivesLODMask` в UE. Значит бакетирование можно
сделать один раз, а не в скаттере на каждое вью.

1. **Виртуальные группы**: `vg = group * kMaxShadowLods + lod`, где `lod` — LOD приёмника из
   `casterLod_`. Ось групп во всех Rung-0-таблицах умножается на `kMaxShadowLods`.
2. **Cull** вычисляет `vg` сам: к нему добавляется SRV на уже существующий `casterLodBuf_`,
   `vg = CasterGroup[c] * kMaxShadowLods + (CasterLod[c] & 0xF)`. Новый CPU-буфер не нужен.
3. **Базы слайсов видимого списка** — префикс-сумма по per-vg счётчикам, считается на CPU
   (обе величины CPU-side) и заливается per-frame. Суммарно это по-прежнему `casters`, поэтому
   **`visibleList_` НЕ растёт** — в отличие от VSM, который дал каждой vg полную ёмкость группы.
4. **`viewLod_[0..3] = 0`** — тир-кривая перестаёт действовать на каскады. VSM не затронут: он
   читает только локальные (`ViewLodAt(vsmView + 4)`) и клипмап-записи.
5. **Draw loop** идёт по vg и пропускает глобально пустые (счётчики известны на CPU), так что
   ×4 по числу ExecuteIndirect не материализуется.

Пер-вью пол (`max(vgLod, viewLod[v])`) сохраняется — он ничего не стоит структурно и нужен, чтобы
`g_shadowLodBiasNearTier` продолжал управлять локальными источниками.

### Критерий приёмки — ВЫПОЛНЕН

| Критерий | Результат |
|---|---|
| `Pass_CSM` | **0.510 → 0.369 мс** (−28 %), `GPU.Frame` 3.245 → 3.081. Не подорожало, а **подешевело** |
| GBV | `--scene-stress=6 --scene-stress-gbv`: **CLEAN**, включая ResizeWindow/SwitchLevel (пересоздают аргументы) |
| VSM не затронут | `Pass_VsmPageRender` 2.032 → 2.026, `GPU.Frame` 4.051 → 4.019. По коду: VSM читает из `viewLod_` только локальные (+4) и клипмап-записи, `Rung0Args` в `vsm_page_setup_cs` **объявлен, но нигде не индексируется**, а видимый список Rung-0 вне `ShadowGpuData` вообще не имеет потребителей |
| Тени на месте | визуально полный набор теней от стволов и фрондов, ничего не потеряно |
| Репро `--set=lod.forced:2` | **0.456 % пикселей посветлело против 0.237 % потемневших** — 2:1 в сторону снятия acne. Направление верное, но эффект скромный |

**⚠ ОТОЗВАНО: «шумовой пол схлопнулся до 0.000 %».** Первая пара прогонов после S3.6 дала
попиксельно одинаковый кадр, и я прочитал это как обретённую темпоральную стабильность. Это было
**совпадение**: оба прогона сняли кадр номер **9641** — одна и та же фаза DLSS-джиттера и одна и та
же история аккумуляции. Следующая пара (кадры 9608 и 9585) дала обычные **2.330 %**.
Настоящий шумовой пол здесь — **2.3…3.0 %**, как и раньше.

Урок ровно тот, что уже записан в методике выше и который я всё равно нарушил: **одной пары мало**,
и у пары надо смотреть номер кадра (`frame=` в шапке профдампа). Совпадение номеров = вырожденное
сравнение.

### ⚠ Честные оговорки про атрибуцию
* Числа `Pass_CSM` и репро `lod.forced` **смешивают два изменения**: исправление индексации
  `shadow_cull_clear_cs` и собственно контракт. Разделить их можно только отдельной сборкой;
  направление у обоих одинаковое, поэтому разделение отложено.
* Визуальный диф S3.6 — **5.66 % и 5.14 %** против шумового пола **2.58 %**, то есть примерно вдвое
  выше пола. Сигнал есть, но одна из сравниваемых пар оказалась вырожденной (см. отзыв выше), так что
  это «уверенно выше шума», а не точное число.

### Что реально изменилось в коде
* `ShadowGpuData`: `numVirtualGroups_ = numMeshGroups_ * kMaxShadowLods`; новый пер-кадровый
  `perGroupVg_` ({base, indexCount, lodRelStart, casterCount}); `RefreshVirtualGroups()` считает
  бакеты и префикс-сумму баз на CPU; `casterGroupCpu_` — копия отображения кастер→группа;
* `viewLod_[v] = 0` для `v < kNumCascades`;
* `shadow_cull_cs.hlsl`: новый SRV `CasterLod` (t4), `vg = CasterGroup[c] * kMaxShadowLods + lod`;
* `shadow_cull_clear_cs.hlsl`: диапазоны стали вью-независимыми, индексация по группе снова корректна;
* `RecordIndirectShadowDraws`: цикл по виртуальным группам, IB по LOD бакета, пустые бакеты
  пропускаются на CPU (поэтому ×4 по оси групп НЕ даёт ×4 по числу ExecuteIndirect);
* **CPU-пути тоже переведены на контракт** (их легко пропустить — индиректный путь их не исполняет):
  `SceneRenderer_Shadows.cpp` больше не передаёт `cascadeIndex` как LOD ни в CPU-фолбэке, ни в
  непараллельной ветке — вместо этого `ReceiverCasterLod(obj, camPos)`, считающая LOD приёмника
  свежо от камеры, как `RefreshCasterLods`. GI-хвост зафиксирован на LOD0 (у GI-групп зарегистрирована
  только LOD0-геометрия), а не на индексе каскада;
* удалена ставшая мёртвой таблица `perViewGroup_`: с LOD, уехавшим в идентификатор группы, диапазоны
  зависят только от (группа, LOD), и ось вью не несла информации.

Видимый список **не вырос**: префикс-сумма по бакетам по-прежнему в сумме даёт `count_`.

---

## S4. Индивидуальное разрешение каскадов + rect-таблица атласа

**Зависит от:** S3.5 (иначе память меряется втрое). Делать после S1/S2. **Эффект:** ×2 плотности c0 — это и есть «лютый нулевой каскад». **Риск:** средний.

### ⚠ Почему — с ИСПРАВЛЕНИЕМ (сверено с дропом 2026-08-31)

Первая редакция утверждала: «UE не раскладывает каскады сеткой: каждый каскад — самостоятельная
запись **со своим разрешением**». **Половина этого неверна.**

Верно: каскад в UE — отдельный `FProjectedShadowInfo`, который аллокатор пакует в атлас, а не ячейка
фиксированной сетки. Неверно: **разрешения у них НЕ индивидуальные.** `ShadowSetup.cpp:5585-5588`:

```cpp
const int32 MaxCSMResolution = GetCachedScalabilityCVars().MaxCSMShadowResolution;
const int32 MinCSMResolution = 32;
FIntPoint ShadowBufferResolution = FIntPoint(
    FMath::Clamp(MaxCSMResolution, MinCSMResolution, (int32)GMaxShadowDepthBufferSizeX) - ShadowBorder * 2, ...);
```

Выражение не зависит от индекса каскада: **все каскады получают ОДИН И ТОТ ЖЕ размер**, заданный
одним cvar `r.Shadow.MaxCSMResolution` (**дефолт 2048**, `ConsoleManager.cpp:4095`).

Отсюда два следствия:
* **наш текущий тайл 2048 уже равен дефолту UE** — по разрешению каскада мы не отстаём;
* **S4 — это ОТСТУПЛЕНИЕ от UE, а не транскрипция.** Идея «дать c0 4096, а c1..c3 оставить 2048»
  в UE не имеет прообраза; там подняли бы `MaxCSMResolution` для всех сразу. Это не значит, что
  шаг плохой — плотность c0 действительно ограничивает картинку, — но обосновывать его ссылкой на
  UE больше нельзя, и меряться он должен строже: сам по себе, а не «как у них».

Шейдер **уже resolution-agnostic** — работает через `cascadeScaleBias[c]` и считает margin как

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

    enum class CascadeAtlasPreset : std::uint32_t { Legacy = 0, Quality = 1 };

    inline CascadeAtlasLayout MakeCascadeAtlasLayout(CascadeAtlasPreset preset, std::uint32_t border)
    {
        CascadeAtlasLayout L{};
        L.border = border;
        switch (preset)
        {
        case CascadeAtlasPreset::Legacy:   // ровно текущее поведение: сетка 2x2 по 2048, 33.5 МБ
            L.width = 4096; L.height = 4096;
            L.tiles = { { {0, 0, 2048}, {2048, 0, 2048}, {0, 2048, 2048}, {2048, 2048, 2048} } };
            break;
        case CascadeAtlasPreset::Quality:  // c0 x2 плотности, c1..c3 БЕЗ ухудшения. 67 МБ (после S3.5)
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

**Почему всего два пресета, без «среднего».** Промежуточный вариант (c0 = 3072) не существует
без того, чтобы **ужать c1..c3 до 1024** — а 1024 на каскад, покрывающий 10…35 м и дальше, это
явно мало и хуже, чем сейчас. Держать в атласе 3072 + 3×2048 требует 7168×4096 = 58.7 МБ, то есть
всего на 12 % дешевле `Quality` при заметно меньшем c0 — отдельного пресета не стоит.
**Средний вариант уже есть и он бесплатный:** слайдеры `maxDistance` / `sliceDistances` из S0.2.
Сокращение покрытия c0 даёт непрерывную регулировку плотности вообще без памяти и без
растеризации (последняя строка таблицы в §3: −76 % текселя).

**⚠ Память.** Атлас 4096×4096 @ R16 = **33.5 МБ**; `Quality` 8192×4096 = **67 МБ**.
Числа приведены **после S3.5** (один атлас). Без S3.5 умножать на `render::kFrameCount` = 3:
100 МБ и 201 МБ соответственно — вот почему S3.5 идёт первым.
`Quality` не использует блок `2048×2048` в углу (`x 6144..8192, y 2048..4096`) = 8.4 МБ, 12.5 %
атласа. Приемлемо: альтернатива — непрямоугольная упаковка ради 8 МБ. Залогировать при создании.

**⚠ Перф-бюджет — ключевой для этого шага.** Цель документа — CSM быстрее VSM, а этот шаг
единственный, который платит временем напрямую: c0 растёт 2048² → 4096², то есть **×4 площади
растеризации самого нагруженного каскада**. Бюджет из S0.4 — около 0.8 мс GPU, а `Pass_CSM` сейчас
0.447 мс, из которых c0 — заметная доля. Поэтому:
* дефолт остаётся **`Legacy`**; `Quality` включается осознанно из S0.2;
* в приёмке — не «рост соответствует площади», а **записанные числа** против таблицы S0.4,
  и явный вывод, остаётся ли `Quality` дешевле VSM. Если `Pass_CSM` подходит к VSM-строке
  (1.267 мс) — весь смысл теряется, и правильный ответ не «принять пресет», а «сократить
  `maxDistance` / сплиты через S0.2 и добрать плотность там».

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
  (`Scene.cpp:1322`) и в `enqueueView` (`Scene.cpp` — `enqueueView(cascadeView)`) — **в обоих режимах**.

То есть вырожденная раскладка в VSM-режиме превратила бы каскадный куллинг из «отбраковывает» в
«пропускает всё» и добавила бы работы там, где её сейчас нет. Guard `atlasReady` остаётся ровно
тем, чем был guard `tileRes == 0`: защитой от «атлас ещё не создан», и в VSM-режиме не срабатывает.

*(Отдельное наблюдение, вне рамок этого документа: каскадные view куллятся каждый кадр и в
VSM-режиме, хотя `Main_CSM` в графе отсутствует. Слоты 0..3 в `UpdateViewFrustums` можно было бы
занулять при `VsmActive()` тем же приёмом, каким Legacy зануляет клипмап-слоты, `Scene.cpp:1334-1339`.
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
// LightingPassConstants (SceneResourceBootstrapper.h:494) + handle + write:
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
`GlassViewCB` (`SceneRenderInternal.h:125`) — **в хвост**, чтобы не сдвинуть существующие смещения.

### Критерий приёмки
* `TextureDebugViewer` (`sources/rendering/debug/TextureDebugViewer.cpp`) показывает новую раскладку:
  c0 занимает большой тайл, c1..c3 — маленькие; ни один каскад не рисует за пределы своего тайла.
* `Legacy`-пресет даёт **идентичный** baseline-скриншот (проверка, что рефакторинг корректен).
* На `Quality` тени в c0 заметно резче, на границах c0↔c1 нет разрывов.
* VSM-режим (Ctrl+V) переключается без крэша, атлас сжимается до 1×1.
* **Перф — числа, а не «соответствует».** Заполнить и приложить к коммиту:

  | Пресет | `Pass_CSM` GPU | Δ к baseline S0.4 | дешевле VSM? |
  |---|---|---|---|
  | `Legacy` | | должно быть **0 %** (проверка корректности рефакторинга) | |
  | `Quality` | | | |

  `Legacy` обязан совпасть с baseline и по времени, и по скриншоту — иначе рефакторинг раскладки
  что-то сломал. Если `Quality` подходит к VSM-строке по `Pass_CSM` — шаг **не принимать как
  дефолт**: цель документа (CSM дешевле VSM) при этом теряется; вместо повышения разрешения
  сокращать покрытие c0 через S0.2 (последняя строка таблицы §3 даёт −76 % текселя без роста атласа).

### Откат
`g_cascadeAtlasPreset = Legacy`.

---

## S5. Настоящий gutter + clamp UV вместо fallback по границе тайла — ✅ СДЕЛАНО 2026-09-01

**Реализовано короче, чем в плане, и без S4.** План строил шаг поверх `CascadeAtlasLayout` из S4
(`layout.ContentSize`, `layout.tiles`, border-scale матрица на проекции). Ничего этого не нужно:
вьюпорт в `Renderer::BindShadowTarget` **уже пер-каскадный**, поэтому достаточно вдвинуть его
внутрь и вывести все текселечные величины из content-размера. Ровно так это и сделано у UE —
вьюпорт на `(X + BorderSize, Y + BorderSize)` размером `ResolutionX` — а border-scale матрица у них
относится к матрице СЭМПЛИРОВАНИЯ, чей эквивалент у нас `atlasScale`/`atlasBias`.

**Что изменилось:**
* `render::kCascadeAtlasBorder = 4` в `ShadowSettings.h` (значение UE, `ShadowSetup.cpp:831`, и так же
  ВЫРЕЗАНО из тайла, а не добавлено вокруг): тайл 2048 → контент **2040**.
* `BindShadowTarget` рисует во внутренний прямоугольник; очистка атласа накрывает всё, поэтому
  border остаётся 1.0 = «далеко» = «не в тени».
* `Scene::UpdateCascades`: `unitsPerTexel`, `texelEstimate`, оба bias'а и `atlasScale`/`atlasBias`
  считаются от `contentRes`, а не от `tileRes`. Тексель стал крупнее на 0.4 % — цена gutter'а.
* `csm_sample.hlsli`: margin-тест шириной в `reach` текселей заменён честным `[0,1]`;
  `uvClampAll` теперь полтекселя внутрь content-прямоугольника; `CsmPcf3x3` получил параметр
  клампа — раньше legacy-ветка полагалась на margin вызывающего, которого больше нет.

**Замер — и он подтверждает оговорку самого плана, а не первую редакцию.**
A/B на ОДНОМ бинаре (margin-тест временно возвращён в шейдер, шейдеры компилируются в рантайме),
камера юзера, натив + фикс. экспозиция:

| дальность теней | пикселей сменило каскад | картинка (meanAbs / пикс > 2) | шумовой пол |
|---|---|---|---|
| 60 м | **0.007 %** | 0.0145 / 0.063 % | 0.0124 / 0.052 % |
| 300 м (штатная) | **0.014 %** (514 пикс.) | 0.0165 / 0.058 % | 0.0079 / 0.016 % |

И это **не кольцо вокруг каждого каскада**: все 514 пикселей лежат одной полоской у горизонта
(строки 11..34). Моя формулировка «видно через Cascade tint» в §6 была преувеличением — вписано
сюда, чтобы не повторять.

**Ради чего шаг тогда сделан** (и это то, что план говорил с самого начала):
* **корректность `Gather` по построению.** Раньше промах аппаратного округления удерживался
  margin'ом и клампом в 1 тексель; теперь он попадает на ОЧИЩЕННЫЕ текселя border'а, то есть
  «не в тени» — безвредно независимо от округления;
* **предпосылка для плотной упаковки атласа (S4)**: как только соседний тайл может оказаться другим
  каскадом, цена промаха вырастает с «чуть грубее» до «тень из другого места сцены»;
* 0.014 % пикселей перестали молча сваливаться на каскад грубее.

### Критерий приёмки

| Критерий | Результат |
|---|---|
| Нет скачка/просвечивания на границе тайла | картинка в пределах ~2× шумового пола, регрессий нет |
| Cascade-tint: переход идёт не по границе тайла | да — демоция исчезла, 514 пикс. → 0 |
| Border остаётся clear | по построению: `ClearDepthStencilView` без rect'а накрывает атлас, а вьюпорт+scissor депт-пасса в него не заходят. **Прямого дампа атласа headless нет — это доказано конструкцией, не замером** |
| Гейт | `--scene-stress-gbv=3 --barrier-cmp --canonical-check`: `verdict: CLEAN`; собраны все ТРИ конфигурации |

### Откат
`kCascadeAtlasBorder = 0` — тогда `contentRes == tileRes`, вьюпорт возвращается на весь тайл, а
кламп UV становится no-op'ом по построению.

---

## S5-original. Настоящий gutter + clamp UV вместо fallback по границе тайла

**Зависит от:** S3. ~~S4~~ — **зависимости от S4 НЕТ** (сверено 2026-08-31): она была артефактом того, что я положил `border` внутрь S4-шной структуры раскладки. Нужен только сам border-констант; текущая сетка 2×2 его принимает без изменений. **Эффект:** корректность выборки у границы тайла + предпосылка для плотной упаковки атласа. **Риск:** низкий.

### ⚠ Сверено с дропом 2026-08-31 — здесь транскрипция ВЕРНА

Редкий случай в этом документе: всё сошлось.
* `SHADOW_BORDER = 4` (`ShadowSetup.cpp:831`) ✅
* Border **вырезается из тайла**, а не добавляется к нему: `MaxShadowResolution = ... - SHADOW_BORDER * 2`
  (`ShadowSetup.cpp:3423`), то есть `ResolutionX` у UE это уже КОНТЕНТ, а выделенная область —
  `ResolutionX + 2*BorderSize` (`ShadowRendering.h:361`). Ровно наш `ContentSize` ✅
* Border-scale матрица: `BorderScale = ResolutionX / (ResolutionX + 2*BorderSize)`,
  `BorderScaleMatrix = FScaleMatrix(FVector(BorderScale, 1.0f))`, `ProjectionOuter = ProjectionInner * BorderScaleMatrix`
  (`ShadowSetup.cpp:921-925`, `ShadowRendering.cpp:1004`) — пост-умножение, **z не масштабируется** ✅
* Вьюпорт ставится на `X + BorderSize, Y + BorderSize` размером `ResolutionX` ✅

### ⚠ Честно о величине эффекта (переоценено в первой редакции)

«Убирает видимое кольцо» — вероятно, преувеличение. Margin срабатывает у края ORTHO-КВАДРАТА,
а слайс вписан в него сферой: точки у края квадрата лежат в дальних УГЛАХ слайса, где blend-полоса
к следующему каскаду обычно уже активна. Ширина полосы отказа сегодня — `reach` текселей в локальных
UV, то есть после S8 (`reach = 2`) около **34 мм** на краю каскада 0. Тонко.

Реальная ценность шага другая и она есть:
* **корректность `Gather`.** S8 сейчас держится на затычке — клампе тапов к прямоугольнику тайла с
  отступом в 1 тексель и margin в 2 текселя. Настоящий gutter из очищенных (1.0 = «не в тени»)
  текселей делает промах аппаратного округления безвредным **по построению**, а не по отступу;
* **предпосылка для S4**: при плотной упаковке соседний тайл может оказаться другим каскадом с
  другим содержимым, и цена промаха вырастает с «чуть грубее» до «тень из другого места сцены».

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

**2) Border-scale проекции.** В `Scene::UpdateCascades` после построения `lightProj` (`Scene.cpp:327`):

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
(`Frustum::FromOrthoBounds`, `Scene.cpp:346`) оставить по **немасштабированным** `minX..maxY`:
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

## S6. Slope-scaled и constant depth bias в shadow-depth vertex shader — ✅ СДЕЛАНО 2026-08-31

**Ключ:** `--set=csm.biasAtWrite:1` (dev-окно: CSM → «Bias (S6: at write vs at sample)» →
«Bias in the depth pass»). **Дефолт OFF** — и это проверенный no-op, см. приёмку.

### Три отступления от плана, все в лучшую сторону

**1) Хвост CB на 224, а не на 240.** План резервировал 240..255, боясь разъехаться с байтом 224
страничного слота VSM (`gWindFade`). Разъехаться не с чем: пермутация `VSM_PAGE=1` **вообще не
объявляет `cbuffer PerView`** — она читает свои пер-вью данные из слота через `LoadPageVP`. Это две
независимые раскладки. Хвост лёг на 224..239, `static_assert(sizeof(PerViewCB) == 240)`, дырки
`_pad224` нет, и 240..255 слота остались свободны как были.

**2) `SHADOW_DEPTH_BIAS` — пермутация PSO, а не рантайм-гейт.** План боялся, что +12 Б на вершину
(NORMAL) ударят по VSM, и предлагал гейтить ALU, а перестановку PSO держать как запасной ход. Но
перестановка **уже была**: четыре VSM-PSO (`indirectShadowPage*`, `indirectShadowPool*`) строятся
после `gd.defines.clear()` и со своим `inputLayoutKey`. Поэтому NORMAL добавлен в **новые** layout'ы
`PosNrm_InstCasterId` / `PosNrmUV_InstCasterId`, которые биндят только два атласных PSO (Legacy CSM
+ spot/point). VSM не платит ни выборкой, ни ALU, ни ветвлением — и это **доказано**, а не
обосновано (см. приёмку).

**3) Bias ПЕРЕЕЗЖАЕТ, а не добавляется.** У UE один и тот же `DepthBias` живёт в депт-пасе, а
сэмплер сравнивает сырую глубину. У нас он жил в сэмплере (`z - b`). Прибавить второй = удвоить.
Поэтому `depthBiasAtWrite` — **один флаг на обе половины**: он же включает write-time bias, он же
обнуляет `b` в `csm_sample.hlsli`. Это и есть «второй заход S8», который план откладывал.

### Что именно транскрибировано из дропа

`ShadowDepthVertexShader.usf:50-105` (`SetShadowDepthOutputs`) и `ShadowRendering.cpp:1840-1912`
(`UpdateShaderDepthBias`):

* `Slope = clamp(NoL > 0 ? sqrt(saturate(1 - NoL²))/NoL : MaxSlope, 0, MaxSlope)` — дословно;
* `ShaderSlopeDepthBias = max(DepthBias * SlopeScaleDepthBias, 0)`, `SlopeScaleDepthBias = 3`;
* `ShaderMaxSlopeDepthBias = 1` (**не 3** — в первой редакции документа стояло 3.0);
* `DepthBias = min(DepthBias, .1f)` с их же комментарием про near-plane clipping.

**Одно расхождение сознательное.** UE берут ось света как **ненормированную** колонку
`(M[0].z, M[1].z, M[2].z)` своей `WorldToShadowMatrix` — у них эта колонка выходит единичной по
построению цепочки матриц. У нас `OrthoOffCenterLH` кладёт туда `1/(far-near)`, так что дословная
транскрипция превратила бы `NoL` в `cos/zRange` и загнала КАЖДУЮ вершину в кламп. Поэтому
`normalize()`. Ровно тот случай, про который [[transcription-half-a-pair]].

### Приёмка — измерено 2026-08-31, Release, wind_test, `--dlss=off --set=exposure.autoExposure:0`

| Критерий | Результат |
|---|---|
| **Дефолт = точный no-op** | новый бинарь на дефолтах против бинаря ДО правки (сборка из стеша): meanAbs **0.038**, 0.17 % пикселей — **ниже шумового пола** 0.054..0.067 / 0.29..0.40 % |
| **VSM не подорожал** | `Pass_VsmPageRender` 0.078 / 0.079 → **0.079 / 0.080** мс; `GPU.Frame` 4.021/4.092 → 4.057/4.062. Разброс пары «до» сам 0.001 |
| **VSM бит-в-бит не тронут** | сильнее замера: DXIL пермутаций `VSM_PAGE=1` и `VSM_PAGE=1+SHADOW_MASKED=1` **побайтово идентичен** до и после (md5 `fcf2d117…`, `02cfece1…`). Двойники пула (`SHADOW_DEPTH_BIAS=0`) отличаются ТОЛЬКО объявлением CB (224 → 240 Б): **199 инструкций до и 199 после** |
| `Pass_CSM` не вырос | 0.125 / 0.123 → **0.125 / 0.126** с фичей ВЫКЛ (выборка нормали есть, ALU гейт съел). С фичей ВКЛ **0.126 / 0.131**, т.е. +0.0045 мс = **+3.6 %** от 0.13 мс пасса, или 0.0045 из 3.1 мс кадра |
| Acne на скользящем свете | метрика: доля пикселей, освещённых в эталоне и потемневших > 20. Пол (тот же ран дважды) **0.0039 %**. При **одинаковом** бюджете bias'а на скользящей поверхности (1/4 от дефолта): на сэмпле **19.93 %**, в депт-пасе со slope **16.37 %** — slope-член снимает **18 % акне** |
| Bias не вырос ради этого | `biasAtWrite:1` + `depthBias:0.75` против дефолтного `depthBias:1.5` на сэмпле: acne **0.0010 %** (ниже пола), meanAbs 0.257 / 2.33 %. То есть тот же результат при вдвое меньшем bias'е на обращённых к солнцу поверхностях |
| Индирект и CPU-фолбэк согласованы | S6 сдвигает картинку на 0.257 (индирект) и **0.287** (CPU-фолбэк) — один порядок, bias применяется одинаково. **NB:** сами два пути расходятся на **8.03** meanAbs, и ровно так же (8.06) с фичей ВЫКЛ — расхождение ПРЕДСУЩЕСТВУЮЩЕЕ, к S6 отношения не имеет (у CPU-пути нет b2, см. комментарий в `gbuffer_instcb_csm.hlsl`). Отдельная задача |
| Шейдеры компилируются | автономный dxc: обе новые `SHADOW_DEPTH_BIAS` пермутации + `gbuffer_csm` / `gbuffer_inst_csm` / `gbuffer_instcb_csm` |
| `static_assert(sizeof(PerViewCB))` | **240**, проходит в обеих конфигурациях |
| Гейт ресурсов | `--scene-stress-gbv=3 --barrier-cmp --canonical-check` в Debug: **`verdict: CLEAN after 3 iterations`**, GBV подключён, компаратор барьеров молчит (2118 enhanced / 0 legacy) |

### S6-fix: галка удваивала бюджет bias'а — исправлено (репорт юзера, 2026-08-31)

Юзер включил чекбокс на wind_test и получил «лютый питер-пеннинг». Причина не в шейдере, а в том,
что **галка молча меняла смысл соседнего слайдера**:

```
на сэмпле (OFF): bias = D * (1 + (1 - NoL))            -> худший случай 2D  (при NoL = 0)
в депт-пасе (ON): bias = D * (1 + slopeScale * slope)   -> худший случай 4D  (при дефолтах UE)
```

То есть один и тот же «Depth bias 1.50» покупал вдвое больше при включённой галке. У UE такого
множителя нет, потому что их сэмплер **вообще не применяет depth bias** — их `CSMDepthBias = 10`
калиброван прямо против `1 + SlopeScale*MaxSlope`. Наш 1.5 калиброван против другой формулы.
Ровно [[transcription-half-a-pair]]: множитель, отменённый в другом пассе.

Исправлено в `ComputeCascadeDepthBias`: константа нормируется как `D * 2 / (1 + slopeScale*maxSlope)`,
так что **худший случай совпадает в обеих арматурах**. Слайдер теперь означает одно и то же с галкой
и без: «максимум bias'а, который я готов платить, в текселях». `slopeScale` / `maxSlope` перестали
менять бюджет и меняют только его РАСПРЕДЕЛЕНИЕ по углам падения — что и есть смысл шага.
В dev-окне под слайдерами теперь строка `effective: X texels facing the sun, Y grazing`, чтобы
бюджет не был спрятан.

**Замер, wind_test, камера юзера `-70.35 26.61 85.11`, натив + фикс. экспозиция.**
Шумовой пол: acne **0.0061..0.0089 %**, meanAbs **0.013..0.017**, lift ±0.004.

| арм | acne | сдвиг яркости ТЕНИ | meanAbs vs OFF@1.5 |
|---|---|---|---|
| OFF, bias 1.5 (эталон) | 0.0061 % | — | — |
| **ON, bias 1.5 — ДО фикса** | — | **+1.190** | **1.093** |
| ON, bias 1.5 — после фикса | 0.0061 % | +0.386 | 0.718 |
| **ON, bias 1.15 — точка совпадения** | 0.0078 % | **−0.133** | **0.251** |
| ON, bias 0.90 | 0.0211 % | −0.979 | 2.287 |
| ON, bias 0.70 | 0.0336 % | −2.031 | 5.172 |

Фикс срезал уход тени в **3.2 раза** (1.190 → 0.386). А точка, где картинка совпадает с сегодняшней,
— **bias 1.15 вместо 1.5**, то есть с S6 тот же результат стоит на **23 % меньше** bias'а.

**Голодный стресс (bias 0.35, чтобы акне вылезло):** OFF **35.93 %** против ON **24.23 %** —
slope-член снимает **треть** акне. Это и есть весь профит шага, и виден он только там, где бюджет
поджат.

**Честный вывод про эту сцену.** Солнце в wind_test стоит на ~28°, а `maxSlope = 1` означает, что
slope-член насыщается на всём, что отвёрнуто от солнца больше чем на 45°. Плоский песок — за
клампом. Поэтому на пляжной сцене при дефолтном bias'е S6 **выигрывать нечего**: акне нет ни в одной
арматуре, и видно только остаточное перераспределение. Шаг окупается там, где бюджет поджат или где
геометрия разнообразна по ориентации (склоны, скаты, пандусы). **Дефолт остаётся OFF.**

### Дефолт переключён на UE-арматуру + ревизия набора ручек (юзер, 2026-08-31)

Юзер: «поставь дефолтом bias 1.15 и включи галку, и оставь только парамы как в UE, а то их 4 и я
путаюсь». Сделано:

* `depthBiasAtWrite = true`, `depthBiasInTexels = **1.15**` (было false / 1.5). Проверено:
  новый дефолт против прежнего — meanAbs **0.248**, 0.80 % пикселей, acne **0.0065 %** (пол 0.007),
  сдвиг тени −0.131. То есть картинка та же, а бюджет bias'а на 23 % меньше.
* Секция «Bias» разбита на две: **«Bias — UE (r.Shadow.CSM*)»** (галка + Depth bias + Slope scale +
  Max slope + строка `effective:`) и **«Bias — not in UE»** (Normal bias). Четыре числа никуда не
  делись, но теперь видно, какие три — транскрипция, а какое одно наше.

**`normalBiasInTexels` НЕ удалён, и это измерено, а не мнение.** У UE legacy CSM действительно не
имеет никакого receiver normal offset (`NormalBias` в дропе встречается только в Lumen/RT/MegaLights,
а `r.Shadow.Virtual.NormalBias` — это VSM). Но у нас он несущий:

| арм | acne | сдвиг тени | meanAbs vs эталон |
|---|---|---|---|
| новый дефолт (normal 1.0, depth 1.15) | **0.0065 %** | −0.131 | **0.248** |
| normal 0, depth 1.15 | **21.77 %** | −4.627 | 12.71 |
| normal 0, depth 1.5 | 0.0415 % | −2.372 | 6.02 |
| normal 0, depth 2.0 | 0.0335 % | −0.285 | 1.22 |

Убрать его ради паритета с UE = либо 21.8 % акне, либо задрать depth до 2.0 и всё равно получить
вчетверо больше акне, чем сейчас, при впятеро большем уходе картинки. UE обходится без него потому,
что их `CSMDepthBias = 10/zRange` — заметно больший бюджет, и часть работы берёт их transition-рампа.
Причина расхождения записана в тултип самой ручки, чтобы вопрос не возникал заново.

**Галка оставлена.** Она больше не «фича», а A/B-рычаг и аварийный откат на сэмплерную арматуру;
по правилу [[controls-must-not-lie]] её место — в удалении, но только после того, как новый дефолт
поживёт на реальных сценах.

Гейт после смены дефолта: `--scene-stress-gbv=3 --barrier-cmp --canonical-check` — `verdict: CLEAN`.

### S6-final: паритет с UE, потом замер, который его побил (2026-09-01)

Юзер: «если у них нет normalBiasInTexels — выпиливай и повторяй остальные парамы как у них».
Сделал буквально, и это вскрыло настоящую причину, по которой normal bias вообще завёлся.

**Наш bias был вчетверо меньше UE-шного, и normal bias это затыкал.**
```
UE:   DepthBias = CSMDepthBias(10) / zRange * WorldSpaceTexelScale,  WorldSpaceTexelScale = radius / res
наш:  depthBiasNDC = depthBiasInTexels * unitsPerTexel / zRange,     unitsPerTexel = 2*radius / tileRes
```
`WorldSpaceTexelScale` — ПОЛОВИНА нашего текселя, значит **их 10 = наши 5.0**. Мы крутили 2.0 → 1.5 →
1.15, то есть меньше четверти их бюджета. То же число задаёт и ширину полутени
(`ComputeTransitionSize` — то же самое выражение), так что рампа и bias теперь совпадают с UE
одновременно, и §6 пункт 6 закрылся сам.

**Проверка размерностей (юзер напомнил, что у UE дефолтные юниты — сантиметры).** В формуле выше
длина стоит и в числителе, и в знаменателе — единицы сокращаются, конверсия 10 → 5.0 верна в любых.
Все остальные перенесённые константы безразмерны (slopeScale 3, maxSlope 1 — кламп на tan,
`min(bias, 0.1)` — уже NDC, receiverBias 0.9, sharpen ×7+1, CascadeTransitionFraction 0.1,
exponent 3) либо в текселях (MaxCSMResolution 2048, SHADOW_BORDER 4). Единственная длинная величина
— `DynamicShadowDistanceMovableLight = 40000` uu = **400 м**, и в S8 она так и считалась
(20000 на строке 1423 — legacy-апгрейд, не дефолт).

**Что убрано.** `depthBiasAtWrite` (переключателя на сэмплерную арматуру больше нет — при верном
бюджете она пере-биасена по построению, то есть контрол бы лгал), поле `biasAtWrite` из `CsmParams`,
вычитание `z - b` в сэмплере, и нормировка `×2/(1+slopeScale·maxSlope)` из
`ComputeCascadeDepthBias` — константа идёт сырой, как у UE. `normalBiasWS[]` в CB переименован в
`cascadeTexelWS[]` и несёт то, чем всегда был под множителем: мировой размер текселя каскада.

**Голый паритет с UE замерен и проигрывает.** wind_test, камера юзера, натив + фикс. экспозиция,
пол: acne 0.007 %, сдвиг тени ±0.004.

| depth | normal | acne | питер-пеннинг |
|---|---|---|---|
| 5.0, normal нет — **буквально UE** | — | 0.0387 % | **+3.130** |
| 3.0 | нет | 0.0363 % | +1.930 |
| 1.0 | нет | 0.0346 % | −0.811 |
| 0.5 | нет | 78.58 % | −6.296 |

Акне обрывается между 0.5 и 1.0, а выше 1.0 стоит **плоско до 7.0** — то есть весь бюджет свыше
единицы не покупает НИЧЕГО, кроме питер-пеннинга. UE ставят 10 потому, что должны накрыть любую
сцену вслепую; мы свою можем померить.

**Юзер посмотрел UE на дефолтах: «и акне и питер пеннинг ппц какие».** Совпало с замером. Значит
`normalBiasInTexels` вернулся — уже не как костыль под заниженный bias, а как ручка, которой у них
нет. Она бьёт по акне **со стороны приёмника**: сдвигает точку выборки, а не записанную глубину,
поэтому питер-пеннингом почти не платит. В CB отдельного массива не завела — скаляр в
`csmFilterParams.w`, умножается на уже лежащий там `cascadeTexelWS[c]`.

**Сетка (depth × normal), тот же пол:**

| depth | normal | acne | питер-пеннинг |
|---|---|---|---|
| 0.25 | 1.0 | 12.08 % | −3.684 |
| 0.5 | 0.5 | 5.02 % | −3.524 |
| 0.5 | 1.0 | 0.0121 % | −0.979 |
| 0.5 | 1.5 | 0.0115 % | +0.318 |
| **1.0** | **0.5** | **0.0000 %** | **+0.238** ← дефолт |
| 1.0 | 1.0 | 0.0027 % | +0.575 |
| 1.5 | 1.0 | 0.0000 % | +1.026 |

**Дефолт `depthBiasInTexels = 1.0`, `normalBiasInTexels = 0.5`: ноль акне при питер-пеннинге в
13 раз меньше, чем на дефолтах Unreal.** Две ручки не взаимозаменяемы, и в этом весь смысл: depth
bias двигает записанную глубину (за это платим отрывом тени), normal bias двигает точку выборки
(почти бесплатно), поэтому тратить надо на вторую, а первую держать ровно на обрыве акне.
Подтверждение дважды: acne 0.0027 / 0.0089 %, сдвиг +0.241 / +0.241.

Гейт: `--scene-stress-gbv=3 --barrier-cmp --canonical-check` — `verdict: CLEAN`.

### Не сделано в этом шаге, сознательно

* **`glass_csm.hlsl`** — единственный из пяти shadow-depth VS, у которого НЕТ `b1`: он держит
  world/view/proj в собственном `PerObject` b0 и не видит ни `gbuffer_common.hlsli`, ни хвоста CB.
  Стекло — тонкий прозрачный кастер, slope-scaled bias ему нечего давать; расширять его CB ради
  этого не стал.
* **`clampToNear` (S7)** прокинут до шейдера и до `ApplyShadowDepthBias`, но CPU всегда шлёт 0.
  Включать его — это S7, вместе с разделением `nearProjLS` / `nearCullLS`.
* **`depthBiasInTexels` не тронут** (остался 1.5). Он верен для арматуры «bias на сэмпле»; при
  `biasAtWrite:1` его надо ронять примерно вдвое — 0.75 измерено выше. Менять дефолт до того, как
  юзер посмотрит на уровне, смысла нет: это ручка вкуса, а не корректности.

### Оригинальный план шага (для истории)

## S6-original. Slope-scaled и constant depth bias в shadow-depth vertex shader

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
размер текселя. Нормировка у нас уже правильная (`Scene.cpp:330-334`) — не хватает slope-члена
и переноса bias в depth-пас.

### Код

**1) `sources/app/scene/SceneRenderConfig.h`** — добавить:

```cpp
    // S6: slope-scaled bias при записи глубины. slopeScale множит константный NDC-bias;
    // maxSlope ограничивает tan(угла) — иначе поверхность ребром к свету требует бесконечный bias.
    // Значения — из UE (сверено 2026-08-31): r.Shadow.CSMSlopeScaleDepthBias = 3,
    // r.Shadow.ShadowMaxSlopeScaleDepthBias = 1. ⚠ maxSlope именно 1.0, а НЕ 3.0 —
    // в первой редакции этого документа было 3.0, то есть втрое мягче клампа, чем у UE.
    float slopeScale = 3.0f;
    float maxSlope   = 1.0f;
    // S7: pancaking. 1 = вершину перед near-плоскостью прижать к ней вместо отсечения.
    float clampToNear = 0.0f;
```

**⚠ ОБНОВЛЕНО 2026-08-31: хвост CB переезжает с 224 на 240.** Когда документ писался, байты
224..255 слота `PageProj` были свободны. Теперь 224..239 занял `gWindFade` (`w2` = camPos + windFadeEnd,
`vsm_page_setup_cs.hlsl:229`, читается через `LoadPageVP`). Осталось ровно **240..255 = 4 float'а** —
столько S6 и нужно, но **запаса больше нет**: следующее поле в этот слот уже не влезет, придётся
расширять шаг слота с 256 байт. Ниже всё смещено на 240; `static_assert` становится **256**, а не 240.

Дополнительно: в основной VSM-пермутации (`VSM_PAGE=1`) **`b1` отсутствует** (см. правило 3), поэтому
«записать нули в `vsm_page_setup_cs`» — уже не единственное, что нужно: значения оттуда читает
`LoadPageVP`, и его надо расширить (`w3 = PageProjRows[b + 15u]`) либо явно передать нули в
`ApplyShadowDepthBias` на этом пути.

**2) `SceneRenderInternal.h`** — расширить `PerViewCB` (`:105-122`), **строго в хвост**:

```cpp
        float windPrevGustMul = 1.0f;
        // S6/S7: параметры bias'а shadow-depth паса. Смещение 240 — ПОСЛЕДНИЕ свободные 16 байт
        // 256-байтного слота PageProj (224..239 занял gWindFade/w2, см. правило 3). Для VSM там
        // пишутся нули, поэтому ApplyShadowDepthBias на VSM-путях — no-op.
        float _pad224[4] = {};          // 224..239: занято gWindFade в VSM-слоте, не трогать
        float shadowConstBias = 0.0f;   // 240
        float shadowSlopeBias = 0.0f;   // 244
        float shadowMaxSlope  = 0.0f;   // 248
        float shadowClampNear = 0.0f;   // 252
    };
    static_assert(sizeof(PerViewCB) == 256, "PerViewCB must match the gbuffer/shadow HLSL layout");
```

**3) `BuildShadowViewCB`** (`SceneRenderInternal.h:231`) — принимать индекс каскада и заполнять:

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
В `Pass_CSM` (`SceneRenderer_Shadows.cpp:253+`) передавать `cascades.depthBiasNDC[cascadeIndex]`,
`cascades.depthBiasNDC[cascadeIndex] * cfg.slopeScale`, `cfg.maxSlope`, `cfg.clampToNear`.
(Это ровно `ShaderSlopeDepthBias = DepthBias * SlopeScaleDepthBias` из UE — сверено.)
**Добавить кламп UE, которого в первой редакции не было:** `DepthBias = min(DepthBias, 0.1f)`
(`ShadowRendering.cpp:1905`, комментарий Epic: «Prevent a large depth bias due to low resolution
from causing near plane clipping»).
Для spot/point передавать нули — их bias-путь этот шаг не затрагивает.

**4) HLSL PerView — в ДВУХ местах, байт-в-байт одинаково.**
`shaders/gbuffer_common.hlsli` (после `:82`) и `shaders/shadow_indirect_csm.hlsl` (после `:74`):

```hlsl
    float4 _pad224;           // 224..239 = gWindFade/w2 в VSM-слоте (см. правило 3)
    float  shadowConstBias;   // 240 (S6)
    float  shadowSlopeBias;   // 244
    float  shadowMaxSlope;    // 248
    float  shadowClampNear;   // 252 (S7)
```

**5) `shaders/vsm_page_setup_cs.hlsl`** — после `:229` добавить (240, НЕ 224 — 224 занят):

```hlsl
    // S6/S7: хвост shadow-bias параметров PerView. VSM-страницы получают НУЛИ: ApplyShadowDepthBias
    // тогда прибавляет 0 и не клампит, т.е. VSM-путь остаётся бит-в-бит прежним. Без этой записи
    // байты 240..255 слота остались бы мусором (см. правило 3). Это ПОСЛЕДНИЕ свободные байты слота.
    PageProj.Store4(po + 240u, asuint(float4(0.0f, 0.0f, 0.0f, 0.0f)));
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

    // S7 pancaking: кастер перед near-плоскостью прижимается к ней, а не отсекается.
    // Сверено с UE (ShadowDepthVertexShader.usf:67-71): там reverse-Z и потому
    // `if (z > w) { z = 0.999999; w = 1; }` — зеркальный эквивалент нашего прямого Z.
    // Эпсилон UE стоит взять: клампить не в ровный 0, а чуть внутрь. UE клампит ДО прибавления
    // bias, мы — после; наш порядок безопаснее (bias не вытолкнет панкейкнутую вершину обратно).
    // Кламп в VS
    // возвращает вершину внутрь фрустума, поэтому DepthClipEnable менять НЕ нужно (что критично:
    // PSO индирект-теней общий с VSM). Побочный эффект тот же, что и в UE: если у треугольника
    // часть вершин клампится, а часть нет — треугольник деформируется. Это принятый компромисс.
    if (params.w > 0.0f) { H.z = max(H.z, 1e-6f); }
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

**9) После этого уменьшить sample-time bias:** `SceneRenderConfig.h` `depthBiasInTexels`
с текущих `1.5` до `0.5…1.0` (подобрать на глаз через S0.2) — его роль теперь берёт depth-пас.

### Критерий приёмки
* На наклонной геометрии (склон, пандус, скат крыши) при низком солнце acne исчезло **без**
  увеличения общего bias.
* Контакт объекта с землёй не «отъехал» дальше baseline (peter-panning не хуже).
* **VSM-режим (Ctrl+V) визуально идентичен baseline** — главная проверка корректности этого шага.
* **VSM-режим не подорожал:** `VsmPageRender` GPU в пределах +2 % от S0.4 — главная проверка
  *цены* этого шага (см. предупреждение в начале). Если вырос — ранний выход в
  `ApplyShadowDepthBias` не сработал (проверить дизассемблер/`--profdump`), следующий ход —
  перестановка PSO.
* Все 5 shadow-шейдеров компилируются; `static_assert(sizeof(PerViewCB) == 256)` проходит.
* Пермутация `VSM_PAGE=1` тоже собирается и её путь получает нули (там `b1` нет — значения идут
  через `LoadPageVP`, см. правило 3).
* Индирект-путь (`render::g_indirectShadowsEnabled`) и CPU-fallback дают одинаковые тени.

### Откат
`slopeScale = 0` → формула становится no-op'ом. Полный откат — вернуть `static_assert` на 224
и убрать хвост CB (вместе с `_pad224`).

---

## §5.5. Где живут настройки теней (переезд 2026-09-01)

Вопрос юзера: «почему настройки теней раскиданы по разным файлам?». Аудит показал пять мест, из них
одно — чистый нарост, и он мой.

**Было.** `rendering/renderables/InstanceTypes.h` — заголовок про ИНСТАНСНЫЕ и submission-типы —
держал `g_shadowMode` / `VsmActive()`, `g_indirectShadowsEnabled`, `g_giIndirectShadowsEnabled`,
`g_csmDebugMode` и `g_csmFilterMode`. Ничего из этого не имеет отношения к раскладке вершин
инстанса; они там оказались потому, что первый теневой тумблер кто-то дописал в конец этого файла,
а дальше все следующие клали рядом.

**Стало.**
* Новый `sources/rendering/shadows/ShadowSettings.h` — процессное состояние теневой СИСТЕМЫ, то,
  что охватывает ОБА алгоритма: `ShadowMode`/`g_shadowMode`/`VsmActive()`, оба индирект-тумблера,
  и `CsmDebugMode`/`g_csmDebugMode`. Дебажный вид остался глобалом сознательно: он в одном ряду с
  `render::g_lodDebugMode`, а `--csm-tint` парсится в `main.cpp` **до** того, как появится Scene.
* `g_csmFilterMode` → `CascadeShadowConfig::filterMode`. Это пер-сценовая настройка КАЧЕСТВА,
  которая крутится вместе с `csmReceiverBias` / `shadowFilterSharpen` / `pcfOverBlurCorrection` в
  одной группе UI; то, что она жила отдельно от них, объясняется только тем, что режим ядра добавлен
  в S8, а три спутника в S8b. Все четыре потребителя (`--set=`, lighting CB, glass CB, dev-окно)
  уже держали `cascadeConfig`, так что переезд ничего не усложнил.
* В `InstanceTypes.h` остались только СТРУКТУРЫ ДАННЫХ, которые теневые пути читают
  (`ShadowViewFrustum`, `ShadowCasterBounds`, нота про общий stride `InstancePerObject`) — это
  раскладки, а не настройки, и им там место.

**Проверка, что переезд механический.** Первый замер соврал: `meanAbs 0.938 / 9.3 %` — но эталон
был снят бинарём ДО того, как юзер поправил `g_csmFilterMode` 2→1 и `slopeScale` 3.0→2.0 (правки в
00:12/00:15, exe от 00:00). Контроль на СОВПАДАЮЩИХ настройках
(`--set=csm.filterMode:2 --set=csm.slopeScale:3.0`) дал **meanAbs 0.0166 / 0.043 %** при шумовом поле
**0.017 / 0.066 %** — ровно ноль. Гейт `--scene-stress-gbv=3 --barrier-cmp --canonical-check`:
`verdict: CLEAN`. Собраны все ТРИ конфигурации, включая `Release_Editor`.

**Осталось разбросанным, и это осознанно:** `vsm::` глобалы клипмапа (другой алгоритм),
`SpotLight::desc_.shadowNormalBias` (пер-источник, из JSON уровня),
`g_shadowLodBias`/`g_shadowLodTierStride` в `LodSelect.h` (это про выбор LOD, а не про тени; и они
намеренно процессные, чтобы переживать смену уровня).
**Не сделано и стоит отдельного шага:** `kCsmBlendFraction = 0.1` вкомпилен в `csm_sample.hlsli`,
хотя у UE это артистское `CascadeTransitionFraction` — работа S10; и **`CascadeShadowConfig` нигде
не сериализуется**, в JSON уровня нет ни одного теневого поля, поэтому вся настройка в дев-окне
живёт до выхода, а единственное постоянное хранилище — дефолты в заголовке.

---

## §6. Что осталось до «1 в 1 как у UE» — сверено по дропу 2026-08-31

Полный аудит после S6/S8b. Всё ниже прочитано в `D:/Programming/ue_strip`, не по памяти.

### Уже совпадает — перепроверять не нужно

| что | наше | UE | где смотрел |
|---|---|---|---|
| Формат атласа | D16_UNORM | `PF_ShadowDepth` = `R16_TYPELESS` | `D3D12RHI.cpp:137` |
| Разрешение каскада | 2048 | `r.Shadow.MaxCSMResolution` = 2048 | `ShadowSetup.cpp:5586` |
| Пер-каскадное разрешение | нет | **тоже нет** | `ShadowSetup.cpp:5585-5588` |
| Фит каскада | мин. охватывающая сфера | `GetShadowSplitBoundsDepthRange`, включая `max(sqrt,1)` | S1 |
| Ядро PCF | 6×6 тент, 9 гейтеров | `Manual5x5PCF` при `r.ShadowQuality`=5 | S8b |
| Sharpen / over-blur / receiver bias | есть, порядок UE | `ShadowProjectionPixelShader.usf:375, 385` | S8b |
| Bias при записи: const + slope | есть | `SetShadowDepthOutputs` | S6 |
| Сэмплер сравнивает СЫРУЮ глубину | да (при `biasAtWrite`) | `Settings.SceneDepth = LightSpacePixelDepthForOpaque` | `ShadowProjectionPixelShader.usf:248` |
| `CascadeBiasDistribution` | всегда тексель-масштабированная форма | дефолт **1** = ровно она | `DirectionalLightComponent.cpp:1010` |

**S9 (receiver-plane bias) для паритета НЕ НУЖЕН** — он живёт в `USE_PCSS`, а это отдельная
пермутация, по умолчанию выключенная. Вычеркнуть из списка «до 1:1».

**Архитектурная разница, которая НЕ является пробелом.** UE рисует каждый каскад отдельным
скрин-спейс проекционным пассом в light attenuation buffer и смешивает их альфой fade-плоскости
(`OutColor.a = BlendFactor`). Мы разрешаем каскад по-пиксельно внутри `lighting_cs`. Результат
эквивалентен, наш путь дешевле; переносить их схему смысла нет.

### Осталось шесть штук, по убыванию видимого эффекта

**1. Нет затухания последнего каскада — жёсткая линия на `maxDistance`.**
UE для ПОСЛЕДНЕГО каскада двигает `FadePlane` ВНУТРЬ на `FadeExtension`, чтобы тень растворилась,
а не оборвалась (`DirectionalLightComponent.cpp:936-941`). У нас `CsmSampleShadow` при `idx == 3`
просто возвращает несмешанный результат, а за каскадом — `return 1.0f`. Это S10, и это самый
заметный пункт.

**2. Полоса смешивания меряется не от той величины и стоит не с той стороны.**
Наше `band = splitNext * kCsmBlendFraction` — 10 % от АБСОЛЮТНОЙ дистанции.
UE: `FadeExtension = (SplitFar - SplitNear) * CascadeTransitionFraction` — 10 % от СОБСТВЕННОЙ
длины каскада (`:925`). На каскаде 2 (35→100 м) это 10 м против 6.5 м.
Плюс UE РАСШИРЯЕТ `SplitFar` на `FadeExtension` для не-последних каскадов (`:929`), то есть держит
полную детализацию до сплита и смешивает ЗА ним; мы тратим последние 10 % каждого каскада на
смешивание внутри него. `CascadeTransitionFraction` = 0.1, кламп ≤ 0.3.

**3. Настоящий gutter (S5) — единственный пункт, который стоит РЕАЛЬНОГО качества.**
UE резервируют `SHADOW_BORDER = **4**` текселя: рендерят в `MaxCSMResolution - 2*4` = **2040** и
кладут это внутрь тайла 2048 с четырёхтексельной каймой (`ShadowSetup.cpp:831, 5587`). Мы рендерим
все 2048, а на краю тайла `CsmSampleChain` **уходит на каскад грубее**. При ядре 6×6 (reach = 3
текселя) это кольцо в 3 текселя вокруг КАЖДОГО каскада, сброшенное на следующий уровень — ровно то
кольцо, которое видно через `Cascade tint`.

**4. Панкейкинг (S7).** `bClampToNearPlane` (`ShadowDepthVertexShader.usf:66-70`). У нас параметр
прокинут до `ApplyShadowDepthBias`, но CPU всегда шлёт 0, потому что сперва нужно разделить
`nearProjLS` / `nearCullLS` — иначе фрустум отсечения съест ровно тех кастеров, ради которых
панкейкинг и существует (ловушка задокументирована в §S7).

**5. Дефолтная раскладка сплитов не их.** `ComputeUeSplitDistances` написан и проверен
(exp 3, 300 м → 7.51 / 30.01 / 97.51 / 300), но `useUeSplitDistribution` по умолчанию **выключен**,
и уровень несёт свои `{10, 35, 100, 300}`. Это одна галка, а не работа.

**6. Отношение «ширина рампы / bias» вдвое не то.** У UE и ширина полутени
(`ComputeTransitionSize`), и константа записи выводятся из ОДНОГО сырого `CSMDepthBias`.
После нормировки S6 наша константа записи = `depthBias * 2/(1 + slopeScale*maxSlope)`, а рампа
по-прежнему берёт сырой `depthBias` — значит отношение полутень/bias у нас вдвое больше UE-шного.
Лечится выбором, к чему привязываться; требует замера, а не правки на глаз.

**И одно, чего у UE нет, а у нас есть: `normalBiasInTexels`.** Убирать нельзя — измерено, что он
несущий (21.8 % акне без него). Само его наличие — симптом пункта 6: их бюджет `CSMDepthBias`
заметно больше, и часть работы берёт рампа.

---

## S7. Pancaking: сжатие диапазона глубины каскада — ✅ СДЕЛАНО 2026-09-01

`nearLS` разведён на `nearProjLS` (тугой, `minZ`, идёт в `OrthoOffCenterLH`) и `nearCullLS`
(широкий, `minZ - casterReachWS`, идёт в `Frustum::FromOrthoBounds` — единственный потребитель).
`lightDistance` = `radius + casterReachWS + 1`. Кламп включается
`CascadeShadowConfig::pancakeCasters` (дефолт **on**), ключ `--set=csm.pancake`, чекбокс
«Pancake casters» в дев-окне. `casterReachWS` жив, сменил только смысл.

**Новый инструмент: `--csm-readout`** пишет таблицу подгонки каскадов в `logs/csm_readout.log`.
Приёмка S7 — это `zRange` и шаг D16, а они были только за GUI, который headless-прогон открыть не
может. Тот же ридаут, что в дев-окне.

### Замер — ридаут (wind_test, штатные дефолты)

| c | zRange ДО (м) | zRange ПОСЛЕ | D16 шаг ДО (мм) | D16 шаг ПОСЛЕ |
|---|---|---|---|---|
| 0 | 196.84 | **36.99** | 3.0036 | **0.5644** |
| 1 | 251.45 | **60.77** | 3.8369 | **0.9273** |
| 2 | 393.43 | **123.22** | 6.0034 | **1.8802** |
| 3 | 830.30 | **322.75** | 12.6696 | **4.9249** |

Ровно то, что обещал план: минус `casterReachWS` из диапазона, **5.3×** на каскаде 0.

### Кламп несущий — проверено отдельно, и первая проверка соврала

План требует убедиться, что кламп реально работает, а не маскирует отбраковку. С камеры юзера
(26 м над землёй) `pancake OFF` против `ON` дало **0.0084 / 0.025 %** — НИЖЕ шумового пола
(0.0187 / 0.049 %), то есть формально «кламп ничего не делает». Причина не в клампе: на этой камере
каскад 0 висит в воздухе над пляжем, и его near-плоскость никто не пересекает.
С **камеры уровня, с земли** (`-30.74 4.75 70.70`, среди пальм) — **4.896 / 10.9 %** против пола
**0.0395 / 0.136 %**, то есть **124× пола**: без клампа кроны срезаются и тени пропадают.
**Вывод для протокола: pancaking проверяется только оттуда, где кастеры выше слайса.**

### Что шаг НЕ дал, и это надо знать

* **Картинка не изменилась.** S7 против pre-S7 на штатных настройках, камера уровня:
  meanAbs **0.0450**, acne 0.0135 % — против пола **0.0617 / 0.0124 %**. Ниже шума. Это ОДНОВРЕМЕННО
  доказательство, что тени не обрезались (иначе была бы дельта порядка 4.9, как при `pancake:0`).
* **Точность не купила меньший bias.** Голодный прогон при `depthBias 0.2`: pre-S7 acne **24.97 %**,
  S7 **24.48 %** — 2 % относительной разницы. Значит акне на этом бюджете упирается в РАЗМЕР
  ТЕКСЕЛЯ (11.27 мм), а не в квантование глубины, даже когда старый шаг D16 (3.0 мм) был крупнее
  самого bias'а (0.2 × 11.27 = 2.25 мм).

**Ради чего он тогда сделан:** снята связь между `casterReachWS` и точностью глубины. Раньше поднять
дальность поиска кастеров (чтобы ловить более высокие/далёкие) означало платить точностью D16 на
ВСЕХ каскадах; теперь это два независимых числа. Плюс каскад 3 перестал ставить «глаз» света внутрь
собственного объёма (старое `max(1, maxDistance)` давало `lightDistance < radius`, и near упирался
в кламп 0.001 — видно в таблице «до»: `nearLS` 1.57/3.01/6.74/18.23 вместо честных значений).

### Критерий приёмки

| Критерий | Результат |
|---|---|
| `zRange` c0 ≲ 60 м, шаг D16 < 1 мм | **36.99 м / 0.5644 мм** |
| Высокие кастеры отбрасывают ЦЕЛЬНУЮ тень (ловушка с near куллинга) | да — S7 против pre-S7 ниже шума; ловушка не сработала |
| Кламп нужен, а не маскирует ошибку | да — `pancake:0` даёт 124× пола с земли |
| Acne не появилось | acne 0.0135 % против пола 0.0124 % |
| Гейт | `--scene-stress-gbv=3 --barrier-cmp --canonical-check`: `verdict: CLEAN`; собраны все ТРИ конфигурации |

**`zPadding` НЕ переведён в радиусы**, вопреки п. 4 плана. Причина: `zPaddingInRadii = 1.0` на
каскаде 3 (radius 344.88) дало бы zPad 345 м вместо 25 — диапазон вырос бы с 322 до 642 м, то есть
точность на дальнем каскаде стала бы ВДВОЕ ХУЖЕ. Аргумент плана («чтобы один NDC-bias работал на всех
каскадах») не применим: `depthBiasNDC` считается пер-каскадно от собственного `zRange`, поэтому
пропорциональность и так есть.

### Ручка запаса + что осталось проверить ГЛАЗАМИ (вопрос юзера)

Вопрос: «нормально будет в 0-м каскаде, если сзади стоит очень высокий кастер?»

Добавлена `CascadeShadowConfig::pancakeSlackWS` (метры, дефолт **0**), ключ `--set=csm.pancakeSlack`,
слайдер «Pancake slack (m)». При `slack == casterReachWS` проекционная near совпадает с
куллинговой, то есть шаг вырождается в состояние ДО S7 — это A/B-рычаг внутри одного бинаря,
без пересборки.

**Что известно из кода.** Три разных риска, и они не равны:
1. *Кастер дальше `casterReachWS` (150 м) к свету* — отбраковывается куллингом, тени нет.
   **К S7 отношения не имеет**: то же самое было и до шага, куллинговая near не менялась.
2. *Кастер целиком перед near-плоскостью* — панкейкается в z≈0. Тень корректна: кламп трогает
   только z, XY не меняется, а глубина «ближе всего» и так верна для всего слайса.
3. *Кастер ПЕРЕСЕКАЕТ near-плоскость* — единственный реальный риск. У треугольника часть вершин
   прижата, часть нет → он деформируется, и на нём возможно неверное самозатенение. UE документируют
   ровно это. Лечится подъёмом `pancakeSlackWS`.

**Замер по пяти точкам в роще (tight против slack=150) — все в пределах шума**, но точки я выбирал
вслепую и половина кадра там вода, так что доказательством случая 3 это не является:

| точка | шумовой пол | tight vs slack-150 |
|---|---|---|
| 1 | 0.0262 / 0.066 % | 0.0364 / 0.082 % |
| 2 | 0.0125 / 0.028 % | 0.0002 / 0.000 % |
| 3 | 0.0282 / 0.110 % | 0.0224 / 0.059 % |
| 4 | 0.0195 / 0.039 % | 0.0145 / 0.029 % |
| 5 | 0.0432 / 0.144 % | 0.0593 / 0.162 % |

**Случай 3 надо ловить руками**, поставив камеру так, чтобы высокий кастер пересекал near-плоскость
каскада 0 — то есть встать ВПРИТЫК к стволу, солнце за ним. Что смотреть: полосу неверного
самозатенения поперёк ствола на высоте перехода. Проверка — слайдер «Pancake slack (m)»: если полоса
уходит с ростом slack, это случай 3; если не меняется — что-то другое.

### S7-fix: панкейкинг рвал тень на границах каскадов (репро юзера, 2026-09-01)

Юзер поставил на wind_test высокий кастер (`box`, `scale [1, 100, 1]`), выключил океан и увидел
**разрывы тени на границах каскадов**; поднятие `pancakeSlackWS` их лечило. Это ровно случай 3 из
таблицы рисков выше, и внёс его S7: до шага near-плоскость была отодвинута на `casterReachWS`, и
панкейкать было нечего.

**Механизм.** Кастер выше слайса ПЕРЕСЕКАЕТ туго подогнанную near-плоскость: часть вершин
треугольника прижимается, часть нет, треугольник деформируется. А near-плоскость у каждого каскада
своя, поэтому соседние каскады деформируют его ПО-РАЗНОМУ — и на стыке тень не сходится.

**Замер** (камера юзера, натив + фикс. экспозиция; эталон — `pancakeSlack:150`, то есть проекция как
до S7, где панкейкинга нет по построению):

| slack, м | потеряно тени | доля тени | zRange c0 | шаг D16 |
|---|---|---|---|---|
| **0** (был дефолт) | 10490 px | **2.38 %** | 35.38 м | 0.540 мм |
| 2 | 118 px | 0.027 % | 37.38 | 0.570 |
| 5 | 107 px | 0.024 % | 40.38 | 0.616 |
| 15 | 96 px | 0.022 % | 50.38 | 0.769 |
| **40** ← новый дефолт | **0** | **0 %** | **75.38 м** | **1.150 мм** |
| 150 (как до S7) | 0 | 0 % | 185.38 | 2.829 |

Уже 2 метра убирают 99 % разрыва, но полностью — только 40. Дефолт поднят до **40**: точность S7
измеренно не давала видимого улучшения (см. выше в этом же шаге), а разрыв тени видно сразу, так что
менять корректность на неощутимую точность нечего. При этом 1.150 мм всё ещё в 2.5 раза лучше
до-S7-шных 2.829 мм — то есть шаг не отменён, а приведён к безопасной точке.

**Значение зависит от контента.** 40 перекрывает самый высокий кастер в сценах проекта; более
высокому нужно больше, и симптом ни с чем не спутать — тень рвётся на границе каскада.

**Заодно исправлен `--csm-readout`:** он снимался на первом кадре, ДО применения `--set=`, поэтому
для любого `--set=csm.pancakeSlack:N` печатал одни и те же числа. Теперь ждёт девятого кадра.
Диагностика, которая тихо противоречит флагам рядом с собой, хуже отсутствующей.

### Откат
`--set=csm.pancake:0` вернёт кламп; `--set=csm.pancakeSlack:150` вернёт проекцию к состоянию до S7;
полный откат — `nearProjLS = nearCullLS`.

---

## S7-original. Pancaking: сжатие диапазона глубины каскада

**Зависит от:** S6 (использует его инфраструктуру). **Эффект:** шаг квантования D16 в c0 с 3.11 мм до <1 мм. **Риск:** средний.

### Почему
`SceneRenderConfig.h:13-19` сам описывает проблему и правильное решение как отложенное: чтобы
кастеры между солнцем и слайсом не отсекались, near-плоскость отодвигается на `casterReachWS = 150` м.
Для c0 это раздувает ортодиапазон до ~204 м на D16.

Механизм клампа уже добавлен в S6 (`params.w`). **PSO менять не нужно** — кламп в VS возвращает
вершину внутрь фрустума, так что `DepthClipEnable` остаётся `TRUE`. Это существенно: PSO
индирект-теней штампуется из общего с VSM шаблона (`ShadowGpuData.cpp:1451-1535`), и его изменение затронуло бы VSM.

### ⚠ Ловушка, без которой шаг ломает сам себя: near-плоскость КУЛЛИНГА

Наивная реализация («сжать `nearLS` и включить кламп») **не работает** и даёт ровно тот баг,
который `casterReachWS` и был призван закрыть. Причина — `Scene.cpp:346`:

```cpp
cascadeView.frustum = Frustum::FromOrthoBounds(cascadeView.invView, minX, maxX, minY, maxY, nearLS, farLS);
```

`FromOrthoBounds` строит ortho-**бокс с настоящей near-плоскостью** (`Frustum::BuildOrthoLs`), и
его 6 планов уходят не только в CPU-куллинг, но и в GPU-куллинг: `Frustum::Planes()`
(комментарий в `Frustum.h`: *«Exposed for GPU shadow culling»*) → `shadowGpu_.UpdateViewFrustums`,
слоты 0..3 (`Scene.cpp:1322`).

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
Заменить `lightDistance` (`Scene.cpp:274`):

```cpp
        // S7: «глаз» ставится за сферой каскада + casterReachWS, а не на фиксированные
        // maxDistance для всех каскадов. Это НЕ влияет на диапазон глубины (его задаёт
        // near/far ниже) — только гарантирует, что любой кастер в пределах casterReachWS
        // к свету имеет z_ls > 0, т.е. лежит внутри ortho-бокса куллинга.
        // (Старое max(1, maxDistance) для c3 давало lightDistance < radius: глаз оказывался
        // ВНУТРИ объёма каскада, и near упирался в кламп 0.001.)
        const float lightDistance = radius + cascadeConfig_.casterReachWS + 1.0f;
```

**3) Развести near проекции и near куллинга.** Заменить `Scene.cpp:321-327`:

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
и ниже, в построении фрустума (`Scene.cpp:346`), поставить `nearCullLS`:
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

## S8. Soft-occlusion + Gather4 tent PCF вместо `SampleCmp` box 3×3 — ✅ СДЕЛАНО (S8 + S8b)

**Зависит от:** S3, S5, **S6 (жёстко, см. ниже)**. **Эффект:** мягкий градиент вместо бинарного сравнения; тот же футпринт фильтра за меньшее число текстурных операций. **Риск:** средний (меняется внешний вид теней).

### Зачем на самом деле — измерено 2026-08-31

Вопрос от юзера: «дистрибуция как в UE, почему такой жёсткий обрыв плотности c0→c1?»
Сверка показала, что **дело не в сплитах**. Дефолты UE (`DirectionalLightComponent.cpp:1017-1031`) —
4 каскада, экспонента 3, 400 м; скачок текселя c0→c1 у них **×4.00**, ровно как у нас. Наш c0 при
300 м даже плотнее (8.41 мм против 11.22 у UE).

Разница в **выборке**, и она количественная:

| c | тексель | наш радиус PCF | наша полутень | полутень UE |
|---|---|---|---|---|
| 0 | 8.41 мм | 1.000 текс | 8.41 мм | 8.41 мм |
| 1 | 33.62 мм | **0.707** текс | **23.78 мм** | 33.62 мм |
| 2 | 109.25 мм | 0.527 текс | 57.55 мм | 109.25 мм |
| 3 | 336.14 мм | 0.398 текс | 133.69 мм | 336.14 мм |

На границе c0→c1 **детализация падает ×4.00, а мягкость растёт только ×2.83.** У UE обе величины
растут ровно ×4: ядро у них фиксировано в текселях, а ширина рампы
`TransitionSize = CSMDepthBias/zRange × (ShadowBounds.W / ResolutionX)` **пропорциональна мировому
текселю каскада**. Поэтому дальний каскад у них размывается ровно настолько, насколько теряет
разрешение — глаз читает непрерывное «размытие вдаль». У нас c1 получает вчетверо более грубые
данные и подаёт их слишком резким краем — читается как обрыв качества.

**Следствие для реализации: `pow(normalBiasWS[0]/normalBiasWS[c], 0.25)` в `CsmSampleChain` надо
УДАЛИТЬ.** Это костыль под отсутствие рампы (комментарий в коде: «фиксированный радиус в 1 тексель
превращал дальний каскад в кашу»). С рампой он не нужен и активно вреден — именно он ломает
пропорцию. Перенести его по инерции = погасить весь эффект шага.

### ⚠ Отношения с S6

У UE рампа получает приёмника **без** sample-time bias:
`Settings.SceneDepth = LightSpacePixelDepthForOpaque` (`ShadowProjectionPixelShader.usf:252`) —
весь bias живёт в depth-пасе, то есть в нашем S6. Если транскрибировать это буквально, не сделав S6,
то `stored ≈ receiver` на любой освещённой поверхности → рампа даёт ≈ 0.5 → **вся сцена в равномерной
серой самозатенённости**.

**Поэтому S8 реализуется в два захода:**
* **сейчас, без S6:** рампа получает `z - b`, то есть существующий sample-time bias СОХРАНЯЕТСЯ и
  просто перестаёт быть бинарным порогом, становясь центром переходной зоны. Это осознанное
  отступление от UE, оно снимает зависимость от S6 и уже даёт главное — пропорциональную мягкость;
* **после S6:** `z - b` → `z`, `depthBiasInTexels` опускается, и путь становится дословно UE-шным.

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
(`lighting_cs.hlsl:47`) / `LinearSampler` (`glass.hlsl:88`).

**Важно:** офсеты задаются **не** через `int2`-параметр `Gather`, а сложением UV — тогда каждый
тап можно заклампить границами контента (S5). С `int2`-офсетами clamp обходится аппаратно.
(UE использует именно `int2`, `ShadowFilteringCommon.ush:255-258` — отличие сознательное и
единственное во всём S8; остальное сверено как идентичное, см. §5.)

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
    float  receiverBiasMin;   // S8: множитель transitionScale при NoL == 0. UE: r.Shadow.CSMReceiverBias = 0.9
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
    float  csmReceiverBiasMin = 0.9f;  // S8: r.Shadow.CSMReceiverBias (⚠ было 0.1 — вдесятеро мимо UE)
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

### Критерий приёмки — ВЫПОЛНЕН (первый заход, без S6)

| Критерий | Результат |
|---|---|
| Отношение «полутень / тексель» одинаково по каскадам | **да** — `pow(texel0/texelC, 0.25)` удалён из gather-ветки, ядро фиксировано 4×4 текселя в каждом каскаде. Было 1.000 / 0.707 / 0.527 / 0.398 |
| Эффект выше шума | **13.86 % и 12.63 %** пикселей против шумовых полов **2.64 %** и **3.42 %** — вчетверо выше, воспроизводится на обеих парах |
| `Pass_Lighting` не выросло | 0.149/0.147 → **0.146/0.143**. Контроль `Pass_CSM` (шаг его не трогает) 0.404/0.412 → 0.408/0.403, т.е. разрешение замера ~1–2 % и падение в пределах шума. `GPU.Frame` 3.16 → 3.10 |
| Нет серой пелены | освещённые пиксели (>140) **167.49 → 168.36**, то есть слегка СВЕТЛЕЕ; теневые 44.34 → 44.57. Пелена дала бы просадку освещённых |
| Откат работает | `--set=csm.filterMode:0` и слайдер «Filter kernel» в dev-окне возвращают старую ветку |
| Шейдеры компилируются | `lighting_cs` и `glass` через автономный `dxc` до запуска |

Визуально: край тени стал плавным градиентом, «хруст» на пятнистых тенях от фрондов ушёл.

### S8b. Три ручки фильтра + выбор ядра — ✅ СДЕЛАНО 2026-08-31

Юзер открыл Unreal и сказал: «там всё-равно CSM тени как-то мягче выглядят». Сверка по дропу
объяснила почему — и вскрыла, что мы транскрибировали **не ту перестановку** шейдера:

* `r.ShadowQuality` в UE по умолчанию **5**, а `ManualPCF` при quality 4-5 выбирает
  `Manual5x5PCF` — **6×6 текселей, 9 gather-квадов**. Я реализовал `Manual3x3PCF` (4×4, 4 гейтера),
  то есть их **q3**, а не их дефолт. Ширина ядра — это и есть абсолютная мягкость.
* `ShadowSharpen` — пер-лайтовое свойство, дефолт **0.0**, в шейдер уходит как
  `GetShadowSharpen() * 7.0f + 1.0f` (`ShadowRendering.h:1299`). Маппинг делаем на CPU, там же где они.
* `ApplyPCFOverBlurCorrection` (возведение видимости в квадрат) применяется **безусловно** ко всем
  фильтрованным теням.
* **Порядок у UE: сначала sharpen, потом over-blur** (`ShadowProjectionPixelShader.usf:375`, затем
  `:385`). У меня было наоборот. При дефолтном sharpen = no-op обе перестановки совпадают — ошибка
  не проявилась бы, пока ручку не вывели наружу. Исправлено.

Наружу выведено четыре контрола (dev-окно, вкладка CSM, секция «Filtering») и столько же
`--set=` ключей:

| контрол | `--set=` | дефолт | что делает |
|---|---|---|---|
| Filter kernel | `csm.filterMode` | **2** | 0 = 3×3 box (старая ветка), 1 = 4×4 tent (UE q3), 2 = 6×6 tent (UE q5) |
| Shadow filter sharpen | `csm.sharpen` | 0.0 | артистские 0..1, в шейдер `×7+1`; полутень сохраняет ШИРИНУ, теряет градиент |
| Receiver bias | `csm.receiverBias` | 0.9 | `r.Shadow.CSMReceiverBias`: расширяет рампу по мере скольжения света вдоль поверхности |
| PCF over-blur correction | `csm.overBlurCorrect` | on | `saturate(sh)²` — возвращает ядру тела тени плотность |

**Замер 2026-08-31, Release, wind_test, `--wind-freeze=3.0`, `--shot-delay=12`.**
Первый заход мерил под DLSS Perf с живой автоэкспозицией — шумовой пол вышел meanAbs **0.79**,
10.8 % пикселей, и `receiverBias` целиком утонул в нём, а знак яркости у `overBlurCorrect`
получился ОБРАТНЫЙ физике (автоэкспозиция компенсирует посветление кадра). Перемер в
`--dlss=off --set=exposure.autoExposure:0` уронил пол до **0.067 / 0.40 %** и выправил все знаки:

| арм | meanAbs | пикс > 2 | Δ яркости кадра | Δ яркости ПОЛУТЕНИ |
|---|---|---|---|---|
| контроль (тот же сетап дважды) | 0.067 | 0.40 % | −0.007 | −0.006 |
| `overBlurCorrect:0` | 2.697 | 23.6 % | **+2.55** | **+3.46** |
| `sharpen:1.0` | 2.712 | 21.6 % | +1.13 | +0.74 |
| `filterMode:0` (3×3 box) | 2.739 | 22.5 % | +2.42 | +2.95 |
| `filterMode:1` (4×4 tent) | 1.479 | 18.7 % | +0.65 | +0.43 |
| `receiverBias:0` | 0.227 | 1.87 % | +0.16 | +0.11 |

Все шесть — выше пола (`receiverBias` в 3.4 раза по meanAbs и в 4.6 по покрытию: ручка тонкая,
но живая), и все шесть в **правильную сторону**: снятие квадрата и сужение ядра осветляют, ширина
ядра монотонно растит перекрытие. «Полутень» здесь = пиксели с яркостью 15..120 в контрольном
кадре, 61.2 % кадра — считать по среднему ВСЕГО кадра нельзя, там доминирует небо.

### Что осталось на второй заход (после S6)
`z - b` → `z`, `depthBiasInTexels` вниз. Сейчас рампа центрирована на существующем sample-time bias —
это работает и снимает зависимость, но точка UE в том, что bias живёт в depth-пасе.

### Исходный критерий приёмки
* **Главный, численный: отношение «полутень / тексель» должно стать ОДИНАКОВЫМ во всех каскадах.**
  Сейчас 1.000 / 0.707 / 0.527 / 0.398 — после шага должно быть 1 / 1 / 1 / 1 (с точностью до
  выбранного радиуса ядра). Это и есть то, из-за чего граница c0→c1 читается как обрыв.
* Граница c0↔c1 перестаёт читаться как скачок резкости — проверять на пологом песке под пальмами,
  где полутень длинная (поза из репорта юзера: `Cam -52.85 4.62 67.49`).
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
`--set=csm.filterMode:0` (dev-окно: «Filter kernel» = 0).

---

## S9. Аналитический receiver-plane depth bias

**Зависит от:** S3, S8. **Эффект:** позволяет расширить ядро PCF без self-shadowing на скошенных поверхностях. **Риск:** низкий.

### Почему
Все тапы фильтра сравниваются с **одной** глубиной приёмника. На поверхности под углом к свету это
даёт self-shadowing на дальних тапах — поэтому радиус и держится узким.

### ⚠ Сверено с дропом 2026-08-31 — первая редакция этого шага была неверна в четырёх местах

| Утверждение первой редакции | Что в UE на самом деле |
|---|---|
| «UE считает наклон через `cross(ddx, ddy)`» | **Нет.** Направленный путь берёт МИРОВУЮ НОРМАЛЬ: `NormalPlaneUV = mul(float4(N,0), TranslatedWorldToShadowUVNormalMatrix); DepthSlopeUV = -NormalPlaneUV.xy / NormalPlaneUV.z` (`VirtualShadowMapProjectionDirectional.ush:16-30`). То есть аналитический вывод — это и есть метод UE, а не наш обходной путь |
| «см. `ShadowPercentageCloserFiltering.ush`» | Не тот файл: это **PCSS**, и он под `#if USE_PCSS`, у которого **дефолт 0**. Найденные там `ddx/ddy` сидят внутри `PCSS_ANTI_ALIASING_METHOD == 6`, а стоит 2 — мёртвая ветка внутри выключенной фичи |
| «кламп наклона на 5°, `sin(5°) = 0.0872665`» | UE клампит **сам градиент**: `clamp(DepthSlopeUV, -0.05, 0.05)`. Это не угол. И **0.05 не переносится буквально** — оно выражено в ИХ диапазоне глубины; наш `vsm_sample.hlsli` уже вывел эквивалент пересчётом единиц (4.17) и задокументировал почему |
| формула без множителя | `ComputeOptimalSlopeBiasDirectional` = **`2.0f * max(0, dot(DepthSlopeUV, OffsetUV))`** — двойка потеряна |

**И главное: в дефолтном CSM-пути UE receiver-plane bias НЕТ вообще.** Это фича VSM
(`VirtualShadowMapProjection*`). Их CSM (`ShadowProjectionPixelShader.usf` → `ManualPCF`) обходится
рампой и write-time bias. Значит S9 для CSM — **отступление от UE**, как и S4, и обосновывать его
надо своими мерками, а не транскрипцией.

### Хорошая новость: транскрипция у нас уже есть
`shaders/vsm_sample.hlsli` содержит проработанный порт (P16.16): множитель 2 сохранён, кламп
градиента выведен пересчётом единиц (`VSM_MAX_DEPTH_SLOPE_UV = 4.17`, с объяснением почему 0.05
нельзя копировать), плюс бонусный бонус `VSM_MAX_SLOPE_BIAS_NDC`, и там же зафиксирован найденный
когда-то знаковый баг. **S9 = перенос этого в `csm_sample.hlsli`, а не вывод с нуля** — с поправкой
на то, что у каскада своя ортопроекция и свой диапазон глубины, то есть кламп надо пересчитать ещё раз.

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

## S10. Затухание последнего каскада + blend-полоса от длины слайса — ✅ СДЕЛАНО 2026-09-01

`CascadeShadowConfig::blendFraction` (0.1) и `distanceFadeFraction` (0.1), оба клампятся в 0.3 как
у UE. Ключи `--set=csm.blendFraction` / `csm.distanceFade`, слайдеры в дев-окне (секция
«Cascade transition»). Плюс `--set=csm.maxDistance` — без него терминатор на маленьком уровне просто
не попадает в кадр, то есть шаг нечем было бы проверить.

**Обе половины сделаны, и они очень разные по отдаче.**

**1. Затухание последнего каскада — настоящая работа.** Было: `CsmSampleShadow` при `idx == 3`
возвращал результат как есть, а за каскадом `CsmSampleChain` отдаёт `1.0` — тени обрывались линией.
Стало: как у UE, fade-плоскость последнего каскада уезжает ВНУТРЬ на
`(sFar - sNear) * distanceFadeFraction`, и тень гаснет в «освещено».
Замер (камера юзера, дальность теней подтянута до 60 м, натив + фикс. экспозиция, пол
**0.0197 / 0.067 %**): `distanceFade 0.1` против `0` — **meanAbs 0.660 / 2.33 %**, яркость +0.63
(тени именно ГАСНУТ, знак верный). `0.3` — 0.776 / 2.90 %. **`distanceFade:0` возвращает линию** —
откат проверен.

**2. Blend-полоса — измерена и оказалась почти пустой.** Формула исправлена на UE-шную (доля
СОБСТВЕННОЙ длины слайса вместо доли абсолютной дистанции), `kCsmBlendFraction` из шейдера убран.
Но эффекта почти нет:

| замер | meanAbs | пикс > 2 |
|---|---|---|
| шумовой пол (дальность 60 м) | 0.0190 | 0.057 % |
| `blendFraction:0` (жёсткое переключение) | **0.0145** | 0.043 % |
| `blendFraction:0.3` (максимум UE) | **0.0193** | 0.057 % |

При сжатых каскадах полоса **неотличима от шума даже выключенной целиком**. На штатных 300 м она
чуть выше пола (0.047 / 0.38 % против 0.019 / 0.075 %), но это всё равно 2.5×, а не порядок.
**Причина в том, что шов, который она компенсировала, убрал S8:** рампа теперь пропорциональна
мировому текселю каскада, поэтому граница и так читается как размытие.
Цена при этом **ниже разрешения замера**: `Pass_Lighting` 0.027/0.026 против 0.026/0.025 при
собственном разбросе контроля `Pass_CSM` в 0.008. Оставлен включённым на дефолте UE — бесплатно, а
на сцене с более резким скачком каскадов пригодится.

### Заодно закрыт вопрос про `slopeScale`

Юзер поставил 2.0 вместо перенесённых 3.0. Пересняли при дефолтах depth 1.0 / normal 0.5
(пол по acne **0.0652 %**):

| slopeScale | acne | питер-пеннинг |
|---|---|---|
| **2.0** | 0.0649 % (= пол) | **+0.419** |
| 3.0 | 0.0633 % (= пол) | +0.616 |
| 2.0, бюджет урезан до 0.5 | 0.1277 % | −0.943 |
| 3.0, бюджет урезан до 0.5 | 0.0906 % | −0.119 |

На штатном бюджете **обе чистые, а 2.0 даёт меньше питер-пеннинга** — выбор юзера верный и остаётся.
3.0 покупает запас по акне, который на этом бюджете не нужен.

### ⚠ Все замеры S6/S8b/S10 сняты с камеры юзера, НЕ с дефолтной камеры уровня

`--cam-pos=-70.35,26.61,85.11 --cam-rot=0.1748,0.8163,-0.3170,0.4502`. На ней `GPU.Frame` **2.316**
против **3.048** на дефолтной камере того же бинаря — разница 0.73 мс, из них 0.53 это
`Pass_RTTrace` (сверху вниз в кадре почти нет воды), плюс `ExecuteBundles` −0.16 и `Pass_Skybox`
−0.07. Для A/B это безразлично (обе руки на одной камере), но **с базовой таблицей §S0.4 эти числа
несравнимы**.

### Критерий приёмки — выполнен

| Критерий | Результат |
|---|---|
| На дальней границе тени плавно исчезают, линии нет | да, 0.660 meanAbs против пола 0.020, яркость растёт |
| Ширина полос пропорциональна длине слайса | да, по построению: `(sFar - sNear) * blendFraction`, границы слайса берутся из `splitsVS[idx]`/`splitsVS[idx+1]`, а `splitsVS[4]` доехал до CB новым полем |
| `distanceFadeFraction = 0` возвращает жёсткую границу | да |
| Стоимость | ниже разрешения замера |
| Гейт | `--scene-stress-gbv=3 --barrier-cmp --canonical-check`: `verdict: CLEAN`; собраны все ТРИ конфигурации |

### Откат
`--set=csm.distanceFade:0` (и `csm.blendFraction:0` для полосы).

---

## S10-original. Затухание последнего каскада + blend-полоса от длины слайса

**Зависит от:** S3. **Эффект:** убирает жёсткий терминатор теней на 300 м. **Риск:** минимальный.

### Почему
`CsmSampleChain` возвращает `1.0` за каскадом 3 — на 300 м тени исчезают линией. UE переносит
fade-плоскость **внутрь** последнего каскада на `(splitFar - splitNear) * fadeFraction` и гасит
тень к 1.0 (освещено).

Плюс: текущая blend-полоса берёт `band = splitNext * 0.1` — долю от **абсолютной** дистанции.
UE берёт долю от **длины слайса**. Для c0 разница мала, для c3 (100…300) существенна: 30 м против 20 м.

**⚠ Сверено с UE 2026-08-31 — полоса у нас с ДРУГОЙ стороны.**
`DirectionalLightComponent.cpp:920-943`: у UE для не-последнего каскада
`FadeExtension = (SplitFar - SplitNear) * CascadeTransitionFraction`, и дальше
**`SplitFar += FadeExtension`** — каскад *расширяется*, чтобы полоса перехода была покрыта им самим
на полном качестве; `FadePlane` при этом остаётся на исходном сплите. Для последнего каскада
наоборот: `FadePlane -= FadeExtension`, плоскость уезжает внутрь — это и есть то, что предлагает S10.

У нас полоса лежит **внутри** каскада N, а сэмплируется каскад N+1, слайс которого начинается только
на сплите. Работает лишь потому, что объемлющая сфера N+1 заметно больше его слайса, плюс есть
fallback-цепочка. Это не гарантия, а везение. При реализации выбрать одно:
* **как UE** — расширить `sliceFar` каскада N на `FadeExtension` при фите сферы (тогда полоса
  честно покрыта каскадом N, а мы гасим его вклад), либо
* оставить текущую схему, но **доказать** замером, что сфера N+1 накрывает полосу во всех
  каскадах и при всех сплитах, которые допускают слайдеры S0.2.

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

## S11. Scissor-оптимизация каскада — ✅ СДЕЛАНО 2026-09-02 (транскрипция `ComputeScissorRectOptim`)

### Что сделано
* `Scene::ComputeCascadeScissor` (`Scene.cpp`) — транскрипция `FProjectedShadowInfo::ComputeScissorRectOptim`
  (`ShadowSetup.cpp:2945`): четыре дальних угла слайса + центр far-плоскости + позиция камеры
  проецируются в тайл, каждый луч «камера → точка» **продлевается до границы тайла** (поэтому это
  КОНУС, а не пирамида — покрывает приёмников за far-плоскостью, которые всё ещё читают этот тайл:
  бленд в следующий каскад, UV-фолбэк), bbox. Срабатывает только когда камера проецируется внутрь
  тайла, иначе весь тайл — как у UE.
* Дельты от оригинала, все осознанные: текселы CONTENT-ректа (не FullRes с бордером — вьюпорт и так
  без гаттера S5); симметричная проекция без DLSS-джиттера (их AsymmetricFOVScale у нас = джиттер);
  `scissorPadTexels` (НАШ, дефолт 4 = 6x6 tent + normal offset; UE не паддят и живут с тапами за
  краем конуса); вырожденный луч (взгляд вдоль солнца) → весь тайл, у UE — молчаливая точка (0,0),
  тянущая прямоугольник в угол; оффсет атласа прибавлен ОДИН раз — **у UE баг: `+= FIntPoint(X, Y)`
  в `ComputeScissorRectOptim` и ещё раз `+ X` в `SetStateForView`**. Вероятно поэтому cvar у них
  выключен по умолчанию и мало кем трогается.
* `CascadeData::scissor[]` / `scissorAreaDbg[]` считаются ВСЕГДА, применяются в `Pass_CSM` через
  `Renderer::BindShadowTarget(..., scissor)` (пересечение с content-ректом, viewport не трогается)
  только при `CascadeShadowConfig::scissorOptim` — дефолт **off**, как `r.Shadow.CSMScissorOptim`.
* Ручки: `--set=csm.scissorOptim:1`, `--set=csm.scissorPad:N`, галка + слайдер в табе CSM (секция
  Fit), столбец «scissor %» в readout-таблице и в `logs/csm_readout.log` (`--csm-readout`, с полем
  `applied=` — чтобы headless A/B ДОКАЗЫВАЛ, что scissor сработал, а не выводил это из нейтральной
  картинки).

### Замер (Release, wind_test, legacy, `--profdump`, 30 с прогрева, по два прогона на конфигурацию)

| камера | scissor % (все 4 каскада) | `Pass_CSM` GPU off | on | Δ |
|---|---|---|---|---|
| камера теней `15.07,5.13,69.20` | **68.0** | 0.370 / 0.377 | 0.345 / 0.347 | **−0.028 мс (−7 %)**; `GPU.Frame` 2.395/2.416 → 2.381/2.381 |
| камера уровня | (доля не снята) | 0.393 / 0.390 | 0.362 / 0.396 | в шуме (разброс пары 0.03) |

Доля одна во всех каскадах — конус, сфера и квадрат масштабируются вместе со sliceFar (при
`hfov 90°` центр минимальной сферы прижат к far-плоскости). На стекле сбоку 70.5 %, спереди 91.4 %.

**32 % площади → 7 % времени.** Scissor — ТОЛЬКО растровая экономия: кастеры вне прямоугольника
всё равно проходят cull по фрустуму каскада (GPU-индирект) и вершинный шейдер. Следующий рычаг,
если CSM-бюджет станет дорог: сузить XY КУЛЛ-фрустума каскада до прямоугольника (у UE этого нет) —
это уже вершины и CPU-сабмишн, а не только филл.

### Приёмка
* Статичная камера, `--dlss=off --wind-freeze`, камера теней: off→on **0.017 %** при поле
  0.014–0.018 % — нейтрально; pad 0 тоже 0.017 %.
* Стекло (demo.json, две камеры на `transparentMesh`, спереди и сбоку): область стекла **0.000 %**
  при любом паде. Предсказанный выше артефакт (отражённые приёмники вне конуса читают нерисованную
  часть тайла как «освещено») на этих ракурсах НЕ воспроизвёлся. Риск остаётся теоретическим,
  дефолт off сохраняем по правилу UE.
* **Ловушка, стоившая часа:** на грани правого кирпичного блока у края кадра дифф off→on 6.2 % —
  «похоже на тапы за границей конуса». Три OFF-прогона дали пол 0.01 / 1.15 / 1.58 % на той же
  области, а off2→on2 = **0.031 %**: область бимодально шумит САМА (сэмплинг текстуры на скользящей
  грани, состояние прогона), монотонности по паду нет (pad16 0.3 %, pad64 2.3 %). **Один замер
  пола — не пол; на подозрительной области брать ≥3 одинаковых прогона, прежде чем винить правку.**
* Взгляд вдоль солнца → весь тайл по конструкции (вырожденный луч; в readout `scissor%=100`).

### Вердикт
Работает, безопасно на проверенных ракурсах, −7 % `Pass_CSM` при 32 % среза. Дефолт off (UE);
включать пресетом, когда CSM-бюджет дорог. Ниже — исходный набросок, оставлен ради рассуждения
о стекле.

---

## S11-original. Scissor-оптимизация каскада (набросок)

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

## S12. Contact shadows — ✅ СДЕЛАНО 2026-09-01 (транскрипция UE, не набросок ниже)

Реализовано НЕ по наброску ниже, а транскрипцией `CastScreenSpaceShadowRay`
(`Shaders/Private/ScreenSpaceShadowRayCast.ush`) и его применения в `DeferredLightingCommon.ush`.
Файл: `shaders/contact_shadow.hlsli`. Применяется в `SampleSunShadow` **в обоих режимах теней** —
трасса читает буфер глубины КАМЕРЫ и ни одной теневой карты, поэтому Legacy CSM и VSM получают
идентичный член. Настройки — отдельная таба «Contact» в дев-окне и `--set=contact.*`.

**Почему это работает на дистанции** (требование юзера): длина трассы у UE — МНОЖИТЕЛЬ ГЛУБИНЫ
СЦЕНЫ (`ContactShadowLength * GetScreenRayLengthMultiplierForProjectionType(SceneDepth).y`), а не
расстояние. Поэтому луч покрывает одинаковое число ЭКРАННЫХ пикселей вблизи и вдали и не
схлопывается ниже пикселя там, где дальнему каскаду уже нечего сказать.

**Замер** (wind_test, камера теней, `--dlss=off --wind-freeze`, шумовой пол 0.005 %):

| | пикселей > 2/255 | покрытие |
|---|---|---|
| Legacy CSM, contact 0 -> 0.05 | **0.952 %** | 54.961 % -> 55.026 % |
| VSM+SMRT, contact 0 -> 0.05 | **0.716 %** | 54.504 % -> 54.656 % |

Покрытие растёт — контакты ДОБАВЛЯЮТ тень, как и должны.

**Допуск сравнения** взят из марша, а не из константы: `abs(rayDepth.z - rayStart.z) * step *
CompareToleranceScale`, где UE ставят 2.0 («меньше муара»). Тот же принцип, что в SMRT.

**Переворотов Z здесь НЕТ** — в отличие от всего, что касается теневых карт. Эта трасса читает
глубину КАМЕРЫ, а она у нас уже reverse-Z, как у UE (чистка 0.0, дефолтный DepthFunc в `Material.h`
= GREATER_EQUAL). Прямой Z остался только в теневых картах.

### Настройки: что от UE, что наше

**От UE полностью:** длина (и оба её прочтения — метры либо множитель глубины сцены, у них выбор
кодируется ЗНАКОМ значения: `ContactShadowLengthInWS = ContactShadowLength < 0`), интенсивность,
число шагов (у них хардкод 8), `CompareToleranceScale = 2.0`. **Мастер-выключатель по умолчанию
ВЫКЛ — тоже как у них**: `ContactShadowLength` на источнике по умолчанию 0, это опциональный
инструмент художника, а не глобальная фича. Не переносили: раздельные интенсивности
casting/non-casting (у нас нет пер-примитивного флага).

**Денойзера для контактных теней у Epic НЕТ.** Денойзеры в дропе есть только у Lumen. Фейда по
дистанции у них тоже нет. Всё, что ниже — наше, и добавлено потому, что у нас низкое солнце над
плоским песком, то есть худший случай для экранного марша:
* `normalOffset` — старт луча смещается по нормали. Луч, начинающийся НА поверхности, неоднозначен
  уже на первом шаге; UE стартуют в затеняемой точке.
* `grazingFade` — гашение по NdotL. При скользящем солнце луч идёт почти параллельно поверхности и
  марш меряет квантование буфера глубины, а не геометрию.
* `minDistance` / `maxDistance` / `fadeBand` в метрах — окно, вне которого член выключен.

### ⚠ ВАЖНО: точки на дальнем песке — это НЕ контактные тени

Юзер обвёл рассыпь тёмных точек на плоском песке (~350 м от камеры) и спросил, как с ней бороться.
Замер в его же конфигурации (VSM + DLSS, камера `-313.89 179.46 124.25`, rot
`0.1178 0.8693 -0.2496 0.4101`), плотность точек в пятне:

| | |
|---|---|
| контакты ВЫКЛ | **12.99 %** — артефакт есть БЕЗ них |
| контакты ВКЛ, без гардов | 16.98 % |
| + normalOffset 0.02 + grazingFade 0.15 | 14.74 % |
| + maxDistance 150 м | 13.03 % — вклад контактов убран |
| **Legacy CSM, тот же ракурс** | **точек нет вообще** |

То есть это **артефакт пути VSM**, а контакты его лишь усиливают. Первый прогон я сделал в Legacy,
где артефакта нет в принципе, и чуть не отчитался «контакты ни при чём» на основании теста, который
фичу не пересекал. Bias не при чём: 10x `clipmapDepthBias` даёт 13.14 %, 4x `clipmapNormalBias` —
13.04 %, то есть это НЕ классическое акне. Отдельная задача, не закрыта.

### Толщина и нормальный офсет — доли длины луча, не метры

Оба были в метрах и оба на этом сломались: длина луча сама есть множитель глубины сцены, растёт с
дистанцией, и константа в метрах либо ничего не делает вблизи, либо убивает настоящие контакты
вдали. Юзер упёрся в границу ползунка ровно поэтому. Теперь `thickness` (дефолт 0.5) и
`normalOffset` (дефолт 0.04) — доли длины луча, одна система координат на всю фичу. В метрах
остались только min/max distance, где метры — настоящая величина.

**Ось толщины — вторая ошибка:** первая версия мерила «позади» вдоль направления НА СВЕТ (луч туда
идёт), а «позади поверхности» в буфере глубины значит «дальше от КАМЕРЫ». При низком солнце оси
почти перпендикулярны, проекция ≈ 0, ручка двигала 0.72 % пикселей против артефакта в 4 пп —
то есть не работала. По оси взгляда — 2.08 %, и кусается.

**Прямой размен на дистанции:** толщина режет и рассыпь, и настоящие тени пальм на 350 м — ствол
отстоит от точки земли на метры и по критерию «позади» отбрасывается вместе с мусором. Одним
порогом это на 3 км не разводится.

### Денойз: что можно, что нет (2026-09-01)

**Временной дизер — сделан, и это UE:** `InterleavedGradientNoise(PixelPos, StateFrameIndexMod8)`,
`uv += FrameId * (float2(47,17) * 0.695)`. При FrameId 0 формула вырождается в статичный IGN, так
что «выкл» = прежнее поведение. Галка `Temporal dither`, дефолт ON.

Замер под DLSS в обведённой области (два прогона на точку, совпадают до 3-го знака):

| | зерно 3x3 | тёмных точек |
|---|---|---|
| temporal OFF | 1.5943 / 1.5998 | 44.04 % |
| temporal ON | 1.4792 / 1.4783 | 44.22 % |

**Зерно −7.3 %, а число тёмных точек НЕ меняется.** Временной фильтр усредняет только ФАЗОВЫЙ шум;
пиксель, чей луч попадает в обратный склон дюны каждый кадр, остаётся тёмным при любой фазе. Эта
рассыпь детерминирована по пикселю, и её не берёт никакое временное накопление — только
пространственный фильтр либо устранение причины (толщина).

**Пространственный блюр возможен, но это новая архитектура:** контакт сейчас считается inline в
lighting_cs и нигде не лежит отдельно. Нужен буфер маски + depth-aware bilateral пасс + повторное
применение. UE для контактов этого не делают. Второй путь — несколько лучей на пиксель с джиттером
по диску солнца (как SMRT), усреднение — цена N×. Не начато; решение за юзером.

**Известный артефакт самих контактов:** на дальних стволах точечная дорожка — банинг восьми шагов
марша. Лечится числом шагов (ручка в табе) либо более сильным дизером фазы.

---

### Все источники + переключатель для локалов — 2026-09-01

Контакты теперь и у spot/point, не только у солнца — у UE это один `GetShadowTermsBase` для всех
типов источников. Общая `ApplyContactShadow` в `contact_shadow.hlsli`; `pointlight_cs` и
`spotlight_cs` получили те же CB-поля с теми же именами (поэтому одна
`render::contact::FillConstants` на все три пасса), `MakeContactParams()` и по три точки вызова
(фронт, трансмиссия с `-N`, тыл).

**Локалам — либо карта, либо контакты, никогда обе** (требование юзера: на источнике с малым
радиусом это дважды затемняет тот же контакт и ничего не даёт). `render::contact::g_localMode`,
`--set=contact.localMode:0|1|2`, комбо «Local lights (spot/point)» в табе Contact:
* 0 — теневая карта, контакты для локалов выключены;
* 1 — контакты ВМЕСТО карты: карта не сэмплируется вовсе (9 тапов атласа → 8 тапов глубины);
* 2 — авто: источники со слотом остаются на карте, без слота получают контакты.
Дефолт 1. Солнце не затронуто — там контакт лежит ПОВЕРХ CSM/VSM по замыслу. В шейдере
`LocalShadowSources(hasSlot, useMap, useContact)`; тест слота — `shadowParams.y >= 0` у спота,
`shadowParams.x >= 0` у поинта.

**Ловушка, пойманная до выкатки:** в CB уходит `EffectiveLocalMode() = g_enabled ? g_localMode : 0`.
Без этого дефолтный режим 1 при ВЫКЛЮЧЕННЫХ контактах (мастер-галка по умолчанию выкл → длина трассы
0) оставлял spot/point вообще без тени.

**Замер** — demo.json, Release, `--dlss=off --wind-freeze --set=exposure.autoExposure:0
--set=contact.temporal:0`, камера по умолчанию, пол (режим 1 дважды) **0.037 %**:

| пара | пикселей > 8/255 |
|---|---|
| выкл → режим 0 (только солнечные контакты) | 1.165 %, темнее 0.77 % |
| режим 0 → 1 (локалы: карта → контакты) | **6.965 %**: светлее 3.29 % (тени карты, до которых трасса не дотягивается — пальмы через весь пляж), темнее 1.24 % (контакты там, где карты не было) |
| режим 0 → 2 (авто) | 0.165 % — почти у всех локалов demo есть слот, авто ≈ карта |

`logs/cb_field_missing.log` не появился — поле найдено в обоих материалах.

**Что смотреть глазами:** в режиме 1 тени пальм от белого спота на пляже сжимаются до длины трассы —
это размен, а не баг; если локалам нужна дальняя тень — режим 0 или 2.

---

## S12-original. Contact shadows (набросок, НЕ реализован в этом виде)

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

**2) Атлас должен быть один, а не `kFrameCount` копий** — иначе кэш пришлось бы копировать между
кадрами. **Это снято шагом S3.5** (там же и ревизия барьеров/`ResourceStateTracker`). Если S3.5
сделан, второй блокер отсутствует; если нет — сделать его первым, он полезен сам по себе.

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
| +S1 (минимальная сфера) ✅ | 13.47 м | 2048 | **13.16 мм (замерено)** | −7.2 % | **0** |
| +S2 (overlap в текселях) ✅ | 11.50 м | 2048 | **11.23 мм (замерено)** | −20.8 % | **0** |
| +S3.5 (один атлас) ✅ | — | — | — | — | **−67 МБ VRAM** (Legacy), картинка не меняется |
| +S4 пресет `Quality` | 11.50 м | 4096 | 5.61 мм | −60 % | ×4 растеризации c0; 33.5 → 67 МБ (после S3.5) |
| +сокращение c0 до 6 м (тюнинг через S0.2) | 6.90 м | 4096 | 3.37 мм | −76 % | c1 берёт на себя 6…35 м |

**Читать эту таблицу надо по последней колонке.** Первые две строки — бесплатное уплотнение
на 21 %, и именно они соответствуют цели «CSM как быстрая альтернатива VSM». Строки S4 — это
покупка качества за время кадра; последняя строка показывает, что тот же результат достижим
перераспределением покрытия, а не памятью и не растеризацией. Дефолт должен собираться из
бесплатных шагов, дорогие остаются пресетами.

Независимо от плотности: **S7** даёт шаг квантования D16 ≈ 0.5 мм вместо 3.11 мм (бесплатно);
**S6 + S8 + S9** позволяют снизить bias (сейчас ~16.8 мм peter-panning) и заменить бинарное
сравнение мягкой рампой — примерно за те же деньги, при условии, что гейт в S6 и паритет
`Gather` в S8 подтверждены замером.

---

## 4. Рекомендованный порядок

```
S0                                  enabler + перф-baseline                 [СДЕЛАНО]
S1  ->  S2                          дешёвые чистые выигрыши плотности        [СДЕЛАНО]
S3                                  рефакторинг-enabler для шейдеров        [СДЕЛАНО]
S3.5                                один атлас вместо 3 копий (-67 МБ)     [СДЕЛАНО]
S3.6                                кастер = LOD приёмника (UE-контракт) [СДЕЛАНО]
S4  ->  S5                          per-cascade разрешение + gutter   [S4 = пресет, не дефолт]
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
* **S3.5 до S4.** Иначе `Quality` меряется в утроенной памяти (201 МБ вместо 67) и будет
  отвергнут по причине, которой на самом деле нет.

Прочее:
* Минимальный набор для эффекта «как в Days Gone»: **S0 → S1 → S2 → S3.5 → S4 → S5 → S7**
  (S3.5 здесь не ради картинки, а чтобы S4 не выглядел втрое дороже по памяти, чем он есть).
* Максимальная отдача на вложенное время: **S1 + S2** (две небольшие правки, −21 % размера
  текселя, ноль стоимости) — и это же самый безопасный первый коммит.
* Самые рискованные шаги — **S6** и **S7** (общие с VSM шейдер, PSO, input layout и CB). После
  каждого обязательна проверка VSM-режима (Ctrl+V) и **на идентичность картинки, и на время**
  `VsmPageRender`.
* Каждый шаг после S0 дописывает свою строку в таблицу S0.4. Документ считается выполненным не
  когда сделаны 13 шагов, а когда в этой таблице CSM-строка **и красивее, и дешевле** VSM-строки.

---

## 5. Сверка с оригиналом UE (`D:/Programming/ue_strip`, 2026-08-31)

Дроп UE лежит локально, и первая редакция этого документа зря объявляла себя «самодостаточной».
Проверены все транскрипции. **Четыре совпали дословно, три числа были неверны.**

### Совпало байт-в-байт (можно не перепроверять)

| Наш шаг | Оригинал | Вердикт |
|---|---|---|
| **S1** сфера каскада | `DirectionalLightComponent.cpp:876-889` (`GetShadowSplitBoundsDepthRange`) | **идентично**, включая формулу `OptimalOffset`, кламп `CentreZ` в `[SplitNear, SplitFar]`, замер радиуса по 8 углам и пол `max(sqrt, 1.0f)` |
| **S1** неджиттернутая матрица | там же: `ViewMatrices.GetProjectionNoAAMatrix()` | UE **тоже** фитит каскады по no-AA проекции — независимое подтверждение фикса джиттера |
| **S8** рампа `CalculateOcclusion` | `ShadowFilteringCommon.ush:151-181` | **идентично**, включая вынос `ConstantFactor` из пер-сэмплового кода |
| **S8** tent-свёртка 4×4 | `ShadowFilteringCommon.ush:97-118` (`PCF3x3gather`) | **идентично терм-в-терм** — 16 слагаемых и `dot(..., float4(1-Fy,1,1,Fy)) * 1/9` сошлись. Выведено независимо, совпало |
| **S8** позиционирование гатеров | `ShadowFilteringCommon.ush:246-259` (`Manual3x3PCF`) | **идентично**: `TexelPos = uv*size-0.5`, `SamplePos = (floor+1)*texelSize`, четыре гатера на ±1 |
| **S8** `Square()` и sharpen | `ShadowProjectionPixelShader.usf:89-91`, `:375` | **идентично**: `Square(Shadow)` и `saturate((Shadow-0.5)*Sharpen+0.5)` |
| **S6** формула slope | `ShadowDepthVertexShader.usf:77-90` | **идентично**: `NoL = abs(dot(колонка z матрицы, нормаль))`, `slope = clamp(sqrt(1-NoL²)/NoL, 0, maxSlope)`, `bias = const + slopeScale*slope` |

### ⚠ Три числа были неверны — исправлены в тексте

| Что | Было в документе | В UE | Последствие ошибки |
|---|---|---|---|
| `maxSlope` (S6) | 3.0 | **1.0** (`r.Shadow.ShadowMaxSlopeScaleDepthBias`, `ShadowRendering.cpp:194-199`) | кламп втрое мягче → на поверхностях под острым углом к свету bias втрое больше нужного, лишний peter-panning |
| `receiverBiasMin` (S8) | 0.1 | **0.9** (`r.Shadow.CSMReceiverBias`, `ShadowRendering.cpp:73-77`) | **вдесятеро мимо**. `lerp(0.9,1,NoL)` — лёгкая коррекция; `lerp(0.1,1,NoL)` расширяет переходную зону в 10 раз на касательных углах и размывает тени в кашу |
| кламп bias | отсутствовал | `DepthBias = min(DepthBias, 0.1f)` (`ShadowRendering.cpp:1905`) | на низком разрешении bias может дорасти до отсечения near-плоскостью |

Подтверждённые значения, которые уже стояли верно: `r.Shadow.CSMDepthBias = 10`,
`r.Shadow.CSMSlopeScaleDepthBias = 3`, `CascadeTransitionFraction`/`ShadowDistanceFadeoutFraction`
как доли **длины слайса**.

### Расхождения по устройству (не ошибки, но знать надо)

* **S8, offsets гатера.** UE даёт смещения через `int2`-параметр `Gather` (аппаратно). Мы складываем
  UV — сознательно, чтобы каждый тап можно было заклампить границами контента (S5). Отличие
  осознанное; при отказе от S5 можно вернуться к `int2` и сэкономить ALU.
* **S8, `TransitionScale`.** UE: `TransitionSize = CSMDepthBias / zRange * (ShadowBounds.W / ResolutionX)`,
  в шейдер уходит `1/TransitionSize` (`ShadowRendering.cpp:1913-1954`). Наше
  `transitionScale = 1/depthBiasNDC` — та же величина. **Но осторожно с константой 10:**
  у UE `ShadowBounds.W / ResolutionX` = radius/res, то есть **половина** нашего `unitsPerTexel`
  (`2*radius/res`). Переносить число 10 буквально нельзя, единица другая.
* **S7, порядок клампа.** UE: `if (bClampToNearPlane && OutPosition.z > OutPosition.w) { z = 0.999999; w = 1; }`
  — **до** прибавления bias, в reverse-Z. У нас атлас прямой Z, зеркальный эквивалент —
  `H.z = max(H.z, 0)`, и в нашем коде это **после** bias. Для ortho `w == 1`, так что присваивание
  `w` нам не нужно. Наш порядок безопаснее (bias не может вытолкнуть панкейкнутую вершину),
  но стоит взять эпсилон UE: клампить не в 0, а в ~1e-6.
* **S10, сторона fade-полосы.** Тут наш план и UE расходятся принципиально.
  UE (`DirectionalLightComponent.cpp:920-943`): для НЕ-последнего каскада
  `FadeExtension = (SplitFar-SplitNear)*CascadeTransitionFraction` и **`SplitFar += FadeExtension`** —
  каскад **расширяется**, чтобы полоса перехода была покрыта им самим на полном качестве, а
  `FadePlane` остаётся на исходном сплите. Для последнего — наоборот, `FadePlane -= FadeExtension`
  (плоскость уезжает ВНУТРЬ), и это ровно то, что S10 и предлагает.
  У нас полоса лежит **внутри** каскада N, а сэмплится каскад N+1, чей слайс начинается только на
  сплите — работает лишь потому, что объемлющая сфера N+1 сильно больше его слайса, плюс есть
  fallback-цепочка. **Это не гарантия, а везение.** При реализации S10 надо либо расширять
  `sliceFar` каскада N на `FadeExtension` (как UE), либо явно доказать, что сфера N+1 накрывает полосу.
* **S6, нормализация.** UE не нормализует ни колонку матрицы, ни нормаль вершины. Мы нормализуем
  обе — безопаснее при неравномерном масштабе, ценой пары инструкций.

### Где что лежит

| Файл UE | Путь в дропе | Для шага |
|---|---|---|
| `ShadowFilteringCommon.ush` | `Shaders/Private/` | S8 |
| `ShadowPercentageCloserFiltering.ush` | `Shaders/Private/` | S9 (ещё не сверялось) |
| `ShadowProjectionPixelShader.usf` | `Shaders/Private/` | S8 |
| `ShadowDepthVertexShader.usf` | `Shaders/Private/` | S6, S7 |
| `ShadowRendering.cpp` / `.h` | `Source/Runtime/Renderer/Private/` | S6, S8 (константы, `UpdateShaderDepthBias`, `ComputeTransitionSize`) |
| `DirectionalLightComponent.cpp` | `Source/Runtime/Engine/Private/Components/` | S1, S10 |
| `ShadowSetup.cpp` | `Source/Runtime/Renderer/Private/` | S4, S5, S11, S13 (ещё не сверялось) |

**Не сверено пока:** S9 (receiver-plane bias), S5 (`SHADOW_BORDER`), ~~S11 (`ComputeScissorRectOptim`)~~ — S11 сверена 2026-09-02, см. её раздел (у UE двойной оффсет X,Y),
S13 (CSM caching). Перед их реализацией — читать оригинал, а не выводить заново.

---

## 6. (устарело) Что ещё можно достать из UE, если понадобится

⚠ Раздел устарел: дроп лежит локально, и сверка уже проведена — см. §5 выше. Таблица оставлена
как указатель на файлы.

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
