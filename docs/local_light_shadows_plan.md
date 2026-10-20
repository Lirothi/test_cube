# Тени локальных источников (spot / point, не VSM) — план

Написан 2026-10-20 по просьбе владельца («глянь, как у нас не VSM тени от локальных источников сделаны, и сравни с UE»
→ «запиши улучшения в план и начни его делать»). Режимы: атласы ламп работают в Legacy И в SDSM
(`render::VsmActive()` = «атласы не нужны», `ShadowSettings.h:97-104`); у владельца `graphics_settings.json` —
`"mode": "sdsm"`, то есть это его тени ламп. VSM-путь локальных теней (`vsm_sample.hlsli`) не трогаем.

## 0. Правила

1. **Сначала UE** — `D:\zalupa\Engine` (5.8.2), каждое решение со ссылкой файл:строка; отступление — с причиной.
2. **A/B в одном бинарнике**, замеры на живом редакторе через MCP с закрытым UI (F2), камеры — ниже (§3).
3. **Гейты по типу правки**: формулы/шейдеры — три конфигурации + inspector/intent regression + Debug с отладочным
   слоем; новые ресурсы/проходы/состояния — плюс `--gbv` narrowed по проходу и `--scene-stress-gbv`.
4. Уровни меняются только с согласия владельца (смещения в уровнях: 32 прожектора, все `0.05` / `0.0001`).

## 1. Как сейчас (разбор 2026-10-20, проверено по коду)

| | У нас | UE 5.8.2 |
|---|---|---|
| Кто получает тень | `kMaxShadowedSpotLights = 8`, `kMaxShadowedPointLights = 4` (`LightManager.h:26,31`), по проекционному размеру, без гистерезиса | без счётного лимита: каждая видимая movable-лампа (`ShadowSetup.cpp:6411-6482`), бюджет — тексели и кэш (`:133-157`) |
| Разрешение | фиксированное: spot 512², point 256²/грань (`RenderTargetManager.cpp:424-428`) | по экранному радиусу × `r.Shadow.TexelsPerPixelSpotlight` 2.546 / `…Pointlight` 1.273 (`ShadowSetup.cpp:276-292`, `:4301-4464`), до `r.Shadow.MaxResolution` 2048, кубы 5 размеров |
| Атлас | `Texture2DArray` 8 слайсов + `TextureCubeArray` 24, D16, ×3 кадра в полёте (~21 МБ) | споты в общем 2D-атласе (`FTextureLayout`, `:5971-6090`), кубы из пула |
| Глубина spot | перспективная D16, near 0.1 (из JSON не читается), шаг ~14 см на 30 м | ЛИНЕЙНАЯ через `SV_Depth` (`ShadowDepthPixelShader.usf:80-86`): шаг = диапазон/65535 |
| Смещения spot | только приёмник: нормаль `shadowNormalBias` + NDC `shadowDepthBias` → в мире ~0.001·w² (`spotlight_cs.hlsl:135-184`); проход глубины без смещения (`SceneRenderer_Shadows.cpp:716`) | в проходе глубины: константа `3·512/((MaxZ−MinZ)·Res)·2·ShadowBias` (~15.36/Res м в мире), наклонная `×3·SlopeBias·min(tan,1)` (`ShadowRendering.cpp:2037-2056`, VS `ShadowDepthVertexShader.usf:97-113`); на приёмнике мягкий переход `60·lerp(0.5,1,NoL)` (`ShadowProjectionPixelShader.usf:240-252`) |
| Смещения point | мировые константы 0.10 м к лампе и 0.05 м по нормали (`SceneRenderer.cpp:209`, `pointlight_cs.hlsl:116`) | только на проекции: `(DB + (1−NoL)·3·SlopeBias·DB)/w`, `DB = 0.02·512/Res·2·ShadowBias` (`ShadowRendering.cpp:1962-1983`, `ShadowProjectionPixelShader.usf:487-511`, `ShadowProjectionCommon.ush:193-194`) = доля расстояния |
| Фильтр | коробка 3×3 `SampleCmp` (spot, point в касательной плоскости) | spot `Manual5x5PCF` (tent 6×6 через 9 Gather, `ShadowFilteringCommon.ush:264-365`); point `CubemapHardwarePCF` 29 дисковых выборок (`ShadowProjectionCommon.ush:151-254`); затем Sharpen и Square |
| Потеря тени | слот −1 → `return 1` сразу (поп) | `FadeAlpha` по разрешению (`r.Shadow.FadeResolution` 64, `ShadowSetup.cpp:534-566`) |
| Point-проходы | 6 отдельных видов/списков на лампу | один инстансированный проход ×6, слой из VS (`ShadowDepthVertexShader.usf:247-257`) |
| Кэш | нет (каждый кадр 8 + 24 полных вида) | `r.Shadow.CacheWholeSceneShadows`: статика отдельно, пропуск прохода без подвижных (`ShadowSetup.cpp:3827-3977`, `:6002-6009`) |
| Near | spot 0.1, point `max(0.2, 0.02·r)` | point 1 см (`ShadowSetup.cpp:1169`) |
| Туман / стекло | туман: одна выборка с тем же NDC-смещением; стекло: свой фильтр для point (диск 8) | туман: point — полный `CubemapHardwarePCF` |

## 2. Шаги

### S1. Смещения как у UE — [полдня]
- **Spot**: смещение уходит в ПРОХОД ГЛУБИНЫ (как UE): константа `15.36/Res·2·shadowBias` м, наклонная
  `×3·shadowSlopeBias·min(tan θ, 1)` к оси прожектора (тот же `ApplyShadowDepthBias`, что у CSM). У нас глубина
  перспективная, поэтому в clip-z добавляется `K·Δw/w` (K = n·f/(f−n)) — сдвиг на Δw метров при любой дальности;
  режим — отрицательный `shadowClampNear` (= −K), раскладка CB (240 байт, её же пишет SDSM) не меняется.
  Приёмник: NDC-смещение `shadowDepthBias` снято (рост w²), нормальное `shadowNormalBias` остаётся.
- **Point**: смещение на проекции по UE — сравниваемое расстояние `m / (1 + DB·(1 + 3·slopeBias·(1 − NoL)))`,
  `DB = 0.02·512/Res·2·shadowBias`; мировые 0.10 / 0.05 сняты. Те же формулы в тумане и стекле.
- Ручки лампы UE: `shadowBias` (0.5), `shadowSlopeBias` (0.5) — JSON, Inspector, у spot и point.
- Приёмка: на wind_test (лампы ночью) и demo — нет acne, отрыв тени не больше, чем был; A/B в одном бинарнике
  (переключатель `shadow.localBiasUe`), кадры у основания пальм.

### S2. Фильтры — [полдня]
- Spot: tent-ядро CSM S8 (`csm_sample.hlsli:146-356`: tent 4×4/6×6, Square, Sharpen) на атлас; мягкий переход UE
  (Gather глубины + `TransitionScale`).
- Point: дисковые выборки `DiscSamples12/29` UE со смещением × длина выборки, затем Square.
- `shadowSharpen` лампы (UE ShadowSharpen, `(s−0.5)·(7·sharpen+1)+0.5`).
- Туман и стекло — те же функции (одна реализация).

#### Что сделано по S1 и S2 (2026-10-20), коммит a255eb3e

- **S1, смещения.** Spot: `BuildSpotShadowViewCB` (SceneRenderInternal.h) даёт проходу глубины UE-константу
  `min(15.36/Res·2·shadowBias, 0.1·(f−n))` м и наклон `×3·shadowSlopeBias`, `maxSlope` 1; `ApplyShadowDepthBias`
  (shadow_depth_common.hlsli) при `clampNear < 0` добавляет в clip-z `K·Δw/w`. NDC-смещение приёмника обнуляется на
  CPU (`shadowParams.w = 0`, кроме VSM — там как было), нормальное `shadowNormalBias` остаётся. Point:
  `shadowParams.y = −DB`, `DB = 0.02·512/Res·2·shadowBias`, `volumetric.y = 3·shadowSlopeBias`;
  `local_shadow_bias.hlsli` — одна формула для pointlight_cs, glass (и его VSM-ветки), fog_scatter; нормальное
  смещение 0.05 и мировые 0.10 сняты (в VSM — как было). Ручки `shadowBias` / `shadowSlopeBias` (0.5) у spot и point:
  JSON, EnvironmentRuntime, Inspector («Shadow Depth Bias» прожектора убран из Inspector — читается только при
  `shadow.localBiasUe:0`).
- **S2, фильтры.** `local_shadow_filter.hlsli`: spot — `SpotShadowTent6x6` (UE Manual5x5PCF, 9 Gather, глубина
  линеаризуется, мягкий переход `60/((f−n)·2·shadowBias)·lerp(0.5,1,NoL)` 1/м), point — `PointShadowDisc29` (29
  выборок UE, смещение × длина выборки), оба `LocalShadowFinish` (Sharpen, затем Square). Новый `float4
  shadowParams3` в конце `SpotLightGpu` (near, масштаб перехода, sharpen, фильтр) — зеркала spotlight_cs, glass,
  rt_lights.hlsli; у point — `volumetric.z/w`. Ручка `shadowSharpen` (UE ShadowSharpen, 0). Стекло: прожектор — тот же
  фильтр, точечная — пока свой диск 8 (нет разрешения куба в его CB; S8).
- **A/B в одном бинарнике**: `--set shadow.localBiasUe:0|1`, `shadow.localFilterUe:0|1` (MCP `set_settings`).
- **Замер** (wind_test, 21:00, лампы на пляже, редактор закрыт, экспозиция закреплена EV 13.1): acne нет ни в одном
  варианте; с UE-смещением тень у основания ствола начинается чуть дальше (при 256² у точечной это 4–7 % расстояния);
  фильтры UE убрали «лесенку» 3×3 на краях теней стволов и листьев, ядро тени плотнее (Square). Цена (get_profile,
  3 раунда): Pass_PointLights 0.049 → 0.078 мс (29 выборок на 3 лампах с тенью), Pass_SpotLights 0.025 → 0.026,
  GPU-кадр 3.80 → 3.89 (шум). demo.json — дневная сцена (EV 15), тени ламп не видны против солнца, расхождений нет.
- Гейты: три конфигурации, intent/inspector regression, `check_logging`, Debug `--gbv --gbv-passes=SpotLights,
  PointLights,SpotShadows,PointShadows` на wind_test ночью — ни одного предупреждения.
- **GBV вычищен до нуля** (владелец: «мне нужен чистый gbv»). `id=1044` — ExecuteIndirect `GBuffer.Indirect` с
  bindless-сигнатурой, меняющей VB/IB/root-аргументы на каждую отрисовку: GBV их не прослеживает. Под GBV
  (`GraphicsDevice::GbvEnabled`) G-буфер теперь рисует группами (тот же PSO, сигнатура только DRAW_INDEXED) —
  шейдеры проверяются точно. Выход: кэш bindless-сигнатур не освобождался до отчёта о живых объектах (`id=582` + её
  root-сигнатура `id=577` + счётчик устройства) — освобождается в `Renderer::Shutdown`; отчёт, где живо только
  устройство (его держат сами интерфейсы отчёта), — одна строка Info `live objects at exit: none`.

### S3. Фейд вместо попа — [полдня]
- Выпадение из набора — плавный `ShadowFadeFraction` (UE `CalculateShadowFadeAlpha`), гистерезис рейтинга.
- Фейд по экранному радиусу (тень лампы-точки вдали гаснет, а не обрывается).

#### Что сделано по S3 (2026-10-20), не закоммичено

- **Рейтинг и фейд как у UE.** `LightManager::SelectShadowedSpots/Points` получают `screenScale = max(W/2·P11,
  H/2·P22)` проекции без джиттера при РЕНДЕР-разрешении (UE `View.ViewRect` — после screen percentage;
  `LocalLightSceneProxy.cpp:104`). Экранный радиус — UE `GetEffectiveScreenRadius`: у точечной `radius/dist`, у
  прожектора — вписанная сфера в конце конуса (центр `pos + dir·R·cos`, радиус `R·sin`, `SpotLightSceneProxy.cpp`).
  Желаемое разрешение = радиус × `TexelsPerPixel` (spot 2.546, point 1.273, `ShadowSetup.cpp:282-292`); по нему
  ранжируются слоты (вместо прежнего «(r/(d−r))²») и считается фейд `CalculateShadowFadeAlpha` (полный выше 64
  текселей, ноль на 32, экспонента 0.25, `ShadowSetup.cpp:534-566`). Тень заводится, только пока фейд > 1/256 (как UE
  `:4353`; ниже 33 текселей формула UE уходит в минус — замер это показал, −0.013 и −0.182). Свет, у которого был слот,
  получает рейтинг ×1.25 (`kShadowSlotHysteresis`) — две лампы одного размера не перекидывают слот туда-обратно.
- **Шейдеры.** `lerp(1, тень карты, fade)`: spot — `SpotLightGpu.shadowParams2.z`, point — новый `float4 shadowFade`
  в конце `PointLightGpu` (зеркала pointlight_cs, glass, rt_lights.hlsli). Обёртки `ComputeSpotShadow` /
  `PointShadowFactor` (spotlight_cs, pointlight_cs) и `SampleSpotShadow` / `PointShadowFactor` (glass) вокруг прежних
  `…Map`; туман (fog_scatter_cs) — на выходе выборки атласа. Контактные тени фейд не трогает (как у UE). VSM — fade 1.
- **A/B**: `--set shadow.localFadeUe:0|1` (MCP `set_settings`); 0 = все затенённые на полной силе, слоты всё равно
  по разрешению.
- **MCP**: `get_readouts` несёт `localShadows: {spots, points}` = `[lightIndex, slot, fade]` для каждого света со
  слотом или в фейде.
- **Замер** (wind_test, 21:00, камера отходит от ламп по прямой, 14 точек 20…560 м): `res·d`, восстановленный из
  измеренного фейда, постоянен до 4 знаков — 8509 у всех трёх точечных, 16295 у обоих прожекторов, то есть
  `screenScale` 742.6 и 742.4 — одна величина для обоих типов, как и должно (окно 2560×1440, DLSS Scale 0.58). Точечные (радиус 9 м) гаснут с 133 до 266 м,
  прожекторы (14 м, 38°) — с 255 до 509 м; дальше слот не занимается вовсе (тени не рендерятся). На 60 м фейд 1, кадр
  с фейдом и без совпадает до шума (0.97 % пикселей > 2/255, как пол); на 200 м (fade 0.5–0.65) тени стволов в
  пятнах ламп заметно прозрачнее, без фейда — полные. Цена — один `lerp` в шейдере и несколько операций на свет на CPU.
- **Гистерезис** (demo.json: 5 точечных с тенью на 4 слота; камера на высоте 22 м смотрит вниз и идёт по x от −40 до
  +40 и обратно, шаг 0.5 м): слот между точечными 6 и 5 переходит при x = +10 туда и при x = −1 обратно — петля 11 м,
  ни одного дребезга (каждая смена набора — один раз). Остаётся поп при ВЫТЕСНЕНИИ по лимиту слотов (вытесненная
  лампа рядом, её фейд 1): у UE лимита по числу нет, есть бюджет атласа — это S4.
- Гейты: три конфигурации, intent/inspector regression, `check_logging`; Debug `--gbv --gbv-passes=SpotLights,
  PointLights,VolumetricFog,Transparent` на wind_test в 21:00 — в `render.validation` ни одного предупреждения.

### S4. Разрешение по экранному радиусу и общий атлас — [день-два]
- Spot: `ScreenRadius·2.546·shadowResolutionScale`, до 2048, вниз до степени двойки; упаковка в 2D-атлас.
- Point: 5 классов размера куба (Max/2…/16, мин 32) по `ScreenRadius·1.273`.
- Бюджет в текселях вместо 8/4 ламп; заодно ЛИНЕЙНАЯ глубина spot (или D32F) и near ~1–2 см (+ флаг меша
  «не отбрасывает тень» — плафоны ламп).

#### Что сделано по S4 (2026-10-20, первая часть — разрешение), не закоммичено

- **Разрешение каждого света — по UE.** В том же отборе S3: `clamped = min(желаемое, Max)`, не меньше
  `r.Shadow.MinResolution` 32 (`ClampShadowRolution`, `ShadowSetup.cpp:3387`, `:4330-4347`). Spot — вниз до степени
  двойки, как UE `1 << (CeilLogTwo(x + 1) − 1)` (`:4448-4458`; бордюр UE в 4 текселя нам не нужен — шейдер зажимает
  выборки внутри квадрата). Point — первая из граней `Max/2, /4, /8, /16` и 32, не больше желаемого
  (`GetCubeShadowDepthZIndex`, `:771-797`; после 128 сразу 32 — как у UE).
- **Атласы.** Слой спота = `Max`, слой грани = `Max/2`; каждый свет рисует в ЛЕВЫЙ ВЕРХНИЙ квадрат своего размера
  (viewport, scissor и очистка — только квадрат, `Renderer::BindSpot/PointShadowTarget`). Ручка UE
  `r.Shadow.MaxResolution` — `render::g_localShadowMaxRes`, по умолчанию 2048 (UE Epic; High = 1024),
  `--set shadow.localMaxRes:<n>` (256…4096, вниз до степени двойки), пересоздание в `Scene::ReconcileShadowMode`
  под общим ожиданием GPU, лог `local shadow max resolution -> …`. Память (один экземпляр, S5): 8·2048² + 24·1024² R16 =
  112 МБ при 2048, 28 МБ при 1024. Счётные слоты 8/4 остались — бюджет в текселях и общий 2D-атлас ещё впереди.
- **Шейдеры.** Spot: `shadowParams2.w` = квадрат в слое (res/слой), `LocalShadowRectUV` масштабирует UV и зажимает
  выборки на 3 текселя внутрь (tent 6×6). Point: атлас — `Texture2DArray` 6·N граней, а не `TextureCubeArray`
  (аппаратный куб всегда читает всю грань); `shadowFade.y` = квадрат, `.z` = 1/res; грань и UV выбираются вручную по
  соглашению D3D (`PointShadowFace` / `PointShadowCubeUV`). Диск UE ищет грань ОДИН раз по центру, выборки линейны по
  смещениям диска; выборка за швом читает краевой тексель той же грани — в той же метрике глубины, что и сравниваемая
  (аппаратный куб сравнивал её с глубиной соседней грани по другой оси). Смещение S1 у spot и point — по разрешению
  самого света. Стекло получило 29-точечный диск UE (S8 больше не упирается в разрешение куба в CB).
- **MCP**: `localShadows` — `[lightIndex, slot, fade, resolution]`.
- **Замер** (wind_test, 21:00, тот же проход камеры): классы по дистанции — точечная 1024 (5 м) → 512 → 256 → 128 →
  32 (с ~70 м), прожектор 1024 → 512 → 256 → 128 → 64 → 32; на стандартной камере точечная у пальмы 1024 вместо
  прежних 256, прожекторы 512 (как было). Грани проверены против независимого пути VSM (тени ламп в пуле, без
  новой адресации): направления теней стволов совпадают, у шва под лампой разрывов нет. Цена: Pass_PointShadows
  0.146 → 0.160 мс (2048 против 512 на этой камере), Pass_PointLights 0.074 мс (с картой; 0.035–0.039 без неё) —
  поштучный выбор грани стоил 0.114, с веткой для швов 0.119, аппаратный куб в S2 — 0.078.
- Разрешение вид тени несёт сам (`SceneView::shadowRes`, ставится в Scene вместе со `shadowBias`): проходы не читают
  LightManager во время записи команд.
- Гейты: три конфигурации, intent/inspector regression, `check_logging`; Debug `--gbv --gbv-passes=SpotShadows,
  PointShadows,SpotLights,PointLights,VolumetricFog,Transparent --sweep=shadow.localMaxRes:512,4096,1024,2048` на
  wind_test в 21:00 (четыре пересоздания атласов, лог `local shadow max resolution -> …`) — ни одного сообщения
  `render.validation`; `--scene-stress=12 --scene-stress-gbv --shadow-mode=sdsm` — CLEAN, 7301 enhanced / 0 legacy, ноль
  предупреждений; Debug `--sweep=shadow.mode:1,2,0,2` (атласы 1×1 в VSM и обратно) — чисто, тени ламп вернулись.

### S5. Один экземпляр атласов вместо трёх (~−14 МБ), как у CSM-атласа.

#### Сделано (2026-10-20, до S4 — иначе S4 мерился бы в утроенной памяти), не закоммичено

- `RenderTargetManager::spotShadowAtlas_` / `pointShadowAtlas_` — единственные владельцы; `DeferredTargets::spotShadow`
  / `pointShadow` — невладеющие алиасы, SRV и DSV по-прежнему в кучах каждого кадра. Создание — один раз после цикла
  по кадрам, резидентность и `Destroy` — один ресурс. Довод как у CSM S3.5: атласы пишут Main_SpotShadows /
  Main_PointShadows, читают световые проходы, туман и стекло ТОГО ЖЕ кадра, все на графической очереди (на async —
  BuildAS, ObjectCompute, RTTrace, CloudShadow/Trace — ни один их не объявляет).
- Гейт: Debug `--scene-stress=12 --scene-stress-gbv --shadow-mode=sdsm` (атласы живые) — verdict CLEAN, 7301
  enhanced-барьер / 0 legacy, выход 0. Единственное предупреждение — `id=886` про дисковый кэш PSO: блоб, записанный
  без GBV, под GBV отвергается как «чужой драйвер» (и наоборот), режимы затирали кэш друг другу. Теперь у GBV свой
  каталог `shader_cache/pso_gbv` (`Material.cpp PipelineCachePath`).

### S6. Однопроходный куб point — VS `SV_RenderTargetArrayIndex`, инстансинг ×6 с маской граней.

### S7. Кэш статических кастеров (Rung 1 шаги 12–15 `docs/shadow_caching_and_virtualization_plan.md`) по схеме UE.

### S8. Согласованность и мелочи
- CPU-фолбэк: LOD приёмника и alpha-test для листвы; отсечение кастеров «лампа + фрустум камеры», `RadiusThreshold`.
- `nearPlane` из JSON; контактные тени поверх карты (UE), не вместо.

## 3. Сцены и камеры для замеров
- **wind_test, лампы на пляже** (добавлены 2026-10-20: 4 точечные, из них 3 с тенью, 2 прожектора с тенью,
  все `onAt: night`): 21:00 (`tod.time:21`), камера `--cam-pos=-62,4,50`, yaw 37.6°, pitch 6° (MCP
  `set_camera {position:[-62,4,50], yawDeg:37.6, pitchDeg:6}`).
- **demo.json**: 9 прожекторов и 5 точечных с тенью (больше лимита 4 — виден поп).

## 4. Гейты
| Шаг | Что гонять |
|---|---|
| S1–S3, S8 | три конфигурации; intent/inspector regression; Debug с отладочным слоем на wind_test ночью; `--gbv --gbv-passes=SpotLights,PointLights` |
| S4–S7 | + `--scene-stress-gbv`, Release `--scene-stress=60`, `--gbv` полный на demo |
