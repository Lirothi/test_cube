# Тени локальных источников (spot / point, не VSM) — план

Написан 2026-10-20 по просьбе владельца («глянь, как у нас не VSM тени от локальных источников сделаны, и сравни с UE»
→ «запиши улучшения в план и начни его делать»). Режимы: атласы ламп работают в Legacy И в SDSM
(`render::VsmActive()` = «атласы не нужны», `ShadowSettings.h:97-104`); у владельца `graphics_settings.json` —
`"mode": "sdsm"`, то есть это его тени ламп. VSM-путь локальных теней (`vsm_sample.hlsli`) трогает только S10
(VSM без лимита по UE, дописан 2026-10-20 по просьбе владельца вместе с S9).

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

#### Что сделано по S3 (2026-10-20), коммит 1afa4962

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
- ~~Бюджет в текселях вместо 8/4 ламп~~ → вынесен в **S9** (атласы) и **S10** (VSM). Остаётся: ЛИНЕЙНАЯ глубина spot
  (или D32F) и near ~1–2 см (+ флаг меша «не отбрасывает тень» — плафоны ламп) — отложено до контента с плафонами
  (замер — в «Что осталось» ниже). UE: `MinLightW = 0.1` (см, то есть 1 мм; `SpotLightSceneProxy.cpp:101`,
  `PointLightSceneProxy.cpp:91`), спот пишет линейную глубину через SV_Depth (`ShadowDepthPixelShader.usf:79-85`).
  - **Флаг Cast Shadow — сделан (2026-10-21).** Это UE `UPrimitiveComponent::CastShadow`:
    - объект: ключ уровня `"castShadow": false` (пишется только выключенным), Inspector «Lighting > Cast Shadow»,
      MCP `setCastShadow` (staticMesh и instancedModels), команда с undo, тест `TestCastShadowIsAFlagThatUndoes`;
    - тени: нет тени ни в одной карте — солнце, лампы, VSM, RT-лучи тени (маска TLAS без `rt::kInstanceMaskShadow`).
      Контактные остаются: они трассируют глубину (у UE для них отдельный `bCastContactShadow`);
    - G-буфер: меш остаётся в таблице GPU-пути (иначе уходит на CPU-отрисовку G-буфера), вердикт едет в
      `CasterBounds.halfExtents.w`, его проверяют отсечение теней, разброс VSM, перебор setup, валидатор;
    - каждая перестройка набора кастеров сбрасывает кэш страниц VSM;
    - ветер считается только по кастерам.
    Подробности — в AGENTS.md, «Cast Shadow — Per Object».
    - Замер на wind_test в 14:00 (736 пальм без тени, редактор закрыт):
      - тени пальм пропали — 44.5 % кадра светлее;
      - SDSM: тень 0.193 → 0.038 мс;
      - VSM: VsmPageRender 1.10 → 0.22 мс, GPU-кадр 3.92 → 2.78 мс;
      - G-буфер на CPU не вырос (0.079 мс), undo — кадр в шуме (0.006–0.013 % при > 24/255).
    - Копия local_shadow_test, загруженная со столбом и пальмой A без тени: включение через MCP даёт тень в SDSM на GPU
      и в CPU-пути, в VSM, в Legacy (6.84–6.89 % кадра), undo — точно назад.
    - **Две ловушки по дороге:**
      - пустить в таблицу ВСЁ видимое с мешем — значит пустить и океан: разнородный общий VB/IB выключил одиночную
        отрисовку страниц VSM (CPU 45 мс). В таблице только «выключенные флагом» (`ShadowSwitchedOff`);
      - кэш страниц VSM сбрасывался неявно — сменой размера таблицы LOD кастеров. Без смены размера включённая тень не
        появлялась в VSM на уровне без ветра (0.001 %), теперь сброс явный.
    - Гейты:
      - три конфигурации, регрессии, `check_logging`;
      - GBV `ShadowCull, VsmPageRequest, VsmPageRender, SpotLights, PointLights` (VSM) и `ShadowCull, SpotShadows,
        PointShadows, SpotLights, PointLights` (SDSM) на уровне с не-кастерами — ноль сообщений, валидатор отсечения PASS
        (46 видов + камера);
      - `--scene-stress=12 --scene-stress-gbv` — CLEAN, 7127 / 0, `live objects at exit: none`.
  - Остаётся от S4: near 1 мм и линейная глубина спота. Ждут контента с плафонами: флаг теперь позволяет выключить
    тень у лампочки внутри её же лампы. — **Сделано для спота (2026-10-21)**, см. ниже; near точечной — нет.
  - **Линейная глубина спота и near 1 мм (2026-10-21).** UE: прожектор пишет глубину сам — VS отдаёт пиксельному
    шейдеру глубину в пространстве лампы и смещение (`ShadowDepthVertexShader.usf:146-158` `TransformShadowSpot`,
    смещение `ComputeDepthBiasDirectionalSpot` :97-113), PS — `SV_Depth = saturate(1 − z·InvMaxSubjectDepth + bias)`
    (`ShadowDepthPixelShader.usf:80-86`); near — `MinLightW` 0.1 см (`SpotLightSceneProxy.cpp:101`). У нас прямой Z,
    поэтому `(w + bias − n) / (f − n)`. Шаг D16 — (f − n)/65535 на любой дальности: 0.2 мм на 14 м против 3 см у
    перспективной глубины с near 0.1 м.
    - Шейдеры: `ShadowLinearDepth` (`shadow_depth_common.hlsli`) и перестановка `SHADOW_LINEAR_DEPTH` в
      `gbuffer_csm`, `gbuffer_inst_csm`, `gbuffer_instcb_csm`, `shadow_indirect_csm` (обычный и masked). Интерполянт
      `w` аффинен в мире, перспективно-корректная интерполяция даёт его точно в каждом пикселе; z растеризации остаётся
      перспективным и нужен только отсечению. Кодировка — в четырёх полях смещения PerViewCB (`BuildSpotShadowViewCB`):
      (const м, slope м, −1/(f − n), n); отрицательный maxSlope — признак режима (у UE maxSlope спота и так 1). Раскладка
      PerViewCB не меняется (SDSM пишет её на GPU).
    - PSO: у каждого теневого материала — близнец с define (`Material::SpotLinearDepthVariant`,
      `MaterialManager::LinkSpotLinearDepthVariant`, из кэшированного desc), связывается при создании в
      `RenderableObject::Init`, `GBufferRenderable::BuildInstancedMaterials` (instcb), `ShadowGpuData` (два атласных PSO
      непрямого пути); у прозрачных — нет (проход спота рисует только непрозрачные корзины). Проходы спота и заполнение
      S7-кэша читают режим один раз и передают его в `RenderShadow` / `RecordShadow` / `RecordIndirectShadowDraws`.
    - Приёмники (`spotlight_cs`, `glass`, `fog_scatter_cs`): `SpotLightGpu.shadowParams4.y` = 1/(f − n) у линейного
      атласа, 0 — перспективный (поле S10e `vsm` переименовано в `shadowParams4`); `SpotReceiverDepth` и мягкий переход
      тента (`SpotOcclusion4`) читают обе кодировки. Ключ S7-кэша получил `encoding`: переключение режима при заданном в
      уровне near не оставит в пуле глубину другой кодировки.
    - Near: `SpotLightDesc::nearPlane` 0 = автоматически (`render::SpotShadowNear`): 1 мм на линейной глубине, 0.1 м на
      перспективной — и в VSM (пул D32 с прямым Z при 1 мм дал бы сантиметровые шаги в конце луча). Заданный в уровне
      `shadowNear` побеждает в любом режиме (не меньше 1 мм). Near берётся из одного места (`SpotLight::GetShadowNear /
      GetShadowFar`) — три копии `max(near, 0.01)` в Scene / SceneRenderer / SpotLight убраны; смена режима
      перестраивает проекцию лампы (`UpdateCachedData` сверяет near). Inspector показывает автоматический near.
    - Ручка: `--set shadow.spotLinearDepth:0|1` (MCP `set_settings`); 0 = прежняя перспективная глубина и near 0.1 м.
    - Замер (MCP, редактор закрыт, SDSM, EV −0.5 на local_shadow_test).
      - Акне (`s9_bias.py`, % освещённых по эталону; линейная / перспективная / линейная): A (точечная, контроль)
        0.35 / 0.29 / 0.25; B 2.72 / 2.70 / 2.71; C 0.65 / 0.70 / 0.74 — одинаково в пределах шума; расхождение с VSM в
        его тенях то же.
      - Близкий кастер (`s4_near.py`): кубик 2 см в 5 см от прожектора B по оси затеняет 1.70 % кадра на линейной глубине
        и 0.07 % на перспективной (отрезан near 0.1).
      - wind_test у фонарей (EV 2, луна выкл.): кадр линейная против перспективной 0.47 % пикселей > 8/255 при шуме 0.45 %.
      - Цена: wind_test `Pass_SpotShadows` 0.014 = 0.014 мс (и без S7-кэша 0.015 = 0.015), GPU-кадр 2.777 = 2.777; demo
        (9 прожекторов, кэш выкл.) 0.258 против 0.243 мс, GPU-кадр 2.944 против 2.927 — PS с SV_Depth без раннего теста
        глубины, +15 мкс.
    - **Near точечной — не тронут.** UE: 1 см (`ShadowSetup.cpp:1169`, перспективный куб D16 — его шаг на краю радиуса
      компенсирует смещение долей расстояния). У нас `max(0.2, 0.02·r)`. На wind_test у каждой точечной в самом свете
      стоит сфера-лампочка r 0.12 м с тенью (`lamp bulb 1..4`): near 1 см зачернил бы все четыре лампы. Сначала —
      `castShadow: false` у лампочек (правка уровня, нужно согласие владельца), потом near.
    - Гейты: три конфигурации, регрессии, `check_logging`; GBV `SpotShadows, LocalShadowCache, SpotLights,
      VolumetricFog, Transparent` на wind_test 21:00 SDSM со свипом режима и кэша (1/0/1, кэш 1/1/0) — ноль сообщений,
      `live objects at exit: none`; `--scene-stress=12 --scene-stress-gbv --shadow-mode=sdsm` — CLEAN, 7395 enhanced /
      0 legacy, ноль предупреждений, `live objects at exit: none`.

#### Что сделано по S4 (2026-10-20, первая часть — разрешение), коммит 1afa4962

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

#### Сделано (2026-10-20, до S4 — иначе S4 мерился бы в утроенной памяти), коммит 1afa4962

- `RenderTargetManager::spotShadowAtlas_` / `pointShadowAtlas_` — единственные владельцы; `DeferredTargets::spotShadow`
  / `pointShadow` — невладеющие алиасы, SRV и DSV по-прежнему в кучах каждого кадра. Создание — один раз после цикла
  по кадрам, резидентность и `Destroy` — один ресурс. Довод как у CSM S3.5: атласы пишут Main_SpotShadows /
  Main_PointShadows, читают световые проходы, туман и стекло ТОГО ЖЕ кадра, все на графической очереди (на async —
  BuildAS, ObjectCompute, RTTrace, CloudShadow/Trace — ни один их не объявляет).
- Гейт: Debug `--scene-stress=12 --scene-stress-gbv --shadow-mode=sdsm` (атласы живые) — verdict CLEAN, 7301
  enhanced-барьер / 0 legacy, выход 0. Единственное предупреждение — `id=886` про дисковый кэш PSO: блоб, записанный
  без GBV, под GBV отвергается как «чужой драйвер» (и наоборот), режимы затирали кэш друг другу. Теперь у GBV свой
  каталог `shader_cache/pso_gbv` (`Material.cpp PipelineCachePath`).

### S6. Однопроходный куб point — инстансинг ×6 с маской граней.
- UE: один инстансированный проход ×6, слой из VS (`ShadowDepthVertexShader.usf:247-257`). У нас после S9 все шесть
  граней лампы лежат ОДНИМ блоком 3f×2f в одной странице атласа (один DSV) — поэтому грань выбирается не
  `SV_RenderTargetArrayIndex`, а `SV_ViewportArrayIndex` (6 viewport/scissor на блок). Делать ПОСЛЕ S9.
- Нужен вариант PSO тени (VS пишет индекс viewport) — та же инфраструктура вариантов, что у линейной глубины S4
  (пять мест создания PSO теней, см. «Что осталось»).

#### Что сделано по S6 (2026-10-20): S6a — один ExecuteIndirect на вид; S6b (настоящий однопроходный куб) отложен

- **Замер перед работой** (wind_test 21:00, стандартная камера): запись теней ламп на CPU — 3.4 мс СУММАРНО по воркерам
  на 20 списков, ~170 мкс на вид. Не кадр рендера, а цикл `RecordIndirectShadowDraws`: 141 группа мешей × 4 LOD =
  564 виртуальные группы, и на КАЖДЫЙ вид — перепривязка VB/IB и `ExecuteIndirect` на каждую группу, у которой есть
  кастер ГДЕ-ТО в сцене (`vgCasterCount_` глобальный), пусть этот вид в неё ничего не отобрал; GPU шагает по тысячам
  пустых записей.
- **S6a.** После отсечения ядро `shadow_args_compact_cs.hlsl` (группа из 64 потоков на вид, общий счётчик в
  groupshared) оставляет непустые записи вида, переписывает начало индексов и базовую вершину на сводный мега VB/IB
  (`GroupLodMega`, как у страниц VSM) и пишет их число; проход тени рисует вид ОДНИМ `ExecuteIndirect` со счётчиком.
  Только атласные виды ламп (`kShadowViewCascades`…активные): каскады SDSM переотсекает позже в кадре, VSM рисует
  локальные виды из тех же строк по страницам. Буферы `localArgs_` / `localCounts_` (покоятся в INDIRECT_ARGUMENT,
  объявлены в точках base/consume построителя отсечения), решение — в `PrepareCullPass` на главном потоке
  (`localCompactFrame_`, как `cmdMerge_`). Нужен мега-буфер (одинаковый шаг вершин, индексы R32; сбрасывается
  `Rebuild` посреди игры до следующей загрузки) — иначе прежний путь. A/B: `--set shadow.localCompact:0|1`.
- **Ловушка:** RAW-вид буфера должен начинаться на 16 байт. 138 видов × 4 Б = 552 Б на регион — вид кадра 1 начинался с
  элемента 138; отладочный слой останавливал Debug в `Rebuild` (лог обрывался, выход 2170), Release падал в первом кадре.
  Регионы округлены до 16 байт.
- **Результат.** Кадр тот же: шесть снимков через один, пары «1 против 0» отличаются на уровне пар «1 против 1» (весь
  кадр > 24/255: 0.0015 % против 0.0012 %; листья у фонаря 0.032 против 0.033 %), так же на local_shadow_test и demo.
  Цена (get_profile, редактор закрыт, ×2 раунда A/B/A/B):

  | | запись на воркерах, сумма | стена Pass_PointShadows / SpotShadows CPU | GPU Point / Spot / ShadowCull |
  |---|---|---|---|
  | wind_test, 20 видов | 3.38 → 0.65 мс | 0.227 → 0.113 / 0.205 → 0.081 мс | 0.160 → 0.155 / 0.016 → 0.015 / 0.114 → 0.119 |
  | demo, 39 видов | 4.68 → 1.07 мс | 0.285 → 0.148 / 0.248 → 0.136 мс | 0.233 → 0.230 / 0.190 → 0.188 / 0.102 → 0.107 |

  Ядро стоит +5 мкс GPU в ShadowCull, пустые записи стоили столько же — GPU в сумме не изменился.
- ~~**S6b отложен (замер).** … GPU упирается в ГЕОМЕТРИЮ …~~ — **вывод был неверным** (исправлено 2026-10-21, при S7):
  «не зависит от разрешения» не значит «геометрия». Замер по ВСЕМУ кадру GPU на demo (вид сверху, 9 прожекторов + 30
  граней): 0 ламп с тенью — 3.26 мс, 1+1 (7 списков) — 3.43, 2+2 — 3.55, 4+4 — 3.70, все (39 списков) — 3.92; без меток
  времени профайлера в списках −0.23 мс, без `ExecuteIndirect` −0.04, без привязки/очистки/копии −0.06, пустые списки
  без меток всё равно ~0.3 мс. Цена — САМИ СПИСКИ (~8 мкс GPU на список: граница, `SetDescriptorHeaps`, метки), а
  статики в кадре почти нет (S7 ниже).
- **S6b сделан как «несколько видов на список»** (не однопроходный куб): `render::g_localShadowViewsPerList`, по
  умолчанию 16, `--set shadow.localViewsPerList:<n>` (1 = прежний веер). Список берёт подряд идущие виды, первый выпускает
  точку атласа, порядок отправки — номер списка. demo: кадр GPU 3.94 мс при 1 → 3.84 (2) → 3.76 (4) → 3.74 (8) → 3.71 (16)
  → 3.72 (64); запись на CPU 1.05 → 0.22 мс суммарно, стены Pass_SpotShadows / PointShadows 0.145 / 0.16 → 0.08 / 0.11.
  wind_test (2 прожектора + 18 граней): 3.53 → 3.44 мс, Pass_PointShadows 0.160 → 0.118. Кадр тот же: local_shadow_test
  16 против 1 — пары на уровне шума (> 24/255: 0.0174 против 0.0167 %); на demo различия только на анимированных
  объектах (сетка сфер, предметы с `IsDynamicCaster`), там же, где шум A против A'. Однопроходный куб через
  `SV_ViewportArrayIndex` после этого даст мало: списков и так 1–2 на проход.
- Гейты: три конфигурации, intent/inspector regression, `check_logging`; Debug `--gbv --gbv-passes=ShadowCull,
  SpotShadows,PointShadows,SpotLights,PointLights --sweep=shadow.localCompact:1,0,1,1;shadow.mode:2,2,1,2` на wind_test в
  21:00 — ноль сообщений `render.validation`, валидатор отсечения PASS; `--scene-stress=12 --scene-stress-gbv
  --shadow-mode=sdsm` — CLEAN, 7303 enhanced (+2: переходы двух новых буферов) / 0 legacy, ноль предупреждений.

### S7. Кэш статических кастеров (Rung 1 шаги 12–15 `docs/shadow_caching_and_virtualization_plan.md`) по схеме UE.
- ~~Замер S6: GPU теней ламп — геометрия статики~~ — неверно, см. S6b выше: в demo статики в кадре почти нет.
- Делать ПОСЛЕ S9: кэшу нужно, чтобы место лампы в атласе не менялось от кадра к кадру (S9d); у UE кэшированные
  прожекторы получают свои цели (`ShadowSetup.cpp:6124-6151`). С S9 ценность выше: без лимита каждая лампа с тенью —
  отдельный рендер глубины.

#### Что сделано по S7 (2026-10-21)

- **UE.** `r.Shadow.CacheWholeSceneShadows 1` (`ShadowSetup.cpp:3867-4090`): кэш на ЛАМПУ; валиден, пока не изменились
  инициализатор (матрица) и размер, — тогда `SDCM_MovablePrimitivesOnly` (копия статики + подвижные), иначе
  `SDCM_StaticPrimitivesOnly` + `Movable`. Подвижные — Movable или `!bGoodCandidateForCachedShadowmap`: материал с WPO
  при `r.Shadow.CacheWPOPrimitives 0` (`PrimitiveSceneProxy.cpp:55-61`, `StaticMeshSceneProxy.cpp:424`). Изменение
  статического примитива сбрасывает кэши его ламп (`FLightPrimitiveInteraction::FlushCachedShadowMapData`,
  `SceneCore.cpp:369-386`). Бюджет `r.Shadow.WholeSceneShadowCacheMb` 150.
- **Подвижные у нас** — бит `render::kCasterLocalDynamicBit` (0x100) в ЗАЛИТОЙ копии `CasterLod`
  (`ShadowGpuData::UploadCasterLods`; сам `casterLod_` не трогается — его сравнение с прошлым кадром есть сигнал смены
  LOD для VSM): GI, `IsDynamicCaster`, сдвинутый в последние kFrameCount+1 кадров (`pending_`), с ветром (БАЗОВАЯ сила
  ветра объекта: ослабление по дистанции перекидывало бы кастер на границе туда-сюда при движении камеры).
  Переход кастера на другую сторону = новый статический набор (`LocalStaticVersion`, + каждый `Rebuild`): все записи
  кэша невалидны (UE сбрасывает только лампы примитива; редактирование — не горячий путь). Лог Debug
  `local shadow cache: N of M casters movable (…)`.
- **Отсечение** на кадре «с разделением» (`ShadowGpuData::LocalSplitFrame`, решает `PrepareCullPass`: кэш и S6a
  включены, не кадр валидации — его CPU-эталон считает всех в одной строке) кладёт статические кастеры видов ламп в свои
  строки `staticArgs_` / `staticVisibleList_` (строка = слот − 4), основные остаются подвижным; уплотнение S6a делает
  то же для статических. `RecordIndirectShadowDraws(..., staticRows)`.
- **Пул** `render::LocalShadowCache` (`rendering/shadows/LocalShadowCache.*`): страницы D16 localMaxRes², свои кучи DSV и
  SRV, buddy-аллокатор квадратов степени двойки (прожектор — квадрат своего разрешения, точечная — шесть квадратов
  граней), записи по виду с ключом (матрицы вида и проекции, сравнение точное, как у UE, + разрешение), LRU-вытеснение
  неиспользованных в этом кадре. Растёт в `Scene::ReconcileShadowMode` (GPU простаивает) до
  `shadow.localCacheMaxPages` (8, потолок 16); с кэшем выкл. и в VSM освобождается. Лог `local shadow cache pool -> …`.
  Рост — по странице за кадр (на старте local_shadow_test в headless 5 шагов) — терпимо, но можно растить сразу на
  нужное число.
- **Кадр.** `Scene::PrepareViews` спрашивает кэш для каждого вида (кроме загороженной лампы: её пустые статические строки
  стали бы её картинкой): попадание / промах / нет места. Проход `Main_LocalShadowCache` (перед прожекторами, один
  список) рисует статику промахов в пул (DEPTH_WRITE → PIXEL_SHADER_RESOURCE) и подтверждает заполнение
  (`CommitFills`); на кадре без разделения не подтверждает ничего. Прожекторы и грани: попадание/заполненный промах —
  копия статики в прямоугольник атласа отрисовкой (`shaders/shadow_cache_copy.hlsl`: треугольник на вьюпорт, `SV_Depth`
  из пула, глубина ALWAYS — D3D12 копирует глубину только целыми подресурсами, UE копирует кэш тоже отрисовкой), затем
  подвижные; без кэша — очистка и статические + подвижные строки. Шейдеры ламп не меняются.
- **Ручки**: `shadow.localCache:0|1` (1 — как UE), `shadow.localCacheMaxPages`, `shadow.localCacheWind:0|1` (UE
  `r.Shadow.CacheWPOPrimitives`: 1 кэширует и ветровые — верно только при замороженном ветре). MCP `get_readouts`:
  `localShadows.cache = {hits, misses, uncached, evicted, entries, pages}`.
- **Замер** (A/B в одном редакторе, редактор закрыт). Кадр тот же везде (пары «вкл/выкл» на уровне шума: local_shadow_test
  > 24/255 0.0187 против 0.0179 %, demo 0.0007 против 0.0004 %). Выигрыш:
  - local_shadow_test (всё статично): Pass_PointShadows 0.062 → 0.041 мс, SpotShadows 0.010 → 0.008, ShadowCull +5 мкс;
  - demo (184 статических из 234; 46 ветровых): кадр −24 мкс при 16 видах на список; с `localCacheWind:1` ещё −15…−35;
  - wind_test: 3049 из 3760 кастеров с ветром — выигрыша нет (кадр 3.436 против 3.440 мс), разделение +8 мкс в отсечении.
  Оставшееся на вид — попиксельная запись глубины (копия или очистка, ~18 мкс на прожектор 512² на demo, ~5 на грань):
  следующий шаг UE — **вид без подвижных берёт кэш напрямую** (без копии; лампам нужна страница пула вместо атласа), и
  его стоит делать, только если ламп с тенью станет много.
- Гейты: три конфигурации, intent/inspector regression, `check_logging`; Debug `--gbv --gbv-passes=ShadowCull,
  LocalShadowCache,SpotShadows,PointShadows,SpotLights,PointLights --sweep=shadow.localCache:1,0,1,1;
  shadow.localViewsPerList:16,1,16,4;shadow.mode:2,2,1,2` на local_shadow_test (заполнения, освобождение и рост пула,
  атласы ↔ VSM) — ноль сообщений `render.validation`, `live objects at exit: none`, валидатор отсечения PASS;
  `--scene-stress=12 --scene-stress-gbv --shadow-mode=sdsm` — CLEAN, 7393 enhanced / 0 legacy, ноль предупреждений.

### S8. Согласованность и мелочи
- CPU-фолбэк: LOD приёмника и alpha-test для листвы.
- ~~Отсечение кастеров «лампа + фрустум камеры», `RadiusThreshold`~~ → в S9e (контроль цены без лимита).
- `nearPlane` из JSON; контактные тени поверх карты (UE), не вместо.

#### Что сделано по S8 (2026-10-21)

- **Ручки смещения лампы снова работают.** С texel-смещением (по умолчанию с S9) `Shadow Bias` / `Shadow Slope Bias` лампы
  двигали только смещение UE — их не было видно. Теперь они масштабируют texel-сдвиг: по нормали
  `localNormalTexels × Shadow Bias / 0.5`, к лампе `localPushTexels × Slope Bias / 0.5`
  (`render::LocalShadowNormalTexels` / `LocalShadowPushTexels`). 0.5 (по умолчанию, уровни их не задают) = прежние
  3.0 / 0.5 тексела, кадр тот же. Проверка: копия local_shadow_test с `shadowBias` 0.25 у точечной A — на полу и плите
  проступает муар акне (17 % пикселей вида темнее на ≥ 3 %). `Shadow Normal Bias` прожектора (только UE-режим и старое
  смещение) в Inspector неактивен, пока texel-смещение включено, и подсказка это говорит. Подсказки Inspector и
  комментарии `SpotLight.h` / `PointLight.h` описывают оба режима.
- **`shadowNear` прожектора** (`SpotLightDesc::nearPlane`, был всегда 0.1): ключ уровня `shadowNear`, рантайм
  (`JsonLevel`), документ редактора (`EnvironmentRuntime`), Inspector «Shadow Near (m)», проверка вставки. Проверка:
  `shadowNear` 5 м у прожектора B — листья пальмы и верх столба (ближе 5 м к лампе) выпали из его тени, куб остался.
  Ближе 0.1 опускать стоит вместе с линейной глубиной спота (остаток S4).
- **Контактные тени поверх карты** — режим 3 `contact.localMode` («Both (UE: contacts on top of the map)»), шейдеры
  прожекторов и точечных. Режимы 0–2 — «либо/либо» владельца — не тронуты, по умолчанию ничего не меняется. Замер
  (local_shadow_test, контактные включены): «вместо карты» меняет 12.9 % кадра, «поверх» против одной карты — 0.04 %:
  короткий луч находит контакт там, где карта уже затеняет.
- **CPU-путь ламп — LOD приёмника** (`ReceiverCasterLod`, LOD чанков от камеры и отсечение чанков фрустумом вида), как у
  каскадов; рисовал LOD0. CPU-путь — запасной: `shadow.indirect:0` и GPU-инстансы, не сложенные в GI.
- **Альфа-тест листвы в CPU-пути — отложен.** PSO тени объектов (`*_csm.hlsl`) без маскированного варианта: листья
  отбрасывают сплошные квадраты. На local_shadow_test вид A: CPU-путь отличается от косвенного на 3.9 % пикселей
  (> 24/255) — пятна тени листьев пальмы под лампой A. Нужны маскированные варианты трёх шейдеров с UV в раскладке и
  смена материала по слотам; ради запасного пути не делаю, пока он не станет основным для чего-то.
- Гейты: три конфигурации, intent/inspector regression, `check_logging`; Debug `--gbv --gbv-passes=SpotShadows,
  PointShadows,SpotLights,PointLights,LocalShadowCache --sweep=shadow.indirect:1,0,1;contact.enabled:1,1,1;
  contact.localMode:3,3,0` на local_shadow_test — ноль сообщений `render.validation`, `live objects at exit: none`.

### S9. Атласы без лимита 8/4 — как классические тени UE (режим владельца: SDSM / Legacy) — [два дня]

**UE.** Счётного лимита нет: тень получает каждая видимая подвижная лампа с фейдом > 1/256
(`ShadowSetup.cpp:4353`, `:6429-6482`). Прожекторы пакуются (`FTextureLayout`) в атласы размера максимального
разрешения; заполнился — заводится следующий (`AllocateAtlasedShadowDepthTargets`, `:5971-6090`, `:6029-6044`), атлас
— степень двойки от занятого (`:6066-6077`). Точечные — свой куб из пула на лампу (`:6215-6257`). Ручки бюджета
ограничивают только кэш, не число ламп (`r.Shadow.WholeSceneShadowCacheMb` 150 — сверх него тень рисуется без кэша,
`:151-157`, `:494-514`; `MaxNum{Point,Spot}ShadowCacheUpdatesPerFrame` −1, `:133-149`).

**У нас.** Лимит 8/4 — это не только атласы (8 слоёв спота, 24 грани), но и фиксированные `std::array` видов
(`Scene.h:329-330`, `SceneFrameData.h:748-749`, сигнатуры проходов), раскладка слотов GPU-отсечения
[4 каскада | 8 | 24 | 10 клипмапа] = `render::kMaxShadowViews` 46 (`RenderConstants.h:31`, static_assert
`Scene.cpp:2957-2964`, `ShadowGpuData.cpp:1691-1701`), DSV по слою (`RenderTargetManager.h:195-198, 400-402`).

- **S9a. Отбор без лимита — в атласном режиме.** Тень получает каждый кандидат с фейдом > 1/256 (правило S3 = UE).
  В VSM лимит 8/4 остаётся до S10 (его 32 локальных вида — фиксированная раскладка): лимит отбора зависит от режима.
- **S9b. Страницы атласа.** `Texture2DArray` страниц D16 размера `2·Max` (4096² при Max 2048 = 32 МБ): спот до
  2048², блок точечной до 3072×2048 помещаются. Страницы добавляются по потребности — НО только в точке
  резидентности (`Scene::ReconcileShadowMode`, GPU idle; правило «ручка, освобождающая ресурс»), не посреди кадра:
  кадр, которому мало страниц, урезает вдвое лампы с наименьшим рейтингом и просит страницу на следующий кадр (лог).
  Потолок — ручка `shadow.localAtlasMaxPages` (UE берёт из пула без потолка; нам нужна защита VRAM). wind_test (2+3
  лампы) — одна страница 32 МБ против нынешних 112 МБ фиксированных.
- **S9c. Упаковка.** Прямоугольники степеней двойки, аналог `FTextureLayout` (полки/skyline). Спот — квадрат res².
  Точечная — ОДИН блок 3f×2f на лампу, грани в фиксированном порядке (грань k → смещение `(k%3, k/3)·f`): одна
  страница, один DSV, шесть viewport — основа S6. Раскладка ставится от крупных к мелким.
- **S9d. Стабильность мест.** Лампа держит свой прямоугольник, пока не изменились её разрешение и страница
  (переупаковка только при изменении) — без этого не работают кэш S7 и временная стабильность.
- **S9e. Цена без лимита.** Виды и строки GPU-отсечения — по фактическому числу (векторы вместо `std::array`;
  кольца `ShadowGpuData` растут grow-only, как `EnsureUavRing`; локальные виды дописываются после каскадов и клипмапа).
  Шейдер отсечения перебирает все виды на каждый кастер (`shadow_cull_cs.hlsl:190`) — O(кастеры × виды): впереди
  тест «кастер ∩ сфера/конус лампы» и UE-шное отсечение «лампа + фрустум камеры» (`IntersectsConvexHulls`,
  `ShadowSetup.cpp` в `AddInteractingPrimitives`) и `r.Shadow.RadiusThreshold` (из S8). — Отсечение сделано
  (2026-10-21, раздел «Что сделано по S9e» ниже); векторы вместо `std::array` — нет (потолок 32 / 16 остаётся).
- **S9f. Данные света и шейдеры.** Новый `float4 shadowAtlasRect` у `SpotLightGpu` / `PointLightGpu` (страница, смещение
  xy, масштаб; зеркала spotlight_cs, pointlight_cs, glass, rt_lights.hlsli, fog_scatter): spot UV = смещение +
  uv·масштаб, point — грань из `PointShadowFace` → смещение грани в блоке; зажим выборок внутри своего прямоугольника
  (`LocalShadowRectUV` + смещение). Индекс слота (`shadowParams.y` / `.x`) остаётся флагом «есть тень» и индексом вида
  VSM. MCP `localShadows`: + страница и прямоугольник.
- **Приёмка.** demo: все 9 прожекторов и 5 точечных с тенью, попа при движении нет; wind_test — картинка не
  меняется (кадры A/B), VRAM по строке `mem:`; цена проходов теней (get_profile, редактор закрыт); полный набор
  гейтов S4–S7 + `--sweep=shadow.localMaxRes` и смена режимов (атласы ↔ VSM).

#### Что сделано по S9 (2026-10-20)

- **Раскладка видов.** Префикс VSM прежний — [4 каскада | 8 спотов | 24 грани | 10 клипмапа] = `kVsmShadowViews` 46
  (VSM-шейдеры и таблицы страниц его не замечают); атласные лампы сверх него — 24 спота + 72 грани, всего
  `kMaxShadowViews` 142 (`RenderConstants.h`, `ShadowViewSpotSlot` / `ShadowViewPointFaceSlot` / `ShadowViewClipmapSlot`).
  Лимит отбора — по режиму: атласы 32 / 16 (`kMaxShadowed*`), VSM 8 / 4 (`kVsmShadowed*`) до S10. Отсечение, очистка
  и валидатор идут по `ShadowGpuData::SetActiveViewCount` = max(46, последний занятый слот + 1): с полными 142
  видами ShadowCull стоил +61 мкс, с активными на wind_test 0.177 → 0.116 мс.
- **Страницы.** Два `Texture2DArray`: страница спота Max×Max, страница точечной 3f×2f (f = Max/2, блок лампы — шесть
  граней, грань k в `(k%3, k/3)·f`), DSV на страницу, вид рисует в свой прямоугольник (`Renderer::BindLocalShadowTarget`:
  viewport, scissor, очистка — только он). Страницы растут в `Scene::ReconcileShadowMode` (GPU idle) до ручки
  `shadow.localMaxPages` (4, потолок 8), лог `local shadow atlas pages -> …`. wind_test ночью: 1 + 3 страницы, 44 МБ
  (до S9 — 112 МБ фиксированных).
- **Упаковка** (`ShadowAtlasPacker`, полки next-fit по убыванию высоты, аналог `FTextureLayout`). Не влезло в
  резидентные страницы — КАЖДАЯ лампа урезается вдвое на шаг (как глобальный LOD-сдвиг VSM UE: сначала неравномерно
  урезались только младшие, падали до 32), ниже пола — выбрасывается младшая по рейтингу; лог `local shadow atlas full …`
  с числом урезанных и сколько страниц хотелось бы.
- **Шейдеры.** `shadowAtlas` у `SpotLightGpu` (угол, размер, страница) и `PointLightGpu` (угол блока, размер грани в
  UV страницы; страница в `shadowFade.y`) — зеркала spotlight_cs, pointlight_cs, glass, fog_scatter, rt_lights.hlsli.
  Грани точечной в атласных режимах рендерятся на 2 текселя шире (fov `2·atan(1 + 2g/res)`, `kPointShadowGuardTexels`),
  читаются во внутренних 90°: диск у шва попадает в настоящую глубину той же метрики, а не в зажатый краевой тексель.
- **MCP** `localShadows`: `[index, slot, fade, res, page, x, y]`. Попутно S10a: `KVIEWLOD_VEC4` 11 → 12.
- **Замер.** demo: все 9 прожекторов и 5 точечных с тенью, по странице каждого типа; `localMaxPages:1` — все лампы
  на шаг меньше, `:4` — 3 страницы (36 МБ). Атлас против VSM на одном кадре: формы теней совпадают, 0.77 % пикселей
  (пол 0.04 %). Валидатор отсечения — PASS на 76 видах.
- Гейты — в конце следующего раздела (общие с ним).

#### Peter panning: texel-смещение и геометрическая нормаль (2026-10-20, владелец: «удели внимание питер пеннингу, я его вижу»)

- **Причина.** Смещения UE (S1) рассчитаны на скользящие приёмники и потому крупные: у точечной — доля расстояния
  (`0.02·512/Res·2·ShadowBias` ≈ 5 текселей при bias 0.5, наклонная часть до ×2.5, крайние выборки диска ×длина до 2.5),
  у спота — мягкий переход `(f−n)/60` м ≈ 23 см. Тень начинается в стороне от основания ствола / столба — это и видно.
  У VSM (смещение в текселях) зазора нет.
- **Texel-смещение** (`render::g_localShadowBiasTexel`, по умолчанию в атласных режимах; VSM не трогает) — модель
  наших VSM-ламп на атласах: приёмник сдвигается на `localNormalTexels` текселей СВОЕЙ лампы (у точечной `2m/res`,
  у спота `2w·tan(outer)/res`) по нормали и на `localPushTexels` × наклон (1/NoL, 1…6) к лампе; в проходе глубины
  смещения нет, доли расстояния в сравнении нет, переход спота — 2 текселя (`kSpotRampTexels`).
- **Сколько по нормали — по радиусу фильтра**, не «тексель»: выборка в r текселях от центра встречает плоскость под
  углом θ на r·tgθ глубже, сдвиг по нормали даёт n/cosθ — нужно n ≥ r. Tent 6×6 спота достаёт 3 текселя; диск точечной
  1.25 + билинейное сравнение каждой выборки + перспектива грани — примерно столько же. По умолчанию **3.0 / 0.5**.
- **Геометрическая нормаль.** На песке wind_test с нормалью G-буфера акне оставалось при любом n (до 8 текселей):
  нормал-мапа ряби наклоняет нормаль от лампы, сдвиг вдоль неё на скользящем свете не даёт глубины — узор акне
  повторял рябь. Сдвиг идёт вдоль нормали, восстановленной из глубины (`LocalShadowGeometricNormal`, как
  `ViewNormalFromDepth` у GTAO: по каждой оси сосед с меньшим скачком глубины, не за краем кадра — иначе зажатый
  сэмплер отдаёт ту же глубину и нижняя строка кадра темнела); стекло берёт вершинную нормаль. Шейдинг, режим UE и
  VSM — на прежней нормали. Цена (wind_test, стандартная камера, A/B в одном бинарнике): Pass_PointLights 0.079 →
  0.083 мс, Pass_SpotLights 0.031 → 0.033.
- **Сцена для замера** — `data/levels/local_shadow_test.json` (новая, по просьбе владельца вместо demo с объектами под
  водой): ночь, без океана, четыре станции — A низкая точечная (столб, куб, сфера, наклонная плита, пальма), B крутой
  прожектор, C скользящий прожектор (луч почти вдоль земли), D высокая точечная. Камеры — §3.
- **Метрика** (`s9_bias.py`): «освещено по эталону» = VSM ≈ кадр без карты (2 %); акне = такие пиксели, которые атлас
  темнит на ≥ 3 %. Экспозиция закреплена НИЖЕ клипа: белый песок прячет акне — прежний замер на wind_test при EV 13
  (S1) говорил «акне нет», при EV 15 тот же кадр давал 25–29 %.
- **Результат** (локальная сцена, EV −0.5, без клипа; A / A низко / B / C / D, % освещённых по эталону):
  texel 3.0 + геом. нормаль — 0.29 / 0.40 / 3.77 / 0.73 / 1.85; UE — 0.33 / 0.38 / 3.94 / 0.49 / 1.86; texel 1.5 по
  нормали G-буфера (первый вариант) — 36.5 / 38.4 / 13.6 / 53.8 / 12.9. B ≈ 4 % у всех — края теней листьев от
  дальней лампы (у VSM мягче), не акне. Основания столбов и стволов прилегают, как у VSM; у UE — видимый зазор
  (кропы C и «A низко»). wind_test (EV 15): темнее UE на ≥ 3 % — 5.8 % кадра при шуме UE-против-UE 2.8 % (было 29 %
  до геометрической нормали); остаток — края длинных теней, сдвинутые нормальным смещением, и самозатенение листвы.
- **Ручки** (`--set` / MCP `set_settings`): `shadow.localBiasTexel:0|1` (0 = UE S1), `shadow.localNormalTexels`,
  `shadow.localPushTexels`. Ручка поиска `shadow.localBiasScale` удалена.
- **Осталось.** На вогнутом ребре (грань куба у земли) оба соседа другого «полотна» — нормаль средняя, линия в пиксель
  темнее (читается как контактная тень; так же у GTAO). Замечено и не тронуто: в режиме VSM на wind_test вокруг
  пальмы у фонаря — тёмная трапеция с прямыми краями (артефакт страниц/кастеров VSM).
- **Гейты S9 + смещения:** три конфигурации, intent/inspector regression, `check_logging`; Debug `--gbv
  --gbv-passes=SpotShadows,PointShadows,SpotLights,PointLights,VolumetricFog,Transparent --sweep=shadow.localMaxPages:
  1,4,2,4;shadow.mode:1,2,0,2` на wind_test в 21:00 (пересоздание страниц, атласы ↔ VSM ↔ Legacy) — ни одного
  сообщения `render.validation`, `live objects at exit: none`; `--scene-stress=12 --scene-stress-gbv
  --shadow-mode=sdsm` — CLEAN, 7301 enhanced / 0 legacy, ноль предупреждений.

#### Что сделано по S9e — отсечение кастеров как у UE (2026-10-21)

- **UE.** Три теста сверх фрустума вида. (1) Взаимодействие «лампа — примитив» (`AffectsBounds`): точечная —
  сфера границ против сферы радиуса (`LocalLightComponent.cpp:42-55`), прожектор ещё сфера против конуса
  (`SpotLightComponent.cpp:190-215`); не зависит от камеры. (2) `BuildLightViewFrustumConvexHull`
  (`ShadowSetup.cpp:4139-4199`, `r.Shadow.LightViewConvexHullCull` 1): выпуклая оболочка лампы и четырёх боковых
  плоскостей фрустума камеры — боковые плоскости, внутри которых лампа, плюс плоскость через лампу и каждое
  силуэтное ребро; кастер вне оболочки не отбросит тень ни на что видимое. (3) `r.Shadow.RadiusThreshold`
  (Epic 0.01, `BaseScalability.ini`; `ShadowSetup.cpp:2134-2144`): кастер с радиусом ≤ 0.01 × расстояние до камеры ×
  `LODDistanceFactor` (FOV / 90) тени не отбрасывает. (2) и (3) UE не применяет к кэшу статики
  (`SDCM_StaticPrimitivesOnly`, `:4522-4540`) и к VSM (`:4436-4442`, `:2134`): картинку, которая переживает кадр,
  камера формировать не должна.
- **У нас.** 16 слотов локального вида (`render::kLocalView*Slot`, `InstanceTypes.h`): 6 плоскостей своего фрустума,
  сфера лампы (центр, радиус), конус прожектора (ось, cos внешнего угла), до 6 плоскостей оболочки (место — 8).
  Шейдер (`shadow_cull_cs.hlsl`) узнаёт локальный вид по индексу (клипмап-диапазон — в CB, не литералами):
  фрустум + границы лампы для всех; оболочка и порог — только для того, что кэш S7 не держит (подвижные, или все в
  кадре без разделения). Сфера проверяется точным расстоянием от бокса (строже UE-шного «сфера против сферы»,
  никогда не режет то, что тот оставляет), конус — формула UE дословно по сфере границ кастера. Оболочка —
  `Frustum::LightViewConvexHull` (внутренние плоскости, порядок DirectX); ориентация проверена численно (160 000
  точек: ни одной, чья теневая линия входит во фрустум, вне оболочки; с перевёрнутым знаком — 475 из 3600, проверка
  ловит). VSM: оболочки нет, порог 0 (как UE); границы лампы — да (от камеры не зависят). GPU-инстансы (GI) от порога
  освобождены: UE проверяет компонент целиком, не экземпляр.
- **Сколько режет** (отрисовок «кастер × локальный вид» после: фрустума → границ лампы → оболочки → порога; числа —
  из валидатора):
  - local_shadow_test, вид A: 108 → 90 → 86 → 86; вид D: 69 → 51 → 45 → 45.
  - wind_test 21:00, у фонарей (−62, 4, 50, yaw 37.6, pitch 6): 1287 → 1061 → 982 → 982; спиной к ним (yaw 217.6):
    404 → 324 → 212; сверху (−40, 30, 40, pitch 60): 1305 → 1070 → 1017 → 1008.
  - demo, камера уровня: 69 → 51 → 22; обратный вид: 465 → 343 → 259.
- **Цена** (GPU, редактор закрыт, A/B в одном бинарнике): wind_test у фонарей Pass_PointShadows 0.119 → 0.104 мс,
  спиной 0.050 → 0.045, прожекторы 0.013 → 0.012; Pass_ShadowCull 0.132 / 0.133 — новые тесты бесплатны. demo:
  Pass_SpotShadows 0.172 мс в обоих — его цена в попиксельной записи глубины вида (копия кэша / очистка), не в
  кастерах. GPU.Frame — в пределах шума (±50 мкс).
- **Кадр не меняется** (пары «то же значение» против «перекрёстных», > 8/255): local_shadow_test 0.081 / 0.070 %,
  wind_test 0.125 / 0.101 %, demo без океана 0.010 / 0.008 % и 0.406 / 0.376 %. С океаном на demo перекрёстные выше
  (0.21 против 0.13 %, но максимум «того же» 0.25) — это блики воды под джиттером DLSS: нулевой контроль (A = B)
  даёт столько же «систематических» пикселей, без океана разницы нет. Порог радиуса на wind_test невидим.
- **Валидатор.** CPU-двойник тестов в `PollValidation` (кадр проверки не разделяет — все кастеры проходят оболочку
  и порог); его строка `local shadow caster cull: …` — счётчики по стадиям. `--set shadow.cullValidate:1` (и MCP
  `set_settings`) запускает проверку заново на текущем виде — так сняты числа выше.
- **Ручки:** `shadow.localBoundsCull:0|1`, `shadow.localHullCull:0|1`, `shadow.localRadiusThreshold:<доля>` (0 = выкл.).
- **Не сделано:** векторы видов вместо `std::array` — потолок 32 прожектора / 16 точечных в атласных режимах
  остаётся (у UE его нет). Цена вида теперь в записи глубины, не в отсечении; следующий рычаг — тот же, что после S7
  (вид без подвижных берёт кэш напрямую).
- Гейты: три конфигурации, intent/inspector regression, `check_logging`; Debug `--gbv --gbv-passes=ShadowCull,
  LocalShadowCache,SpotShadows,PointShadows,SpotLights,PointLights --sweep=shadow.localHullCull:1,0,1,1;
  shadow.localBoundsCull:1,1,0,1;shadow.localRadiusThreshold:0.01,0.01,0,0.5;shadow.mode:2,2,1,2` на
  local_shadow_test — ноль сообщений `render.validation`, `live objects at exit: none`, валидатор PASS; MCP-прогоны на
  трёх уровнях — PASS на каждом виде. Буферы, барьеры и проходы не менялись (только CB отсечения 32 → 48 байт и
  содержимое слотов фрустумов) — scene-stress не гонял.

### S10. VSM без лимита — как UE (нужно, только если основным станет режим VSM) — [3–5 дней]

**UE.** Идентификаторы VSM раздаются каждый кадр двумя счётчиками (`VirtualShadowMapArray.cpp:1086-1105`,
`ShadowSceneRenderer.cpp:644-680`): прожектору 1, точечной 6 (`ShadowSceneRenderer.cpp:398`). Одна текстура таблицы
страниц, на VSM регион 128×192 (16k виртуальных, 8 мипов: `VirtualShadowMapDefinitions.h:17-20, 37-38`; смещение по
id — `VirtualShadowMapPageAccessCommon.ush:127-147, 192-215`), растёт строками (`VirtualShadowMapArray.cpp:2649-2658`;
практический потолок ~5400 полных VSM — оценка по размеру текстуры, констант нет). **Дальние лампы — «одна
страница»**: если по консервативному мипу вся карта лампы влезает в одну страницу 128×128 (`ShadowSceneRenderer.cpp:
329-396`), она берёт дешёвый слот из 8192 (`VirtualShadowMapDefinitions.h:66-67`), обновляется по очереди по одной за
кадр (`r.Shadow.Virtual.MaxDistantUpdatePerFrame` 1, `:217-259`). Мип — по пикселю: размер пикселя камеры в
пространстве тени (`VirtualShadowMapProjectionCommon.ush:96-118`, `VirtualShadowMapDefinitions.h:178-196`) +
`ResolutionLodBiasLocal` (`ShadowSceneRenderer.cpp:61-74, 275-281`). Пул один на всё (`MaxPhysicalPages` 2048, Epic
4096); переполнение — грубее мип (`PropagateMappedMips`, `VirtualShadowMapPageManagement.usf:337-383`) и динамический
сдвиг LOD при загрузке > 0.85 (`VirtualShadowMapCacheManager.cpp:155-187, 696-721`), тени не пропадают. Кэш — по
лампе (`FLocalLightCacheKey`, `VirtualShadowMapCacheManager.h:146-154`), лампа без запросов держит страницы 10 кадров
(`VirtualShadowMapCacheManager.cpp:1336-1350`). Страницы размечаются только там, куда лампа достаёт (сетка ламп,
`VirtualShadowMapPageMarking.usf:257-270`).

**У нас.** 42 фиксированных вида (8 + 24 + 10 клипмапа), 341 запись таблицы на вид (2048 виртуальных, 5 мипов,
`VirtualShadowMap.h:32-83`), пул 1024 страницы D32 = 64 МБ; слот = номер вида (`vsm_sample.hlsli:192`,
`VirtualShadowMap.cpp:1583-1585`, `vsm_page_setup_cs.hlsl:265`). Память не мешает (1.4 КБ таблицы на вид: удвоение
ламп = +88 КБ). Мешают: литералы, продублированные в C++ и шейдерах (`vsm_addressing.hlsli:8,29`
`VSM_MAX_VIEWS 42` / `VSM_NUM_LOCAL_VIEWS 32`, `vsm_page_request_cs.hlsl:18-24`, `vsm_sample.hlsli:153` `kSpotViews 8`,
`vsm_page_setup_cs.hlsl:339-340` `view < 32u`); маска «вид изменился» `uint2` — **жёсткий потолок 64 вида**
(`vsm_page_setup_cs.hlsl:72`, `VirtualShadowMap.cpp:1402-1413`); проход запроса страниц перебирает ВСЕ локальные
виды для КАЖДОГО пикселя (`vsm_page_request_cs.hlsl:77`); кэш по слоту — смена лампы в слоте перерисовывает вид
целиком (`VirtualShadowMap.cpp:1411-1413`); мип — по расстоянию до камеры с одним `g_refDist`
(`vsm_addressing.hlsli:40`), не по размеру пикселя.

- **S10a. Предшаг (мелкий, можно в любой момент).** Один источник для счётчиков: константы C++ → defines шейдеров при
  компиляции; маска изменившихся видов — массив uint по числу видов; ~~поправить `KVIEWLOD_VEC4`~~ (сделано в S9:
  12); лог пула пишет «D16, 32 МБ», на деле D32 и 64 МБ. — **Сделано (2026-10-21):** `Material.cpp
  EngineShaderDefines` даёт каждой компиляции (DXC и запасной D3DCompile, и ключу кэша шейдеров) `TC_SHADOW_CASCADE_VIEWS`,
  `TC_VSM_SPOT_VIEWS`, `TC_VSM_LOCAL_VIEWS`, `TC_VSM_CLIPMAP_LEVELS`, `TC_MAX_SHADOW_LODS` из `render::kShadowView*` /
  `kMaxShadowLods`; `vsm::` строится из них же. Литералы ушли из `vsm_addressing.hlsli` (без define — `#error`),
  `vsm_page_request_cs`, `vsm_sample` (`kSpotViews 8`), `vsm_page_setup_cs` (`KVIEWLOD_VEC4`, `rung0View = view + 4`,
  `KMAX_SHADOW_LODS`), `vsm_page_scatter_cs`, `vsm_hzb_post_cs`, `shadow_cull_cs` / `shadow_cull_post_cs` /
  `cam_cull_post_cs` (`kMaxShadowLods`, `kCascadeViews`). Маска «вид изменился» — `uint4 gViewDirtyMask[KVIEWDIRTY_VEC4]`
  (бит на вид, 128 на uint4; C++ `vsm::kViewDirtyVec4`), потолка 64 больше нет. Лог пула: `D32 … 64.00 MB`.
  Проверка: кадр VSM на wind_test 21:00 и поведение кэша страниц — те же, что у бинарника HEAD (A/B со спрятанными
  правками: PageRender на стоянке 4.33 / 4.08–4.35 мс, `vsm.pageCaching` 1 против 0 одинаково и там, и там — на wind_test
  каждый кадр что-то движется); GBV по `VsmPageRequest, VsmPageRender, ShadowCull` в VSM на wind_test и local_shadow_test
  — чисто.
- **S10b. Динамические id VSM** каждый кадр: прожектору 1, точечной 6; таблица страниц растёт grow-only в точке
  резидентности; виды — общие векторы S9e; id VSM лампы — в её данных (вместо номера слота).
  — **Сделано как «VSM на всей раскладке отсечения» (2026-10-21).** Вид VSM = слот отсечения − каскады для всех 138:
  [8 спотов | 24 грани | клипмап | ещё 24 спота | ещё 72 грани] (`vsm::SpotView / PointFaceView / ClipmapView /
  LocalView / IsClipmapView`, в шейдерах `VsmSpotView` …, `vsm_addressing.hlsli`), так что `rung0View = view + 4` верен
  для каждого вида, клипмап не сдвинулся, а VSM отбирает те же 32 прожектора / 16 точечных, что атласы (было 8 / 4).
  Слот лампы и есть её id: отбор S3 и так раздаёт слоты каждый кадр по рейтингу. Таблица страниц — фиксированная на
  максимум, 341 × 138 = 47058 записей (188 КБ), без роста: при потолке 32 / 16 рост не стоит кода. Проходы ходят только
  до последнего занятого вида (`BuildVsmViews` → счётчик: запрос, разброс, настройка). Попутно — ошибка ещё S9:
  `Scene::PrepareViews` держал задачи видов в `inl_vector<…, 48>`, а видов на отсечение до 135; в Release переполнение
  писало за край стека (падение `0xc0000005`, `Scene.cpp:2527`, в Debug только assert). Ёмкость = 160 со static_assert.
  Проверка: уровень-копия local_shadow_test с 10 прожекторами кольцом и 6 точечными — в VSM тень у всех 10 + 4 видимых
  (раньше у 8 + 4), валидатор отсечения PASS на 48 видах.
- **S10b′. Акне VSM у ламп (владелец увидел на этом уровне: штриховка четырёхугольниками и светлые швы).** Было и до
  S10b (уровень 8 + 4, одни старые слоты, — так же). Две причины: сдвиг приёмника считался в текселях ЗАПРОШЕННОГО мипа,
  а выборка падает на более грубый, если страницы нет, — вся такая страница уходила в акне; и сдвиг шёл вдоль нормали
  с нормал-мапой (урок S9). Теперь (`vsm::g_localTexelBias`, `--set vsm.localTexelBias:0|1`) тексель — того мипа,
  который реально читается (`VsmSpotSampledLevel` / `VsmPointSampledLevel`, `vsm_sample.hlsli`), вдоль геометрической
  нормали из глубины, а сдвиг по нормали 1.0 → **1.5 текселя** (досягаемость PCF 3×3 со сравнением билинейным: правило S9
  «сдвиг ≥ радиус фильтра»). Замер без DLSS, экспозиция ниже клипа, против атласа (темнее ≥ 3 % / светлее ≥ 3 %): 8 + 4 —
  старое 12.6 % / 1.37 %, новое 1.0 текселя 0.51 / 1.72, 1.5 — 0.07 / 1.73, 2.0 — 0.04 / 1.74; 10 + 6 — 10.0 → 0.09 %.
  Ноги теней на месте. `--set vsm.localLateralTexels / vsm.localDepthPushTexels` добавлены (были только в окне
  разработчика). Внимание: `graphics_settings.json` владельца хранит `localLateralBiasTexels: 1.0` и перекрывает
  новое умолчание (1.0 даёт 0.5 % акне). Стекло и туман — по-прежнему своим смещением.
- **Найдено, не сделано: переполнение пула VSM.** На том же уровне запрошено 4162 страницы при пуле 1024 — 3138 не
  досталось, и такая страница читается «освещено» (`VsmSampleNDC`: нет резидентного мипа → 1): тени сферы и ствола у
  станции A пропадают (это и есть «светлее атласа» 1.7 %, от сдвига не зависит). Так было и до S10b; больше ламп —
  больше страниц. Лечится S10e (UE: грубее мип и динамический сдвиг LOD при загрузке > 0.85).
  — **Сделано (S10e, часть 1 — динамическое разрешение UE, 2026-10-21).** `VirtualShadowMapCacheManager.cpp:683-722`
  дословно: загрузка = 1 − свободные / пул (счётчик свободных уходит в минус на отказах, как UE
  `LastFreePhysicalPages`), цель = сдвиг того кадра + log2(загрузка / 0.85), к ней — ×0.5 за кадр при перегрузке,
  ×0.1 через 10 кадров без неё, в пределах [0, 2] (`r.Shadow.Virtual.DynamicRes.MaxResolutionLodBias` /
  `MaxPagePoolLoadFactor`; `--set vsm.dynamicResMaxLodBias`, `vsm.dynamicResMaxPoolLoad`, 0 = выкл.). Счётчики
  копируются каждый кадр в кольцо из 16-байтовых слотов (`countersReadback_`, точки `countersCopy` / `countersRestore`
  в проходе запроса) и читаются `VirtualShadowMap::UpdateDynamicResolution` на главном потоке, когда слот вернулся;
  сдвиг того кадра хранится рядом (UE возвращает его в том же сообщении). Сдвиг входит в выбор мипа как
  `vsm::LocalRefDist() = refDist · 2^−b` — запрос страниц, прожекторы, точечные, туман, стекло. Предупреждение
  `VSM page pool overflow: N page allocations were not served …` — раз на эпизод, как у UE. **Ловушка:** обновление
  VSM на неподвижной камере засыпало через kFrameCount + 1 кадров — регулятор переставал получать данные, а страницы
  старого сдвига (мип 0) не уходили из пула через LRU, и пул так и стоял переполненным (сдвиг 2.0, отказы 605).
  Теперь VSM не засыпает, пока пул «оседает» (`DynamicResSettling`: есть отказы или сдвиг дальше 0.05 от цели).
  Замер (уровни-копии без DLSS, против атласа, «светлее» = пропавшие тени): 8 + 4 — 2.11 → 0.36 %, 10 + 6 —
  1.57 → 0.32 %; в редакторе пул садится за ~0.5 с на загрузку 0.841 при сдвиге 1.58 и держится. Остаток — тени
  на сдвиге 1.58 мягче атласных. Если пулу не хватает и на сдвиге 2, VSM будет обновляться каждый кадр (дороже, но
  без замороженных дыр). Гейты: три конфигурации, регрессии, `check_logging`, GBV `VsmPageRequest, VsmPageRender,
  SpotLights, PointLights` со свипом сдвига и режимов — ноль сообщений; `--scene-stress=12 --scene-stress-gbv` — CLEAN,
  6896 enhanced / 0 legacy (+62 — переходы копии счётчиков). В отчёте о живых объектах этого прогона —
  `VSM.DummySrvs` (известная открытая утечка с DLSS, плавает: в прогоне S10b часом раньше её не было; найдена и закрыта
  в S10c — гонка ленивого создания, DLSS ни при чём).
  Ещё не сделано в S10e: разметка страниц по сетке ламп, мип по размеру пикселя.
  — **S10e, часть 2 (2026-10-21): мип по размеру пикселя и разметка только «своими» лампами.**
  - Мип локальной лампы — UE (`VirtualShadowMapProjectionCommon.ush:96-118`, `VirtualShadowMapDefinitions.h:178-196`):
    радиус пикселя камеры на приёмнике (глубина вида × min(1/(W·P00), 1/(H·P11)) при рендер-разрешении) проецируется в
    вид лампы (× масштаб проекции по x / глубину приёмника в нём), в текселях × 2; мип = floor(log2 + сдвиги)
    (`VsmLocalMipFromPixel`, `vsm_addressing.hlsli`; `vsm::LocalMipParams`). Учитывает расстояние до лампы, ширину конуса
    и режим DLSS — у прежнего «расстояние до камеры / refDist 5 м» ничего этого не было. Запрос и выборки прожекторов /
    точечных — одно и то же число (`VsmSpotShadowFrom` / `VsmPointShadowFrom`); стекло и туман — по-прежнему по
    расстоянию (их мип падает на грубее, если запрошенного нет). Ручки: `vsm.localMipFromPixel:0|1`,
    `vsm.resolutionLodBiasLocal` (UE `r.Shadow.Virtual.ResolutionLodBiasLocal`, 0); подсказка «LOD ref distance» в
    окне разработчика говорит, что локальные поверхности им больше не управляются.
  - Разметка: приёмник вне досягаемости лампы (её сфера, `SceneView::lightSphere`) страниц ей не просит, а у точечной
    проецируется только в ту грань, куда смотрит его направление (`VsmCubeFace`, то же правило, что у выборки) — вместо
    шести (UE размечает пиксель только лампами его клетки сетки ламп). `ViewProjEntry.cull`, `params.y` = грань.
    `vsm.requestLightCull:0|1`.
  - **Ловушка:** смена любой ручки, от которой зависит запрос (режим мипа, сдвиги, refDist, разрешение), не будила VSM
    на неподвижной камере: страницы оставались от старого выбора, а выборка читала по новому. Теперь это часть проверки
    «ничего не изменилось» (`SceneRenderer::vsmLastRequestKey_`).
  - Замер (MCP, редактор закрыт, VSM против атласа SDSM на том же кадре):
    | | загрузка пула | сдвиг дин. разрешения | VsmPageRender | GPU-кадр |
    |---|---|---|---|---|
    | wind_test 21:00, мип по пикселю | 0.48 | 0 | 2.64–2.80 мс | 5.75–5.99 мс |
    | wind_test, мип по расстоянию | 0.85 | 0.66 | 3.62–3.91 мс | 6.74–7.31 мс |
    | уровень 8 + 4, по пикселю | 0.82–0.85 | 0.04–0.11 | — | — |
    | уровень 8 + 4, по расстоянию | 0.84 | 1.58–1.60 | — | — |
    Кадр против атласа тот же или ближе (wind_test темнее 2.37 / 2.41 %, 8 + 4 — 0.33 / 0.44 %). Отсев в запросе на
    wind_test: пул 0.52 → 0.48, запрос 0.120 → 0.115 мс, кадр тот же; на 8 + 4 (все лампы в одну точку) — без разницы.
  - Гейты: три конфигурации, регрессии, `check_logging`; GBV `VsmPageRequest, VsmPageRender, SpotLights, PointLights`
    на wind_test в VSM со свипом обеих ручек — ноль сообщений, `live objects at exit: none`.
  - Не сделано: настоящая сетка ламп (клетки экрана со списками ламп) — нашего отсева сферой хватает, пока ламп
    десятки. `ResolutionLodBiasLocalMoving` — сделано в части 3.
  — **S10e, часть 3 (2026-10-21): `ResolutionLodBiasLocalMoving` UE — подвижная лампа на мип грубее.**
  - UE: `r.Shadow.Virtual.ResolutionLodBiasLocalMoving` 1 (`ShadowSceneRenderer.cpp:68-72, 277-280`); сдвиг =
    `InterpolateResolutionBias(неподвижный, движущийся, MobilityFactor)` = lerp(n, max(n, m), f)
    (`VirtualShadowMapArray.cpp:5712`); f = 1 в кадр сдвига лампы и линейно падает до 0 за
    `r.Shadow.Scene.LightActiveFrameCount` 10 кадров (`ShadowScene.cpp:187-222`, `UpdateForRenderedFrame`).
  - У нас: `LightManager::UpdateLightMobility` раз в кадр (после `UpdateSpotLightCache`) сравнивает позу тени каждой
    лампы с прошлой — прожектор: позиция, направление, дальность, косинус конуса, near; точечная: позиция, радиус — и
    помнит кадр последнего сдвига; лампа, увиденная впервые, считается стоящей (иначе загрузка уровня — кадр сдвига
    всех ламп). `vsm::LocalMovingMipBias(f)` = f · (max(n, m) − n) — только добавка к общему сдвигу
    `resolutionLodBiasLocal`, который уже стоит в мипе.
  - Добавка — на лампу, одна и та же в запросе и в выборке (иначе выборка читала бы мип, которого никто не просил):
    вид VSM несёт её в новом `ViewProjEntry.mip.x` (`SceneView::vsmMipBias`, `BuildVsmViews`), прожектор и точечная
    в шейдере освещения — в новом `float4 vsm` в конце `SpotLightGpu` / `PointLightGpu` (стекло и RT-лампы получили
    то же поле, не читают его). Решение «дальняя лампа» (S10d) тоже считает с ней.
  - Пока хоть одна лампа подвижна, VSM не засыпает на неподвижной камере (`lightsSettling` в `DecideFrame`): иначе
    кадр, на котором добавка сошла в 0, остался бы на грубом мипе.
  - Ручки: `--set vsm.resolutionLodBiasLocalMoving` (UE 1), `vsm.lightActiveFrameCount` (UE 10). Readout `vsmPool`:
    `mobileLights`.
  - MCP: `move` теперь достаёт лампы по имени (`EditorActionDesc::reachesLights`, ещё у `setOnAt`) — через ту же
    `EditEnvironmentCommand`, что Inspector и гизмо, одной записью истории вместе с объектами; имя лампы из уровня
    (`"name": "ring spot 3"`, у сущности — «Spot Light 3») теперь находится и поиском аутлайнера
    (`editormatch::kSearchPropertyKeys`). Раньше до лампы через MCP было не дотянуться: выделение MCP не делает.
    Гейт — `TestMoveReachesLights` (по имени, по имени из уровня, лампы + пальмы одной записью, undo, `delete`
    ламп не видит).
  - Замер (MCP, редактор закрыт, уровень 10 + 6, 10 прожекторов ходят по ±2 см ~90 раз в секунду, 3 с на значение,
    1 → 0 → 1 → 0):
    | сдвиг | загрузка пула | VsmPageRender | VsmPageRequest |
    |---|---|---|---|
    | 1 (UE) | 0.68–0.71 | 0.18–0.19 мс | 0.47–0.48 мс |
    | 0 | 0.85 | 0.20 мс | 0.52–0.53 мс |
    Через секунду после остановки `mobileLights` 0, кадр против стоячего — 0.011–0.014 % (> 8/255), шум.
  - Гейты: три конфигурации, регрессии, `check_logging`; GBV `VsmPageRequest, VsmPageRender, SpotLights, PointLights`
    — ноль сообщений, `live objects at exit: none`; `--scene-stress=12 --scene-stress-gbv` — CLEAN, 7663 enhanced /
    0 legacy, `live objects at exit: none` (структуры ламп выросли на 16 байт).
- **S10c. Кэш по лампе**, не по слоту (ключ: лампа + матрица); лампа без запросов держит страницы N кадров (UE 10).
  — **Сделано (2026-10-21).**
  - Липкие слоты (`LightManager.cpp StickyShadowSlots`): лампа держит свой слот, пока она в наборе; дыру ушедшей
    занимает последняя, новые — в конец. Раньше отбор раздавал слоты по рейтингу заново каждый кадр, и две лампы,
    поменявшиеся местами в рейтинге, менялись видами.
  - Перенос страниц (UE: `NextData` → id этого кадра, на GPU `UpdatePhysicalPageAddresses`,
    `VirtualShadowMapPhysicalPageManagement.usf:155-200`). Scene отдаёт владельца каждого вида (`SetViewOwners`:
    прожектор = номер лампы, грань = лампа · 8 + грань), `PlanViewRemap` сравнивает с тем, для кого сделаны страницы
    пула. Где лампа сменила вид — проход `vsm_page_alloc_remap_cs.hlsl` (поток на физическую страницу, два прохода, до
    touch): старая запись таблицы стирается, страница записывается под новым видом, `PhysOwnerPrev` идёт за ней (кэш
    видит её старой, не новой). Таблица прошлых матриц `prevViewVp_` тоже переезжает: тест «вид изменился» сравнивает
    лампу с её же прошлой матрицей (сдвинулась — перерисовка, как раньше). Вид, на который приезжают и чьи страницы
    никуда не едут, освобождается.
  - Парковка (UE `r.Shadow.Virtual.Cache.MaxLightAgeSinceLastRequest` 10, `AllocateUnreferenced`,
    `VirtualShadowMapCacheManager.cpp:1314-1377`). Лампа, ушедшая из набора, держит страницы 10 кадров: на своём виде,
    пока его никто не занял, иначе — переносом на свободный (сверху, вне диапазона проходов). Вернулась вовремя —
    страницы едут к ней. Запаркованный вид никто не рисует (у лампы нет вида отсечения). Поэтому кадр, который мог бы
    изменить его страницы незаметно, освобождает всё запаркованное на следующем плане (`parkedStale_`): forceAll (кэш
    выкл., прогрев, смена LOD кастеров), ветер на локальных (их страницы и так перерисовываются каждый кадр), сдвиг
    кастера (`MoverCount`). По истечении срока — так же. Сравнение матриц для запаркованного вида пропускается: иначе
    нулевая матрица пустого слота пометила бы его изменившимся, и страницы бы очистились.
  - Ручки: `--set vsm.lightPageRemap:0|1`; `vsm.maxLightAge:<кадры>` (0 = без парковки); `vsm.lruFrames` — возраст
    страниц пула, тот же, что в настройках. Readout `vsmPool`: `remapFrames / remapMoved / remapDropped`,
    `parked / parkedBack / parkedExpired / parkedStale`.
  - Проверка (MCP, редактор закрыт).
    - demo: точка 6 уходит из середины, точка 4 переезжает 4 → 1 (6 граней); страницы точки 6 паркуются и под ветром
      честно портятся. Кадр «перенос вкл. против выкл.» 0.26 / 0.24 % (> 24/255) при шуме 0.23 %.
    - Уровень 10 + 6 (без ветра), уход с крайнего слота и возврат: 6 граней запаркованы на месте и вернулись.
    - Уход из середины: 6 граней уехали на свободные виды и вернулись на другой слот (moved 18 = 6 + 6 + 6). Кадр после
      возврата против свежего пула — 0.0077 % (> 8/255) при шуме «свежий против свежего» 0.0071 %.
    - С выключенным кэшем: 6 испорчены, назад не отданы, кадр тот же.
    - Цены не видно: рендер страниц на этих уровнях 0.13–0.34 мс, и переход стоит столько же.
    - **Оговорка:** с настоящим возрастом 10 кадров на 300+ fps вышедшая лампа через MCP вернуться не успевает (вызов
      дольше 10 кадров — так и истекло), поэтому механизм проверен с возрастом 100 и LRU 120.
  - **Возраст страниц как у UE (владелец: «подними лимит для lru до 1000»).** Был LRU 16 кадров без запроса и никакого
    выселения под давлением: пустых страниц нет — запрос отказан, даже если рядом лежат страницы, которые никто не
    просил. Простой подъём до 1000 при такой схеме оставил бы полный пул отказывающим, пока старые страницы досиживают
    свой срок. Поэтому перенесён порядок UE (`VirtualShadowMapPhysicalPageManagement.usf`: `UpdatePhysicalPages`,
    `PackAvailablePages`, `AppendPhysicalPageLists`; `PopPhysicalPageList` берёт с конца).
    - `vsm_page_alloc_freelist_cs.hlsl` — одна группа из 1024 потоков сортирует пул в groupshared (битонная сортировка):
      список = [кэш-страницы, не запрошенные в этом кадре, от недавних к давним | пустые]. Map берёт с конца: сначала
      пустые, потом самые давние.
    - Map выселяет взятую кэш-страницу (стирает запись прежнего владельца, как `AllocateNewPageMappings`).
    - Порог по умолчанию — `MaxPageAgeSinceLastRequest` 1000 (`vsm::kLruFrameThreshold`, предел ручек
      `kMaxLruFrames` 1000: настройки, `--set vsm.lruFrames`, слайдер); в `graphics_settings.json` владельца 16 → 1000
      (файл не коммитится).
    - Счётчик свободных теперь = длина этого списка, как UE `FeedbackStatusCS` (размер AVAILABLE), так что загрузка пула
      для динамического разрешения = «запрошено / пул», без страниц, держащихся в кэше.
    - **Ловушка:** наш setup перерисовывает КАЖДУЮ грязную резидентную страницу, запрошенную или нет (прореженный
      запрос `requestDownscale` пропускает страницы, которые пиксели всё же читают). С возрастом 1000 под ветром вид,
      от которого камера отвернулась, продолжал перерисовываться: wind_test, камера в движении — VsmPageRender
      1.85 → 2.43 мс, GPU-кадр 4.9 → 5.45 мс. UE незапрошенную страницу не рисует вовсе (держит флаги инвалидации и
      рисует при следующем запросе). Наш эквивалент по цене: страница, не запрошенная ≥ 16 кадров (старый LRU — окно,
      которое нужно прореживанию), которую прошлый рендер признал грязной, освобождается при следующем выделении
      (`PerPageDirty` прошлого рендера, u5 списка; `vsm::g_redrawUnrequestedFrames`, `--set
      vsm.redrawUnrequestedFrames`, 0 = выкл.). Чистые страницы — без ветра, дальние неподвижные уровни клипмапа —
      живут до 1000.
    - Замер (MCP, редактор закрыт; A → B на 1 с → A).
      - wind_test, камера в движении: VsmPageRender 1000/16 — 1.91, 16/16 — 1.85–1.87, 1000/0 — 2.41–2.44 мс.
      - Проход запроса и выделения 0.124–0.127 мс (было 0.120–0.131) — сортировка не видна.
      - Загрузка пула 0.48 — как была.
      - Уровень 10 + 6 без ветра: пул под давлением (0.84), вид B выселяет страницы A в порядке LRU при любом пороге —
        пик возврата одинаковый, ~0.5 мс.
      - Кадр против свежего пула — в шуме везде.
    - Гейты: три конфигурации, регрессии, `check_logging`; GBV `VsmPageRequest, VsmPageRender` на wind_test с облётом
      камеры (1211 кадров) — ноль сообщений; `--scene-stress=12 --scene-stress-gbv` — CLEAN, 7127 / 0,
      `live objects at exit: none`.
  - Гейты:
    - три конфигурации, регрессии, `check_logging`;
    - GBV `VsmPageRequest, VsmPageRender, PointLights, SpotLights` в Debug-редакторе на 10 + 6 с парковкой, возвратом,
      переносом и порчей — ноль сообщений;
    - `--scene-stress=12 --scene-stress-gbv` — CLEAN, 7127 enhanced / 0 legacy, `live objects at exit: none`. При смене
      уровня лампы старого паркуются и на следующем кадре портятся (перестройка кастеров), так что чужое не переезжает;
      перенос, кроме того, сторожит сравнение матриц.
  - Попутно — утечка `VSM.DummySrvs` (с S10e числилась «плавающей с DLSS»). Это гонка в ленивом
    `Renderer::EnsureVsmDummySrvs`: два прохода на рабочих потоках в кадре 1 оба не находили кучу и оба её создавали;
    второй `CreateDescriptorHeap` писал поверх первого через `GetAddressOf()` без Release. Исправлено флагом и мьютексом
    с двойной проверкой. В Debug-логе — одна строка `VSM dummy SRVs created`, отчёт на выходе — `none`.
- **S10d. Дальние лампы — одна страница** (у нас страница 128 = мип 4 из 2048): одна запись таблицы, свой диапазон
  id, обновление по очереди по одной за кадр.
  — **Сделано (2026-10-21)** — UE `r.Shadow.Virtual.DistantLightMode` 1 / `MaxDistantUpdatePerFrame` 1
  (`ShadowSceneRenderer.cpp:33-46, 210-260, 329-430`; `UpdatePhysicalPages`: `bSkipDynamicInvalidations = IsSinglePage()`).
  - Классификация — `GetConservativeMipLevelLocal`. Пиксель камеры берётся у ближнего к ней края сферы лампы (глубина
    вида − радиус, не меньше 0), глубина в тени — весь радиус: это самый мелкий мип, который может запросить любой
    приёмник этой лампы. Если он последний (4), карта лампы — одна страница на вид, и лампа дальняя. На CPU работает
    та же формула (`vsm::LocalMipFromPixel` = `VsmLocalMipFromPixel`, при `localMipFromPixel 0` — то же по расстоянию).
    Решение принимается раз в кадр в builder'е прохода рендера страниц (`SceneRenderer::PlanVsmDistantLights` →
    `VirtualShadowMap::PlanDistantLights`) по списку видов `BuildVsmViews`: в `cull` теперь всегда сфера лампы, а отсев
    запроса включается флагом `localMip.w`.
  - Страницы дальней лампы не пачкаются ветром и движущимися кастерами, кастеры в них рисуются жёстко (без качания —
    как в дальних уровнях клипмапа, чтобы кэш и свежий рендер совпадали). Перерисовка — по очереди
    (`gDistantMask` / `gDistantUpdate` в CB setup): за кадр `vsm.maxDistantUpdatePerFrame` ламп (UE 1, < 0 = все), та,
    что ждёт дольше всех. Лампа, только что ставшая дальней, перерисовывается сразу и в бюджет не входит (у UE флаг —
    часть ключа кэша). Новая страница, сдвиг матрицы и forceAll действуют, как раньше.
  - Наш попиксельный мип и так давал дальней лампе одну страницу на вид, а таблица страниц у нас фиксированная. Поэтому
    «свой диапазон id» и «одна запись таблицы» ничего не дают, и их нет. Выигрыш — в перерисовке по очереди: под ветром
    каждая локальная страница перерисовывалась каждый кадр. Режима UE 2 («все дальние») нет: он требует заставить запрос
    брать один мип.
  - Ручки: `--set vsm.distantLightMode:0|1`, `vsm.maxDistantUpdatePerFrame`. Readout `vsmPool`: `distantLights`,
    `distantUpdates`, `distantTurned`.
  - Замер (MCP, редактор закрыт, wind_test 21:00, VSM).
    - С 100 м (камера (14, 10, 130), рысканье −135°): 5 дальних ламп (2 прожектора + 3 точечные), обновлений ровно одна
      лампа за кадр. VsmPageRender 0.166 → 0.142 мс, GPU-кадр 3.24 → 3.18 мс, кадр против атласа тот же (0.83 / 0.48 %).
      SpotLights 0.037 → 0.08 мс при той же работе прохода — перекрытие с асинхронной очередью: кадр в целом короче.
    - У фонарей (обычная камера замеров) дальних ламп нет — кадр и цена те же. Порог по UE строгий: точечная лампа
      радиусом 9 м становится дальней, когда её сфера начинается дальше ~60 м (рендер-разрешение DLSS).
  - Гейты: три конфигурации, регрессии, `check_logging`; GBV `VsmPageRequest, VsmPageRender, PointLights, SpotLights`
    в Debug-редакторе на wind_test (дальний вид, ближний, снова дальний, режим 0) — ноль сообщений;
    `--scene-stress=12 --scene-stress-gbv` — CLEAN, 7127 / 0, `live objects at exit: none`.
- **S10e. Разметка страниц по сетке ламп** (тайловые списки) вместо перебора всех видов на пиксель; мип по размеру
  пикселя + `ResolutionLodBiasLocal`; переполнение пула — грубее мип и динамический сдвиг LOD при загрузке 0.85.
  — Сделано всё, кроме сетки ламп (части 1–3 выше): динамическое разрешение, мип по пикселю, отсев сферой и гранью,
  `ResolutionLodBiasLocalMoving`.
- Порядок: S9 раньше (общие векторы видов и строки отсечения S9e переиспользуются); S10a — независим.

### Что осталось и чего оно стоит — замеры после S5 (2026-10-20, wind_test 21:00, редактор закрыт)

- **Линейная глубина спота / near ~1 см (остаток S4).** — Спот сделан 2026-10-21 (S4, «Линейная глубина спота и near
  1 мм»); near точечной ждёт `castShadow: false` у лампочек wind_test. Разбор на тот момент: Акне нет: камера у фонаря (прожектор 1024, смещение UE
  1.5 см против шага D16 ~1–1.2 см на краю пятна) — на открытом песке кадр с картой / без карты 0.98–1.00, 1024 и
  512 одинаковы; мягкий переход S2 (`60/((f−n)·2·bias)`, ~23 см) прячет шаг. Линейная глубина понадобится, когда
  опустим near: кастер ближе 10 см (spot) / 20 см (point) к лампе сейчас отрезан — плафон, рама фонаря. Такого
  контента нет (лампочки wind_test — сферы r 0.12 м ВНУТРИ near). Делать вместе с флагом «Cast Shadow» (иначе
  near 1 см сделает лампочку непрозрачной для своей же лампы). Флаг уже есть — `RenderableObject::SetCastsShadow`,
  но не выведен в уровень/Inspector, и инстанс-батчи решают за весь батч (`InstancedDrawBatch::CastsShadow`).
  Линейная глубина = вариант PSO тени на каждый вид (UE `TShadowDepthPS<PerspectiveCorrect>`): пять мест создания
  PSO теней (RenderableObject, InstancedDrawBatch, GBufferRenderable instcb, ShadowGpuData indirect,
  GpuInstancedModels) — та же инфраструктура, что нужна S6.
- **Бюджет вместо лимита 8/4 (остаток S4) → S9 (атласы) и S10 (VSM).** Лимит — не только атласы: раскладка слотов
  GPU-отсечения (`render::kMaxShadowViews` 46 = 4 каскада + 8 + 24 + 10 клипмапа) и 32 локальных вида VSM
  (`kNumLocalVirtualViews`, `VSM_MAX_VIEWS`, таблицы страниц). На demo лимит стоит одного прожектора и одной точечной
  (поп при вытеснении); на wind_test 2 + 3 — лимит не достигается.
- **S6 / S7 (скорость).** Кадр упирается в GPU: `Renderer::BeginFrame` ждёт 1.4 мс, CPU.Frame = GPU.Frame = 3.46 мс.
  Тени ламп на GPU: Pass_PointShadows 0.16 мс, Pass_SpotShadows 0.017 мс; запись на CPU — 3.3 мс СУММАРНО по
  воркерам (20 списков параллельно). S6 (один проход на куб) снимает в основном CPU-запись — в FPS сейчас не
  видно; S7 (кэш статики) на wind_test мало даст: главные кастеры — пальмы, они качаются от ветра.
- **Контактные тени «поверх карты» (S8).** У владельца `shadows.contact.localLightMode: 0` (только карта) — это и
  есть умолчание UE (`ContactShadowLength` 0 = нет контактных у лампы); «поверх» важно, только если их включить.

## 3. Сцены и камеры для замеров
- **wind_test, лампы на пляже** (добавлены 2026-10-20: 4 точечные, из них 3 с тенью, 2 прожектора с тенью,
  все `onAt: night`): 21:00 (`tod.time:21`), камера `--cam-pos=-62,4,50`, yaw 37.6°, pitch 6° (MCP
  `set_camera {position:[-62,4,50], yawDeg:37.6, pitchDeg:6}`).
- **demo.json**: 9 прожекторов и 5 точечных с тенью (больше лимита 4 — виден поп). Для смещений НЕ годится: часть
  объектов под водой.
- **local_shadow_test.json** (смещения, акне, peter panning): ночь, без океана, экспозиция закреплена `exposure.
  manualEv100:-0.5` (ниже клипа). Камеры `x,y,z,yaw,pitch`: A `5,3.5,5,-135,25`, A низко `3,0.8,2.5,-147.5,18`,
  B `21,4,5,-120,25`, C `-10,2.5,-2,-90,15`, D `5,4,12,-51,30`. wind_test для того же — EV 15, `tod.moonEnabled:0`
  (тени луны в VSM и SDSM разные), камеры `-60.5,3.6,52.5,37.6,28` и `-56,4.5,54,50,20`.

## 4. Гейты
| Шаг | Что гонять |
|---|---|
| S1–S3, S8 | три конфигурации; intent/inspector regression; Debug с отладочным слоем на wind_test ночью; `--gbv --gbv-passes=SpotLights,PointLights` |
| S4–S7 | + `--scene-stress-gbv`, Release `--scene-stress=60`, `--gbv` полный на demo |
| S9 | как S4–S7 + `--scene-stress-gbv --shadow-mode=sdsm` (атласы живые), `--sweep=shadow.localMaxRes` и `shadow.mode` (атласы ↔ VSM), demo: все лампы с тенью, `mem:` |
| S10 | как S4–S7 в режиме VSM (`--scene-stress-gbv` по умолчанию VSM) + wind_test / demo в VSM, загрузка пула |
