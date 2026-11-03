# Игровой поток и поток рендера — план

Написан 2026-11-02 по просьбе владельца («напиши план для внедрения game thread и разделение текущего меин треда на
рендер и гейм»). **Статус (2026-11-03): G0-G9 сделаны** (G6/G7 закрыты в G8b): поток рендера под
`frame.renderThread` — **по умолчанию включён** (решение Q1 после гейтов G9), параллельно его кадру идёт игровая половина
следующего — сообщения, UI, редактор, MCP, `Scene::Tick` (§G8); `--rt-audit` и счётчик задержки ввода (§G9). G10a-b
сделаны: одна дорога к очередям и RHI-поток под `frame.rhiThread` (по умолчанию включён с 2026-11-03, §G10b); G10c
замерен и не нужен; G10d-1 — замер загрузки и кэш `mesh.json` на время загрузки (смена уровня 250 → ~155 мс), дальше —
решение владельца (§G10d). До того: роли потоков и
фазовые проверки, кадр на одном потоке разделён на игровую половину, `Scene::Sync` и рендер; у рендера свои камера,
солнца, ветер, настройки каскадов, копии света и всё, что он читает у объектов (трансформы, видимость, материалы,
анимация рендера, часы океана). По пути найдены и исправлены две
гонки, которые были и без второго потока: фоновая проба шейдеров обходила `MaterialManager::materials_`, пока кадр
вставлял в него (`Material.h:306`, теперь под `materialsMtx_`), и свежая таска (пул пуст) не наследовала контекст
создателя (§G0).

Цитаты сверены с деревом на HEAD `d280738f` (2026-11-02), UE — 5.8.2 в `D:\zalupa\Engine`, пути UE от этого корня.
Перед шагом открыть `file:line`, на который он опирается: дерево уходит из-под планов за день (см. `async_compute_plan.md`).

Имена: **GT** — игровой поток (game thread), **RT** — поток рендера (render thread). Ручка — `frame.renderThread`.

---

## 0. Правила

1. **Сначала UE.** Модель — UE: GT = главный поток процесса (окно, сообщения, тик мира, UI), RT — отдельный поток,
   который рисует кадр N, пока GT тикает N+1, и GT не уходит дальше чем на кадр (`FFrameEndSync`,
   `Source/Runtime/RenderCore/Private/RenderingThread.cpp:2454-2574`; комментарий 2460-2461: «Legacy game code assumes
   the game thread will never get further than 1 frame ahead of the render thread»). Отступление — в §6 с причиной.
2. **Один бинарник, переключается на лету.** `frame.renderThread` 0|1 (Developer Controls > Frame,
   `graphics_settings.json` `frame.renderThread`, `--set`, MCP `set_settings`). 0 = обе половины кадра на одном потоке
   по очереди, тем же кодом, — это UE с `GIsThreadedRendering = false` (`RenderingThread.cpp:610`; `IsInRenderingThread`
   тогда истинно и на GT, `Source/Runtime/Core/Private/HAL/ThreadingBase.cpp:279-281`). Переключение — только на
   границе кадра: RT допивает кадр и останавливается / запускается, как UE `LatchRenderThreadConfiguration`
   (`Source/Runtime/Launch/Private/LaunchEngineLoop.cpp:5606` → `RenderingThread.cpp:762-792`). **По умолчанию 0**, пока
   не пройдены гейты G9 и не решил владелец (§7).
3. **Это возможность, не оптимизация.** На сегодняшних сценах FPS не изменится (§2). Критерий шага — правильность:
   картинка на полу шума, гейты чистые. Время — проверка регрессии, не цель.
4. **Шаги не сливать.** G1-G7 делаются и доказываются в режиме 0, на одном потоке: фазовые проверки (§3.4) ловят
   нарушение владения без второго потока. Поток появляется только в G8. Шаг, который лишь переставляет код,
   доказывает, что он ИНЕРТЕН: тот же кадр, тот же трейс в пределах шума.
5. Правила репо: диагностика — события `LOG_*`, никаких новых `logs/<name>.log`; CRLF в исходниках; три конфигурации
   (Debug — единственная с `WITH_EDITOR` и `/WX`); гейты по типу правки; живые замеры через MCP, 2-4 запуска на шаг;
   **коммит только по просьбе**.

---

## 1. Что есть сейчас

### 1.1 Кадр — один поток

`App::Run` (`sources/app/App.cpp:2332-2752`), по порядку:

1. `Profiler::BeginFrame`, `logging::SetFrameNumber`, `g_renderStats/g_visibilityStats.NextFrame`,
   `WaitForTrackedAsyncTasks` (2338-2342), **`renderer.BeginFrame()`** (2343) — внутри
   `WaitForFrame(currentFrameIndex_)` (`Renderer.cpp:740`): ждём кадр N-3 на GPU (`kFrameCount = 3`,
   `RenderConstants.h:21`), сбрасываем кольца кадра.
2. `input.NewFrame`, `PeekMessage`-насос (2350-2364), `BeginImGuiFrame` (2372: `ImGui_ImplDX12_NewFrame`,
   `ImGui::NewFrame`, `ImGuiLayer.cpp:226-249`), `Profiler::Tick`.
3. Безголовые полёты камеры (2390-2430), `renderer.Tick` (2433: шрифты, наблюдатель шейдеров, `ApplyPendingHotReloads`),
   `TickMemoryReport` (2437).
4. `appController_.Tick` (2440): горячие клавиши, `DeveloperWindow::Draw` (`AppController.cpp:202`),
   `scene.SetRenderSettings` (209), `EditorController::Draw` (214; внутри — обслуживание MCP), камера, запуск сборки HUD
   на воркере (244).
5. `scene.Tick` + `levelManager.Tick` (2444-2445). `Scene::Tick` (`Scene.cpp:1586-1688`): объекты `ParallelFor`
   (1601, 1616), плавучесть (1612), ветер и `vfx::g_wind*` (1650-1658), время суток → `dirLight_`/`skySun_` (1685),
   ночные лампы (1687).
6. Загрузка уровня, если запрошена: `WaitForPreviousFrame` + `UploadBatch` (2448-2475).
7. `WaitForHudBuild` (2477), **`scene.Render`** (2480): примирения при простое GPU (`Scene.cpp:3298-3299`),
   `ShadowGpuData::UpdateForFrame`, `PrepareViews` (3353: `camera_.CalcMatrices`, публикация `frameData_`, отсечение
   на тасках, выбор LOD, окклюзия), `RefreshCasterLods`, выгрузка фрустумов, `SceneRenderer::Render`
   (`SceneRenderer.cpp:993-1022`): граф пассов на воркерах → `WaitForTrackedAsyncTasks` → эпилог (ImGui + текст) →
   `EndFrame` = `ExecuteCommandLists` (`Renderer.cpp:1730`) + `Present` (1781) + `SignalFrame` (1795).
8. Харнесс после Present (`--set`, `--shot`, `--sweep`, MCP-скриншот, `--profdump`, `--trace`; 2487-2696),
   `Profiler::EndFrame`, memtrace/memgate, ограничение кадра (2727-2738).

Вся запись кадра заканчивается внутри `scene.Render`: ни одна таска записи не переживает кадр (джойны
`RenderGraph.h:767-771` и `SceneRenderer.cpp:998-1001`). Кадр переживают только `SubmitDetach`-таски (стриминг
текстур, декод, проба шейдеров, миниатюры).

### 1.2 Цифры (трейсы Release, wind_test, `traces/`)

| трейс | период кадра | `WaitForFrame` (ждём GPU) | CPU кадра (`Whole Cycle`) | `Scene::Tick` | `Scene::Render` | граф (`ExecuteParallel`) | submit + Present | `PrepareViews` | `RefreshCasterLods` |
|---|---|---|---|---|---|---|---|---|---|
| 10-30 18:49:09 | 3.74 мс | 2.09 | 1.56 | 0.098 | 1.40 | 0.59 | 0.42 | 0.13 | 0.11 |
| 10-30 18:48:52 | 3.99 | 2.38 | 1.51 | 0.096 | 1.35 | 0.58 | 0.44 | 0.10 | 0.09 |
| 10-30 15:19:19 | 3.82 | 2.44 | 1.30 | 0.082 | 1.17 | 0.52 | 0.36 | 0.10 | 0.07 |
| 10-28 20:55:36 | 4.16 | 2.65 | 1.40 | 0.089 | 1.25 | 0.53 | 0.37 | 0.12 | 0.08 |

Разобрано по всем 120 кадрам трейса (`python tools/trace_thread_breakdown.py <trace>`, средние на кадр). Воркеры
(24 потока) суммарно заняты ~5.2 мс за кадр. **Кадр упирается в GPU**: главный поток больше половины кадра стоит в
`WaitForFrame`. Игровая часть — `Scene::Tick` 0.08-0.10 мс плюс ImGui и `AppController` — меньше 0.2 мс без
редактора. В редакторе (Debug, трейс 2026-07-14) `EditorController::Draw` был ~0.87 мс из 5.45 мс кадра.

### 1.3 Рендер читает живое игровое состояние

`SceneFrameData` — уже наполовину «пакет кадра»: настройки копируются ПО ЗНАЧЕНИЮ в `PrepareViews`
(`frameData_.settings = renderSettings_`, `Scene.cpp:2101`; туман, небо, облака, экспозиция, цвет 2107-2208). Но
указатели в живые объекты (`SceneFrameData.h:956-1018`): `camera`, все виды (`mainView`, `cascadeViews`,
`clipmapViews`, `spot/pointShadowViews`), `lightManager` (неконстантный), `skybox`, **`objects`** (сам
`vector<unique_ptr>`), `dirLight`, `skyLight`, `cascadeConfig` («тот же объект, что правит окно разработчика»,
:994-995), `shadowGpu`, `localShadowCache`, `vsm`, `sdsm`, окклюзия, **`wind`**, **`ocean`**,
`selectedEditorObjectIds` (указатель в вектор сцены, `Scene.cpp:2209-2215`).

Поля рендераблов, которые читает запись: матрицы модели и прошлая, границы, видимость, слои, `CastsShadow`, параметры
материалов по слотам (`GBufferRenderable.cpp:97-157`; `matParamses_` — обычный вектор, атомарны только версия и ключ,
`.h:366-370`), `emissiveScale_` (атомарный, пишет `TickNightLamps`), сила ветра (`EffectiveWindStrength` читает
глобальный `g_windFadeOriginWS`, `WindState.h:131-139`), LOD-вердикты. Свет: дескрипторы точечных и прожекторов из
живых объектов (`SceneRenderer.cpp:160-270`), солнце (`SceneRenderer_Lighting.cpp:350-351, 1150-1152`), ветер
(`SceneRenderInternal.h:247-258`; ~20 мест в `SceneRenderer_*`), камера (34 чтения `frame_->camera`).

### 1.4 Рендер пишет в то, что пишет игра (точки гонок)

- Камера: `CalcMatrices` пишет `prevViewProj_`, джиттер, `historyRevision_` (`Scene.cpp:2071`, `Camera.cpp:70-140`);
  игра двигает ту же камеру (`Camera::UpdateFromInput`, `AppController.cpp:237`).
- Солнце: `SetAtmosphereTransmittance` / `SetCloudVisibility` (`Scene.cpp:2169-2172`) в тот же `dirLight_`, который
  переписывает `TimeOfDay::Tick` (1685).
- `LightManager`: кэш прожекторов, мобильность, выбор теневых, упаковка атласов (`Scene.cpp:3326-3328, 2371-2388`);
  игра пишет туда же — мерцание `PointLight::Tick` (`PointLight.cpp:73-97`), уровни ламп, правки редактора.
- Таски `PrepareViewQueue`: `SelectLod` (`RenderableObject.cpp:346-379`), `MarkCameraVisible` (`Scene.cpp:1858-1866`),
  окклюзия (1951-1986), ленивые кэши границ за const-геттерами (`RenderableObject.cpp:528-536`).
- Воркеры записи: `drawLodFade_`, `currentDrawSlot_` (`GBufferRenderable.cpp:706-755`); океан — `lengthScales_`,
  след пены (`OceanRenderable.cpp:749-752, 1306-1371`); частицы — `frameCounter_`, `spawnAccum_` (его же пишет
  `ParticleEmitterObject::Tick`, `ParticleEmitterObject.cpp:274-308`).
- Кольцо `OceanReadback`: `BuildPass` (рендер, `OceanReadback.cpp:138-226`) и `Poll` (игра, плавучесть, 262-356) пишут
  одно кольцо.

### 1.5 Игра трогает GPU

- `Renderer::WaitForPreviousFrame` — единая точка полного простоя (`Renderer.cpp:1869-1878`): **59 вызовов**, из них
  35 в редакторе. `UploadBatch::SubmitAndWait` (`UploadBatch.cpp:38-64`: `ExecuteCommandLists` на основную очередь +
  простой) — ещё **37 мест**. Загрузка уровня (`App.cpp:2451-2468`), команды редактора (спавн, удаление, дубликат,
  материалы...), `RefreshShadowGpuForEditor` (7 мест), окружение (`EnvironmentRuntime.cpp:94-120, 467, 510-521`),
  живые сеттеры Developer Controls (DLSS, масштабы разрешения → `RecreateDeferredTargets` → простой).
- Мимо простоя: миниатюры (`AssetThumbnailCache.cpp:1517-1687` — своя запись, `Submit` и `Signal` на основную
  очередь с кадрового потока), превью Mesh Editor (`MeshEditorPreviewScene.cpp:226-323` — пишет и отправляет на
  слот свопчейна `GetCurrentFrameIndex()`), `RequestObjectIdPick` (`Renderer.cpp:899`), `RequestDebugPreview`,
  `CreateImGuiTextureId` (дескриптор в слот `currentFrameIndex_`, `Renderer.cpp:1114-1128`), `Ocean ApplyConfig`.
- MCP: `screenshot` обслуживается после Present (`App.cpp:2515` → `Screenshot.cpp:78-156`: простой + копия +
  `SubmitAndWait`); `get_readouts` читает экспозицию, свет, тени, VSM, Tessy (`App.cpp:2041-2160`); `set_settings` →
  `ApplySweepValue` (`App.cpp:390-1653`, 174 прямых записи глобалов и сцены).

### 1.6 UI: ImGui, HUD, текст

- `ImGui::NewFrame` — в начале кадра (2372), виджеты — в `AppController::Tick`, а **`ImGui::Render()` и запись
  draw data — внутри эпилога рендера** (`ImGuiLayer.cpp:251-267` ← `Renderer::RenderImGui` ← `Pass_Overlay`,
  `SceneRenderer_Post.cpp:668-697`). Draw data используется живьём, без копии.
- ID текстур ImGui — дескриптор в слоте текущего кадра и список ресурсов для переходов, сбрасываемый в
  `BeginFrame` (`Renderer.cpp:818, 1074-1095, 1114-1128`).
- HUD: `BuildHud` на воркере пишет в `TextManager` параллельно `scene.Tick` (`AppController.cpp:247-320`), джойн —
  `App.cpp:2477`; затем `overlayPrepTask` (`SceneRenderer.cpp:938-952`) дописывает оверлей профайлера в тот же
  `TextManager` и строит буфер из кольца кадра (`TextManager.cpp:435-487`).

### 1.7 Ручки

**267** `inline g_*` в заголовках, атомарных — **4** (`g_cleanCapture`, `g_pipelineStatsRequested`,
`g_gbvSelfTestDraws`, `g_suppressImGuiDraw`). Больше всего: `ShadowSettings.h` (62), `VirtualShadowMap.h` (47),
`TessySettings.h` (23), `Renderer.h` (22), `LodSelect.h` (20), `StreamingSettings.h` (19). Пишут их на кадровом
потоке: `ApplySweepValue` (`--set`, `--sweep`, MCP), `GraphicsSettings.cpp` (90), `DeveloperWindow.cpp` (26 записей
+ 67 привязок ImGui прямо к `&render::g_*`), горячие клавиши, Inspector. Читают воркеры записи. Рендер сам пишет
часть: `g_lodOutputWidthPx` (`Scene.cpp:2063`), `g_preExposure` (`SceneRenderer.cpp:409`), `g_dynamicLodBias`
(`VirtualShadowMap.cpp:405, 1592`), `g_fogDistantSkyLight` (`SceneRenderer_Graph.cpp:257`), статистика видимости.
Смена ручки посреди записи кадра (тени Legacy → VSM, одни пассы видят старое значение, другие новое) — не
«безвредная гонка», а расхождение ресурсов.

### 1.8 Инфраструктура

- `TaskSystem` (LockFree, `TaskSystem.h:3-4`): один пул, `0.75 x hardware_concurrency` (`App.cpp:2288`).
  **`WaitForTrackedAsyncTasks` джойнит ОДИН глобальный список** (`TaskSystemLockFree.cpp:764-786`): вызов с GT
  ждал бы `DispatchTrack`-таски RT. Таска наследует тег памяти создателя (`TaskSystemLockFree.h:63-65`) — тот же
  механизм понесёт роль потока (§3.4).
- Профайлер: `CPU_SCOPE` держит эпоху кадра; сэмпл, чья эпоха кончилась на `EndFrame`, выбрасывается
  (`Profiler.h:178-203`, `Profiler.cpp:494-499`); `frameOpen_`, `frameNo_` — неатомарные.
- Лог: номер кадра — атомарный глобальный `g_frame` (`Log.cpp:56, 133`). `LOG_*_ONCE`/`EVERY_N` — атомарны.
- `MemoryReport` — «только главный поток» (`MemoryReport.h:50-93`); `g_visibilityStats` — обычные целые со снимком
  «на главном потоке после джойна» (`VisibilityStats.h:12-15`); `thread_local` состояние записи (`g_clBindState`,
  `CommandListBindState.h:10-49`; `RenderContextPool::tls_`).
- Проверок потока нет вообще: ни одного `IsMainThread`/assert по id потока. Контракты «main thread only» — в
  комментариях (`ShadowGpuData.h:394`, `VirtualShadowMap.h:800, 851-855, 889`, `LocalShadowCache.h:15, 83`,
  `OcclusionHistory.h:10`, `TextureStreamingManager.h:26`).
- Незащищённое создание объектов D3D12: `MaterialManager::materials_` (исправлено, см. шапку), ленивые сигнатуры
  команд (`Renderer.cpp:2436-2515`, check-then-create; исправлено в G0), счётчики `FrameScheduler`
  (`FrameScheduler.h:92`). `InputLayoutManager` гонки не имеет: раскладки регистрирует только конструктор, дальше —
  чтение. Защищены: `BindlessHeap`, `SamplerManager`, перепись GPU, кэш `Texture2D`.
- Уже есть свои потоки: LogWriter, писатель трейса профайлера, писатель memtrace, сокет MCP («EditorMcp»,
  `EditorMcpServer.cpp:1301`), потоки локальной модели (`LlmIntentSource.cpp:484-485`), воркеры пула.

### 1.9 Обратные чтения (readbacks)

| что | производит | потребляет |
|---|---|---|
| высоты океана | рендер, `Main_OceanReadback` | игра: плавучесть, `Scene::Tick` (`OceanBuoyancy.cpp:335-361`) |
| экспозиция, пиксель- и солнце-проба | рендер (`ExposureMetering.cpp:268-312`, кольцо без фенса) | рендер + игра (Inspector, MCP `get_readouts`) |
| разбиения SDSM, статистика VSM/ShadowGpu/Tessy | рендер (`Scene.cpp:3337-3342`) | рендер + игра (Developer Controls, MCP) |
| пик объекта | игра просит, рендер пишет и читает (`Renderer.cpp:929-1022`) | игра, следующий кадр (`ViewportGizmo.cpp:628`) |

---

## 2. Что это даст и чего не даст

- **На wind_test FPS не изменится**: кадр упирается в GPU (§1.2). Изменится критический путь CPU: сейчас он —
  сумма игры и рендера (~1.3-1.5 мс), станет максимумом из двух (~1.2-1.4 мс у RT) плюс синхронизация.
- **Выигрыш появится там, где кадр упирается в CPU**: редактор (панели ImGui — сотни микросекунд в Release и
  ~0.9 мс в Debug, всё это сейчас стоит перед записью кадра), будущая игровая логика (геймплей, физика, ИИ — растут на
  GT и не отъедают у записи), лёгкие сцены без ограничения FPS.
- **Цена**: +1 кадр задержки от ввода до картинки в режиме 1 (GT на N+1, пока RT рисует N; при 270 fps это ~3.7 мс),
  копии состояния на синхронизации, сложность — ещё один способ сломаться. Возможная компенсация задержки —
  `kFrameCount` 3 → 2 (§7, Q4).
- **Не изменится**: простои GPU из редактора и загрузки уровня по-прежнему останавливают кадр (они паркуют RT, §3.7);
  асинхронная загрузка уровней — отдельный план.

---

## 3. Целевая модель

### 3.1 Потоки

- **GT — нынешний главный поток процесса.** Окно создано на нём (`App::InitWindow`), Win32 требует качать сообщения
  на потоке-создателе; UE качает на GT (`LaunchEngineLoop.cpp:5784` → `WindowsPlatformApplicationMisc.cpp:218-242`).
  На GT: сообщения и ввод, ImGui от `NewFrame` до `Render()`, `AppController` (HUD-строки, Developer Controls,
  редактор, обслуживание MCP), `Scene::Tick`, `LevelManager`, харнесс (решения «когда»), ограничение кадра.
- **RT — новый `std::thread` «RenderThread».** На RT: `Renderer::BeginFrame` (ожидание GPU), `PrepareViews`,
  `ShadowGpuData`, граф, эпилог (рисование снимка ImGui и текста), submit, Present, обратные чтения, скриншоты,
  пик, превью и миниатюры (их отправка в очередь).
- Воркеры — общий пул; таски наследуют роль создателя.
- Отдельного RHI-потока (submit) нет: submit + Present — 0.36-0.44 мс; это G10, вне плана.

### 3.2 Кадр

```
GT:  [сообщения, ввод][ImGui-виджеты, редактор, MCP][Scene::Tick N+1][ImGui::Render]──ждёт RT «N отпустил сцену»──[Sync N+1]──► пакет N+1 ─┐
RT:  [BeginFrame: ждёт GPU N-2][PrepareViews N][граф N][джойн]──«сцена N отпущена»──[эпилог: ImGui N, текст N][submit, Present N] ◄────────┘
```

GT отдаёт пакет N+1 и сразу тикает N+2. Перед синхронизацией N+2 он ждёт, пока RT отпустит сцену кадра N+1 — так GT
никогда не уходит дальше чем на кадр (UE `FFrameEndSync::Sync`, `RenderingThread.cpp:2506-2510`: фенс на кадр,
ожидание до одного висящего). «Сцена отпущена» — точка после джойна записи (`SceneRenderer.cpp:1001`), а не после
Present: синхронизация N+2 идёт, пока RT отправляет и показывает N+1 (эпилог читает только пакет). Первая версия может
ждать конец кадра целиком, как UE, — проще; ранняя точка — отдельный подшаг G8 с замером.

### 3.3 Три вида состояния — главный инвариант

**Между синхронизациями у каждого изменяемого поля один пишущий поток, и другой поток его не читает.**

| вид | кто пишет | кто читает | примеры |
|---|---|---|---|
| **игровое** | GT, когда угодно | только GT | `pos_`/`rotEuler_`/`scale_`/`renderOffset_`, поза камеры, часы суток, `WindState`, дескрипторы света, параметры материалов «как правит редактор» |
| **рендерное** | RT (и его таски), когда угодно | только RT; GT — опубликованную копию | LOD-вердикты, окклюзия, штампы видимости, `drawLodFade_`, джиттер и история камеры, `ShadowGpuData`, VSM, SDSM, обратные чтения |
| **замороженное** | только в Sync | оба, когда угодно | матрицы модели и прошлая, мировые границы, видимость/слои/`castShadow`, рендер-копии света и солнца, пакет кадра |

`Sync` — единственное место, где данные переходят между потоками, и в нём никто больше не работает: RT отпустил
сцену, GT стоит в Sync. Это UE `SendAllEndOfFrameUpdates` (`LevelTick.cpp:1120` → `ActorComponent.cpp:2645-2677`:
грязные трансформы уходят в рендер одной пачкой) с рендерной стороной `FScene::Update`
(`Source/Runtime/Renderer/Private/RendererScene.cpp:5534-5636`). Разница: UE шлёт команды в очередь RT и держит
отдельный прокси (`FPrimitiveSceneProxy`); у нас RT стоит, и GT пишет рендер-копии прямо в объект. Прокси-класс на
каждый рендерабл не заводим (§6, D1).

### 3.4 Роли потоков и фазовые проверки

- `thread_local` роль: `Game`, `Render`, `Sync`, `None` (воркер без таски). GT ставит `Game`, RT — `Render`; в Sync
  GT временно `Sync`. Таска наследует роль создателя, как сейчас тег памяти (`TaskSystemLockFree.h:63-65`).
- **В режиме 0 роль ставит ФАЗА**: `GameFrame` идёт под `Game`, `RenderFrame` под `Render`. Поэтому нарушение
  инварианта ловится на одном потоке, детерминированно, без гонки — каждый шаг G2-G7 проверяется до появления RT.
- Проверки (Debug, `WITH_EDITOR` не нужен): `CHECK_GAME()`, `CHECK_RENDER()`, `CHECK_SYNC_OR_PARKED()` — в сеттерах
  игровых полей, в писателях рендерных, в `Scene::Add*/Remove*`, в `Renderer::BeginFrame/EndFrame`, `UploadBatch`,
  в мутирующих сеттерах `Renderer`. Отказ — `LOG_FATAL` с ролью, фазой, кадром и `file:line`, затем assert (Fatal
  сбрасывается синхронно, запись переживает диалог). UE: `check(IsInGameThread())` / `check(IsInRenderingThread())`
  (`Source/Runtime/Core/Public/CoreGlobals.h:703-745`).

### 3.5 Пакет кадра

`RenderFramePacket` — кольцо из двух; GT заполняет один, RT читает другой. Внутри:

- то, что `PrepareViews` и так копирует по значению (настройки, туман, небо, облака, экспозиция, цвет, GTAO, bloom),
  + `CascadeShadowConfig`, ночные `skyMoon`/`nightSky`;
- поза камеры и запрос «срез истории» (`Camera::ResetHistory`), `dt`, номер игрового кадра;
- рендер-солнце и небесное солнце, ветер (`WindState` + то, что сейчас в `vfx::g_wind*`), выделение редактора
  (копия вектора);
- изменения ручек кадра (§3.8);
- снимок ImGui (§G6), строки HUD;
- запросы: пик, превью текстуры, скриншот, превью Mesh Editor, задания миниатюр, смена DLSS, ресайз;
- структурные команды сцены (добавить / убрать объект или свет, §3.6).

### 3.6 Состав сцены и удаление

- Объект живёт в `Scene::objects_` (GT). Добавление и удаление — команды, применяемые в Sync, где RT стоит: RT
  выбрасывает объект из своих очередей (`SceneView` хранит указатели между кадрами — `Scene.h:267-270`,
  `BumpStaticSetVersion`).
- Сам объект удаляется не раньше, чем GPU отпустит его ресурсы: сегодняшний путь (простой GPU в команде редактора)
  остаётся через парковку RT (§3.7); для игровых спавнов без простоя — корзина «удалить через `kFrameCount + 1`
  кадров», как `FPendingCleanupObjects` (`RenderCore/Public/RenderDeferredCleanup.h:10-38`; удаление на кадр позже
  синхронизации, `LaunchEngineLoop.cpp:6079-6095`).

### 3.7 GPU с игрового потока — парковка RT

UE `FlushRenderingCommands` (`RenderingThread.cpp:1274-1316`): остановить запись, допить RHI, дождаться всех
потоков. У нас: **`Renderer::WaitForPreviousFrame()`, вызванный с GT, сначала паркует RT** — RT дописывает и
показывает текущий кадр, отвечает «стою», GT ждёт GPU как раньше и делает своё (`UploadBatch`,
`AddInitializedEditorObject`, `RemoveEditorObject`, `RecreateDeferredTargets`, загрузку уровня). RT стоит до
следующей передачи пакета — Sync тогда идёт сразу. Одна точка закрывает все 59 + 37 мест: они уже начинают с
`WaitForPreviousFrame`. Сеттеры `Renderer`, которые НЕ начинают с простоя, становятся запросами пакета (§G7).

### 3.8 Ручки

UE: `ECVF_RenderThreadSafe` держит две копии (`Core/Public/HAL/IConsoleManager.h:92-98`, `TConsoleVariableData::
ShadowedValue[2]`, 1898-1957) и обновляет копию RT командой рендера (`RenderingThread.cpp:493-552`). У нас:
**`render::g_*` остаются копией RT** — сотни читателей не меняются. Писатели на GT идут через
`knob::Set(&render::g_x, v)`: запись в очередь, применение в Sync по порядку. Привязки ImGui — через локальную копию
(`float v = render::g_x; if (SliderFloat(..., &v)) knob::Set(&render::g_x, v);`). `tools/check_render_knobs.py`
(как `check_logging.py`) находит прямую запись `g_*` вне разрешённых файлов (разбор командной строки до старта RT,
рендерные писатели §1.7). Глобалы, которые пишет рендер, GT читает из опубликованного снимка.

### 3.9 Окно и ожидания GT

DXGI может синхронно слать сообщения окну из `Present`/`ResizeBuffers` (наш же комментарий: WM_SIZE приходит
«реентерабельно внутри DXGI», `Renderer.cpp:698-701`). Если GT ждёт RT, не качая сообщения, а RT в `ResizeBuffers`
ждёт ответа окна — взаимная блокировка. Поэтому **каждое ожидание GT на RT — `MsgWaitForMultipleObjectsEx` с
прокачкой сообщений** (UE в ожидании `FFrameEndSync` обрабатывает задачи GT, `RenderingThread.cpp:2562-2566`).
`WM_SIZE` пишет ожидаемый размер в пакет (сейчас — поля `Renderer`, `Renderer.cpp:1880-1886`), ресайз — на RT в
`BeginFrame`, как сейчас.

---

## 4. Шаги

Каждый шаг: собрать три конфигурации, проверки шага, ничего не коммитить без просьбы.

### G0. Инструменты и база (поведение не меняется)

- Роли потоков и проверки §3.4 (пустые в Release), наследование роли тасками, номер кадра в логе и профайлере —
  `thread_local`, наследуется тасками (у записи лога — кадр своего потока).
- `WaitForTrackedAsyncTasks` — список на роль (или на поток-владельца), а не один на процесс; лишний джойн в начале
  цикла (`App.cpp:2342`) убрать — `SceneRenderer.cpp:1000` джойнит то же раньше.
- Сигнатуры команд (`Renderer.cpp:2436-2515`) и `InputLayoutManager` — под мьютекс или создание при старте.
- База: `--trace=120` на трёх видах wind_test (роща, ванная, туннель) и трейс редактора (Debug и Release_Editor,
  панели открыты), разбор `tools/trace_thread_breakdown.py` (с G8 — `--thread` для GT и RT). Записать цифры сюда.
- **Готово**: сборки, `--scene-stress=12` чистый, картинка трёх видов на полу шума против HEAD (`--wind-freeze`,
  `--time-freeze`).

**Сделано (2026-11-02).**
- `sources/core/threading/ThreadRole.{h,cpp}`: роли `None/Game/Render/Sync` (`thread_local`), `RoleScope`, флаг «рендер
  припаркован» (§3.7: ставит `Renderer::WaitForPreviousFrame`, вызванный из роли Game; снимает `Scene::Sync`).
  Проверки `TC_CHECK_GAME / RENDER / SYNC / GAME_PARKED_OR_SYNC` — только Debug: нарушение пишет Error один раз на место
  («thread role: <что> ran as <роль>, needs <роли> (file:line)») и считается; в конце `App::Run` и стресса строка
  `thread roles: clean` / `... N violations at M sites`. Процесс не останавливают — гейт читает вердикт.
- Таска наследует роль создателя, как тег памяти (`TaskWithDeps::role_`, `RunTask` ставит `RoleScope`). **Найдено по
  пути:** таска, созданная заново (пул пуст — первые кадры, всплески), не вызывает `Prepare`, и не получала ни роль, ни
  тег памяти создателя — с M3 свежие таски считали память в «untagged». Теперь оба захватывает и конструктор
  (`TaskSystemLockFree.cpp`). Без этого tracked-таска без роли не попадала в список своего кадра и дописывала
  командный список после отправки: `RENDERER INVARIANT FAILURE: SubmitTimeline::RegisterDirect: batch index outside
  the active range` (`0xC0000409`, один запуск из четырёх). Проверка в `TrackFrameTask` (роль не `None`) ловит это
  детерминированно: без захвата роли — одно нарушение в кадре 0; доказано прогоном.
- `WaitForTrackedAsyncTasks` — список на роль (`trackedFrameTasks_[4]`), ждёт список вызывающего; джойн в начале
  цикла (`App.cpp`) убран — рендер джойнит свои до отправки (`SceneRenderer.cpp:1000`).
- Ленивые сигнатуры команд — под `fixedCmdSigMtx_`, уровень mesh shader — атомарный (`Renderer.cpp`).
- Номер кадра в логе и профайлере «на поток» — перенесено в G8: пока поток один, он один на всех.

### G1. Две половины кадра на одном потоке

- `App::Run` → `GameFrame()` (п. 2-6 и харнесс-решения §1.1) + `Sync()` + `RenderFrame()` (`BeginFrame` в начале
  цикла, как сейчас, — режим 0 обязан сохранить нынешний порядок ожидания GPU; остальное — `scene.Render`, эпилог,
  Present, исполнение запросов харнесса после Present).
- `Scene::Render` делится на `Scene::Sync()` (GT-сторона: копии из `PrepareViews` по значению, `SyncSceneState` из
  `PostTick`) и `Scene::RenderFrame()` (RT-сторона).
- `ImGui::Render()` переезжает в конец `GameFrame` (draw data пока живая — один поток). До переезда проверить, что
  после `AppController::Tick` никто не зовёт ImGui (`grep "ImGui::" sources/rendering sources/app/scene`).
- `SceneStress` (`SceneStress.cpp:904-1035`) — свой цикл, переводится на те же половины: это главный гейт.
- **Готово**: картинка на полу шума, трейс по скоупам в пределах шума (среднее, два прогона — пол), scene-stress
  чистый, фазовые проверки §3.4 включены и молчат (на G1 они проверяют только `Scene::Add*/Remove*` и
  `Renderer::BeginFrame/EndFrame`).

**Сделано (2026-11-02).** Порядок кадра в режиме 0 НЕ менялся; фазы получили роли:
- `App::Run`: загрузка и выход — `Sync`; цикл — `Game`; `BeginFrame` + сбор GPU-таймингов, `renderer.Tick` +
  `TickMemoryReport` (пока на своём месте, в G8 уедут в начало кадра RT), `scene.Render`, смена DLSS после кадра,
  MCP-скриншот и `--shot` — `Render` (`RoleScope`); `scene.Sync()` — `Sync`. Харнесс после Present решает на `Game`,
  исполняет на `Render`. Стресс (`SceneStress.cpp RenderFrames_`) — так же; его драйвер (операции «игры») — `Game`;
  нарушения ролей входят в вердикт (`FAULT op=thread-roles`).
- `Scene::Sync()`: из `PrepareViews` вынесены копии игровых значений — настройки и туман/небо/облака с оттенками
  суток, луна и ночное небо, пропускание атмосферы и облачность солнца, экспозиция с ночной камерой, цвет, выделение
  редактора (`Scene.cpp`). `PrepareViews` публикует только указатели рендера. `Scene::Render` без `Sync` в этом кадре
  делает его сам и пишет Error один раз.
- `ImGui::Render()` — в конце игровой половины (`Renderer::EndImGuiFrame` → `ImGuiLayer::EndFrame`); эпилог только
  записывает draw data. Кадр, который никто не закрыл, закрывает `ImGuiLayer::Render`, как раньше.
- Проверки: `Scene::Add*/Remove*/Clear` — `GAME_PARKED_OR_SYNC`, `Scene::Tick` — `GAME`, `Scene::Render`,
  `Renderer::BeginFrame/EndFrame` — `RENDER`, `Scene::Sync` — `SYNC`. Одно нарушение нашлось и исправлено:
  `scene.replicate` (`--set`) добавлял объекты без парковки — теперь сначала `WaitForPreviousFrame`.
- `SyncSceneState` остаётся в `PostTick` — переезжает в Sync с проверками полей в G4a.
- Проверено: три конфигурации без предупреждений; Debug `--scene-stress=12` — CLEAN, `thread roles: clean` (все
  операции: спавн/удаление редактора, перезагрузка и смена уровня, ресайз, DLSS, масштабы), Debug-кадр wind_test —
  `thread roles: clean`; Release `--scene-stress=12` — CLEAN; картинка wind_test (ветер и часы заморожены, без HUD,
  Release) до/после — доля пикселей > 8 уровней: вид A 1.6-2.2 % при поле этого вида 1.2-2.3 % (два запуска одного
  бинаря), вид B (cam -11.6 4.17 73.6) 0.04 %, вид C (ванная, cam -22.19 1.53 86.70) 0.05 %; редактор
  (`--editor --shot`, Release_Editor) рисует все панели. `Scene::Sync` — 0.001 мс на кадр. Время — A/B против HEAD
  (`4812effa`, собран во временном worktree, запуск из основной папки), чередуя дважды, трейс 120 кадров, wind_test,
  ветер и часы заморожены, мс на кадр база / G1: период кадра 5.10, 5.10 / 4.97, 4.87; `Whole Cycle` 2.56, 2.39 /
  2.22, 2.19; `Scene::Render` 1.98, 2.13 / 2.00, 1.99; `Scene::Tick` 0.17, 0.18 / 0.16, 0.14; граф 0.86, 0.96 /
  0.90, 0.90; submit + Present 0.60, 0.61 / 0.59, 0.58 — в пределах разброса прогонов одного бинаря.

### G2. Сцена делится на игровую и рендерную

- `SceneRenderState` (RT-владение): `sceneRenderer_`, `frameData_`, все `SceneView`, `shadowGpu_`,
  `localShadowCache_`, `vsm_`, `sdsm_`, `sdsmCache_`, окклюзия, `hzbTester_`, рендер-камера, рендер-солнца,
  рендер-`LightManager` (G3). `Scene` оставляет игровое: `objects_`, позу камеры, настройки уровня, `timeOfDay_`,
  `windState_`, `nightLamps_`, `buoyancy_`, дескрипторы света. Это `UWorld` против `FScene`. Механический перенос —
  граница видна в коде и грепается.
- **Камера**: `CameraPose` (GT: позиция, ориентация, FOV, маска слоёв, запрос среза) и `RenderCamera` (RT:
  `CalcMatrices`, джиттер, `prevViewProj_`, `historyRevision_`, `UpdateDlssCameraData`). Гизмо и пикинг на GT берут
  нерджиттеренные матрицы, посчитанные из позы на GT (`ViewportGizmo.cpp:655-663`).
- **Солнце**: `TimeOfDay::Tick` пишет игровые `dirLight_`/`skySun_`; Sync копирует их в рендер-солнца; пропускание
  атмосферы и облачность (`Scene.cpp:2169-2172`) пишет RT в свою копию; GT читает их (если читает) из опубликованного.
- **Ветер**: `WindState` и `vfx::g_windFadeOriginWS` / `g_windDriftXZ` / заморозка-шаг — в пакет;
  `WindDistanceFade` и частицы читают пакет.
- Указатели `SceneFrameData` (§1.3) по одному: в пакет по значению (`cascadeConfig`, выделение, ветер, солнца) или в
  `SceneRenderState` (виды, тени, окклюзия). `objects` остаётся указателем — поля объектов делит G4.
- **Готово**: фазовые проверки на камере, солнце, ветре молчат на scene-stress и на скрипте редактора через MCP
  (§5); картинка на полу шума.

**Сделано (2026-11-02).** Без отдельного `SceneRenderState`: рендер-копии лежат в `Scene` рядом с игровыми, с
комментарием «пишет только Sync и рендер, игра не читает»; перенос в одну структуру — механический, отложен до G8
(тогда он отделит то, что уходит на RT).
- **Камера**: игровая `camera_` (`CameraRef`, проверка GAME) и рендерная `renderCamera_` (`RenderCamera`, проверка
  RENDER). `Camera::TakePose` в Sync копирует позицию, углы, объектив, слои; срез (игровой `ResetHistory`, который все
  четыре места — `set_camera`, обрамление, закладки, MCP — делают сразу после своего `CalcMatrices`) переносится
  ревизией: рендер-камера повторяет его после СВОЕГО `CalcMatrices`, до чтения ревизии и `UpdateDlssCameraData`.
  Игровая камера считает свои матрицы в Sync (`CalcMatrices`, тот же джиттер кадра) — гизмо, пикинг и HUD читают их
  следующий кадр, как раньше читали матрицы `PrepareViews`. Все рендерные функции `Scene` (78 мест), океан
  (`GetDepthParams`), MCP-скриншот («камера этого кадра») — на рендер-камере.
- **Солнце**: пропускание атмосферы и облачность по-прежнему пишутся в игровой `dirLight_` в Sync (их
  `GetEffectiveColor` читает превью Mesh Editor), затем `renderSun_ = dirLight_`, `renderSkySun_ = skySun_`;
  `frameData_.dirLight / skyLight` указывают на копии. Каскады, клипмап, SDSM и константы океана читают копии;
  блик на воде от фазы луны — поле кадра `sunGlintScale`.
- **Ветер**: `renderWind_` (кадр, SDSM-константы); `vfx::g_windFadeOriginWS` и `g_windDriftXZ` пишет Sync, а не
  `Tick` (тик частиц и раньше видел значение прошлого кадра).
- **Остальные указатели кадра**: `cascadeConfig` — копия `renderCascadeConfig_` (Developer Controls / `--set` правят
  игровой), выделение редактора — копия `renderSelection_` (рамка и LOD-вид). `objects`, `lightManager`, `ocean`,
  `skybox` остаются указателями: их делят G3/G4, состав меняется только с припаркованным рендером.
- Проверки на аксессорах `Scene`: `CameraRef`, `GetDirectionalLight`, `DirectionalLightRef`, `SunLightRef`,
  `GetWindState`, `CascadeConfig` — GAME; `RenderCamera`, `RenderSun`, `FrameData` — RENDER.
- Найдено для G4: видимость объекта чистит рендерные очереди из игры (G4b').
- Проверено: три конфигурации; Debug-кадр wind_test, Debug `--scene-stress=12` — `thread roles: clean`, CLEAN;
  Debug-редактор через MCP (`set_camera` со срезом и без, чистый скриншот и серия, `get_readouts`,
  `set_settings` / `reset_settings`, `setTime`, спавн и отмена) — ни одного нарушения ролей и ни одной ошибки в логе,
  иконки ламп (их проецирует игровая камера) на своих местах; картинка против G1 (собран во временном worktree),
  Release, ветер и часы заморожены: вид A 0.51-0.96 % пикселей > 8 уровней при поле 0.10-1.21 %, B 0.06 %, C 0.04 %;
  `Scene::Sync` 0.001-0.002 мс.

### G3. Свет

- Игровые дескрипторы (`PointLightDesc`/`SpotLightDesc`): их пишут уровни ламп (`Scene.cpp:1745-1756`), редактор
  (`setLight`), окружение. Рендер-`LightManager` получает копии в Sync по версии (грязный — копируем) и дальше
  делает своё: кэш прожекторов, мобильность, выбор теневых, атласы, GPU-буферы.
- Мерцание (`PointLight::Tick`) — чисто визуальное: на RT, `dt` из пакета (правило §G4).
- Структурные изменения (свет добавлен / удалён, `RebuildLights` с ростом буферов, `EnvironmentRuntime.cpp:510-521`)
  — под парковкой, как сейчас.
- **Готово**: проверки на писателях `LightManager` молчат; ночь wind_test с «Lamps» (23:00) и свечи ванной — на полу
  шума (мерцание выключить: `flicker` 0, иначе два снимка одного плеча различаются).

**Сделано (2026-11-02).** Отступление: мерцание осталось на игре (`PointLight::Tick`) — его результат и так переезжает
копией, отдельный тик на RT не нужен.
- `PointLight`: игровые `baseDesc_` / `desc_` (эффективный: уровень лампы и мерцание) / версия; рендер-копия
  `renderDesc_` + версия (`SyncToRender`, `GetRenderDesc`); отрисовка (`BuildModel`, `RenderColor`) — по копии.
- `SpotLight`: сеттеры пишут только `gameDesc_` и игровой уровень лампы; `SyncToRender` переносит изменение в рендерный
  `desc_` побайтно (`memcpy`, чтобы `memcmp` не видел разницы в выравнивании и версия — ключ теневых кэшей — двигалась
  только от настоящей правки) и пересчитывает кэш (конус, матрицы, версия), который раньше считался прямо в сеттерах.
- `LightManager::SyncToRender` — из `Scene::Sync` и в конце загрузки уровня. Рендерные читатели (`ConsiderLightOcclusion`,
  `PrepareViews`, `EnsureFrameResources`, `SelectShadowed*`, `UpdateLightMobility`) — на `GetRenderDesc`.
- Проверки: `SetDesc`, `Tick`, `SetLampLevel` — GAME; `GetRenderDesc` — RENDER; `LightManager::SyncToRender` —
  GAME_PARKED_OR_SYNC. Игра рендерные данные света не читает (проверено грепом: ни конусов, ни `EffectiveFluxLm`).

### G4. Рендераблы (самый большой шаг; подшаги a-e коммитятся отдельно)

Правило для всех: **если результат тика не читает никто в игре — это анимация рендера, и тик переезжает на RT** с
`dt` из пакета.

- **G4a. Трансформы.** `SyncSceneState` (`RenderableObject.cpp:544-576`: матрица, прошлая, `worldBoundsCache_`,
  `movedThisFrame_`) уходит из `PostTick` в Sync — это и есть `SendRenderTransform`. Матрицы и границы становятся
  замороженными. GT-читатели (`RaycastEditorObject`, гизмо, `GetAuthoredModelMatrix`) читают их свободно: в
  перекрытии их никто не пишет. Ленивые кэши за const-геттерами (`RenderableObject.cpp:528-536`) — только в Sync.
- **G4b. Рендерные вердикты.** `cameraLod_`, `cameraLodFade_`, `chunkLods_`, `chunkVisCamera_`, `cameraVisibleFrame_`,
  `cameraOccluded_`, `drawLodFade_`, `currentDrawSlot_`, маски чанков — RT. GT-читатели (отладка LOD, Developer
  Controls, MCP) — через опубликованное.
- **G4b'. Видимость и набор кастеров** (найдено в G2): `Scene::NotifyEditorShadowCasterVisibilityChanged` и
  `SetOceanVisible` зовут `BumpStaticSetVersion` из игры БЕЗ парковки — а он чистит очереди всех рендерных видов
  (`Scene.cpp BumpStaticSetVersion`) и меняет `staticSetVersion_`, который читает `PrepareViews`. Стек команд
  редактора сравнивает версию сразу после команды (`EditorCommandStack.cpp RefreshShadowsIfCasterSetChanged`) и тогда
  паркует. Сделать: игра поднимает флаг «набор изменился» (и свою версию для стека команд), очереди чистит и
  рендер-версию двигает Sync.
- **G4c. Материалы и излучение.** Параметры слотов: игровая копия (её правит редактор, `MaterialParamsRef`,
  `SetObject*`) и рендер-копия, копируется в Sync по уже существующей атомарной версии (`GBufferRenderable.h:367-370`).
  То же `emissiveScale_`, сила ветра, `castShadow`, видимость, слои.
- **G4d. Анимация рендера на RT.** `GpuInstancedModels::Tick` (`deltaTime_`, `instanceRotations_`,
  `GpuInstancedModels.cpp:304-344`), `ParticleEmitterObject::Tick` (`ParticleEmitterObject.cpp:274-299` — тогда
  `spawnAccum_` пишет один поток), `OceanRenderable::Tick` кроме часов (`UpdateClipLevels`, окна surf-sim и мокрого
  песка, `OceanRenderable.cpp:709-736, 1245-1304`). Часы океана — игровые: из них идёт ветер (`Scene.cpp:1649-1651`),
  заморожены в Sync.
- **G4e. Плавучесть.** RT по готовому слоту `OceanReadback` публикует снимок высот (кадр, срез, сами высоты); GT
  `OceanBuoyancy::Tick` сэмплирует снимок вместо `Poll` по общему кольцу. Задержка та же (высоты и сейчас на
  несколько кадров старше).
- **Готово (каждый подшаг)**: проверки на полях подшага молчат на scene-stress и скрипте редактора; картинка на полу;
  `--wind-freeze=0` и `=1.5` дают РАЗНЫЕ кадры (анимация не потерялась — то, на чём проваливается замёрзшая тень).

**Сделано (2026-11-02), все подшаги одним коммитом.**
- **a.** `SyncSceneState(Frame)` — из `PostTick` в `Scene::Sync` (`ParallelFor`, скоуп `Scene::Sync.Objects`); он
  единственный пишет матрицы, прошлую матрицу, границы, флаг движения (проверка GAME_PARKED_OR_SYNC). Входы трансформа
  (`MarkTransformDirty`: позиция, поворот, масштаб, смещение плавучести) — GAME. Позиция для рендера — `renderPos_`
  (затухание ветра, источник частиц, уровень воды: `GetRenderPosition`, `GetRenderWaterLevel`).
- **b.** Вердикты (LOD, окклюзия, штампы) игра не читает (греп) — без изменений.
- **b'.** Видимость и флаг тени — две копии: игровая (`SetVisible`/`IsVisibleGame`, `SetCastsShadow`/`CastsShadowGame`:
  команды редактора, плавучесть, `ocean.visible`) и рендерная (`IsVisible`, `CastsShadow`), которая берёт игровую в
  Sync (`SyncSwitchesToRender`) или сразу, если рендер припаркован. `BumpStaticSetVersion` двигает игровую версию
  (стек команд), очереди и рендер-версию (`ApplyStaticSetToRender`) — сразу при парковке, иначе в Sync;
  `RefreshShadowGpuForEditor` сначала паркует, переносит переключатели и только потом пересобирает кастеры.
- **c.** Параметры материалов: рендер-копия `renderMatParamses_` + масштаб излучения, берутся в Sync, если писатель
  (`MaterialParamsRef`, `SetWindStrength`, шесть `SetObject*`, `ObjectMaterialEdited`, загрузка, `SetEmissiveScale`)
  поднял `paramsDirty_`; ключ совместимости инстансов пересчитывается от копии. До первой синхронизации (загрузка,
  рендер припаркован) рендер читает игровые. RT берёт альбедо из копии (`RenderMaterialParams`).
- **d.** `Tick` частиц и GPU-инстансов только копит `dt`; границы, накопитель спавна, лог, повороты — в их
  `SyncSceneState`. Океан: часы — игровые (ветер, плавучесть), `renderElapsedTime_` и всё, что следует за зрителем
  (окна сёрф-сима и мокрого песка, клип-уровни), — в `SyncSceneState`; 8 рендерных чтений часов внутри океана и два
  снаружи — на копии. Объекты синхронизируются в Sync ДО записи глобалов ветра: охват частиц читает дрейф прошлого кадра,
  как читал его `Tick`.
- **e.** Плавучесть: `Request` / `Poll` кольца обратного чтения — в `OceanBuoyancy::SyncReadback` (Sync); `Tick`
  сэмплирует принятую поверхность. Высоты на кадр старше (под замороженным ветром — те же).
- Проверено (G3 и G4 вместе): три конфигурации; Debug-кадры wind_test в 15:00 и 23:00, Debug `--scene-stress=12` —
  `thread roles: clean`, CLEAN; Debug-редактор через MCP (прошлый прогон + скрыть лодки `setEnabled` и отмена, выключить
  их тень `setCastShadow` и отмена, `ocean.visible` 0/1) — ни нарушений ролей, ни ошибок, на снимке лодки скрыты;
  картинка против G2 (временный worktree), Release, ветер и часы заморожены: A 0.19-0.28 % пикселей > 8 уровней при поле
  0.25-0.35 %, B 0.14 %, C 0.05 %, ночь в ванной (лампы, свечи) 0.04 %. Цена: `Scene::Sync` 0.05 мс (объекты 0.042 —
  бывший `PostTick`), `Scene::Tick` на 0.013-0.016 мс легче. В режиме 1 это время RT стоит — следить в G8.

### G5. Ручки

- `knob::Set` + очередь + применение в Sync (§3.8); перевести писателей: `ApplySweepValue`, `GraphicsSettings`,
  `DeveloperWindow` (записи и 67 привязок), горячие клавиши `AppController`, Inspector, Ocean Controls.
- Глобалы, которые пишет рендер, — в опубликованный снимок для GT.
- `tools/check_render_knobs.py`, код выхода = находки; вписать в AGENTS.md рядом с `check_logging.py`.
- **Готово**: скрипт 0; `--sweep` по десятку ручек из разных файлов даёт те же кадры, что до шага; MCP `set_settings`
  двигает картинку (доказать на одной ручке с метрикой и полом).

**Сделано (2026-11-02).** Отступление от §3.8: не очередь записей, а ДВОЙНИК у каждой ручки — очередь меняла бы смысл
«записал и тут же прочитал» в игровом коде (`ApplySweepValue`, живые сеттеры), а двойник его сохраняет.
- `core/threading/Knob.h`: `knob::Register(g_x_game, g_x, "ns::g_x")` и `knob::Exchange()` — трёхстороннее слияние с
  последним переданным значением: изменила игра — в рендер, изменил рендер (одноразовые запросы, которые он гасит:
  `g_csmDumpReadout`, `g_dumpBarriers`, `g_surfSimPokeRequest`...; откат `g_shadowMode` в Legacy) — обратно в игру, оба —
  побеждает игра. `KNOB(type, name, init)` — для новых ручек.
- 228 ручек, которые пишут игровые файлы (`App.cpp` — `--set`/`--sweep`/MCP, `GraphicsSettings.cpp`,
  `DeveloperWindow.cpp`, `AppController.cpp`, `main.cpp`, `InspectorPanel.cpp`, `OceanControlsWindow.cpp`), получили
  двойника в своих 22 заголовках; игровые файлы читают и пишут двойника (895 ссылок), код рендера не тронут. Сделано
  скриптом (набор ручек — из игровых файлов в HEAD, определения — с учётом пространств имён).
- `Exchange`: в начале `Scene::Sync`; при парковке рендера из игры (`Renderer::WaitForPreviousFrame` — живой сеттер,
  который паркует для пересборки целей, видит только что заданное значение); на загрузке после командной строки и после
  `graphics_settings.json`; в харнессе стресса.
- `tools/check_render_knobs.py` (код возврата = находки): игровой файл, тронувший рендерную копию ручки с двойником.
  Доказано: подброшенное чтение `render::g_shadowMode` в `MemoryTab.cpp` — одна находка.
- Не сделано (по плану — G6/G8): рендерные глобалы, которые читает игра (`g_preExposure`, статистика видимости...), —
  опубликованным снимком; пока игра читает рендерную копию.
- Проверено: три конфигурации; Debug-кадр с `--set=shadow.mode:2;lod.pixelError:8;tessy.lod:1;contact.enabled:0` —
  SDSM с кадра 1, `tessy.lod` в камере и атласах, `thread roles: clean`; Debug `--scene-stress=12` — CLEAN; картинка с
  тем же `--set` (+ `vsm.smrtRayCount:2`) против G4 (временный worktree), Release: A 1.0-1.97 % пикселей > 8 уровней
  при поле 1.9 %, B 0.04 %.

### G6. UI, HUD, MCP

- **ImGui**: после `ImGui::Render()` на GT — снимок `ImDrawData` (`ImDrawList::CloneOutput()` на каждый список) в
  пакет; RT рисует снимок. `ImGui_ImplDX12_NewFrame` создаёт объекты устройства лениво — создать их при старте, на GT
  остаётся проверка. Сверить версию бэкенда: если она обновляет текстуры шрифтов в `RenderDrawData`, это уже RT.
- **ID текстур**: на GT — виртуальные (индекс запроса в пакете: ресурс + SRV-описание); RT в начале эпилога пишет
  дескрипторы в слоты своего кадра и подменяет ID в командах снимка. Как прокси ресурсов Slate.
- **HUD**: `BuildHud` пишет строки в пакет, `TextManager` и `overlayPrepTask` — только RT.
- **MCP**: `get_readouts`/`get_profile`/`get_memory` — из снимка, который RT публикует в конце кадра под мьютексом;
  `screenshot` — `ServiceScreenshot` переезжает на RT после Present (уже ждёт на условной переменной);
  `set_settings` — через `knob::Set` и команды сцены. Задания очереди MCP по-прежнему исполняет GT в `Draw` редактора.
- Slate в UE делает то же: рисует на GT, отдаёт буфер элементов на RT командой
  (`SlateRHIRenderer.cpp:1404-1512, 1749-1752`), буфер возвращается командой (736-739).
- **Готово**: панели редактора и Developer Controls на снимке экрана — пиксель в пиксель против режима до шага
  (`--editor --shot`); MCP-скриншот и `get_readouts` работают; ImGui-клики через живой редактор (владелец).

**Сделано (2026-11-02), частично.**
- ImGui: `ImGuiLayer::EndFrame` (игра) закрывает кадр и клонирует списки отрисовки (`CloneOutput`) в снимок; эпилог
  рендера записывает снимок, а не живые списки ImGui. Цена с открытыми панелями редактора (Release_Editor, wind_test):
  `ImGui::EndFrame` 0.018 мс на кадр.
- HUD: задача форматирует строки в игровой `hudLines_`; `AppController::PublishHud` в Sync открывает текстовый буфер и
  пишет их (`TextManager` пишут только синхронизация и рендер — оверлей профайлера, LOD-вид).
- MCP: `get_readouts` (экспозиция, тени ламп, VSM, Tessy — состояние рендера) исполняется в Sync
  (`editormcp::ServiceHandOver` из App) — задание ставится в очередь передачи вместо исполнения в `Draw` редактора;
  `screenshot` и так на рендере после Present (G1); `set_settings` пишет двойники ручек (G5).
- Осталось для G8 (с одним потоком безвредно, с RT — гонка):
  - ID текстур ImGui (`CreateImGuiTextureId`) игра пишет в слот дескриптора по `currentFrameIndex_` рендера — нужны
    виртуальные ID, которые RT превращает в дескрипторы своего кадра;
  - обновление текстур атласа ImGui 1.92 (`ImDrawData::Textures`) бэкенд делает при записи — с RT перенести на игру;
  - задача HUD читает у рендера FPS, режим DLSS, масштаб — нужен опубликованный снимок;
  - `DebugDrawSystem` (сброс в `BeginFrame`): проверить, кто из игры в него пишет.
- Проверено: три конфигурации; Debug-редактор через MCP (все вызовы прошлых шагов + `get_readouts` дважды) — ни нарушений
  ролей, ни ошибок; Debug `--scene-stress=12` — CLEAN; на снимках HUD (FPS, `Cam:`, `Time:`) и все панели редактора.

### G7. GPU с игрового потока

- Парковка RT (§3.7) за `Renderer::WaitForPreviousFrame` на GT; `UploadBatch::Begin/SubmitAndWait` на GT —
  `CHECK_SYNC_OR_PARKED()`.
- **В очередь отправляет только RT** (кроме `UploadBatch` под парковкой): задания миниатюр
  (`AssetThumbnailCache.cpp:1517-1687`) и превью Mesh Editor (`MeshEditorPreviewScene.cpp:226-323`) — запросы пакета,
  RT записывает и отправляет их до кадра. Streamline проксирует очередь (`Renderer.h:103-107`), и полагаться на
  свободнопоточность `ID3D12CommandQueue` через прокси не будем.
- Запросами пакета становятся: `RequestObjectIdPick` (результат — опубликованный, на кадр позже, чем сейчас),
  `RequestDebugPreview`, `OnResize`, смена DLSS из горячих клавиш, `Ocean ApplyConfig`.
- Загрузка уровня — под парковкой целиком (`scene.Clear` рушит объекты, на которые смотрят очереди RT).
- **Готово**: scene-stress (смена уровней, ресайз, спавн/удаление редактора) чистый; скрипт редактора через MCP
  (§5) чистый; превью Mesh Editor и миниатюры на снимке.

**Сделано (2026-11-02), частично.**
- `Renderer::ExecuteOrDefer` / `SignalOrDefer` / `FlushHandOverSubmits`: работа для GPU, которую игра записывает вне
  кадра, уходит в очередь при синхронизации (Sync) в порядке отправки; из рендера, в Sync или при припаркованном рендере —
  сразу (сначала то, что уже ждёт). `UploadBatch::Submit` идёт через это (миниатюры, превью Mesh Editor, все загрузки);
  сигнал фенса миниатюр — через `SignalOrDefer`. Владелец пакета держит список и аллокатор до конца GPU-работы, как и
  раньше (миниатюра — до фенса, превью — до повторного слота кадра), операция дополнительно держит ссылку на список.
- `Renderer::WaitForPreviousFrame` сначала сбрасывает отложенное (простой GPU покрывает и его), из игры — паркует (G1)
  и обменивает ручки (G5). `UploadBatch::SubmitAndWait` поэтому верен из любой роли без правки вызывающих.
- Осталось для G8: настоящая приостановка RT за парковкой (с одним потоком «припаркован» = флаг); запросы пакета —
  `RequestObjectIdPick`, `RequestDebugPreview`, `OnResize`, DLSS с горячих клавиш, `Ocean ApplyConfig`.
- Проверено: три конфигурации; Debug-редактор через MCP — миниатюры трёх ассетов перепеклись с нуля (кэш удалён, PNG
  записаны заново), ни нарушений ролей, ни ошибок; Debug `--edit-mesh` — превью Mesh Editor рисуется, `thread roles:
  clean`; Debug `--scene-stress=12` — CLEAN.

### G8. Поток рендера

- `RenderThread`: старт, остановка, имя потока (лог и профайлер), исключение на RT (удаление устройства бросает) —
  поймать, передать GT, остановиться чисто.
- Синхронизация §3.2 (ожидание с прокачкой сообщений §3.9), кольцо пакетов из двух, переключение
  `frame.renderThread` на границе кадра.
- Профайлер: эпохи кадра — по потоку (GT и RT открывают и закрывают свои), маркеры `Frame N` у обоих в трейсе;
  `CollectGpuResults` — на RT.
- Куда что: `renderer.Tick` и `TickMemoryReport` — в начало кадра RT (в G1 стоят на месте под ролью Render),
  memtrace/memgate `OnFrameEnd` и ограничение кадра — GT, `g_renderStats/g_visibilityStats.NextFrame` — RT.
- Номер кадра в логе — `thread_local`, наследуется тасками (как роль); потоки вне кадра (MCP, модель, писатели) берут
  кадр GT. Перенесено сюда из G0: пока поток один, кадр один на всех.
- Приоритет: пул общий, а `ParallelFor` тика может занять воркеры, нужные графу RT. Сначала измерить; если RT
  голодает — полоса высокого приоритета для тасок роли `Render` (UE: именованные потоки и приоритеты задач).
- Ранняя точка «сцена отпущена» (§3.2) — подшаг с замером.
- **Готово**: §5 целиком.

**Сделано (2026-11-03), первая версия.**
- `threading::RenderThread` (`core/threading/RenderThread.{h,cpp}`): поток `Render` (лог) / `RenderThread` (профайлер),
  роль Render. `Kick` отдаёт ему кадр, `WaitIdle` ждёт этот кадр с прокачкой сообщений (`MsgWaitForMultipleObjectsEx`,
  §3.9; WM_QUIT, встреченный в ожидании, снова ставится в очередь цикла) и перебрасывает на GT исключение кадра —
  дальше как на одном потоке. Ручка `frame.renderThread`: Developer Controls > Frame «Render thread»,
  `graphics_settings.json` frame.renderThread, `--set` / MCP; по умолчанию 0; цикл берёт её в начале кадра.
- Порядок режима 1 (`App::Run`): [сообщения, часы, `Scene::Tick`, `LevelManager::Tick`] перекрываются с `Scene::Render`
  прошлого кадра на RT → ожидание RT → работа после Present (харнесс, скриншоты MCP и `--shot`, конец кадра
  профайлера, ограничение кадра) → начало кадра (`BeginFrame`, результаты GPU) → UI, редактор, MCP, загрузка уровня →
  Sync → Kick. Режим 0 — прежний кадр: те же куски (лямбды в `App::Run`) в прежнем порядке.
- **Отступление от §3.2**: UI и редактор пока НЕ в перекрытии. Всё, что трогает рендер вне `Scene::Render`, идёт на GT,
  пока RT стоит, — поэтому остатки G6/G7 в этом порядке не нужны: ID текстур ImGui пишутся после `BeginFrame` своего
  кадра, атлас ImGui меняется только в UI, а RT рисует снимок, пока GT симулирует; задача HUD, пик, превью, DLSS с
  клавиш — тоже при стоящем RT, ресайз из `WM_SIZE` только запоминается и применяется в `BeginFrame` на GT. По смыслу
  режим 1 отличается одним: `Scene::Tick` идёт до UI своего кадра, так что правка редактора доходит до производного
  состояния (солнце от часов, лампы) кадром позже; то, что копирует Sync, — в тот же кадр.
- Парковка (§3.7): `Renderer::WaitForPreviousFrame` из игровой роли зовёт `threading::ParkRender()` — хук
  RenderThread ждёт кадр в полёте; без потока хука нет. После парковки RT стоит до следующего Kick.
- Найдено по пути: `WindState::Tick` и часы океана (`OceanRenderable::Tick`) из `Scene::Tick` читали и писали
  рендерные копии ручек заморозки ветра (`g_windFreeze` / `g_windFrozenTime` / `g_windStep`) — теперь игровые
  двойники; в перекрытии это была бы гонка с рендером.
- Стресс-харнесс: `RenderFrames_` при `frame.renderThread:1` делит кадр так же; последний кадр дожидается до возврата,
  так что «взбалтывание» между вызовами встречает стоящий RT.
- Профайлер и лог: кадр профайлера закрывается после ожидания RT и открывается перед UI — оба на GT, пока RT стоит,
  поэтому эпохи по потокам не понадобились; симуляция следующего кадра попадает в текущий кадр профайлера. Новые
  скоупы `Game: Simulate`, `Game: Wait Render Thread`, `RenderThread Frame`. Номер кадра в логе — общий, ставится в
  начале кадра.
- Цифры (Release, wind_test, виды B и N, `--profdump`): CPU-кадр тот же (4.22 / 4.26 и 3.96 / 3.98 мс — упор в GPU);
  `Scene::Render` на RT 1.58 / 1.48 мс (на одном потоке 1.53 / 1.45), GT ждёт его 1.13 мс, симуляция в перекрытии
  0.07 мс. Выигрыша на wind_test нет и не ожидалось (§2): этот шаг — каркас.
- Проверено: три конфигурации; картинка режима 1 против 0 — B 0.07 %, C 0.02 %, N 0.02 % пикселей > 8 уровней,
  вид A в пределах пола (0.4 %); Debug `--scene-stress=12` и Release `--scene-stress=40` в режиме 1 — CLEAN, `thread
  roles: clean`, ошибок 0; Debug-редактор в режиме 1 через MCP (весь сценарий прошлых шагов: камера с резом и без,
  скриншоты, `get_readouts`, `set_settings`, `setTime`, спавн и отмена, видимость и тени лодок, океан) — ни ошибок, ни
  нарушений ролей; Release_Editor в режиме 1 — три миниатюры перепеклись с нуля пиксель в пиксель с прежними;
  `check_logging` и `check_render_knobs` — 0.
- Осталось для G8b: UI и редактор в перекрытие — для этого виртуальные ID текстур ImGui, обновление атласа ImGui на
  GT, опубликованный снимок рендера для задачи HUD и Developer Controls, запросы пакета (пик, превью, DLSS с клавиш,
  `Ocean ApplyConfig`) и разбор остальных вызовов рендера из UI (~50 разных, §1.6); `renderer.Tick` /
  `TickMemoryReport` на RT; ранняя точка «сцена отпущена»; приоритет тасок RT (сначала замерить). Затем G9.

**Сделано (2026-11-03), G8b: UI и редактор параллельно рендеру.**
- Порядок режима 1 теперь §3.2: [сообщения, часы, кадр ImGui, UI, редактор, MCP, `Scene::Tick`, загрузка уровня,
  `ImGui::Render`] идут параллельно `Scene::Render` прошлого кадра → ожидание RT → работа после Present → начало кадра
  (`BeginFrame`, `renderer.Tick`, строка памяти) и Sync при стоящем RT → Kick. `BeginFrame` и `renderer.Tick` остались на
  GT: ожидание GPU в `BeginFrame` при упоре в GPU и так ничего не отнимает, а всё, что они пишут (размеры, FPS, кадр),
  UI читает без гонки. Флаг `renderFramePending`: кадр, который UI успел припарковать, всё равно проходит работу после
  Present.
- ImGui (`ImGuiLayer`): ID текстур из игровой половины ВИРТУАЛЬНЫЕ (запрос «ресурс + вид», номер под битом 62, один на
  ресурс и вид за кадр). `Renderer::HandOverImGui` в Sync (после `BeginFrame`) пишет дескрипторы в слот кадра, ставит
  настоящие ID в списки снимка, объявляет текстуры из `MarkImGuiTextureShaderReadable`, грузит изменённые текстуры атласа
  1.92 (`ImGui_ImplDX12_UpdateTexture` на GT при свободной очереди) и отдаёт снимок рендеру (`renderSnapshot_`). RT рисует
  только его: эпилог больше не закрывает «незакрытый» кадр ImGui — в перекрытии это был бы открытый кадр GT. Показанные
  текстуры живут до записи (`imguiKeepAlive_`); снятый дескриптор (`ReleaseImGuiTextureDescriptors`) вырезает свои
  команды кадра.
- Ленивая парковка `threading::WaitRenderForGame(site)`: вызов из игровой половины, который дотягивается до того, что
  читает кадр в полёте, сначала ждёт этот кадр — корректно по построению, ценой перекрытия этого кадра. Стоит на
  сеттерах `Renderer` (wireframe, DLSS, масштабы рендера и отражений, пауза джиттера), на выдаче подсистем, которые
  меняет редактор (`GetMeshManager`, `GetMaterialDataManager`, `GetTextureStreaming`, `GetTextManager`, `Exposure`,
  `GetDeferredForFrame`, `GetCanonicalState`), на `RequestObjectIdPick`, `RequestDebugPreview`,
  `ReleaseImGuiTextureDescriptors`; на входах открытых Developer Controls и окна океана, `EnvironmentRuntime::Apply /
  ApplyChange / Remove` и сервиса MCP, когда есть задания. Превью Mesh Editor при занятом RT простаивает GPU перед
  перерисовкой: его слот по `GetCurrentFrameIndex` в перекрытии — слот кадра в полёте. Задача игровой половины ждать не
  может — Error один раз. Раз в 5 с в режиме 1: `render thread: the game half waited for its frame N times in 5 s:
  <site> xK, ...`.
- Без ожиданий: пик (`ConsumeObjectIdPick` каждый кадр — результат и флаг атомарные), debug-линии гизмо
  (`DebugDrawSystem` и так под мьютексом, рендер рисует из своего scratch), геттеры, которые пишет только GT (FPS,
  размеры, режим DLSS — их сеттеры паркуют).
- Известное незакрытое: `DlssHandler::HandleAllocationFailure` на RT пишет размер рендера, который читает UI (путь
  отказа); вызов рендера мимо перечисленного поймает только G9 (`--rt-audit`); экспозиция в Inspector пока ждёт кадр
  (нужен опубликованный снимок показаний).
- Цифры (Release_Editor, wind_test, вид B, панели редактора открыты, трейс 120 кадров, `trace_thread_breakdown.py`):
  режим 0 — `Whole Cycle` 1.632 мс (рендер 1.208, UI 0.197); режим 1 — игровая половина 0.340 мс (UI 0.200,
  `Scene::Tick` 0.077) параллельно кадру RT (~1.25 мс), ожидание 0.917, при стоящем RT 0.087 (Sync 0.063): критический
  путь CPU ~1.34 мс против 1.63. Период тот же (3.72 мс — упор в GPU). Без редактора: игровая половина 0.115 мс,
  ожидание 1.077. (Средние `--profdump` для режима 1 врут: у `Scene::Render` больше, чем у охватывающего его кадра RT —
  мерить трейсом; `trace_thread_breakdown.py` теперь считает кадры и для потока без своих маркеров.)
- Проверено: три конфигурации; картинка режима 0 до и после G8b и режима 1 против 0 — в пределах пола (B, C, N ≤ 0.14 %,
  A ≤ 0.83 % при поле 0.4-0.83 %); Debug-редактор в режиме 1 со всем сценарием MCP — ни ошибок, ни нарушений ролей,
  ожидания только на вызовах MCP; Developer Controls (Frame, VSM) открытыми в режиме 1 — ждут каждый кадр, ошибок нет;
  Debug `--scene-stress=12` и Release `--scene-stress=40` в режиме 1 — CLEAN, `thread roles: clean`; Release_Editor в
  режиме 1 со сценарием MCP — ошибок нет; узкий GBV (`Overlay,Skybox` + самопроверка Skybox, редактор, режим 1) — id=935
  пойман, других ошибок нет, `live objects at exit: none`; `check_logging`, `check_render_knobs` — 0.
- Найдено по пути (было и в режиме 0): под `--editor --gbv` отчёт устройства на выходе называл живыми иконки Content
  Browser и гизмо и миниатюру — их владельцы (панели редактора) переживают устройство, а `AssetThumbnailCache::ReleaseAll`
  не звал никто. Теперь `AppController::ReleaseGpuResources` (Developer Controls, редактор: миниатюры, атласы иконок,
  `Texture2D::Release`) перед `Renderer::Shutdown`.
- Осталось: G9 (`--rt-audit`, задержка ввода, решение о дефолте); снимок показаний экспозиции для Inspector; ранняя
  точка «сцена отпущена»; приоритет тасок RT.

**Сделано (2026-11-03), ручка `frame.oneFrameThreadLag` (§7, Q2).** UE `r.OneFrameThreadLag`: 1 (по умолчанию, как в UE) —
игровая половина идёт на кадр вперёд рендера; 0 — после передачи кадра GT сразу ждёт его (`App::Run` и стресс-харнесс):
ни перекрытия, ни кадра задержки. Developer Controls > Frame «One frame thread lag» (активна при «Render thread»),
`graphics_settings.json` frame.oneFrameThreadLag, `--set frame.oneFrameThreadLag:0`. Release, wind_test, вид B, режим 1:
период 3.70 мс при 1 против 3.94 мс при 0 (упор в GPU: без перекрытия в кадре появляются пустоты), ожидание рендера на GT
1.08 / 1.41 мс; картинка в пределах пола (B 0.46 %, C 0.06 %, N 0.05 %). Debug: вид A и `--scene-stress=12` при 0 —
CLEAN, `thread roles: clean`.

**Трейс** (2026-11-03, владелец): дорожки идут GPU Graphics, GPU Compute, MainThread, RenderThread, дальше рабочие потоки
в порядке первой записи — `tid` в этом порядке (на него опирается любой просмотрщик) и `thread_sort_index` у каждой
дорожки (`Profiler::WriteTraceJson`). GPU-дорожки назывались «GPU Queue» / «GPU Queue (async compute)».
`Whole Cycle` в обоих режимах — кадр главного потока от начала (после ожидания GPU в `BeginFrame`) до момента, когда
кадр отрисован (владелец: в режиме 1 он оборачивал одну передачу и потерял смысл). На одном потоке это один блок; с
потоком рендера он держится через оборот цикла (`std::optional<Profiler::ScopedCpu>`): передача («Game: Hand-over»),
игровая половина следующего кадра («Game: Simulate»), ожидание рендера. Вид B: 1.38 мс в режиме 0, 1.53 в режиме 1
(передача 0.08 + игровая половина 0.14 + ожидание 1.31), 1.56 при `oneFrameThreadLag` 0. Период при упоре в GPU гуляет
от запуска к запуску: вперемежку режим 0 — 4.18 / 3.91 мс, режим 1 — 3.64 / 3.82.

### G9. Гейты и решение о дефолте

§5 в режиме 1; трейс GT/RT (перекрытие видно на дорожках), задержка ввода (счётчик §5), цифры в этот файл.
Решает владелец (§7, Q1).

**Хвосты G8b (2026-11-03).**
- Экспозиция в Inspector и Mesh Editor: `Renderer::ExposureForGame()` — показания (`LatestReadback`, готовность) и
  гистограмма, скопированные в `HandOverImGui`; UI больше не ждёт кадр, чтобы их показать. Кнопка «Reset adaptation»
  по-прежнему ждёт (`Exposure()`, клик).
- `DlssHandler::HandleAllocationFailure` на RT — мёртвый путь: в `AllocateResourcesIfNeeded` само выделение закомментировано
  (`res = eOk`), а единственный вызов — из сеттера, который паркует. Записи с RT нет.
- Голодание RT в общем пуле: не видно. Вид B, 240 кадров вперемежку: кадр RT 1.39-1.41 мс в режиме 1 против
  `Scene::Render` 1.31-1.60 мс в режиме 0 на соседних запусках. Полоса высокого приоритета не нужна.
- Ранняя точка «сцена отпущена»: нечего выигрывать. После ожидания игровому потоку остаётся только то, что требует
  Present (`BeginFrame`, очередь: `FlushHandOverSubmits`, атлас ImGui), а передача — 0.08 мс; вся остальная игровая
  работа уже идёт параллельно кадру RT.

**Сделано (2026-11-03), G9.**
- **`--rt-audit[=<с>]`** (`app/diagnostics/RtAudit.{h,cpp}`, 600 с по умолчанию; поток рендера включается сам):
  случайные паузы 0-2 мс (спин) в начале игровой половины, перед `closeUi` и вокруг `Scene::Render` на RT; RT в начале
  и в конце своего кадра хеширует то, что заморозила передача (`Scene::HashFrozenState`: у каждого объекта матрица модели,
  прошлая, позиция, видимость, тень; рендер-дескрипторы точечных и прожекторов). Сдвинутый хеш — запись игровой
  половины во время кадра; вердикт `rt audit: PASS|FAIL <поле> xN`, код выхода = число полей.
  **`--rt-audit-selftest`** пишет матрицу объекта #0 из игровой половины в каждом двадцатом кадре, пока RT рисует
  (`RenderBusyForGame`; запись при пустом RT — не гонка) — должен быть FAIL с именем поля (тогда код выхода 0; PASS =
  аудит слеп, код 1). Проверено: самопроверка 30 с — `FAIL -- object #0 model matrix x57` за 3593 кадра (первая версия:
  раз в 60 кадров); чистый прогон 60 с — PASS, 11643 кадра, 1455 объектов; 10 минут (wind_test, Release) — PASS,
  118264 кадра, ошибок 0.
  **Покрытие важнее длительности** (владелец спросил, зачем 10 минут): 600 с на одном статичном виде добавляют редкие
  окна времени, но не пути кода. Поэтому аудит едет и на стрессе — `--scene-stress=N --rt-audit` (смена уровней, спавны,
  ресайзы; вердикт в конце харнесса, код выхода харнесса), и в редакторе со сценарием MCP (`--editor --rt-audit=<с>`).
  Debug `--scene-stress=12 --rt-audit` — PASS; с самопроверкой — `FAIL -- object #0 (editor id 1) model matrix x1,
  previous model matrix x1` (ожидаемо); в первой версии самопроверка в стрессе писала раз в 60 кадров и попала туда,
  где RT ничего не рисовал (42 кадра, начало вызова `RenderFrames_`) — отсюда `RenderBusyForGame`.
- **Задержка ввода**: игровой поток считает прочитанный ввод, передача ставит на кадр этот номер, кадр после Present
  сравнивает его с текущим — строка `frame: input to present N frames (max M)` раз в 5 с с потоком рендера и одна на
  выходе. wind_test: режим 0 — 0.00 (max 0), режим 1 — 1.00 (max 1), `oneFrameThreadLag 0` — 0.00. Под паузами аудита
  0.95-0.99 (кадр иногда успевает до чтения ввода).
- **Смена уровня через MCP**: инструмент `open_level {path, discardChanges}` (дорога File > Open,
  `EditorController::McpLevelHost::Open`): отказ при несохранённых правках без `discardChanges`, отказ без файла;
  загрузка на следующем кадре. Шлюз `intent_regression` (`TestMcpDrivesTheSameRoad`): в списке, требует path, без
  level host отказ — PASS. Вживую в режиме 1 (Release_Editor и Debug-редактор): wind_test → demo → wind_test, камера
  отвечает из каждого уровня, ни ошибок, ни нарушений ролей.
- **`--scene-stress=150`** в режиме 1 (Release): CLEAN, `mem tags: STABLE` (3 уровня, 23 шага).
- **Решение Q1: режим 1 по умолчанию** (`threading::g_renderThreadEnabled` и снимок настроек — true); `0` в Developer
  Controls / `--set frame.renderThread:0` — один поток. Гейты, трогающие порядок кадра, гоняются в обоих режимах.

### G10. RHI-поток, параллельный тик, асинхронная загрузка

Владелец: делать (Q5, 2026-11-03), после G9. `kFrameCount` 2 — нет (Q4: не снижать). Расписан 2026-11-03.

**Что трогает очередь GPU сегодня** (инвентарь по `ExecuteCommandLists` / `Signal` / `Wait` / `Present`):
- отправка кадра `Renderer::ExecuteTimelineAndPresent` (RT, 0.36-0.44 мс, §1.2): сбор списков по сегментам, два
  оборачивающих списка (начало кадра профайлера; переход backbuffer в PRESENT, статистика списков, конец кадра
  профайлера), ожидания и сигналы между очередями (`FrameScheduler::WaitCrossQueue` / `SignalCrossQueue`), пустая работа
  compute (`--async-empty-submit`), `Present`, `SignalFrame` (кадровые заборы обеих очередей), индекс следующего кадра —
  `swapchain.CurrentBackBufferIndex()` ПОСЛЕ Present;
- отложенная работа игры: `Renderer::FlushHandOverSubmits` (G7) — в Sync и при парковке;
- сигнал профайлера: `Profiler::EndFrame` сигналит `gpuDrainFence` на обе очереди — с GT, после кадра;
- загрузка атласа ImGui: `ImGui_ImplDX12_UpdateTexture` — своя отправка и ожидание (в `HandOverImGui`, G8b);
- простой GPU: `FrameScheduler::WaitForGpuIdle` (сигнал + ожидание обеих очередей) — `WaitForPreviousFrame`;
- диагностика при старте (`RtSmoke`, `HzbCullSelfTest`) — до цикла кадра.

**G10a. Одна воронка операций очереди (поведение не меняется).** Все перечисленное, кроме старта, идёт через
`Renderer`: FIFO операций очереди (выполнить списки, сигнал, ожидание, Present, отправка кадра), сейчас исполняется
сразу на вызывающем потоке. `ExecuteTimelineAndPresent` делится на запись (RT: сбор, оборачивающие списки) и пакет
отправки кадра (сегменты, ожидания и сигналы, Present, `SignalFrame`); индекс следующего кадра предсказывается
(FLIP_DISCARD из `kFrameCount` буферов идёт по кругу) и сверяется после Present (расхождение — Error). Сигнал профайлера
— операцией той же воронки (порядок в очереди: кадр N, его сигнал, работа игры кадра N+1, кадр N+1 — как сейчас).
Готово: картинка и трейс режимов 0 и 1 без изменений, стресс, аудит.

**Сделано (2026-11-03), G10a.** `rendering/core/RhiQueue.h` — дорога к очередям: `Run(op)` (сейчас — сразу, по порядку),
`Flush()`, `Threaded()`. `ExecuteTimelineAndPresent` записывает кадр и отдаёт пакет `FrameSubmit` (индекс кадра,
предсказанный следующий, номер кадра, vsync) в `Renderer::SubmitFrame_` — сегменты, ожидания и сигналы между очередями,
Present, `SignalFrame`; предсказание сверяется после Present (`frame submit: the swapchain's next backbuffer is ...`,
Error один раз; не встретилось). Через `rhi_` идут `ExecuteOrDefer` / `SignalOrDefer` / `FlushHandOverSubmits` и сигнал
профайлера (`Profiler::SetQueueSignalHook`, ставит App, снимается перед `Renderer::Shutdown`); `WaitForPreviousFrame`
опустошает `rhi_` перед простоем, `HandOverImGui` — перед загрузкой атласа (`ImGuiLayer::HasTextureUpdates`). Попутно:
`--set=frame.renderThread` / `frame.oneFrameThreadLag` действуют с первого кадра (остальной `--set` — после него), иначе
«режим 0» начинался кадром режима 1. Проверено (Release, вид A — пары подряд, без фоновой сборки): полы 0.97 / 0.20 %,
против сборок до G10a 0.1-0.9 %, B/C/N ≤ 0.55 %; прогон того же вида под параллельной сборкой Debug разошёлся на 3 % —
нагрузка на CPU, не код (листва). Debug-вид — `thread roles: clean`; Release `--scene-stress=40 --rt-audit` — CLEAN, аудит
PASS; `--scene-stress=40 --set=frame.renderThread:0` — CLEAN.

**G10b. RHI-поток** (`frame.rhiThread`, по умолчанию 0 до замера; UE: `r.RHIThread.Enable`, синхронизация GT —
`r.GTSyncType` 0 = кадр N-1 RT, затем N-2 RHI, `RenderingThread.cpp:2441-2550`). RT кладёт пакет кадра в FIFO и
заканчивает кадр раньше Present; поток «RHI» исполняет FIFO по порядку. Точки синхронизации:
- `BeginFrame` (GT, перед `Sync`): RHI отправил кадр слота (его `SignalFrame`) — иначе нет значения забора для
  `WaitForFrame`; ресайз (`ApplyPendingResize`) — FIFO пуст;
- `WaitForGpuIdle` / `WaitForPreviousFrame` (парковка): сначала FIFO пуст, потом простой GPU;
- работа после Present (скриншоты MCP, `--shot`, `SaveBackbufferPng`): Present своего кадра исполнен;
- загрузка атласа ImGui в `HandOverImGui`: FIFO пуст (только когда атлас меняется — редко);
- исключение на RHI (удаление устройства бросает из Present): ловится, перебрасывается в ближайшей точке
  синхронизации, как RT → GT в G8.
Профайлер: дорожка «RHIThread» сразу после RenderThread. Проверки: `--rt-audit` (RHI не трогает замороженное, но
сдвигает окна), стресс 150, сценарий MCP в редакторе, узкий GBV (`Overlay,Skybox`). Замер: кадр RT без ~0.4 мс отправки;
с VSync Present перестаёт держать RT. Решение о дефолте — по замеру.

**Сделано (2026-11-03), G10b.** `RhiQueue` (`rendering/core/RhiQueue.{h,cpp}`): у каждой операции номер (`Run`
возвращает его), `WaitFor(номер)` / `Flush()`; `SetThreaded` запускает или гасит поток «RHI» (роль Render, в профайлере
«RHIThread»), выполнив всё переданное. Ручка `frame.rhiThread` (UE `r.RHIThread.Enable`; Developer Controls > Frame «RHI
thread», `graphics_settings.json` frame.rhiThread, `--set` — с первого кадра), **по умолчанию 1** (решение владельца после замера, 2026-11-03; было 0); включается и гаснет в
`Renderer::BeginFrame`. Точки синхронизации:
- `BeginFrame` ждёт отправку кадра ПЕРЕД последним отданным (`prevSubmit_`): RHI отстаёт от записи не больше чем на кадр
  (UE GTSyncType 0: игра N+2, рендер N+1, RHI N), а последняя отправка слота (её `SignalFrame` — значение забора для
  `WaitForFrame`) ещё старше. Ожидание одного слота пускало бы запись на `kFrameCount - 1` = 2 кадра вперёд. Сегменты
  кадра — по слотам (`submitSegments_[kFrameCount]`): RHI отправляет слот k, пока следующий кадр собирает другой;
- `WaitForPreviousFrame` (простой GPU, парковка, ресайз, скриншоты) — `Flush`, потом простой;
- `HandOverImGui` — `Flush` перед загрузкой атласа (G10a);
- `lastPresentedIndex_` ставит запись (буфер, отданный на Present), скриншот всё равно сначала опустошает дорогу;
- пустая compute-работа `--async-empty-submit` записывается вместе с кадром (из пула его слота), RHI её только отправляет;
- исключение операции (удаление устройства бросает из Present) ловится на RHI и перебрасывается ближайшим `WaitFor` /
  `Flush`; `Shutdown` гасит поток после последнего простоя.
Ожидание RHI доставляет сообщения, ПОСЛАННЫЕ окнам ждущего потока (`MsgWaitForMultipleObjectsEx(QS_SENDMESSAGE)` +
`PeekMessage(PM_QS_SENDMESSAGE)`, посланные остаются циклу): DXGI может послать окну сообщение из Present на RHI-потоке, а
главный поток в `BeginFrame` (режим 1 — на нём) ждёт RHI. Профайлер: скоупы потока, перешедшие границу кадра, он
выбрасывал (эпоха `sampleEpoch_`), а отправка кадра N идёт, пока игровой поток закрывает N и открывает N+1 — в трейсе
пропадали `Service3` и сама операция; `Profiler::MarkThreadCrossFrame()` оставляет их кадру, который их соберёт.
Проверено (Release с полным LTCG — после смены раскладки `Renderer` инкрементальный дал +0.17 мс GPU-кадра на обоих кругах,
с полным разницы нет): картинка — ночь 0.011 %, вид A 2.7-3.2 % при поле режима 0 на нём же 2.8 % (листва, номер кадра);
Debug — `thread roles: clean`, сценарии MCP G2 и G9 (скриншоты, серия, правки с undo, миниатюры, `open_level` туда и
обратно) без ошибок; `--scene-stress=150 --rt-audit` — CLEAN, mem tags STABLE, аудит PASS (454 кадра);
`--scene-stress=40 --set=frame.renderThread:0` — CLEAN; GBV (`Skybox,Tonemap` + самопроверка Skybox, id=935 пойман) —
чисто, `live objects at exit: none`. Расхождение предсказанного буфера с цепочкой (`frame submit: ...`) — ни разу.

Замер (wind_test, стартовый вид, `--trace=120`, ветер и время заморожены, по два круга, выкл / вкл):

| | период | GPU-кадр | кадр RT | отправка на RT | ожидание GT потока рендера |
|---|---|---|---|---|---|
| 2560×1440 | 4.866 / 4.863 → 4.878 / 4.884 мс | 4.85 → 4.86-4.88 | 1.22 → 0.88-0.89 | 0.38 → 0.035 | 1.10 → 0.77 |
| VSync (1 круг) | 4.891 → 4.850 | 4.87 → 4.84 | 1.62 → 0.88 | 0.76 → 0.036 | 1.50 → 0.77 |
| 640×360 | 2.687 / 2.680 → 2.663 / 2.657 | 2.67 → 2.65 | 1.14-1.15 → 0.83-0.85 | 0.35 → 0.033 | 1.04-1.06 → 0.73-0.75 |

На RHI-потоке — 0.32-0.35 мс операции кадра (`Service3` 0.24 сегменты, `Service4` 0.10 Present; с VSync Present 0.43).
Таблица снята при ожидании слота; с `prevSubmit_` (финальная сборка) круг тот же: период 4.888 → 4.879, кадр RT 1.23 →
0.96, с VSync 4.895 → 4.874 и 1.47 → 0.94; скоуп «Wait RHI Thread» не встретился ни разу — RHI не отставал на кадр.
Кадр wind_test упирается в GPU даже в 640×360, поэтому период тот же (±1 %, шум): RHI-поток снимает 0.3 мс с потока
рендера (0.7 с VSync) — запас CPU, который станет кадрами, когда упор будет в CPU. Владелец: «включай rhi поток по умолчанию» — включён.

**G10c. Параллельный тик игровых систем.** `Scene::Tick` на wind_test — 0.08 мс: объекты уже идут `ParallelFor`, а свет,
ветер, сутки, лампы и буй — мелкие и связаны порядком (сутки ставят солнце, лампы читают его высоту, буй — часы океана).
Сначала сцена, где тик заметен (тысячи анимированных объектов, частицы); без неё — не делать.

**Замер (2026-11-03), G10c — не делается.** Release, настройки по умолчанию, `--profdump`: `Scene::Tick` на wind_test
(1454 объекта) 0.072 мс, из них объекты 0.037 + 0.019 (уже `ParallelFor`), буй 0.005, ветер 0.002; occlusion_test (624)
0.032; e1_particles_test (частицы на GPU) 0.012. Вся игровая половина до ожидания («Game: Simulate») — 0.135 / 0.086 /
0.050 мс. Распараллеливать нечего; сцены, где тик заметен, в проекте нет (её создание — запись в `data/levels`,
спросить владельца).

**G10d. Асинхронная загрузка уровней.** Сейчас загрузка паркует всё (`WaitForPreviousFrame`, `UploadBatch::SubmitAndWait`
в `loadPendingLevel`), и кадр стоит всё время загрузки. Цель: разбор уровня, чтение и декодирование мешей и текстур —
задачами на воркерах, пока рисуется старый уровень (или экран загрузки); подмена — на границе кадра короткой парковкой
(`Scene::Clear` + готовые объекты и ресурсы). Сначала замер: сколько стоит загрузка wind_test и что в ней дорого
(трейс загрузки, `BOOT_SCOPE`).

**Замер и первый шаг (2026-11-03), G10d-1.** Фазы загрузки теперь в загрузочном профиле и для смены уровня во время
работы: `Level switch (runtime)` (App и стресс) → `WaitForPreviousFrame (level switch)`, `Level::Unload`,
`Scene::InitializeCommonResources`, `JsonLevel::Load`, `Scene::FinalizeLevelLoad` → `SceneResources::Finalize` (`Objects
Init`, `RefreshObjectMaterials`), `ShadowGpuData::Rebuild` / `EnsureMegaBuffer`, `UploadBatch::SubmitAndWait`. Дерево —
в дампе загрузочного профиля на уровне Debug (`--log-level=debug`; стресс дампит в конце, `--stress-level=<уровень>`
стартует в нём и перезагружает его первой операцией). Перезагрузка wind_test (кэши тёплые) стояла **250 мс**: выгрузка 33,
`JsonLevel::Load` 129 (своих 113), Finalize 82, остальное 6. Своё время `JsonLevel::Load` было чтением файлов: каждый из
1454 объектов разбирал свой `mesh.json` с диска, статический меш — дважды (его создатель и `CreateStaticMeshFromJson`) —
~2900 чтений; кэш для этого (`MeshAssetScanCache`) был, но загрузка его не передавала. Теперь `JsonLevel::Load` держит
`SceneObjectFactory::MeshAssetLoadScope` — на время загрузки каждый `ResolveMeshAsset` без своего кэша на этом потоке
читает `mesh.json` один раз (кэш живёт одну загрузку, поэтому правка файла видна следующей). После: `JsonLevel::Load`
129 → 38-42 мс (создание объектов 100 → ~14), смена уровня **250 → 147-166 мс**, загрузка при старте 778 → 670-690.
Что осталось на тёплой перезагрузке wind_test (≈150 мс):
- `Objects Init` 80 мс: **океан 51** (шесть PNG через WIC, без DDS, 23.6 — общий кэш текстур слабый, а старый океан
  уничтожен до создания нового; `simulation_->Initialize` 23.2; `BuildMesh` 12.7) и 1454 статических меша 21.5;
- выгрузка 30 (уничтожение объектов, сброс сцены); `Skybox + IBL` 15; разбор JSON и создание объектов ~25; тени и
  отправка ~11.
При холодной загрузке (старт, первый визит) к этому — меши 210-240 мс, текстуры 150-220, PSO из кэша 110-170.

**Что дальше в G10d (решение владельца).** Настоящая асинхронность — это `Objects Init` на воркере, пока рисуется старый
уровень: загрузка мешей и текстур, материалы, PSO, запись загрузок в своё `UploadBatch` (отправка — через `Rhi()`). Для
этого `MeshManager`, `MaterialDataManager` и создание графических материалов должны стать потокобезопасными (сейчас под
мьютексом только `MaterialManager::materials_`, `BindlessHeap`, общий кэш текстур и кэш glTF-материалов), разбор уровня —
разделиться на «подготовить» (без сцены) и «применить» (на парковке), а старый уровень — жить, пока новый не готов
(тогда и слабый кэш текстур попадает). Выигрыш — холодная загрузка уходит из кадра (сотни мс), тёплая — до ~30-40 мс
парковки; цена — потокобезопасность трёх менеджеров и риск гонок, которые видны только под нагрузкой (гейты: `--rt-audit`,
стресс, редактор). Дешёвые синхронные срезы без неё: DDS для шести PNG океана (запись в `textures/` — спросить), океан не
пересоздаёт симуляцию и сетку, когда пресет тот же, небо не пересоздаётся, когда текстура та же.

---

## 5. Как проверять

- **Картинка**: режим 1 против режима 0 на трёх видах wind_test и на виде владельца, `--wind-freeze`,
  `--time-freeze`, кадр с одинаковым номером; сначала пол (режим 0 против себя), потом A/B. Свечи и облака — выключить
  (двигаются со временем).
- **Фазовые проверки**: Debug-прогон scene-stress и скрипта редактора — ноль `LOG_FATAL` фаз.
- **`--rt-audit`** (новый): случайные задержки 0-2 мс в точках RT и GT (перекрытие каждый кадр другое) + контрольная
  сумма замороженных полей в начале и в конце кадра RT (изменилась — GT писал в перекрытии; лог называет поле).
  Прогон 10 минут, вердикт строкой в сессионном логе (`rt audit: PASS|FAIL ...`), код выхода. **Доказать, что ловит**:
  `--rt-audit-selftest` пишет в матрицу объекта с GT посреди кадра — должен быть FAIL с именем поля.
- **Scene-stress**: `--scene-stress=150` с `frame.renderThread:1`; GBV — один раз в конце, суженный до пассов,
  которые трогал шаг, плюс самопроверка Skybox (AGENTS.md, «GPU-Based Validation»).
- **Скрипт редактора через MCP** (scratchpad, по образцу `tessy_baseline.py`): спавн, удаление, отмена,
  `setSurface`, `setLight`, `setTime`, `set_settings`, смена уровня, `screenshot` — по кругу, в обоих режимах; ни
  ошибок, ни расхождений документа.
- **Задержка**: номер игрового кадра, в котором прочитан ввод, против номера кадра, ушедшего в Present; режим 0 — 0,
  режим 1 — 1.

---

## 6. Отступления от UE и риски

- **D1. Нет прокси-объектов.** UE держит `FPrimitiveSceneProxy` и шлёт команды (`RendererScene.cpp:1382-1520,
  1614-1721`). У нас рендерабл один, поля поделены по видам §3.3, RT стоит во время Sync. Причина: прокси — переписать
  каждый рендерабл (статика, инстансы, океан, частицы, террейн), а дележ полей даёт тот же инвариант и проверяется
  фазами в режиме 0.
- **D2. Ручки без двух копий.** UE — `ShadowedValue[2]`; у нас `g_*` — копия RT, GT пишет через очередь (§3.8).
- **Риск: пропущенное чтение.** Не тот кадр трансформа → неверные векторы движения → размазанный DLSS; редкий,
  недетерминированный. Защита: фазовые проверки, `--rt-audit`, картинка с перекрытием.
- **Риск: ручка посреди кадра** — расхождение ресурсов → device removed. Защита: G5 до G8, скрипт.
- **Риск: окно** — блокировка DXGI ↔ окно (§3.9).
- **Риск: голодание RT** на общем пуле (G8).
- **Нет TSan под MSVC.** Поэтому собственные проверки (§3.4, §5), а не надежда на санитайзер.

## 7. Решения владельца

Ответы владельца 2026-11-03.

- **Q1.** Режим 1 по умолчанию после G9? — **Да.**
- **Q2.** Отставание: один кадр (UE `r.OneFrameThreadLag` 1, `RenderingThread.cpp:2435-2439`) или режим без
  перекрытия (GT ждёт RT ДО тика: меньше задержки, меньше выигрыша)? — Спросил, как в UE. UE 5.8
  (`D:\zalupa\Engine\Source\Runtime\RenderCore\Private\RenderingThread.cpp`): `r.OneFrameThreadLag` 1 по умолчанию —
  `FFrameEndSync::Sync` в конце кадра GT держит один забор рендер-потока, GT ждёт кадр N-1 RT и уходит не дальше чем на
  кадр (2497-2510; комментарий 2460: «старый игровой код считает, что GT не уходит дальше чем на кадр»); 0 — полная
  синхронизация, GT не начинает кадр, пока RT не закончил прошлый (2497-2499). Отдельно `r.GTSyncType` (0 по умолчанию,
  2441-2450): кроме RT, GT ждёт и RHI-поток — 0 = до двух кадров перекрытия с ним (2536-2543), 1 = кадр N-1 RHI, 2 = флип
  swapchain (меньше задержка, только с VSync, 2526-2555). Наш режим 1 = UE по умолчанию (`OneFrameThreadLag` 1); глубину
  GPU держит `kFrameCount` (Q4). **Решение: как UE, плюс ручка полной синхронизации** — `frame.oneFrameThreadLag`
  (сделано, §G8).
- **Q3.** Редактор в режиме 1 или всегда в 0 (стабильность важнее панелей)? — **В режиме 1.**
- **Q4.** `kFrameCount` 3 → 2, чтобы вернуть кадр задержки? — **Не снижать.**
- **Q5.** G10 (RHI-поток) — когда-нибудь? — **Делать** (после G9).

## 8. Исполнителю

Сборка (PowerShell, все три после каждого шага; Debug — первым, он строже):

```
& "C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/MSBuild.exe" "D:/Programming/test_cube/test_cube.vcxproj" /p:Configuration=Debug /p:Platform=x64 /m /nologo /v:minimal
& "C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/MSBuild.exe" "D:/Programming/test_cube/test_cube.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
& "C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/MSBuild.exe" "D:/Programming/test_cube/test_cube.vcxproj" /p:Configuration=Release_Editor /p:Platform=x64 /m /nologo /v:minimal
```

Трейс: `x64\Release\test_cube.exe --level=data/levels/wind_test.json --trace=120 --shot-delay=8 --wind-freeze=3`,
разбор скриптом по всем кадрам, среднее, два прогона — пол. Исходники CRLF, проверять байтами (AGENTS.md, «Line
Endings»). `python tools/check_logging.py` перед коммитом. Коммит — только по просьбе владельца.
