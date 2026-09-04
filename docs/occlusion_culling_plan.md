# План системы видимости (occlusion culling) в `D:\programming\test_cube`

Документ для ИИ-исполнителя, в стиле `docs/csm_improvement_plan.md`. **Исходники UE 5.6 лежат
локально** — `D:/Programming/ue_strip` (`Shaders/` и `Source/`, карта в `ue_strip/README.md`).
Читать оригинал ПЕРЕД тем, как выводить что-то из первых принципов: разведка 2026-09-03
(§2) дала точные файлы и строки; ниже они цитируются как ориентиры, копий кода Epic в документе нет.
Весь код приведён на типах и конвенциях `test_cube` (`mat4`/`float3`, reverse-Z в камере,
`Frustum` с inward-плоскостями, имена полей CB проекта).

Каждый шаг — независимо внедряемый и независимо проверяемый. Один шаг = один коммит.

**Цель.** Три вещи, которых сегодня нет:

1. **Sub-object frustum-cull** — чанки террейна и члены инстанс-батчей отбрасываются по-чанково /
   по-инстансно, а не «весь объект или ничего» (§1, факт F2: 100 чанков острова рисуются всегда).
2. **Occlusion-cull камеры** — то, что закрыто холмом/стеной, не рисуется в G-buffer вообще.
   Основа — уже существующая HZB-пирамида (§1, F4) и двухфазная схема Nanite (§2.3): фаза 1 против
   HZB прошлого кадра, фаза 2 — против HZB текущего.
3. **GPU-driven G-buffer** — камерный пасс на `ExecuteIndirect` по образцу теневого пути
   (`ShadowGpuData`), потому что результат GPU-теста видимости должен потребляться там же, на GPU,
   без readback'а. Это одновременно и рычаг против CPU-сабмишна (см. `mesh-refactor-progress`:
   «сцена CPU-submission-bound при масштабировании»).

Сквозные ограничения (как у CSM-плана): **тени не окклюжен-кулятся камерой** (правило UE, §2.5) —
кастер за спиной обязан бросать тень в кадр; **VSM не трогать**; **RT-структуры (BLAS/TLAS) не
зависят от видимости** — отражения видят закрытую геометрию; **редакторный `ObjectId` readback
и селекция** должны пережить переезд на indirect.

---

## 0. Правила работы (обязательно к прочтению)

1. **Line endings.** CRLF во всех C++/HLSL/project-файлах (`AGENTS.md`), проверять скриптом оттуда.
   `Write` эмитит CRLF в LF-файлы — считать CR-байты, а не верить `grep`.
2. **Читать дроп UE первым.** Разведка §2 — карта; формулы транскрибировать, а не выводить.
   Константа из чужого движка несёт ЕГО единицы: `OCCLUSION_SLOP = 1.0` у UE — это **1 см**, у нас
   `0.01f`.
3. **Граница цикла шейдера — только литерал**, никогда из CB (`GPU loop bound from CB`): число
   плоскостей, число мипов HZB, число тапов — константы `static const uint`.
4. **`RecordComputeDispatch` делит на 8** (`ComputeDispatch.h:20`) → `[numthreads(8,8,1)]`; 1-D
   кернел = `dtid.y != 0 → return`, как в `shadow_cull_cs.hlsl:71`.
5. **`[RootSignature]` на VS И PS** (blob берётся из VS, `Material.cpp:1675-1684`); без него
   материал молча не собирается и пасс молча пропускается (`Compiles is not loads`).
6. **Таблицы дескрипторов позиционны** — регистр = позиция в `RenderContext` (`RenderContext.h:8-24`).
7. **Нет `dynamic_cast`** — internal RTTI через виртуальные аксессоры (`AsRenderableObject`,
   `AsInstancedDrawBatch`).
8. **Reverse-Z камеры**: clear `0.0`, `GREATER_EQUAL` (`Material.h:78`); HZB «furthest» = **min**
   device-Z (`hzb_build_cs.hlsl:75-76`). Тест видимости: объект виден ⇔ его БЛИЖАЙШАЯ точка
   (max device-Z) ≥ min по футпринту HZB. У UE ровно так же (`NaniteHZBCull.ush:195-201`), флипов нет.
9. **Матрицы для куллинга — без джиттера** (`Camera::GetProjMatrixNoJitter`,
   `GetPrevViewProjMatrixNoJitter`, `Camera.h:75-81`); джиттер DLSS — только в растре.
10. **Замер**: `--set=ocean.visible:0` (океан меняет фазу между сессиями — S11 CSM-плана дал −25 lum
    по воде при нуле на песке), `--dlss=off`, `--wind-freeze`, `--set=exposure.autoExposure:0`,
    временные дизеры выкл; **пол = ≥3 одинаковых прогона на подозрительной области**; артефакт
    (файл) обязан РОДИТЬСЯ, лог этого не доказывает.
11. **Приёмка картинки — попиксельно**: A/B в одном бинаре, «нейтрально» = дифф на уровне пола;
    для occlusion это не опция, а определение корректности: **консервативный cull не меняет ни
    одного пикселя**.
12. **Каждый шаг заканчивается гейтами**: три конфига собираются (Debug — ассерты, `WITH_EDITOR`
    только в Debug/Release_Editor), `--log-stress` = 0, `cull validation PASS` в сессионном логе
    (валидатор GPU-cull починен 2026-09-03, headless — `--set=shadow.giIndirect:0`), при правках
    пассов/барьеров/ресурсов — `--scene-stress-gbv=20` CLEAN (гейт-дисциплина по типу правки).
13. **Не мерить occlusion на сегодняшних сценах без стресс-ручки** (§1, F1): камерная геометрия —
    0.13–0.27 мс GPU, эффект утонет в шуме. S0 вводит размножение сцены и «стену».

---

## 1. Текущее состояние (baseline, 2026-09-03, HEAD + uncommitted S11/S14)

### F1. Числа (Release, `--profdump`, 30 с прогрева, дефолт = Legacy CSM)

| сцена / камера | `CPU.Frame` | `GPU.Frame` | `RenderObjectBatch` GPU | `ExecuteBundles` | `Ocean.Surface` | `Pass_ObjectCompute` | `Pass_CSM` |
|---|---|---|---|---|---|---|---|
| atoll, камера уровня | 1.522 | 1.483 | 0.132 | 0.072 | 0.129 | 0.247 | 0.041 |
| wind_test, остров `-312.27,218.14,119.23` | 1.930 | 1.915 | 0.271 | 0.060 | 0.264 | 0.223 | 0.016 |
| wind_test, камера теней `15.07,5.13,69.20` | 2.682 | 2.664 | 0.257 | 0.198 | 0.249 | 0.246 | 0.343 |

CPU-сторона видимости: `Scene::PrepareViews` 0.024–0.050 мс, `SceneRenderQueue::Cull` 0.011–0.050
(5 вью), `RenderObjectBatch.Async` 0.13–0.20. **Вывод:** сегодня ни GPU, ни CPU не упираются в
камерную геометрию — occlusion здесь инфраструктура под масштаб, а не лечение симптома. Поэтому
приёмка перфа идёт на стресс-сцене (S0), а приёмка корректности — на сегодняшних.

**S0 (2026-09-03): стресс-сетка и «стена»** — Release, `--profdump`, 30 с прогрева, по два прогона,
океан ВКЛ (рецепт S0.4 CSM-плана); `frustum`/`chunks`/`tris` — камера, из `visibility_readout.log`:

| сцена / камера | K | objectsIn | frustum | chunks | tris (M) | `CPU.Frame` | `RenderObjectBatch.Async` CPU | `GPU.Frame` | `RenderObjectBatch` + `ExecuteBundles` GPU |
|---|---|---|---|---|---|---|---|---|---|
| wind_test, камера уровня | 1 | 619 | 242 | 90 | 1.35 | 3.051 / 3.054 | 0.138 / 0.139 | 3.034 / 3.039 | 0.073 + 0.315 / 0.076 + 0.333 |
| wind_test, камера уровня | **4** | 9889 | 1620 | 360 | 1.81 | 3.632 / 3.619 | 0.425 / 0.426 | 3.617 / 3.602 | 0.082 + 0.328 / 0.077 + 0.328 |
| occlusion_test, **wall** | 1 | 624 | 171 | 90 | 1.17 | 2.155 / 2.167 | 0.172 / 0.168 | 2.137 / 2.149 | 0.111 + 0.133 / 0.112 + 0.133 |
| occlusion_test, **wall** | **4** | 9969 | 3641 | 810 | 2.31 | 2.884 / 2.882 | 0.433 / 0.437 | 2.870 / 2.866 | 0.132 + 0.251 / 0.134 + 0.261 |
| wind_test, остров | 1 | 619 | 619 | 90 | 0.76 | 1.969 / 1.920 | 0.196 / 0.198 | 1.955 / 1.905 | 0.288 + 0.057 / 0.266 + 0.062 |
| wind_test, остров | **4** | 9889 | 3714 | 990 | 1.81 | 2.663 / 2.667 | 0.560 / 0.555 | 2.655 / 2.657 | 0.287 + 0.222 / 0.292 + 0.217 |

Что это говорит: **даже ×16 (≈10k объектов, 3.6k в фрустуме) кадр не становится CPU-submission-bound**
— `RenderObjectBatch.Async` растёт с 0.14 до 0.56 мс, `GPU.Frame` на +0.6–0.7 мс, потому что инстансинг
сворачивает 610 пальм трёх видов в единицы батчей, а копии добавляют инстансы, не draw'ы. Значит:
* перф-приёмка occlusion-шагов — по `tris`/`instances`/`chunks` из readout и по GPU-паре
  `RenderObjectBatch + ExecuteBundles`, а не по `CPU.Frame`;
* самая тяжёлая точка — **остров K=4**: 3714 объектов и 990 чанков в фрустуме, 1.81 M треугольников;
  **wall K=4** — 3641 объектов за стеной, из них камера видит только стену: это и есть целевой срез для
  S3a/S3b/S5 (occluded ≈ всё, кроме стены и песка);
* CPU-submission-bound сцену для S4 надо будет делать ИНАЧЕ — разными мешами/материалами (батчинг
  не сворачивает), не K копиями одного уровня; отметить в S4.

### F2. Что кулится, а что нет (проверено по коду)

* Камерный cull — **целый объект** по мировому AABB: `SceneRenderQueue.cpp:126` / `:376`
  (`frustum.Intersects(bounds)`), из общего источника `cameraObjectSource_` (`Scene.cpp:1516-1526`).
* **Чанки террейна не кулятся камерой вообще.** `RenderableObject::Render` для чанкованного меша:
  `for (s < n) mesh->DrawSubmesh(cl, s, chunkLods_[s])` (`RenderableObject.cpp:189-197`) — **90** чанков
  `atoll_island` (`chunkGrid: 10`, десять пустых выброшены при бейке; измерено S0-readout'ом) каждый кадр. При этом пер-чанковые боксы ЕСТЬ (`Mesh::GetSubmeshBounds`,
  `Mesh.h:136`) и каждый кадр переводятся в мир ради выбора LOD-тира (`RenderableObject.cpp:291-306`).
  В тенях чанки уже кулятся по-чанково на GPU (`docs/terrain_shadow_chunking_plan.md`, S2: пер-сабмешные
  bounds кастеров, 2708 кастеров / 56 групп) — камера отстала.
* Инстанс-батчи: `BuildInstancedBatchesForBucket` (`SceneRenderQueue.cpp:255`) строит батч из
  **уже прошедших фрустум** членов; порог `kInstancingThreshold = 8` (`InstanceTypes.h:113`) стоит на
  размере ВСЕГО бакета (`:260`) и на длине рана (`:297`). Occlusion уменьшит число инстансов, не батчей.
* GI-листва (`GpuInstancedModels`): для камеры **GPU-cull нет**, LOD-партиция на CPU
  (`GpuInstancedModels.cpp:143-179`), кап `kMaxLodInstances = 256` (`GpuInstancedModels.h:103`), один
  `DrawInstanced` на тир (`:249`); фрустум-тест — по облаку целиком. В уровнях `data/levels/*.json`
  объектов `gpuInstanced` нет — GI сегодня не главный потребитель.
* `ExecuteIndirect` — только тени: `ShadowGpuData.cpp:2070`, `SceneRenderer_Shadows.cpp:321`,
  `VirtualShadowMap.cpp:1075`. **Depth pre-pass нет.**

### F3. Порядок пассов (`SceneRenderer_Graph.cpp`)

`Main_ShadowCull :304` → тени → **`Main_GBuffer :408`** (пишет глубину) → `Main_VsmPageRequest` →
`Main_VsmPageRender` → **`Main_Hzb :502`** → `Main_Gtao` → `Main_RTTrace` (async) → `Main_Lighting` →
… → `Main_Transparent :1243` (глубина DEPTH_WRITE) → `Main_ObjectIdReadback :1327` → … → `Main_DLSS`.
G-buffer внутри — свой граф: `GBuffer_Driver` (clear), `GBuffer_OpaqueSimple` (bundles, chunk 2),
`GBuffer_OpaqueComplex` (direct, chunk 32), `GBuffer_Selected` (`SceneRenderer_Geometry.cpp:228-269`).
Драйвер — `RenderObjectBatch` (`:56`), fan-out через `TaskSystem::DispatchTrack` (`:142`), по объекту
`obj->Render(renderer, cl, camera, viewCB)`; `InstancedDrawBatch` лежит в том же бакете и рисуется
тем же вызовом (per-tier `DrawInstanced`, `InstancedDrawBatch.h:82-86`).

### F4. HZB уже есть — и почти та, что нужна

* Строится `Pass_Hzb` (`SceneRenderer_Lighting.cpp:58`) СРАЗУ после G-buffer'а из глубины текущего кадра
  (только opaque); шейдер `hzb_build_cs.hlsl`, `[numthreads(8,8,1)]`, один dispatch на мип.
* **Две цепочки в одном dispatch**: `D.hzb` = **min** device-Z = FURTHEST (reverse-Z) — для GTAO;
  `D.hzbClosest` = max = CLOSEST — только при `decisions_.ssrHiz`, для SSR. Для occlusion нужна первая.
* Формат `R32_FLOAT` (`RenderConstants.h:100`), мип 0 = **половина render-разрешения, округление вверх**
  (`Renderer.cpp:2220-2225`, общая сетка с GTAO), **не степень двойки** — в отличие от UE
  (`SceneTextureReductions.cpp:134-138`: `RoundUpToPowerOfTwo >> 1`). Следствие: формула выбора мипа
  и размер текселя считаются от реальных размеров `max(1, size >> mip)`, а не через экспонентный трюк
  UE (`NaniteHZBCull.ush:135-193`). Нечётные хвосты сворачиваются (`hzb_build_cs.hlsl:81-94`).
* Состояние покоя `NON_PIXEL_SHADER_RESOURCE`; на время билда вся цепочка в UAV с UAV-барьерами между
  уровнями (барьерный слой переводит ресурс целиком, `RenderTargetManager.cpp:488-493`).
* **HZB и глубина прошлого кадра живы**: `DeferredTargets deferred_[kFrameCount=3]`
  (`RenderTargetManager.h:341`), слот `(f + 2) % 3` не трогается до своего следующего кадра.
  Никто сегодня не читает через слоты — но ничего не мешает: это и есть «HZB прошлого кадра» для
  фазы 1 без единой новой аллокации. Матрицы прошлого кадра: `Camera::GetPrevViewProjMatrixNoJitter()`
  (`Camera.h:81`), смена камеры — `GetHistoryRevision()` (`:86`).

### F5. GPU-driven инфраструктура теней — что переиспользуем

`ShadowGpuData`: `CasterBounds` (32 байта: центр+радиус, half-extents; `InstanceTypes.h:87-92`),
`ShadowViewFrustum` (16 плоскостей, `:101-106`), регистрация при загрузке `Rebuild` + `UpdateForFrame`
для муверов, `UpdateViewFrustums` каждый кадр, cull `shadow_cull_cs.hlsl` → args
`[view][group*4+lod]` (`kArgStride = 20`) + visible list как per-instance vertex stream (slot 1),
один `ExecuteIndirect` на виртуальную группу (`ShadowGpuData.cpp:1989-2070`); `Ring`/`UavRing`
(`ShadowGpuData.h:286-324`); readback-валидатор с латентностью `kFrameCount` (`:2137-2206`).
Сигнатура — `Renderer::GetDrawIndexedCommandSignature()` (`Renderer.h:597`), только DRAW_INDEXED.
Материалы G-buffer'а, bindless и содержимое mega-VB/IB — см. факты в S4.

### F6. Статистика

`render::RenderStats` (`RenderStats.h:11-32`): `drawCalls`, `primitives` — HUD `DeveloperWindow.cpp:334`.
**Нет** счётчиков «видимых объектов/инстансов/чанков по вью» и нет их в `--profdump`
(`Profiler.cpp:1477-1509` пишет только скоупы). S0 это заводит.

### F7. Масштаб уровней

`wind_test.json`: 620 объектов (222 coconut + 217 date + 171 curly palm + 2 tent + остров + дно +
камни), 12 мешей; `atoll.json`: 107. Остров — один объект, 100 чанков, 4 LOD (82.9k/41.5k/20.7k/10k
треугольников), AABB 361 × 16 × 388 м.

---

## 2. Как это устроено у UE 5.6 (разведка 2026-09-03, файлы дропа)

### 2.1. Hardware occlusion queries (классический путь)

* Включение `r.AllowOcclusionQueries` (`SceneVisibility.cpp:354-366`). Bounds примитива кэшируются
  при добавлении и расширяются на `OCCLUSION_SLOP = 1.0` (см = **0.01 м** у нас; `ScenePrivate.h:69`,
  `PrimitiveSceneInfo.cpp:1834-1844`), плюс пер-кадровое расширение недавно затестированных
  (`SceneVisibility.cpp:2826`, cvar'ы `:206-243`).
* Два батчера на вью: индивидуальные и групповые по **16 боксов** (`SceneRendering.h:347`,
  `SceneOcclusion.cpp:465-534`): 8 углов AABB в динамический VB, draw 12·N треугольников между
  `BeginRenderQuery/EndRenderQuery`, depth-test без записи, цвет выкл (`:1243-1265`).
* **Латентность k = `r.NumBufferedOcclusionQueries` = 1** на десктопе (`ConsoleManager.cpp:3989-3994`,
  `SceneOcclusion.cpp:107-133`): результат кадра N читается в начале N+1; отсутствие результата =
  **виден** (`SceneVisibility.cpp:2774-2795`).
* История `FPrimitiveOcclusionHistory` (`ScenePrivate.h:108-266`): occluded в прошлом кадре ⇒ **тест
  каждый кадр, но групповой** (`:2871-2876`); виден и «definite» ⇒ стохастический ре-тест
  (`:2880-2889`); новый примитив ⇒ **виден** (`:2706-2712`); бокс пересекает near-plane ⇒ виден и
  definite (`:2831-2846`); смена камеры / большое движение ⇒ все результаты игнорируются
  (`:5367-5380`, `bIgnoreExistingQueries`).
* Сабпримитивные запросы (HISM): `Proxy->GetOcclusionQueries`, примитив отбрасывается только если
  **все** сабзапросы occluded (`:2971-2980`).

### 2.2. HZB-тестер (`r.HZBOcclusion`, `FHZBOcclusionTester`)

* Атлас bounds **256×256** (`SceneRendering.h:424-425`, до 65536 примитивов), две текстуры
  `PF_A32B32G32R32F` center/extent, заливка блоками 8×8 (`SceneOcclusion.cpp:946-1058`); `Extent.w = 1`
  — валидная запись.
* Шейдер `Shaders/Private/HZBOcclusion.usf:30-41`: `BoxCullFrustum` → если видим и НЕ пересекает
  near-plane → `GetScreenRect(…, 4)` → `IsVisibleHZB(Rect, true)` (4×4 футпринт). Сравнение reverse-Z:
  `Rect.Depth >= MinDepth`, где `Rect.Depth = CullRectMax.z` — **ближайшая** точка бокса против min
  по футпринту **furthest**-HZB (`NaniteHZBCull.ush:74`, `:195-201`).
* HZB — **текущего** кадра (`EHZBType::FurthestHZB`, `SceneOcclusion.cpp:1035`), `Submit` сразу после
  билда HZB (`DeferredShadingRenderer.cpp:520-523`); результат — readback, читается **следующим
  кадром** (`SceneVisibility.cpp:3133-3171`), `Map` ждёт GPU.
* BuildHZB (`SceneTextureReductions.cpp:116-171`): pow2, половина, полная цепочка, closest и furthest —
  **раздельные** текстуры; до 4 мипов на dispatch (`HZB.usf`, `GROUP_TILE_SIZE 8`); closest округляется
  вверх до fp16 ради консервативности (`RoundUpF16`, `HZB.usf:57-61`).

### 2.3. GPU instance culling и двухпроходный Nanite

* GPUScene-cull (`r.InstanceCulling.OcclusionCull`, дефолт 0): **один** проход против HZB **прошлого
  кадра** (`BuildInstanceDrawCommands.usf:190-213`): бокс инстанса в `PrevLocalToTranslatedWorld ×
  PrevTranslatedWorldToClip`, `GetScreenRect(HZBTestViewRect, PrevCull, 4)`, `RoundUpF16(Depth)` против
  самоокклюжена, `IsVisibleHZB`. У инстансов без истории `PrevLocalToWorld == LocalToWorld`.
* **Nanite two-pass** (`r.Nanite.Culling.TwoPass = 1`, `NaniteCullRaster.cpp:319-322`; выключается без
  `PrevHZB`, `:3822-3824`):
  1. main pass: cull `CULLING_PASS_OCCLUSION_MAIN` против **prev-HZB** с prev-матрицами; НЕ прошедшие
     пишутся в список `WriteOccludedInstance` (`NaniteInstanceCulling.usf:221-226`, `:444-448`);
     **фрустум прошлого кадра не проверяется** (`bSkipPrevCullFrustum = true`, «post pass will clean up
     bad guesses», `NaniteCullingCommon.ush:476-481`);
  2. растр видимых, затем **новая HZB из результата main pass'а** (`BuildHZBFurthest`,
     `NaniteCullRaster.cpp:6474-6500`);
  3. post pass: только отложенный список, против **текущей** HZB и текущего фрустума
     (`CULLING_PASS_OCCLUSION_POST`, `:6505`; `NaniteCullingCommon.ush:495-508`).
  Итог консервативен: всё, что видно в этом кадре, нарисовано в этом кадре; «плохие догадки» фазы 1
  стоят лишь второго теста.

### 2.4. Проекция бокса → прямоугольник экрана → мип HZB (`NaniteHZBCull.ush`)

* `BoxCullFrustumPerspective` (`:444-540`): 8 углов инкрементально, `MinW/MaxW`, `MinZ = MaxW·P[2][2] +
  P[3][2]`, `MaxZ = MinW·…` (`:505-506`), `RectMin.z = MinZ/MaxW`, `RectMax.z = MaxZ/MinW` (`:527-528`).
  **Near-plane:** `bCrossesNearPlane = (MinW <= MaxZ)` (`:509,516`); при `MinW <= 0 && MaxW > 0` rect =
  весь экран (`:520-524`); все вызывающие пропускают HZB-тест при `bCrossesNearPlane` ⇒ **виден**.
* `GetScreenRect` (`:71-107`): NDC → пиксели с правилом центра пикселя (`+0.5 / −0.5`),
  `bOverlapsPixelCenter`, `HZBTexels = Pixels >> 1` (мип 0 = половина), `HZBLevel =
  MipLevelForRect(HZBTexels, 4)`.
* `MipLevelForRect` (`:40-69`): `MipLevelXY = firstbithigh(zw − xy)`, `MipLevel = max(max(x,y) −
  (log2(footprint) − 1), 0)`, +1 если квантование выравнивания ломает покрытие.
* `GetMinDepthFromHZB` (`:135-193`): 4×4 = четыре `GatherLODRed`, min; вырожденные строки/столбцы
  маскируются 1.0. Есть плоскостной вариант `IsVisibleHZB(Rect, PlaneHZB)` (`:273-332`) — не наш первый
  шаг.
* Сфера: `SphereToScreenRect` (Mara & Morgan 2013, `:565-595`) + near-clip варианты.

### 2.5. Тени и occlusion

* **Occlusion камеры НЕ кулит кастеров**: `PrimitiveVisibilityMap` — только main view; сбор
  сабжектов теней идёт по фрустуму тени. Видимость камерой лишь помогает тени избежать
  собственного запроса (`SubjectsVisible`, `ShadowSetup.cpp:2898-2908`).
* Каскады directional: `r.Shadow.OcclusionCullCascadedShadowMaps = 0` по умолчанию («rapid view
  changes reveal new regions too quickly for latent occlusion queries», `SceneOcclusion.cpp:56-62`);
  **каскад 0 не запрашивается никогда** (`:1149`).
* VSM: из HW-запросов исключён (`:1130-1134`), результат игнорируется («can result in incorrectly
  cached pages», `ShadowSetup.cpp:4510-4519`); у VSM свой HZB по страницам (`r.Shadow.Virtual.UseHZB`) —
  **не предмет этого документа**.

---

## 3. Наш дизайн — что берём, что нет и почему

| UE | Берём? | Почему |
|---|---|---|
| Hardware queries + история | **да, как S3a** | Это ДЕФОЛТ UE на десктопе (`r.HZBOcclusion = 0`): запрос точен до пикселя против полной opaque-глубины, история гасит мигание, групповые запросы по 16 делают occluded-примитивы почти бесплатными. Плюс запросы сфер влияния spot/point — пропуск целых теневых пассов. Владелец: «мне всё надо» (2026-09-03). |
| HZB-тестер с readback | **да, как S3b** | Альтернатива S3a на той же истории и том же потребителе (`vis.method`): один compute-тест против текущей HZB + readback — без draw'а боксов, но консервативнее (половина разрешения, 4×4 футпринт). Правила UE (near-plane ⇒ виден, новый ⇒ виден, смена камеры ⇒ сброс) общие. |
| Двухпроходный cull против prev-HZB + cur-HZB | **да, как S5** ✅ | Ноль латентности и ноль поппинга по построению; prev-HZB у нас бесплатна (F4). Требует GPU-driven G-buffer (S4). Сделан 2026-09-04: `Main_ShadowCull` (фаза 1) → `GBuffer_Indirect` (A) → `Main_HzbA` → `Main_CamCullPost` → `Main_GBufferB` → `Main_Hzb`. |
| `OCCLUSION_SLOP`, расширение боксов | да | 0.01 м; плюс наш шаг для ветра: боксы кастеров НЕ паддятся под sway (`FillBounds`), для камеры пад обязателен — иначе качающаяся крона у края бокса мигает. |
| Occlusion теней глазами КАМЕРЫ | **нет** | Правило UE (§2.5). Каскады/VSM/локалы никогда не смотрят на результаты камеры. |
| Occlusion теней со стороны СВЕТА (HZB по каскаду / по странице VSM) | **да, как S5b** | UE делает это для VSM (`r.Shadow.Virtual.UseHZB`, `NonNanite.UseHZB`): кастер, целиком за более близким кастером от света, в карту не пишет. Двухпроходно — без латентности. Владелец: «сотня объектов под стеной едет в теневой проход» (2026-09-04). |
| Sub-primitive queries (HISM) | да, как чанки | Чанк террейна = сабпримитив: объект виден ⇔ хотя бы один чанк виден. |

**Порядок фаз в кадре после S5** (`Main_GBuffer` распадается):

```
Main_VisCull1   (compute)  фрустум + prev-HZB(слот f+2) с prev-матрицами → args_A, occludedList
Main_GBufferA   (indirect) ExecuteIndirect по args_A
Main_HzbA       (compute)  HZB из глубины после фазы A   (это сегодняшний Main_Hzb, переехавший)
Main_VisCull2   (compute)  только occludedList против HZB_A и ТЕКУЩЕГО фрустума → args_B
Main_GBufferB   (indirect) ExecuteIndirect по args_B
Main_HzbB       (compute)  финальная HZB для GTAO/SSR (дешёвая: 28 мкс, `async_compute_plan.md:350`)
```
(Как легло в S5: `Main_VisCull1` = камерная ветка `Main_ShadowCull`, `Main_GBufferA` = `GBuffer_Indirect`
внутри `Main_GBuffer`, `Main_VisCull2` = `Main_CamCullPost`, `Main_HzbB` = `Main_Hzb` на прежнем месте;
`gb.pGbufDone` = `Main_GBufferB`, на него пересажены все потребители G-buffer'а.)

Пока S4 не сделан, S3a/S3b дают объектную/чанковую окклюзию CPU-пути с латентностью в кадр —
поппинг при резком повороте камеры лечится правилом «смена камеры ⇒ сброс» и тем, что
«не затестирован ⇒ виден». После S4/S5 они остаются для transparent/glass/complex и для
теней локальных источников (S3a.6).

---

## S0. Инструментирование и стресс-сцена — ✅ СДЕЛАНО 2026-09-03 (commit 7f46331)

**Зависит от:** ничего. **Эффект:** без него ни один следующий шаг не измерим. **Риск:** нулевой.

### Что сделано
* `render::VisibilityStats` (`sources/rendering/core/VisibilityStats.h`): слоты камера + c0..c3,
  поля `objectsIn / objectsFrustum / objectsOccluded / chunksIn / chunksDrawn / instancesDrawn /
  trianglesSubmitted`; пишет `Scene::PrepareViewQueue` ДО батчинга (объект считается один раз),
  снапшот `NextFrame()` рядом с `RenderStats` в `App.cpp`. HUD — таблица в табе Render;
  headless — `--vis-readout` → `logs/visibility_readout.log` на кадре 600 (`Scene::Render`).
  Треугольники — CPU-оценка по выбранному LOD (чанки — по тиру чанка); для fused-пути
  `objectsIn == objectsFrustum` по построению (очередь держит только выживших).
* `--set=scene.replicate:K` (`App.cpp`, `ReplicateLevelObjects`): читает JSON активного уровня
  (`LevelManager::GetActiveLevelSourcePath`), клонирует каждый включённый `staticMesh` K×K−1 раз
  сеткой, **центрированной на оригинале** (K=4: смещения −1..2 шага), шаг = статический экстент
  уровня (`Scene::ComputeStaticBounds`, без океана) × 1.1; спавн — по рецепту редакторского
  `SpawnMeshCommand`: `ResolveMeshAsset` → `CreateStaticMeshFromJson` → `UploadBatch` →
  `AddInitializedObject`. Ничего на диск. wind_test: шаг 397 × 427 м, K=2 → +1854 меша,
  `cull validation PASS` на 11052 кастерах.
* `data/levels/occlusion_test.json` — копия wind_test + стена (`box`, 1 × 10 × 44 м в `(10, 5, 70)`,
  западнее рощи), четыре столба 0.3 × 8 м в `x = 6`, `z ∈ {58, 64, 76, 82}`, спот с сферой целиком за
  стеной (`(30, 6, 70)`, range 12) и спот перед ней (`(4, 5, 60)`, range 6).
  Камеры (кватернион по рецепту look-at из `csm-scissor-optim`):
  * **`wall`** — `--cam-pos=1.50,3.00,70.00 --cam-rot=0.0000,0.7071,0.0000,0.7071` — стена во весь
    кадр, снизу полоска песка/воды; в фрустуме камеры 170 объектов, из них роща за стеной — то, что
    occlusion обязан срезать;
  * **`wall_side`** — `--cam-pos=1.50,3.00,50.00 --cam-rot=0.0000,0.2417,0.0000,0.9703` — столбы и
    край стены, 53 объекта в фрустуме.

### Readout-образец (wind_test, камера теней, `--set=ocean.visible:0`, кадр 601)

```
view     objectsIn  frustum  occluded  chunksIn  chunksDrawn  instances   triangles
camera         618      317         0        90           90        317      638437
c0             618       17         0        90           90         17      144490
c1             618       80         0        90           90         80      594362
c2             618      443         0        90           90        443     2464712
c3             618      618         0        90           90        618     2761789
```
Уже видно, куда идти: c3 держит ВСЕ 618 объектов и 2.76 M треугольников (S14-объём режет не всё,
роща целиком в конусе дальнего каскада), и 90 чанков острова во всех пяти вью — S1.

### Замер K∈{1,4} — см. F1 (строки добавлены S0).

### Почему
F1 и F6: камерная геометрия сегодня 0.13–0.27 мс, счётчиков видимости нет. Occlusion нельзя ни
измерить, ни принять без (а) сцены, где геометрия доминирует, и (б) чисел «сколько отброшено».

### Код
1. `render::VisibilityStats` (новый, рядом с `RenderStats.h`): по вью (камера + 4 каскада)
   `objectsIn / objectsFrustum / objectsOccluded / instancesDrawn / chunksIn / chunksDrawn /
   trianglesSubmitted`; атомики, снапшот `NextFrame()`, HUD в табе Stats, и **headless** —
   `--vis-readout` → `logs/visibility_readout.log` (через `diag::ArtifactFile`, кадр > 600 — на 9-м
   уровень ещё стримится, см. S14 CSM-плана), формат как `csm_readout.log`.
2. Стресс-ручка `--set=scene.replicate:K` (`App.cpp`, блок `--set`): после загрузки уровня каждый
   `staticMesh` клонируется K×K раз сеткой с шагом = AABB уровня (объекты реальные, через
   `SceneObjectFactory`, никаких ассетов на диске — правило «Ask before writing assets» не задето).
   Ожидание: при K=4 на wind_test 620 → ~10k объектов, `RenderObjectBatch.Async` и `Pass_GBuffer` CPU
   должны стать доминирующими — это и есть цель для S4.
3. «Стена»: камера на wind_test **за холмом острова**, смотрящая в него в упор, так что роща за
   холмом закрыта; координаты подобрать по `s14`-рецепту look-at (`csm-scissor-optim` память,
   квaтернион = `XMQuaternionRotationMatrix(GetRotationMatrix())`); записать в этот документ.
   **Тестовый уровень можно замусоривать** (разрешение владельца 2026-09-03): копия
   `data/levels/wind_test.json` → `data/levels/occlusion_test.json` со стенами-окклюдерами
   (большие `staticMesh`-боксы), тонкими окклюдерами (столбы — разводят S3a и S3b) и спотами за
   стеной (S3a.6). Это единственный ассет, который план пишет; `models/`/`textures/` не трогаются.
4. Профдамп-скрипт по образцу `s14_prof.ps1`: `--profdump` на трёх камерах (уровень, стена, остров) ×
   K ∈ {1, 4}, по два прогона.

### Критерий приёмки
* Таблица F1 дополнена строками K=4 и «стена»; `visibility_readout.log` рождается headless и содержит
  по-вью счётчики (`chunksIn = 90` для острова на камере — было заявлено 100, readout поправил).
* `--log-stress` 0/0, три конфига.

### Откат
Ручки инертны без флагов.

---

## S1. Пер-вью маска чанков по фрустуму (camera + Legacy CSM CPU-loop) — ✅ СДЕЛАНО 2026-09-03 (commit eeb43a7)

**Зависит от:** S0 (счётчики). **Эффект:** чанки за спиной и вне каскада не рисуются; на острове —
до ~70 % чанков камеры (сфера видимости против сетки 10×10). **Риск:** низкий; ловушка — общий
`chunkLods_` между камерой и тенями.

### Почему
F2: боксы чанков есть и уже переводятся в мир каждый кадр в `SelectLod`; фрустум-тест рядом не сделан.
В тенях GPU-cull уже пер-чанковый (`terrain_shadow_chunking_plan.md` S2), но Legacy CSM CPU-loop
(`Pass_CSM`, `indirect=false`) и камера рисуют все 90.

### Что сделано (как построено, с отличиями от плана ниже)
* `RenderableObjectBase::SelectLod(const Camera&, const Frustum& cameraFrustum)` — фрустум камерного
  вью едет вместе с камерой (`SceneRenderQueue::SelectLods(camera, view.frustum)`, `Scene.cpp`
  PrepareViewQueue): маска считается теми же плоскостями, которыми только что прошёл объектный cull.
* `RenderableObject`: `chunkVisCamera_` рядом с `chunkLods_` (маска, тир по-прежнему выбирается для
  ВСЕХ чанков — их читают тени), пишется в `SelectLod` от того же `boxes[s].Transform(model)`, что и тир;
  `Render` скипает чанки с нулём. Предикат один — `static RenderableObject::ChunkInFrustum(worldBox,
  frustum)` (невалидный бокс = виден, как у объектного cull; `g_visChunkMask` = откат).
* `RenderableObjectBase::RenderShadow(..., UINT lod, bool chunkCameraLods, const Frustum* chunkCullFrustum
  = nullptr)`: оба CPU-цикла каскадов в `SceneRenderer_Shadows.cpp` (параллельный и серийный,
  `chunkCameraLods=true`) передают `&view.frustum` — это S14-объём; чанк тестируется НА МЕСТЕ от
  `mesh->GetSubmeshBounds()[s].Transform(model)`. Остальные вызовы (споты/точки/клипмап, GI,
  бейки shore-depth/SDF) оставляют `nullptr` → рисуют всё. Переопределения (`InstancedDrawBatch`,
  `DebugGrid`) параметр игнорируют.
* `GpuInstancedModels::BuildLodPartition(camPos, frustum)`: инстанс не попадает в remap, если его
  сфера мимо фрустума. Сфера — вокруг ПИВОТА инстанса (`model.TransformPoint(offset)`), радиус
  `(|centre_local| + radius_local) × maxScale(object)`: инвариант к Y-повороту, который живёт на GPU
  (`instance_anim.hlsl`), CPU-копия угла в решение о видимости не входит. Тени и RT не трогает —
  `DrawGeometry`/`GetRtInstances` по-прежнему идут по `instanceCount_`. Новый виртуал
  `GetCameraInstanceCount()` (дефолт = `GetInstanceCasterCount()`) — только для счётчиков.
* Счётчики (`AccumulateVisibility(c, queue, sourceCount, frustum, cameraView)`): `chunksDrawn`,
  `trianglesSubmitted`, `instancesDrawn` считают то, что фрустум вью ОСТАВЛЯЕТ ниже объекта. Камера
  читает маску и visible-инстансы; каскад пересчитывает `ChunkInFrustum` от своих боксов сам.
* Ручки: `--set=vis.chunkMask:0` (`render::g_visChunkMask`, `VisibilityStats.h`), чекбокс «Chunk /
  instance frustum mask (S1)» под таблицей видимости в табе Render.

**Отличия от текста плана.** `chunkBoundsWS_` НЕ добавлен: `SelectLod` бежит только для объектов,
которые камера видит, поэтому кэш мировых боксов устарел бы ровно для закадрового кастера, ради
которого теневой путь и существует. 90 `AABB::Transform` на каскад дешевле любой логики свежести.

### Ловушки
* **Prepare-задачи вью бегут параллельно.** Счётчик каскада НЕ может читать `chunkVisCamera_` — его
  в этот момент пишет камерная задача. Поэтому предикат пересчитывается от статических данных
  (боксы меша + матрица объекта), и это же делает `RenderShadow`. (S0 уже читал `chunkLods_` из
  теневой задачи — байтовые значения, гонка безобидна, но новые массивы под неё не подкладывать.)
* GI-облако: тест по AABB, повёрнутому CPU-копией угла, был бы «почти верным» — сфера вокруг
  пивота верна при любом угле и стоит столько же.
* Пред-существующая ошибка в session-логе `UNBOUND SRV table at root index 1 -- shader:
  shaders/exposure_histogram_cs.hlsl` — есть и в сессиях редактора ДО S1 (16:29, HEAD 7f46331),
  к шагу не относится. **Починена вместе с S2:** `CSClear` делит root signature с `CSBuild`, а та
  ДЕКЛАРИРУЕТ SRV-таблицу; clear-диспатч шёл с пустой таблицей (`{ }`) → теперь биндит тот же
  `D.sceneSRV` (`SceneRenderer_Post.cpp`). Проверка: session-лог обычного прогона — 0 строк ERROR.

### Замер (4 запуска, свёрнутыми; Release, `--shadow-mode=legacy --dlss=off --wind-freeze
--set=exposure.autoExposure:0 --set=ocean.visible:0`, кадр 601)
Камера теней wind_test (`--cam-pos=15.07,5.13,69.20`), `--set=shadow.indirect:0` — чтобы CPU-петля
каскадов исполняла маску:

| view   | frustum | chunksIn | chunksDrawn off→on | tris off→on        |
|--------|---------|----------|--------------------|--------------------|
| camera | 317     | 90       | 90 → **47**        | 638 437 → 627 314  |
| c0     | 17      | 90       | 90 → **3**         | 144 490 → 121 872  |
| c1     | 80      | 90       | 90 → **10**        | 594 362 → 580 935  |
| c2     | 443     | 90       | 90 → **44**        | 2 464 712 → 2 461 468 |
| c3     | 618     | 90       | 90 → **87**        | 2 761 789 → 2 761 720 |

`drawCalls` 612 → **353** (259 чанковых draw'ов меньше за кадр). Треугольников мало (−1.7 % у
камеры): срезанные чанки — LOD3 за спиной; выигрыш S1 — draw'ы и вершинная работа, не растр.
Стена (`occlusion_test.json`, дефолтный indirect-путь): камера 90 → **26**, c3 20/90.
Картинка: off→on **0.030 %** пикселей при поле (on дважды) 0.042 % — идентична; readout on/on2
совпадает до счётчика. Три конфига собраны чисто.

### Критерий приёмки
* ✅ Камера теней wind_test: картинка идентична (0.030 % при поле 0.042 %), `chunksDrawn` < `chunksIn`
  во всех пяти вью; Legacy-тени идентичны (в кадре — CPU-петля с маской). VSM код S1 не исполняет
  (`ShadowGpuData` читает только `chunkLods_`, который не менялся) — профдамп `Pass_VsmPageRender`
  не гонялся, паритет по построению.
* ✅ Камера «стена»: 26 из 90 чанков. `--cam-fly` серия на мигание НЕ гонялась (лимит запусков);
  тест консервативный AABB — тот же, что объектный, миганий ждать неоткуда.
* Debug-ассерт не нужен — дифф покрывает.

### Откат
`--set=vis.chunkMask:0` — маска всегда 1, GI-инстансы не режутся (чекбокс в табе Render — то же).

---

## S2. Библиотека HZB-теста + self-test (enabler для S3b/S5) — ✅ СДЕЛАНО 2026-09-03 (commit eeb43a7)

**Зависит от:** ничего (HZB есть). **Эффект:** одна функция `IsVisibleHZB` в HLSL и её CPU-зеркало,
доказанные на синтетике. **Риск:** средний — формулы UE рассчитаны на pow2-HZB.

### Почему
Все дальнейшие шаги тестируют боксы против HZB; ошибка тут = невидимые объекты, и найти её по картинке
трудно. Nanite-формулы транскрибируются, но F4 (не-pow2 половинная HZB) требует двух осознанных дельт.

### Код
`shaders/hzb_cull.hlsli` — транскрипция `NaniteHZBCull.ush` в наших конвенциях:
* `BoxCullFrustumPerspective(center, extent, worldToClip, proj)` — 8 углов, `MinW/MaxW`, `RectMin/Max`
  в NDC и device-Z; **`bCrossesNearPlane = MinW <= MaxZ`**; при `MinW <= 0` rect = весь экран.
  Матрицы — БЕЗ джиттера (правило 9).
* `GetScreenRect(rectNdc, viewSizePx, footprint=4)` → пиксели (правило центра пикселя), `hzbTexels =
  pixels >> 1`, `level = MipLevelForRect(hzbTexels, 4)`. **Дельта 1:** размер текселя мипа =
  `1.0 / max(1, hzbSize >> level)` от РЕАЛЬНЫХ размеров (`D.hzbWidth/Height`), не экспонентный трюк.
  **Дельта 2:** кламп координат к `(size >> level) − 1` вместо опоры на pow2.
* `GetMinDepthFromHZB(rect, level)` — четыре `GatherRed` (SM 6.x есть, правило 6 CSM-плана), min;
  вырожденные строки/столбцы футпринта маскируются **1.0** — нейтральный элемент для `min`, как у
  UE (`NaniteHZBCull.ush:158-159`; они тоже reverse-Z, так что «1.0 = ближе всего» и не влияет на
  минимум). Кейс «вырожденный футпринт» входит в self-test — проверять там, а не рассуждением.
* `IsVisibleHZB(rect)`: `rect.depthNearest >= minDepth` (reverse-Z, как у UE); `RoundUpF16(depth)` для
  боксов, принадлежащих самим окклюдерам (самоокклюжен) — как в `BuildInstanceDrawCommands.usf:200`.
* CPU-зеркало в `sources/rendering/visibility/HzbCull.h` (те же формулы на `float`), нужно валидатору.

Self-test `--hzb-cull-selftest` (по образцу `--gbv-selftest`/`--cull-benchmark`, вердикт в
`logs/hzb_cull_selftest.log`): синтетическая пирамида (плоскость-окклюдер на d, дырка), набор боксов:
перед плоскостью / за / пересекающий near / у края экрана / целиком вне экрана / крошечный (< 1 текселя)
— ожидаемые вердикты заданы руками; GPU-версия через `RecordComputeDispatch` над буфером боксов и
readback; CPU-зеркало обязано совпасть с GPU до бита вердикта.

### Что сделано (как построено, с отличиями от текста выше)
* `shaders/hzb_cull.hlsli` — транскрипция `NaniteHZBCull.ush` с именами UE (`HzbMipLevelForRect`,
  `HzbGetScreenRect`, `HzbGetMinDepth`, `HzbIsVisible`, `HzbBoxCullFrustumPerspective`); текстура
  и размер мипа 0 идут параметрами (`Texture2D<float> hzb, uint2 hzbMip0Size`) — библиотека не
  знает регистров потребителя. Row-vector `mul(p, M)`, reverse-Z, FURTHEST-цепочка.
* **Дельта 1 переформулирована:** вместо «размер текселя от реальных размеров» — **целочисленный
  `Load(int3(texel, level))`**. У UE `GatherLODRed` — расширение компилятора, которого в SM 6.0
  нет; их же fallback (`SampleLevel` по 16 текселям) в целых координатах не требует текселя вовсе.
  16 `Load` вместо 4 gather — цена enabler'а, S5 пересмотрит при замере.
* **Дельта 2 — не «страховка», а корректность:** мип-0 тексель `t` на уровне `L` = `t >> L`, и при
  ширине 617 → 308 (floor) тексель 616 >> 1 = 308 = за краем. `hzb_build_cs` сложил хвост в
  ПОСЛЕДНИЙ тексель, кламп `(max(1, size >> L) − 1)` попадает ровно туда. Кейс `corner_fold`
  бьёт именно в него (y = 89 при высоте уровня 89).
* Маскировка вырожденных строк/столбцов — повтором крайнего текселя (`min(x + {0,1,2,3}, x1)`),
  как в fallback UE; для `min` это то же, что их `1.0` в gather-пути.
* **`RoundUpF16` НЕ транскрибирован:** он компенсирует f16-квантование HZB UE; наша R32 хранит
  минимум точно, а «+1 ulp f32» не покрыл бы ошибку проекции углов. Если S5 покажет ложные
  самоокклюзии — добавлять эпсилон по замеру, не по аналогии.
* CPU-зеркало `sources/rendering/visibility/HzbCull.h` (`namespace hzb`), тот же порядок float-операций;
  загрузчик текселя — коллбэк, кламп внутри `GetMinDepth`.
* Self-test — **автономный харнесс** `sources/rendering/visibility/HzbCullSelfTest.cpp` по образцу
  `--rt-smoke` (своё устройство, без окна и рендерера; dxc с флагами движка `-Zpr -HV 2021`, RS из
  DXIL), а не дispatch внутри кадра: шаг «ничего не меняет в картинке» и не должен трогать кадр.
  Синтетика: вью 1234×717 (нечётное, не pow2 — все fold-правила билда срабатывают), reverse-Z
  перспектива, плоскость-окклюдер на 10 м с дырой [700,800)×[300,400) px; пирамида 617×359, 10
  уровней, редукция = `hzb_build_cs` на CPU. Матрицы нетривиальны, но точны во float (камера в
  (3,2,−5) + `localToWorld = Translation(eye)` → локальные координаты = view). 13 кейсов
  (перед/за/в дыре/на кромке дыры/near/за кадром/за far/1–2 px/крупный → грубый мип/угол с
  fold/±5–15 мм от плоскости). Сравнение GPU↔CPU по ПОЛЯМ: пиксельный rect, тексельный rect,
  уровень, `minDepth` побитно, флаги, вердикт; вердикт CPU ↔ рукописное ожидание.
  Запуск `--hzb-cull-selftest` → session-лог (категория `render.validation`: строка на кейс +
  вердикт `hzb cull self-test: PASS ...`), код выхода = число провалов. Отдельных файлов нет. `tools/check_shaders.py` знает `hzb_cull_selftest_cs.hlsl`.

**Замер 2026-09-03:** `PASS 13 cases, gpu == cpu mirror`, 1.1 с. Поле `depth` (не сравнивается)
расходится в последней цифре у половины кейсов (0.004267426 vs 0.004267425) — FMA-контракция
GPU против раздельных mul/add CPU; кейсы держат запас от границ пиксельных центров и глубины
именно поэтому. Картинка не затронута (ничего в рендере библиотеку не вызывает).

### Критерий приёмки
* ✅ Self-test: все 13 кейсов, GPU == CPU по всем полям; `tools/check_shaders.py hzb` — 2/2.
* ✅ Ни одного изменения в картинке (шаг не подключён к рендеру).

### Откат
Файлы не используются.

---

## S3a. Hardware occlusion queries + история (транскрипция `SceneOcclusion.cpp` / `FPrimitiveOcclusionHistory`) — ✅ СДЕЛАНО 2026-09-04 (uncommitted, **дефолт `queries`**)

**Зависит от:** S0, S1 (чанки как сабпримитивы). **Эффект:** пиксельно-точная окклюзия CPU-пути
против полноразрешённой opaque-глубины, с историей, которая гасит мигание; spot/point с закрытой
сферой влияния пропускают свой теневой пасс целиком. **Риск:** средний — латентность и её правила;
это самый «правиловый» шаг документа, транскрибировать, не изобретать.

### Почему
Дефолт UE на десктопе (`r.HZBOcclusion = 0`, `SceneVisibility.cpp:128-131`; `r.AllowOcclusionQueries`,
`:354-366`). Запрос — реальный растр бокса против глубины после base pass: тонкий окклюдер (ствол,
столб, перила) закрывает ровно то, что закрывает; HZB-тест (S3b) на половинном разрешении и 4×4
футпринте такое пропускает. История (`FPrimitiveOcclusionHistory`) превращает «результат прошлого
кадра» в стабильную видимость без мигания на границе.

### Код
1. **Query heap.** `ID3D12QueryHeap` типа `D3D12_QUERY_HEAP_TYPE_OCCLUSION` (счётчик сэмплов — нужен
   истории для `LastPixelsPercentage`, как `RQT_Occlusion` у UE; `BINARY_OCCLUSION` дешевле, но
   стохастика ре-теста без процента не работает). Ёмкость — литерал `kMaxOcclusionQueries` (16384)
   × `kFrameCount` регионов; `ResolveQueryData(heap, first, count, readback, offset)` в readback-буфер
   региона кадра — тот же паттерн, что `valReadback_` валидатора (`ShadowGpuData.cpp:1970`, `:2137`).
   Латентность `vis.queryLatency`: **1** = как UE (`r.NumBufferedOcclusionQueries = 1`,
   `ConsoleManager.cpp:3989-3994`) — в начале кадра N ждать фенс кадра N−1 (у UE `Map` тоже блокируется
   на GPU, `SceneOcclusion.cpp:846-853`); **`kFrameCount`** = без стола, латентность 3 кадра. Дефолт —
   по замеру стола в S0-стрессе.
2. **Пасс `Main_OcclusionQueries`** сразу после `Main_GBuffer` (UE: `RenderOcclusion` после
   BasePass, до transparent — стекло и вода не окклюдеры). Граф: `Use(depth, DEPTH_READ)`, RTV нет
   (`OMSetRenderTargets(0, nullptr, FALSE, &dsv)`, как теневые пассы). PSO `occlusion_query.hlsl`:
   VS строит бокс из per-instance `{center, extent}` (структурированный буфер в `Ring`) по
   `SV_VertexID` и общему IB на 16 боксов (`SceneOcclusion.cpp:335-363`); PS пустой (у UE он есть только
   для show-flag `OcclusionMeshes`, `:1293-1298`). Depth `GREATER_EQUAL` **без записи** (наш reverse-Z-
   эквивалент их `CF_DepthNearOrEqual`, `:1243-1265`), color write mask 0, растр как у UE — front faces;
   «камера внутри бокса» в запрос не попадает: её ловит правило near-plane (п.4).
   Опция `vis.downsampledQueries` = `r.DownsampledOcclusionQueries` (`:79-99`, тест против half-res
   max-depth) — второй этап, у нас есть готовая half-res `D.hzb` мип 0 как источник.
3. **Батчеры** — транскрипция `FOcclusionQueryBatcher` (`SceneRendering.h:342-396`,
   `SceneOcclusion.cpp:465-534`): **индивидуальный** (1 бокс = 1 запрос) и **групповой**
   (`OccludedPrimitiveQueryBatchSize = 16` боксов = 1 запрос); `BatchPrimitive` пишет 8 углов в
   динамический VB (`Ring`), `Flush` = `BeginQuery / DrawIndexedInstanced(12·N) / EndQuery` на батч;
   порядок флаша `r.OcclusionQueryDispatchOrder` (групповые первыми, `:87-94`, `:1383-1404`). Троттлинг
   числа запросов в полёте — как `SceneVisibility.cpp:3012-3072` (порог, минимум 10 % прогресса,
   сортировка по `lastQuerySubmitFrame`); у нас потолок = `kMaxOcclusionQueries`.
4. **История** `sources/rendering/visibility/OcclusionHistory.*` — транскрипция
   `FPrimitiveOcclusionHistory` (`ScenePrivate.h:108-266`): `pendingQuery[kFrameCount]`,
   `pendingQueryFrame[]`, `grouped[]`, `lastTestFrame`, `lastConsideredFrame`, `lastProvenVisibleTime`,
   `lastConsideredTime`, `lastPixelsPercentage`, `wasOccludedLastFrame`, `stateWasDefiniteLastFrame`,
   `becameEligibleForQueryCooldown`, `hzbTestIndex` (S3b). Решающее дерево — `SceneVisibility.cpp:2700-2935`,
   **дословно**:
   * записи нет → создать, **виден**, не definite (`:2706-2712`);
   * `GetHistoryRevision()` изменился / первый кадр / `lastRenderTime + probablyVisibleTime < now` /
     большое движение камеры (`CameraRotationThreshold`, `CameraTranslationThreshold` — у UE свойства
     UEngine; у нас `vis.cutAngle`/`vis.cutDistance`) → `ignoreExistingQueries`, все видны (`:5367-5380`,
     `:2715-2719`);
   * результат есть: `occluded = (pixels == 0)`, `pixels% = pixels · oneOverNumPossiblePixels`
     (render-разрешение, не output — DLSS!), `definite = !grouped` (`:2738-2795`);
   * результата нет: `k > 1` → наследовать прошлое состояние; иначе `occluded =
     (lastProvenVisibleTime + probablyVisibleTime < now)` (`:2774-2783`);
   * бокс пересекает near-plane (`nearPlane.PlaneDot(origin) < -BoxPushOut`) → виден, definite, без
     запроса (`:2831-2846`, `:2920-2925`); ближе `neverOcclusionTestDistance` → не тестировать (`:2827-2830`);
   * планирование запроса (`:2866-2902`): occluded в прошлом кадре → запрос **каждый кадр, групповой**;
     виден и definite → стохастика `f = max(lastPixels% / maxOcclusionPixelsFraction, 1);
     run = f · rnd < maxOcclusionPixelsFraction`; всё остальное → индивидуальный каждый кадр;
     сабпримитивы — никогда не групповые (`:2868`);
   * `lastProvenVisibleTime = now` только при `visible && definite` (`:2930-2933`);
   * боксы: `+ OCCLUSION_SLOP` (**0.01 м**) всегда (`ScenePrivate.h:69`); после паузы в тестах >
     `framesNotTestedToExpand` (5) — расширение на `expandNewlyTested` в течение
     `framesToExpandNewlyTested` (2) кадров (`SceneVisibility.cpp:206-236`, `:2810-2823`); наш пад под
     ветер (см. S3b.1);
   * `TrimHistory` каждые 6 кадров по `probablyVisibleTime` (`SceneOcclusion.cpp:262-281`).
   Параметры `probablyVisibleTime`, `maxOcclusionPixelsFraction`, `cameraRotationThreshold`,
   `cameraTranslationThreshold` у UE — `UPROPERTY(config)` класса `UEngine`
   (`Source/Runtime/Engine/Classes/Engine/Engine.h:1711-1725`), не cvar'ы; **`Config/` в дропе НЕТ**,
   так что их дефолты из исходников не восстановить — брать документные 8.0 с / 0.1 / 45° / 10000 см
   (= 100 м) и **отметить в коде как непроверенные**. У нас `--set=vis.probablyVisibleTime`,
   `vis.maxPixelsFraction`, `vis.cutAngle`, `vis.cutDistance` (метры!).
5. **Сабпримитивы** (§2.1, `:2679`, `:2968-2980`): чанки террейна и члены инстанс-батчей — свои bounds
   и свои запросы (никогда групповые); объект виден ⇔ **любой** сабпримитив виден; результат — в
   `chunkVisCamera_` (S1) и в фильтр членов батча (батч строится из видимых, F2).
6. **Запросы для теней** (`SceneOcclusion.cpp:1109-1192`): spot/point — **сфера влияния света одним
   запросом** (`SOQ_LightInfluenceSphere`, `:1135-1144`); закрыта → `Pass_SpotShadows`/`Pass_PointShadows`
   для этого слота и его лайтинг-пасс пропускаются (потребитель — слоты `LightManager`; кастеры
   света при этом НЕ кулятся). Каскады — `vis.occludeCascades`, дефолт **0** (UE: `r.Shadow.
   OcclusionCullCascadedShadowMaps = 0`, `:56-62`), каскад 0 — **никогда** (`:1149`); VSM — никогда
   (`:1130-1134`, `ShadowSetup.cpp:4510-4519`). Нет результата → **не** occluded (`IsShadowOccluded`,
   `:283-306`).
7. **Потребитель** — общий с S3b: `OcclusionHistory::IsOccluded(slot)` в `Scene::PrepareViewQueue`
   для камерного вью (предикат `frustum.Intersects(b) && !occluded`); теневые вью историю **не
   читают** (Debug-ассерт для `SceneView::Type::Shadow`). Переключатель `vis.method = off | queries |
   hzb`; дефолт после приёмки — **queries** (как у UE), до неё — `off`. Счётчики в readout:
   `queriesIndividual`, `queriesGrouped`, `objectsOccluded`, `chunksOccluded`, `lightsOccluded`,
   фактическая латентность в кадрах.

### Что сделано (как построено, с отличиями от текста выше)
* **`sources/rendering/visibility/OcclusionHistory.{h,cpp}`** (`namespace vis`) — CPU-половина:
  `OcclusionHistory` = `FPrimitiveOcclusionHistory` (записи в `robin_hood::unordered_map` по ключу
  `{RenderableObjectBase*, sub}`, sub 0 = объект, 1 + индекс чанка = чанк, у света — его адрес) +
  дерево решений `ProcessPrimitive` (`SceneVisibility.cpp:2700-2935`) дословно + два батчера
  (`FOcclusionQueryBatcher`, групповой по 16 / индивидуальный) + правила сброса (`:5367-5380`) +
  `TrimHistory` раз в 6 кадров. Кадр: `BeginFrame` (главный поток, после `CalcMatrices`, до камерной
  задачи: результаты кадра N − latency, `ignoreExistingQueries`, trim, пустой план) → `Consider`
  (камерная задача, по одному на примитив/чанк: вердикт + постановка запроса) → `EndConsider`
  (план в порядке диспатча: групповые первыми). Ручки `vis::g_occlusion` = `--set=vis.method /
  queryLatency / probablyVisibleTime / maxPixelsFraction / cutAngle / cutDistance / neverTestDistance`;
  UEngine-значения помечены в коде как непроверенные (в дропе нет Config/).
* **`OcclusionQueries.{h,cpp}`** — GPU-половина: `ID3D12QueryHeap` OCCLUSION на `16384 × kFrameCount`,
  readback-буфер по региону на слот кадра, статический IB на 16 боксов (`GCubeIndices`), PSO
  `shaders/occlusion_query.hlsl` (VS = 8 углов через **джиттерный** viewProj кадра — глубина
  растрирована им же; PS пустой; depth `GREATER_EQUAL` без записи, colour write 0, `numRT 0`).
  `Record` = динамический VB из `AllocDynamic`, `BeginQuery / DrawIndexedInstanced(36·n, base =
  firstBox·8) / EndQuery` на батч, `ResolveQueryData` использованного диапазона; `ReadResults`
  ищет слот по номеру кадра (`GetCurrentFrameIndex` не обязан совпадать с `frame % 3`).
* **Пасс `Main_OcclusionQueries`** (`SceneRenderer_Graph.cpp`, сразу после `Main_GBuffer`; `Main_Hzb`
  объявляет его пререквизитом, чтобы `DEPTH_READ` встал раньше SRV-потребителей глубины): билдер
  молчит без плана; тело (`SceneRenderer_Geometry.cpp::Pass_OcclusionQueries`) биндит `D.dsv`
  read-only тем же способом, что скайбокс под `DEPTH_READ`, render-res viewport.
* **Потребитель** — `Scene::ApplyOcclusion` в `PrepareViewQueue` камерного вью после `SelectLods` и до
  батчинга: opaque-бакеты, чанкованный объект = сабпримитивы (`Consider` на каждый чанк, прошедший
  фрустум, никогда групповой; occluded → `chunkVisCamera_[s] = 0`; объект уходит, только если ВСЕ
  его рассмотренные чанки occluded), остальные — по AABB объекта, групповой допускается.
  Debug-ассерт на тип вью. Счётчики: `objectsOccluded`, `chunksOccluded`, строка
  `occlusion method=… individual=… grouped=… dropped=… tested=… latency=… entries=… ignored=…
  lightsOccluded=…` в readout и в табе Render (комбо метода + слайдер латентности).
* **S3a.6, сферы влияния локалов** — `Scene::ConsiderLightOcclusion` на главном потоке ДО камерной
  задачи (история однопоточна; задача стартует после): спот — `GetConeBounds()` (AABB конуса,
  теснее сферы), point — бокс сферы; индивидуальный запрос; вердикт → `spotLightOccluded_[i]` /
  `pointLightOccluded_[i]` (через `SceneFrameData`). Occluded свет: (1) его теневые вью получают
  **reject-all фрустум** (`RejectAllFrustum()`: одна плоскость на 1e30) — CPU-cull пуст, GPU-cull
  (те же плоскости) пуст, пасс чистит регион атласа и ничего не рисует, барьерный поток не тронут;
  (2) в GPU-буфере света `range = 0`, `intensity = 0` (`SceneRenderer.cpp`, слот по индексу
  сохранён). Только Legacy-атлас (`!render::VsmActive()`) — VSM вердикты не берёт (правило UE).
  Кастеры света НЕ кулятся.
* **Латентность.** Дефолт `kFrameCount` = 3 (без стола); `vis.queryLatency:1` = ждать фенс слота
  кадра N−1 (`Renderer::WaitForFrameSlot`, публичная обёртка приватного `WaitForFrame`) — как
  блокирующий `Map` UE.
* **`--set` в стресс-харнесс:** разбор `--set=` поднят выше ветки `--scene-stress`
  (`ParseFixedSettingsArg`), применение вынесено в `App::ApplyFixedSettings`, харнесс зовёт его
  после бутстрапа — иначе GBV-гейт валидировал дефолт (`off`), а не пасс.

**Отличия от текста плана.** (1) Растр обеих сторон бокса (`CULL_NONE`), не только фронтальных:
тест по задним граням считал бы дальнюю сторону бокса, лежащую ЗА поверхностью самого объекта —
стена окклюдила бы саму себя; цена 12 треугольников на бокс. (2) Прозрачные бакеты в S3a не
кулятся: их зеркалит отсортированный список transparent-записей очереди, снятие из одного без
другого рисует объект всё равно; UE кулит и их — отдельный шаг. (3) Троттлинг п.3 не транскрибирован:
потолок жёсткий (`kMaxOcclusionQueries`), сверх него — `dropped` в readout (примитив держит прошлое
состояние). (4) `vis.downsampledQueries` не сделан. (5) Ключ истории — адрес объекта, не уникальный
id: смена `staticSetVersion_` (добавление/удаление) чистит историю целиком (у UE id уникален).
(6) **Стохастический ре-тест распространён на сабпримитивы** (у UE сабзапрос идёт каждый кадр:
«custom code knows what it is doing and will group internally»): наши сабпримитивы — 60-метровые
чанки, и 615 видимых чанков, запрашиваемых каждый кадр, стоили 0.7 мс CPU-записи за 3 % среза
(остров K=4). После дельты 1100 → 550 индивидуальных запросов, запись 0.70 → 0.38 мс.

### Ловушки
* **Слот readback ≠ `frame % 3`.** `GetCurrentFrameIndex()` не обязан совпадать с номером кадра по
  модулю (ресайз, первый кадр) — `OcclusionQueryHeap` помнит, какой кадр в каком слоте, и ищет по
  номеру.
* **История однопоточна, а prepare-задачи вью параллельны** (урок S1): `Consider` только на
  камерной задаче; локальные источники рассматриваются на главном потоке ДО её старта. Вердикты
  читаются потребителями после `tasks.Wait(mainViewTask)`.
* Джиттер: боксы через `GetViewProjMatrix()` (с джиттером DLSS), не через no-jitter — глубина, против
  которой тест, растрирована джиттерным. Правило 9 (§0) — про cull-фрустумы, не про тест по глубине.
* `LengthSquared` у `Math::float3` нет — `Dot(self)`.

### Замер 2026-09-04 (Release, `--shadow-mode=legacy --dlss=off --wind-freeze --set=exposure.autoExposure:0
--set=ocean.visible:0`, латентность 3, кадр 601)

| камера | objects frustum | occluded | chunks drawn (S1) → S3a | drawCalls | queries ind/grp | картинка off→on |
|---|---|---|---|---|---|---|
| стена (occlusion_test) | 170 | **167** | 26 → **2** (24 occluded) | 77 → **5** | 24 / 11 | 0.038 % (пол 0.042 %) |
| камера теней (wind_test) | 317 | 0 | 47 → **21** (26 occluded) | 80 → 54 | 77 / 0 | 0.021 % |
| стена + свет (S3a.6) | 170 | 167 | 26 → 2 | 77 → 5 | 25 / 11, **lightsOccluded=1** | 0.042 % |

Каскады c0..c3 во всех прогонах побайтно те же, что при `off` (тени камерой не кулятся). Полёт
вдоль стены (`--cam-fly=0,6`, кадр в движении за концом стены, латентность 3): дифф off→on 0.305 %,
и это HUD-цифры FPS плюс кромка пальм у края кадра — ни одного пропавшего объекта.
GBV-гейт: `--scene-stress-gbv=20 --set=vis.method:1 --level=data/levels/occlusion_test.json` →
`verdict: CLEAN`, 0 GBV-ошибок (после починки `--set` в харнессе).

### Стоимость на K=4 (`--profdump`, 30 с прогрева, Legacy, DLSS по умолчанию — рецепт таблицы F1; по два прогона)

| | стена K=4 off | стена K=4 **on** | остров K=4 off | остров K=4 **on** (после дельты 6) |
|---|---|---|---|---|
| objects frustum / occluded | 3641 / 0 | 3641 / **3634** | 3714 / 0 | 3714 / 98–120 |
| drawCalls, primitives | 116, 3.03 M | **19, 0.57 M** | 112, 2.60 M | 98–100, 2.58 M |
| queries ind / grp | — | 540 / 227 | — | 549–562 / 7–8 |
| GPU.Frame, мс | 2.865 / 2.851 | **2.712 / 2.693 (−5.5 %)** | 2.652 / 2.694 | 2.693 / 2.678 (шум) |
| GPU `ExecuteBundles` + `RenderObjectBatch` | 0.270 + 0.125 | 0.037 + 0.121 | 0.233 + 0.266 | 0.219 + 0.282 |
| GPU `Pass_OcclusionQueries` | — | 0.052 / 0.050 | — | 0.031 / 0.030 |
| CPU.Frame, мс | 2.875 / 2.859 | **2.723 / 2.698 (−5 %)** | 2.659 / 2.701 | 2.698 / 2.681 (шум) |
| Whole Cycle, мс | 2.435 / 2.436 | 2.471 / 2.466 (ровно) | 2.263 / 2.296 | 2.463 / 2.433 (**+0.15–0.2**) |
| CPU `Pass_OcclusionQueries` (запись, на воркере) | — | 0.528 / 0.529 | — | 0.383 / 0.379 |
| CPU `prepareQueue` (в т. ч. `Consider`) | 1.128 / 1.104 | 1.220 / 1.198 | 0.761 / 0.765 | 1.059 / 1.039 |
| CPU `RenderObjectBatch.Async` | 0.433 / 0.436 | 0.106 / 0.105 | 0.554 / 0.566 | 0.512 / 0.500 |

Читается так: за стеной occlusion окупается на обеих шкалах (GPU −5.5 %, CPU-кадр −5 %, Whole
Cycle ровно: 0.5 мс записи запросов на воркере съедает 0.33 мс сэкономленного сабмита). На
открытом виде сверху (3.7k видимых объектов, 3 % среза) GPU и CPU-кадр в шуме, а Whole Cycle
+0.15–0.2 мс — это `Consider` на 4.3k записей (хэш-lookup + трансформ 990 боксов чанков), не
запросы. Следующая оптимизация, если понадобится: слот истории прямо в объекте вместо хэша и
кэш мировых боксов чанков из `SelectLod`. K=1 на сегодняшних сценах (~900 объектов) — четверть
этого. **Латентность 1** (`vis.queryLatency:1`, стена K=4): `Renderer::WaitForFrame` 2.04 мс,
CPU.Frame 2.72 → 4.76 мс — стол на кадр, дефолтом быть не может; остаётся ручкой.
**Латентность 2** (один прогон, та же стена): CPU.Frame 2.70 → 2.98 (+10 %), Whole Cycle 2.47 → 2.92
(+18 %), и GPU.Frame 2.69 → 3.02 — ожидание фенса кадра N−2 в начале кадра N рвёт подачу команд,
GPU получает пузыри. Не бесплатно и не посередине: дефолт остаётся 3.

### Критерий приёмки
* ✅ Статичная камера «стена» и камера теней: картинка идентична (пол); `objectsOccluded` 167,
  `chunksOccluded` 24 / 26.
* ✅ Тонкий окклюдер против S3b: камера теней, остров — queries 26 чанков, тестер 5 (S3b, Ловушки).
* ✅ `--cam-fly` при латентности 3: кадр в движении без дыр; латентность 1 реализована и
  замерена: стол 2.0 мс/кадр.
* ✅ Спот за стеной: `lightsOccluded=1`, картинка та же; «выход из-за стены» не гонялся.
* ✅ Стоимость на K=4 — таблица выше; запросы дешевле того, что экономят, там, где есть что
  экономить, и в шуме там, где нет.
* ✅ Тени каскадов не зависят от `vis.method` (readout c0..c3 идентичен); VSM код не исполняет.
* **Дефолт `queries` с 2026-09-04**, как у UE на десктопе. `--set=vis.method:0` — откат.

### Откат
`vis.method:off` — пасс не регистрируется, история не обновляется, предикат чисто фрустумный.

---

## S3b. HZB-тестер с readback (транскрипция `FHZBOcclusionTester`) — альтернатива S3a на той же истории — ✅ СДЕЛАНО 2026-09-04 (uncommitted, ручка `vis.method:2`; дефолт остаётся `queries`)

**Зависит от:** S1, S2, история из S3a. **Эффект:** тот же потребитель, что у S3a, без draw'а боксов:
один compute-dispatch на все слоты против текущей HZB. **Риск:** низкий — консервативнее S3a.

### Почему
У UE это `r.HZBOcclusion = 1` (`SceneVisibility.cpp:3102-3107`): дешевле на десятках тысяч примитивов,
где батчи боксов уже стоят своего; в нашем движке HZB и паттерн readback есть — цена шага мала. Держать
оба метода = возможность A/B на одной сцене (тонкие окклюдеры) и запасной путь на железе, где запросы
дороги.

### Код
1. **Слоты** — общие с S3a (та же история, `hzbTestIndex`): бокс = мировой AABB `+ 0.01 м` **и** ветровой
   пад для качающихся мешей (наша дельта: `FillBounds` теней не паддит — там нельзя, здесь нужно).
   Структурированный буфер `{center, extent}` в `Ring` вместо 256×256-текстуры UE (`SceneOcclusion.cpp:
   946-1058`) — у нас это лишняя косвенность; кап 65536 = литерал шейдера.
2. **Пасс `Main_VisTest`** сразу после `Main_Hzb` (граф: `Use(D.hzb, NON_PIXEL)`, `Use(results, UAV)`),
   compute `vis_test_cs.hlsl` (`[numthreads(8,8,1)]`, 1-D): на слот — `BoxCullFrustumPerspective` →
   near-plane ⇒ виден → `GetScreenRect(…, 4)` → `IsVisibleHZB` (S2); байт на слот в `UavRing`; копия в
   readback (`CopyBufferRegion`, как валидатор), чтение через `vis.queryLatency` кадров (те же два режима,
   что в S3a.1). Это дословно `HZBOcclusion.usf:30-41` + `SceneVisibility.cpp:3133-3171`.
3. **Применение** — через историю S3a (результат без пикселей = бинарный: `definite = true`, стохастика
   ре-теста не нужна — тест каждый кадр на все слоты стоит один dispatch).

### Что сделано (как построено, с отличиями от текста выше)
* **`sources/rendering/visibility/HzbOcclusionTester.{h,cpp}`** (`vis::HzbOcclusionTester`) — GPU-половина:
  кольцо боксов в UPLOAD-куче (`kMaxHzbTests` = 65536 = UE `SizeX·SizeY` × `kFrameCount` регионов ×
  32 байта = 6 МБ, persistently mapped, SRV на регион), буфер вердиктов DEFAULT+UAV на ОДИН регион
  (256 КБ: пишется диспатчем и копируется в том же пассе), readback на `kFrameCount` регионов
  (768 КБ), свой non-shader-visible heap на 3 SRV + 1 UAV (в shader-visible стейджится
  `StageSrvUavTable`, как у всех пассов), compute-материал `shaders/vis_test_cs.hlsl`.
  `RecordTest` = min/max плана → `{center, extent}` в регион слота, CB = **джиттерный** viewProj +
  проекция + viewRect + размер мипа 0, `Dispatch(ceil(n/64))`; `RecordReadback` =
  `CopyBufferRegion` в регион слота; `SlotOfFrame`/`ReadResults` — как у `OcclusionQueryHeap`
  (слот по номеру кадра).
* **`shaders/vis_test_cs.hlsl`** — транскрипция `HZBTestPS` (`HZBOcclusion.usf:30-41`) на
  `hzb_cull.hlsli`: `HzbBoxCullFrustumPerspective(center, extent, identity, worldToClip,
  viewToClip)` → `isVisible && !crossesNearPlane` → `HzbGetScreenRect(viewRect, …, 4)` →
  `HzbIsVisible`. Без теста `overlapsPixelCenter` — у `HZBTestPS` его нет (см. Ловушки).
* **История** (`OcclusionHistory`): `BeginFrame(…, OcclusionMethod)` вместо `bool`, `FrameResults::
  hzbVisible` (uint на индекс теста), `AddHzbBounds` = `FHZBOcclusionTester::AddBounds` (индекс в
  списке кадра = индекс теста, едет в тех же `pendingQuery`-слотах — поле `hzbTestIndex` убрано),
  ветка Hzb в `Consider`: вердикт бинарный и **definite** (`:2727-2733`), тест на КАЖДЫЙ рассмотренный
  примитив каждый кадр (`:2859-2862`, без группировки и стохастики), `lastPixelsPercentage` как при
  отсутствии результата. Правило near-plane, правила сброса, trim, латентные слоты — общие с S3a.
* **Пасс `Main_VisTest`** (`SceneRenderer_Graph.cpp`, пререквизит `Main_Hzb`, потребителей нет —
  читает CPU через `vis.queryLatency` кадров): две точки — (HZB `NON_PIXEL`, вердикты `UAV`) →
  (вердикты `COPY_SOURCE` = каноническое состояние, в нём кадр их и оставляет). Гейт повторяет гейт
  пирамиды: план с методом Hzb на кадре без HZB не тестируется против того, что лежит в текстуре.
  Тело `SceneRenderer_Geometry.cpp::Pass_VisTest`: обе точки эмитятся всегда (правило «без раннего
  выхода после `BeginCL`»).
* **Выбор производителя** — `Scene::PrepareViews`: `vis.method` → тот производитель, у которого
  есть устройство (`EnsureResources` на главном потоке; билдер проверяет только `Ready()`), одна
  история на обоих; `frameData_.hzbTester`. Комбо в табе Render: «hzb tester»; `--set=vis.method:2`;
  `tools/check_shaders.py` компилирует `vis_test_cs.hlsl` (второй потребитель библиотеки S2).

**Отличия от текста плана.** (1) uint на бокс, не байт: структурированный буфер, побайтная упаковка
потребовала бы `ByteAddressBuffer` + атомики ради 192 КБ. (2) **Ветровой пад не добавлен**: боксы те
же, что у S3a (паритет — иначе A/B мерил бы боксы, а не метод); полёт S3a без него дыр не показал, а
HZB-тест консервативнее растра бокса (footprint 4×4 на уровне покрывает не меньше прямоугольника).
(3) Своё UPLOAD-кольцо, не `ShadowGpuData::Ring` (приватная статика того класса). (4) Боксы плана —
общие `boxes` (min/max), в `{center, extent}` переводятся при заливке.

### Ловушки
* **`HZBTestPS` не проверяет `bOverlapsPixelCenter`** — эта проверка есть у Nanite-cull'а и у
  «модели потребителя» в self-test S2 (там `!overlapsPixelCenter` = не виден). Скопировав её в S3b,
  примитив с прямоугольником между центрами пикселей был бы срезан историей и выскочил бы, когда
  дорос до пикселя. У тестера прямоугольник без площади сохраняет валидный 1-тексельный footprint
  (`HzbGetScreenRect`) и проходит тест глубины как все.
* Буферы создаются в COMMON, что бы ни просили; каноническое состояние = где кадр ОСТАВЛЯЕТ ресурс
  (`COPY_SOURCE` после копии) — переход `COPY_SOURCE → UAV` следующего кадра заодно упорядочивает
  копию до нового диспатча, поэтому регион вердиктов один.
* **Консервативность на острове.** Камера теней: queries срезают 26 чанков, тестер — 5. Footprint
  4×4 на выбранном уровне покрывает до 2× прямоугольника по оси, а бокс 60-метрового чанка за
  холмом упирается прямоугольником в горизонт — в footprint попадают тексели неба/дальнего
  рельефа (глубина 0), min по ним = «виден». Стена заполняет footprint целиком — там числа
  совпадают с queries один в один. Это и есть «тонкий окклюдер» из критерия приёмки: не баг.
* Слот readback ≠ `frame % 3` (урок S3a) — тестер тоже ищет слот по номеру кадра.

### Замер 2026-09-04 (Release, `--shadow-mode=legacy --dlss=off --wind-freeze --set=exposure.autoExposure:0
--set=ocean.visible:0`, латентность 3, кадр 601; `vis.method:0` → `vis.method:2` в одном бинаре)

| камера | objects frustum | occluded | chunks drawn (S1) → S3b | drawCalls | тестов | картинка off→hzb |
|---|---|---|---|---|---|---|
| стена (occlusion_test) | 170 | **167** | 26 → **2** (24 occluded) | 77 → **5** | 193 | 0.044 % (пол 0.031–0.042 %) |
| камера теней (wind_test) | 317 | 0 | 47 → 42 (**5** occluded; queries: 26) | 80 → 75 | 359 | 0.016 % |

Стена: те же 167 / 24 / 5, что у S3a, `lightsOccluded=1` (сферы локалов идут через ту же историю).
Каскады c0..c3 побайтно те же, что при `off`. Полёт вдоль стены (`--cam-fly=0,6`, тот же путь и
момент, что у S3a): дифф off→hzb 19.2 %, **но off→off между двумя запусками 15.3 %** — кадр в
движении ловится по часам, и два запуска ложатся на разные миллиметры пути (пара S3a попала в один
и тот же момент случайно: 0.3 %). Маска диффа — шиммер песка и кромки пальм, в обоих кадрах те же
пальмы, ни одного пропавшего объекта (смотрено глазами, `s3b_fly_side.png`).

### Критерий приёмки
* ✅ Те же, что S3a п.1, 3, 6: стена и камера теней — картинка = пол; тени не зависят от метода;
  полёт без дыр.
* ✅ На тонком окклюдере S3b консервативнее S3a: остров, камера теней — **5 чанков против 26**
  (зафиксировано, причина в Ловушках), стена — один в один.
* ✅ Перф: `Main_VisTest` на K=4 — один dispatch, GPU 0.028–0.031 мс (план: ≤ 30 мкс), запись
  0.06 мс; таблица ниже.
* ✅ GBV-гейт: `--scene-stress-gbv=20 --shadow-mode=legacy --set=vis.method:2` (Debug) →
  `scene-stress verdict: CLEAN after 20 iterations`, тестер создан в первом кадре (пасс задействован).
* Три конфигурации (Debug / Release / Release_Editor) собраны чисто; `tools/check_shaders.py` 51/51.

### Стоимость на K=4 (`--profdump`, 30 с прогрева, Legacy, DLSS по умолчанию — рецепт таблицы F1; hzb по два прогона, queries по одному, ОДИН бинарь)

| | стена K=4 **queries** | стена K=4 **hzb** | остров K=4 **queries** | остров K=4 **hzb** |
|---|---|---|---|---|
| objects occluded / chunks occluded | 3634 / 536 | **3634 / 536** (один в один) | 112 / 0 | 122 / 0 |
| drawCalls, primitives | 19, 0.57 M | 19, 0.57 M | 103, 2.58 M | 97, 2.58 M |
| запросов ind + grp / тестов | 540 + 227 | 4167 тестов | 540 + 7 | 4317 тестов |
| GPU.Frame, мс | 2.757 | **2.635 / 2.627 (−4.5 %)** | 2.657 | 2.654 / 2.629 (шум) |
| GPU пасс метода | `OcclusionQueries` 0.052 | `VisTest` 0.031 / 0.030 | 0.032 | 0.029 / 0.028 |
| CPU.Frame, мс | 2.765 | **2.644 / 2.637 (−4.5 %)** | 2.662 | 2.657 / 2.638 (шум) |
| Whole Cycle, мс | 2.515 | 2.465 / 2.450 (−2 %) | 2.434 | 2.401 / 2.407 (−1 %) |
| CPU запись пасса (воркер) | 0.530 | **0.061 / 0.061** | 0.380 | 0.060 / 0.061 |
| CPU `prepareQueue` (в т. ч. `Consider`) | 1.225 | 1.155 / 1.151 | 1.044 | 1.012 / 1.022 |
| CPU `RenderObjectBatch.Async` | 0.107 | 0.101 / 0.102 | 0.514 | 0.473 / 0.483 |

Читается так: там, где есть что срезать (стена), тестер срезает то же самое дешевле — нет ни
батчей боксов на воркере (0.53 → 0.06 мс), ни 767 запросов на GPU (0.052 → 0.030 мс), и это видно
в кадре целиком (−4.5 % на обеих шкалах); `prepareQueue` тоже чуть легче (нет батчеров и
стохастики, только push бокса). На открытом виде оба в шуме. Остров срезает 122 объекта против
112 у queries — не «строже», а ровнее: групповой запрос по 16 occluded-боксам с одним видимым
членом переводит все 16 в «видим» на кадр (правило UE), тестер групп не имеет — отсюда и разброс
98–120 у queries в таблице S3a. Где тестер проигрывает — на терренe за холмом (камера теней: 5
чанков против 26, см. Ловушки), и это единственная причина, по которой **дефолт остаётся
`queries`** (как у UE на десктопе): на сегодняшних сценах срез теней-камеры — чанки, и их queries
видят лучше. Ручка `vis.method:2` — для A/B на новых уровнях; если уровни окажутся «стеночными»
(интерьеры, город), тестер — кандидат в дефолт по этой таблице.

### Откат
`vis.method:off` / `queries`.

---

## S4. G-buffer на `ExecuteIndirect` (GPU-driven камерный путь) — ✅ СДЕЛАН 2026-09-04 (uncommitted, **дефолт ON**, ручка `gbuffer.indirect`)

**Зависит от:** S0 (стресс-сцена — иначе не измерить), S1. **Эффект:** CPU-сабмишн opaque-геометрии
исчезает как класс; предпосылка S5. **Риск:** высокий — материалы, PSO-варианты, object-id, паритет
картинки.

### Почему
Теневой путь уже GPU-driven (F5); камерный — `RenderObjectBatch` по объекту с bundles. Результат
GPU-теста видимости на CPU-пути потребить нельзя без readback'а и латентности (S3a/S3b); чтобы окклюжен
был «в этом же кадре» (S5), draw должен идти из args, которые пишет cull.

### Код (структура; детали материалов — см. подраздел ниже)
1. **Реестр камерных инстансов** — расширение `ShadowGpuData` до общего GPU-реестра или сестринский
   класс `GBufferGpuData` на тех же `Ring`/`UavRing`: `CasterBounds` (те же) + per-instance данные для
   G-buffer'а: мировая матрица (3×4), `objectId` (для `Main_ObjectIdReadback` и селекции), индекс
   материала, LOD-тир камеры (`GetCameraLod`) + fade (dithered crossfade — `lod-crossfade` память:
   fade едет по трём каналам, `InstancePerObject` расти НЕ должен — stride теней).
2. **Cull-кернел камеры** `gbuffer_cull_cs.hlsl` (копия `shadow_cull_cs.hlsl` на один вью): фрустум
   (16 плоскостей, литерал) → args `[group*4+lod]` + visible list (per-instance stream, slot 1) — **в S4
   без HZB**; S5 добавит.
3. **Draw**: один `ExecuteIndirect` на виртуальную группу (mesh × LOD), как `RecordIndirectShadowDraws`
   (`ShadowGpuData.cpp:1989`); PSO выбирается по группе (opaque / masked; complex/multi-slot остаётся
   на CPU-пути — как `OpaqueComplex` сегодня). Материалы группы биндит CPU перед `ExecuteIndirect`
   (444 групп теней сегодня — это дёшево); bindless — только если он уже есть у RT (уточняется).
4. **Паритет**: `--set=gbuffer.indirect:0|1`, оба пути живут в одном бинаре (как `shadow.indirect`).
   **⚠ Предпосылка:** CPU-путь теней и GPU-путь теней сегодня расходятся на 10.14 % пикселей
   (chip 2026-09-03, «Investigate CPU vs GPU shadow path mismatch»); та же дисциплина здесь: сначала
   найти, ПОЧЕМУ два пути дают разное, потом объявлять паритет.
5. `GBuffer_OpaqueSimple` становится `GBuffer_Indirect` (+ CPU-хвост для того, что не поехало);
   `Main_ObjectIdReadback` читает id из инстанс-стрима; `GBuffer_Selected` (stencil) — через тот же
   стрим (флаг выделения per-instance).

#### Материалы, буферы, сигнатуры — факты (2026-09-03)

* **Как G-buffer биндится сегодня** (`shaders/gbuffer_common.hlsli:38-70`, `gbuffer.hlsl:4-9`):
  `PerObject` CB в `b0` (world, prevWorld, baseColor, metalRough, `alphaCutoff` (−1 = без alpha-test),
  texOffsScale, texFlags, `objectId`, emissive, ветер ×4, `lodFade`), `PerView` в `b1`, `SurfaceParams`
  в `b2`, текстуры `t0..t2` (albedo, MR, normal) + `s0`; PSO/RS одни на opaque и masked — маска
  решается в PS по `alphaCutoff`, **отдельная PSO-пермутация для masked НЕ нужна** (в отличие от
  теней, где `MaskedShadowsActive()` выбирает PSO, `ShadowGpuData.cpp:356-665`). Инстансированный
  вариант (`gbuffer_inst*.hlsl`) — тот же `b0` как `InstanceArray { InstancePerObject inst[256] }`
  (`GBUFFER_MAX_INSTANCES = kMaxInstancesPerDraw = 256`) по `SV_InstanceID`; текстуры — per draw.
* **Что это значит для indirect**: CB-массив на 256 не масштабируется на произвольный visible list.
  Пермутация `GBUFFER_INDIRECT`: `InstancePerObject` (та же структура — ЗЕРКАЛО CB, правило
  «extend the namespace») лежит в `StructuredBuffer<InstancePerObject>` и читается по id инстанса из
  per-instance vertex stream slot 1 (как `CASTERID` у теней), `lodFade` — там же, по тиру виртуальной
  группы. `objectId` для `Main_ObjectIdReadback` и флаг селекции — поля той же записи.
* **Материал = группа**: группы теней — per submesh (B3: «submesh groups share their mesh's slice»,
  `ShadowGpuData.h:470`), у сабмеша один материал ⇒ CPU биндит `SurfaceParams` + `t0..t2` один раз
  перед `ExecuteIndirect` группы. 444 групп на wind_test — это сотни биндов, но не тысячи draw'ов; на
  K=4 число групп НЕ растёт (растут инстансы). Bindless (`ResourceDescriptorHeap[]`, SM 6.6, уже в
  деле у RT — `sources/rendering/rt/BindlessTable.h:16-34`, `GeometryInfo{vb/ib/albedoTexIndex/
  mrTexIndex}`) — **второй этап**: `MaterialRecord` в structured buffer, индексация в PS по id
  инстанса, один `ExecuteIndirect` на LOD-тир всей сцены. Не в S4: сначала паритет.
* **Mega-буферы теней пригодны как есть**: `megaVB_` копирует ВЕСЬ VB меша (`MegaCopy::vbBytes`,
  `megaStride_ = stride0` — полный вершинный стрид, `ShadowGpuData.cpp:1188`), input layout'ы
  `PosOnly_InstCasterId`/`PosNrmUV_InstCasterId` (`:1518-1590`) лишь читают подмножество атрибутов с
  тем же стридом. Для G-buffer'а нужен layout с полным набором (Pos/Nrm/Tangent/UV/Color) + slot 1
  — `InputLayoutManager.cpp` по образцу `PosNrmUV_InstCasterId`. **Ограничение**: один стрид на
  всё (`stride0` первого меша) — меши с другим форматом вершин в mega-буфер не попадают и остаются на
  CPU-пути; проверить, сколько таких в `models/`.
* **Сигнатура**: есть только `DRAW_INDEXED` (`Renderer.cpp:2102-2108`, `kArgStride = 20`). Пер-группового
  `ExecuteIndirect` ей достаточно; сигнатура с `CONSTANT`-аргументами понадобится только на bindless-этапе.
* **Что остаётся на CPU-пути** в S4: `OpaqueComplex` (multi-slot, `InstanceSlotCount() > 1`,
  `IInstanceable.h:37`), transparent/glass/ocean, GI-листва (до S7), чужой вершинный формат.

### Что сделано (как построено, с отличиями от текста выше)
* **Реестр — тот же `ShadowGpuData`**, не сестринский класс: у него уже есть записи всех кастеров,
  боксы (по чанкам), группы (mesh × submesh), виртуальные группы (× LOD-тир приёмника =
  камерный LOD), mega-таблицы. Что добавлено: (1) **запись per-slot** — `GBufferRenderable::
  FillInstanceDataForSlot(out, slot)`; `Rebuild`/`UpdateForFrame` заполняют каждый caster-id
  (сабмеш) параметрами ЕГО материального слота (`MaterialSlotOf` = `subs[s].materialSlot` с
  клампом, как в CPU-цикле); теневой VS читает только world/wind — тени не тронуты;
  (2) **`groupGbuf_`** — на статическую группу `MaterialData*` + indirect-PSO ПЕРВОГО объекта
  группы; (3) **eligibility** — объект идёт indirect, только если у КАЖДОГО его слота PSO и
  `MaterialData` совпадают с групповыми (переопределённая текстура на общем меше → CPU-путь целиком,
  без разрыва объекта пополам); штамп `RenderableObjectBase::GBufferIndirect()`.
* **Камерный ряд cull'а** (`shadow_cull_cs.hlsl`): фрустум камеры — слот `kMaxShadowViews` кольца
  фрустумов (Scene передаёт 47), четвёртое слово `CullParams` = `gCamView` (~0 = выкл). Камерные
  выходы ОТДЕЛЬНЫЕ: `camArgs_` (одна строка виртуальных групп) + `camVisibleList_` (2 слота на
  кастер) + `perGroupVgCam_` (свои базы) — теневые строки и VSM-scatter (который читает базы
  `perGroupVg_`) ничего не узнают о втором слоте фейда. Per-frame таблицы `casterCam_` (bit0 =
  eligible ∧ **прошёл CPU-фрустум в этом кадре** (штамп `MarkCameraVisible(frame)`), bit1 = skip:
  вердикт S3a для объекта / маска S1+S3a для чанка) и `casterFade_` (вес кроссфейда; > 0 ⇒ кастер
  эмитится в СВОЙ тир и в следующий — два draw'а CPU-пути). Cull-clear засевает camArgs из
  камерных баз (numViews = 1).
* **Шейдер `gbuffer_indirect.hlsl`**: `GBUFFER_SKIP_PEROBJECT` + тот же `BaseVS`/`FetchShadingValuesP`/
  `FinalizeGBuffer`; запись читается по `CASTERID` из slot 1 (layout `PosNormTanUV_InstCasterId` =
  полный вершинный формат + per-instance uint), материальные значения уходят в PS плоскими
  `nointerpolation`-атрибутами (5 float4) — PS буфер записей не читает, тот остаётся в NON_PIXEL;
  `lodFade = (f > 0) ? (groupLod == tier ? −f : +f) : 0` (b0 = LOD виртуальной группы, четыре CB на
  кадр). PSO на слот — `GBufferRenderable::BuildIndirectMaterials`: тот же desc, что у CPU-PSO слота
  (RT, stencil, sampling-дефайны, ALPHA_TEST, cull mode, EDITOR_OBJECT_ID), сменены шейдер и layout.
* **Draw** — `ShadowGpuData::RecordIndirectGBufferDraws`: один список (см. Ловушки), по виртуальным
  группам с кандидатами: VB/IB меша (LOD-IB группы), `md->StageGBufferBindings` (t0..t2, s0) +
  `StageGBufferSurfaceParams` (b2) — то же, что CPU-draw сабмеша, — t3..t5 = записи/фейды/LOD'ы
  (одна таблица на пасс), `Bind` с wireframe, `ExecuteIndirect` по args группы; непривязанный
  root-параметр (материал без текстур) → draw пропущен, как в CPU-пути (`GBufferBindingGuard`).
  Внутренний пасс `GBuffer_Indirect` параллельно CPU-хвосту; `gb.pShadow`-цепочка не тронута.
* **CPU-хвост**: `Scene::PrepareViewQueue` (камера) после счётчиков S0 и ДО батчинга выкидывает
  eligible-объекты из opaque-бакетов; `ApplyOcclusion` пишет вердикт на объект
  (`SetCameraOccluded`), `RefreshCasterLods` (после PrepareViews) собирает таблицы. Не-кастеры,
  GI-облака, шейдерные override'ы, чужой layout, объекты с per-object текстурой — CPU-путь как был.
* **Валидатор** расширен камерным рядом: readback camArgs за теневыми, CPU-эталон = фрустум камеры
  ∧ флаги ∧ фейд ×2 → `cull validation PASS: 46 views + camera match CPU (…; camera N)` в каждом
  прогоне (N = 946 стена, 1530 камера теней, 2484 с широкой полосой фейда, 16375/16655 на K=4).
* Ручки: `--set=gbuffer.indirect:0|1`, галка в табе Render со счётчиком eligible-слотов;
  hot-reload материалов → `InvalidateGroupMaterials` → Rebuild реестра.

**Отличия от текста плана.** (1) Не «GBufferGpuData», а расширение реестра теней (объекты без
тени остаются на CPU — на текущих уровнях их нет). (2) Материальные значения per-instance едут
VS→PS атрибутами, не чтением из буфера в PS. (3) Bindless не начат (второй этап, как и записано).
(4) `Main_ObjectIdReadback` и стенсил выделения не тронуты: objectId пишет тот же PS из записи;
клик по объекту в Release_Editor руками не проверялся.

### Ловушки
* **Веер по воркерам вредит.** Запись по 7 спискам (по 64 виртуальные группы, как `RenderObjectBatch`)
  на K=4: CPU суммарно 0.16 → 0.43 мс, GPU 0.19 → 0.28 мс — накладные на список (аллокатор, бинд
  таргетов, холодный PSO/VB) дороже ~1000 `ExecuteIndirect`, которые один поток пишет за 0.16.
  Оставлен один список.
* **Кандидаты только из CPU-фрустума.** Без штампа `MarkCameraVisible` объекты вне фрустума несли
  устаревшие вердикты и давали пустой `ExecuteIndirect` почти на каждую из 448 групп: стена K=4 — 7
  видимых объектов, 0.10 мс записи. Со штампом — 0.03.
* Константа локального порядка списка сидела внутри `#if WITH_EDITOR` — Debug собрался, Release нет.
* `readout primitives` считает только CPU-draw'ы: с indirect `primitives=559132` — это хвост
  (скайбокс и т. п.), не сцена.

### Замер 2026-09-04 (Release, `--shadow-mode=legacy --dlss=off --wind-freeze --set=exposure.autoExposure:0
--set=ocean.visible:0`, `gbuffer.indirect:0 → 1` в одном бинаре)

| ракурс | drawCalls off → on | камерный ряд валидатора | картинка off→on |
|---|---|---|---|
| стена (occlusion_test) | 5 → 1 | 946, PASS | 0.038 % (пол 0.03–0.04 %) |
| камера теней (wind_test) | 54 → 1 | 1530, PASS | 0.031 % |
| камера теней, `lod.fadeBand:0.35` (≈950 кастеров в двух бакетах) | 62 → 1 | 2484, PASS | **0.024 %** |

Кроссфейд, masked-листва (ALPHA_TEST-PSO на слот), террейн по чанкам (SHADING_MODEL 2, EXACT-LOD
на чанк), occluded-объекты/чанки S3a — всё в этих трёх парах, и всё на полу.

### Стоимость на K=4 (`--profdump`, 30 с прогрева, Legacy, DLSS по умолчанию; ОДИН бинарь; остров по два прогона)

| | остров **off** (×2) | остров **on** (×2) | стена **off** | стена **on** |
|---|---|---|---|---|
| CPU `RenderObjectBatch.Async` (воркеры) | 0.508 / 0.509 | **0.038 / 0.041** | 0.102 | 0.040 |
| CPU `GBuffer_Indirect` (один поток) | — | 0.073 / 0.093 | — | 0.033 |
| CPU `Pass_GBuffer` (поток пасса) | 0.056 / 0.056 | 0.142 / 0.163 | 0.053 | 0.108 |
| CPU.Frame, мс | 2.719 / 2.844 | 2.802 / 2.726 | 2.690 | 2.916 |
| Whole Cycle, мс | 2.640 / 2.740 | 2.754 / 2.672 (шум ±0.1) | 2.566 | 2.865 (хитчи, max 6.0) |
| GPU opaque: `ExecuteBundles` → `GBuffer_Indirect` | 0.222 / 0.230 | **0.222 / 0.196** | 0.037 | 0.072 |
| GPU.Frame, мс | 2.720 / 2.775 | 2.891 / 2.722 (шум ±0.1) | 2.679 | 2.897 |

Читается так: CPU-сабмишн opaque-геометрии как класс исчез (0.51 мс воркерного времени → 0.04, на
их место 0.08 мс последовательной записи ~600 `ExecuteIndirect`), GPU той же геометрии ровно
(бандлы 0.22 → indirect 0.20–0.22), кадр в шуме ±0.1 на обеих шкалах; стена — 30 мелких
`ExecuteIndirect` вместо трёх бандлов (+0.035 GPU). На K=4-репликации выигрыша по wall-clock и
не ожидалось (инстансинг сворачивал копии — F1); ценность S4 — в S5, где вердикт GPU-cull'а
потребляется в том же кадре, и в сценах из РАЗНЫХ мешей.

### Критерий приёмки
* ✅ Камера теней и «стена», `--dlss=off`, океан выкл: G-buffer **попиксельно идентичен** CPU-пути
  (дифф = пол) — включая masked-материалы и LOD-crossfade (пара с полосой 0.35). ⏳ `objectId`
  readback: тот же PS из той же записи, клик в Release_Editor — руками (не гонялся).
* ✅ K=4: `RenderObjectBatch.Async` ↓ 12×; `Pass_GBuffer` теперь СОДЕРЖИТ запись (0.08 мс) — суммарный
  CPU G-buffer'а 0.56 → 0.13 мс; `GPU.Frame` не хуже (шум); `cull validation PASS` с камерным рядом.
* ✅ `--scene-stress-gbv=20 --shadow-mode=legacy --level=…/occlusion_test.json` (Debug, дефолт ON) →
  `scene-stress verdict: CLEAN after 20 iterations`; реестр перестраивался на каждой смене уровня
  (GI-облака, чанкованный остров, 6…92 групп), камерный ряд валидатора PASS на каждом.
* **Дефолт ON** — оба пути в одном бинаре, `gbuffer.indirect:0` = откат. Три конфигурации собраны.

### Откат
`gbuffer.indirect:0`.

---

## S5. Двухфазный HZB-occlusion внутри GPU-driven G-buffer'а (Nanite two-pass) — ✅ СДЕЛАН 2026-09-04 (uncommitted, **дефолт ON**, ручка `gbuffer.hzb`)

**Зависит от:** S2, S4 (S3a/S3b не обязательны — они остаются для CPU-пути и теней локалов).
**Эффект:** закрытая геометрия не рисуется, без латентности и без поппинга.
**Риск:** средний — муверы, смена камеры, первая HZB после загрузки.

### Почему
§2.3: фаза 1 против prev-HZB отбрасывает почти всё закрытое; фаза 2 против HZB текущего кадра
возвращает «плохие догадки». Всё, что видно в этом кадре, нарисовано в этом кадре — консервативность
по построению, а не по латентности. У нас prev-HZB бесплатна (F4).

### Код
1. `Main_VisCull1` = S4-cull + HZB-тест: `Use(Deferred((f+2)%3).hzb, NON_PIXEL)` — **чтение через
   слоты кольца** (новое для графа: декларировать ресурс ДРУГОГО слота; убедиться, что барьерный
   компилятор трактует его как самостоятельный ресурс в состоянии покоя `NON_PIXEL` — F4). Матрицы —
   `GetPrevViewProjMatrixNoJitter()`; для муверов (`UpdateForFrame` знает их) — бокс расширить объединением
   prev/cur (наша дельта: у UE `PrevLocalToWorld` из GPUScene, у нас его нет). Не прошедшие HZB (но
   прошедшие фрустум-ИЛИ-нет — как у Nanite, фрустум прошлого кадра не проверяется) → `occludedList`
   (UAV counter + список).
   Валидность prev-HZB: `hzbFrameStamp[slot] == frame − 1` **и** `GetHistoryRevision()` не менялся
   **и** размер не менялся (resize) — иначе фаза 1 = чистый фрустум (Nanite: two-pass выключается без
   `PrevHZB`).
2. `Main_GBufferA` — `ExecuteIndirect` по `args_A`.
3. `Main_HzbA` — сегодняшний `Pass_Hzb`, только раньше; HZB после фазы A уже содержит всех окклюдеров,
   прошедших фазу 1.
4. `Main_VisCull2` — только `occludedList`, против `HzbA` и **текущего** фрустума/матриц → `args_B`.
5. `Main_GBufferB` — `ExecuteIndirect` по `args_B` (те же группы; второй набор args).
6. `Main_HzbB` — финальная HZB (для GTAO/SSR/RT-prereq `pHzb`, `async_compute_plan.md:958-1006`:
   RTTrace зависит от `pHzb` — прережим сохраняется на `HzbB`). Цена: +28 мкс.
7. Ветер: качающиеся кроны у границы бокса — пад бокса кастера камеры на амплитуду sway (S3b п.1).

### Что сделано (как построено, с отличиями от текста выше)
Имена пассов из блока «Порядок фаз» легли на существующие так: `Main_VisCull1` = камерная ветка
`Main_ShadowCull` (S4) + HZB-тест; `Main_GBufferA` = внутренний `GBuffer_Indirect` в `Main_GBuffer`
(не переименован); `Main_HzbA` — новый; `Main_VisCull2` = `Main_CamCullPost`; `Main_GBufferB` — новый;
`Main_HzbB` = сегодняшний `Main_Hzb` на своём месте (он и так стоял после G-buffer'а — переезд не
понадобился, RT-prereq `pHzb` не тронут).
1. **Фаза 1** — `shaders/shadow_cull_cs.hlsl`, камерная ветка: после фрустума кандидат проходит
   `CamHiddenLastFrame` — `HzbBoxCullFrustumPerspective(prevViewProj, prevViewToClip, skipFrustum=true)`
   (правило Nanite main pass: боковой фрустум прошлого кадра не проверяется, near-crossing = виден)
   → `HzbGetScreenRect` → `HzbIsVisible` против **HZB прошлого слота** (t12 = `Deferred((f+2)%3).hzb`
   через `Renderer::GetDeferredForPrevFrame()`). Спрятанный кастер идёт ОДИН раз в `camDeferred_` (u6),
   счётчики `DeferredCount[8]` (длина списка) и `[9]` (вес: кроссфейд считается за два — чтобы
   валидатор сходился), остальные — в `camArgs_`/`camVisibleList_` как в S4. Новый CB `b2 CameraHzbCB`
   (288 Б, зеркало `ShadowGpuData::CameraHzbParams`: prev/cur `viewProj` + `viewToClip`, `viewRect`,
   `hzbSize`, `prevValid`, `on`), заполняется `SceneRenderer::DecideFrame` через `SetCameraHzb(...)` ДО
   сборки графа; RS cull'а: `CBV(b0..b2)`, SRV ×13, UAV ×7 (плейсхолдеры при выключенной ручке —
   таблица без дыр, шейдер их не читает: `on = 0`).
2. **Матрицы — ДЖИТТЕРНЫЕ** (`GetPrevViewProjMatrix`/`GetPrevProjMatrix` и текущие `GetViewProjMatrix`/
   `GetProjMatrix`), не no-jitter из текста: пирамида собрана из джиттерной глубины, точное
   совпадение лучше близкого.
3. **Валидность prev-HZB** — `DecideFrame`: билдер `Main_Hzb` штампует `camHzbBuilt_[slot]` (номер
   кадра, размер рендера, `Camera::GetHistoryRevision()`); prev-слот валиден, если штамп == кадр−1 ∧
   размер тот же ∧ ревизия та же ∧ размеры пирамид P и D совпадают. Иначе `prevValid = 0`: фаза 1 —
   чистый фрустум, но ТРИ пасса остаются (форма кадра от истории не зависит, только работа; HzbA
   на таком кадре — 0.03 мс впустую, один кадр). Лог `camera hzb cull: on=… prev-valid=… (frame N)`
   только по смене состояния. Декларация `P.hzb` в базовой точке cull'а — `NON_PIXEL` (его каноническое
   после `Main_Hzb`): «чтение через слоты кольца» барьерный компилятор принял без правок, GBV CLEAN.
4. **`Main_HzbA`** — `Pass_Hzb(passA=true)`: та же сборка, свой scope `Pass_HzbA`, декларации как у
   `Main_Hzb` минус hand-over gb1 (это дело финальной пирамиды — от неё зависит RTTrace).
5. **Фаза 2** — `shaders/cam_cull_post_cs.hlsl` (`Main_CamCullPost`): поток на запись deferred-списка
   (длина из `DeferredCount[8]`), текущие матрицы против D.hzb после фазы A, `skipFrustum=false`,
   near = виден → `camArgsB_`/`camVisibleListB_` (те же базы `perGroupVgCam_`, кроссфейд в два
   бакета), `DeferredCount[10]` += вес. Args B сеются третьим cull-clear'ом в `RecordCull`.
6. **`Main_GBufferB`** — `Pass_GBufferB`: один список, `BindGBuffer(None)`,
   `RecordIndirectGBufferDraws(..., passB=true)` — те же группы и PSO, args/list B. Декларации целей =
   список `Main_GBuffer` (RT + depth WRITE; `Main_HzbA` между ними читал depth как NON_PIXEL).
   `gb.pGbufDone` = pass B (или `Main_GBuffer`), на него пересажены `Main_OcclusionQueries`,
   `Main_VsmPageRequest`, `Main_Hzb`, `Main_Gtao`, `Main_RTTrace`, `Main_Lighting` — ни один
   потребитель не читает G-buffer без пикселей фазы B.
7. **Счётчики** `DeferredCount` 8 → 12 uint (48 Б; cull-clear чистит 12), тот же readback-ring, что у
   S5b; `gpuHzbDeferred`/`gpuHzbDrawnB` в строке `occlusion` у `--vis-readout`, галка + строка в
   табе Render, `--set=gbuffer.hzb:0|1`.
8. **Валидатор** — камерный ряд теперь `A + DeferredCount[9] == CPU-фрустум` (счётчики едут в тот же
   readback за args); `cull validation PASS: … camera 4 + 942 deferred`. Вердикты фазы B НЕ
   проверяются: они зависят от глубины кадра, которой у CPU нет (текст выше просил readback HZB-мипа
   — не сделано, self-test S2 покрывает библиотеку).
9. **Дельты от текста, оставленные по построению:** пад боксов под ветер и объединение prev/cur
   боксов муверов — плохая догадка фазы 1 исправляется фазой 2 в том же кадре (проверено ветром:
   209 отложено, 4 дорисовано в B, картинка целая); `overlapsPixelCenter` не применяется (правило
   Nanite-кластера, у HZBTestPS его нет — как в S3b); имя ручки `gbuffer.hzb`, не `vis.hzb`.

### Ловушки
* **С дефолтными queries S5 видит только «хвост».** Bit1 `CasterCam` (вердикт S3a) выкидывает
  occluded-объекты из кандидатов ещё на CPU, поэтому у стены K=4 фаза 1 откладывает 8 кастеров, а не
  16 тысяч (валидаторный кадр 6, до прогрева истории — 16371). GPU-эффект S5 виден только при
  `vis.method:0` — и там он один даёт то же, что queries (таблица ниже). Кто кого кулит — S6.
* `kIdentity` в `shadow_cull_cs.hlsl` был объявлен НИЖЕ новой функции — HLSL требует порядок
  объявлений (dxc: «use of undeclared identifier»); константа поднята.
* CPU фазы B: 0.21 мс на воркере при `vis.method:0` (`ExecuteIndirect` на каждую группу-кандидата,
  почти все пустые; при queries — 0.08). Whole Cycle не сдвинулся (свой пасс = своя задача), но при
  росте числа групп это первый кандидат: маска групп с `deferred > 0` через readback с латентностью
  (консервативно «все», пока не пришёл).
* На статичной камере `gpuHzbDrawnB = 0` всегда — фаза B рисует только когда глубина изменилась
  (движение, ветер); проверять её путь ветром или полётом, не статикой.

### Замер 2026-09-04 (Release, `--shadow-mode=legacy --dlss=off --wind-freeze --set=exposure.autoExposure:0
--set=ocean.visible:0 --shot-delay=8 --vis-readout`, ОДИН бинарь, `--set=gbuffer.hzb:0|1`)
| Камера | off vs on, % пикселей | валидатор (кадр 6) |
|---|---|---|
| стена (occlusion_test, K=1) | 0.033 (пол 0.03–0.04) | `camera 4 + 942 deferred` |
| камера теней (wind_test) | 0.025 | `camera 1525 + 5 deferred` |
| полёт `--cam-fly`, K=4, `vis.method:0` | — (не паритет) | `4707 + 2695`; readout deferred 2695, B 0; лог чист |
| камера теней, K=4, `vis.method:0`, **ветер ON** | — | `6882 + 200`; readout deferred 209, **B 4** |

`camera hzb cull: on=1 prev-valid=1 (frame 2)` во всех прогонах; Debug GBV
`--scene-stress-gbv=20 --level=occlusion_test` → `scene-stress verdict: CLEAN after 20 iterations` (на
каждой смене уровня лог показывает `prev-valid=0` → `=1` через кадр — cut сбрасывает историю, как задумано).

### Стоимость на K=4 (стена, `--profdump`, 30 с прогрева, Legacy, DLSS по умолчанию; ОДИН бинарь)
| | GPU.Frame | Pass_ShadowCull | GBuffer_Indirect | HzbA | CamCullPost | GBufferB | Pass_Hzb | CPU.Frame | CPU Pass_GBuffer | CPU Pass_GBufferB |
|---|---|---|---|---|---|---|---|---|---|---|
| queries, hzb 0 | 2.822 | 0.613 | 0.073 | — | — | — | 0.041 | 2.819 | 0.114 | — |
| queries, hzb 1 | 2.844 | 0.616 | 0.076 | 0.033 | 0.010 | 0.001 | 0.029 | 2.852 | 0.110 | 0.078 |
| `vis.method:0`, hzb 0 | 2.993 | 0.631 | 0.348 | — | — | — | 0.031 | 2.996 | 0.218 | — |
| `vis.method:0`, hzb 1 | 2.801 | 0.628 | 0.079 | 0.035 | 0.010 | 0.003 | 0.029 | 2.817 | 0.240 | 0.211 |

Вердикт: с queries — в шуме (+0.02 мс = HzbA + post; cull +0.003); без CPU-окклюжена — **−0.19 мс
GPU (−6.4 %)**, `GBuffer_Indirect` 0.35 → 0.08, кадр РАВЕН пути queries (2.80 vs 2.82) при нулевой
латентности и без CPU-запросов (540 + 228 в кадр). **Дефолт ON:** цена ≈ 0.045 мс GPU при queries,
взамен — вердикт в том же кадре для всех indirect-объектов (хвост латентности queries закрыт);
снимать ли с них queries — S6.

### Критерий приёмки
* ✅ **Попиксельная идентичность** с `gbuffer.hzb:0` на статичных камерах (пол). Покадровая серия
  полёта не снималась (S5b: разброс между запусками 15–19 % по часам — метрика слепа); вместо неё
  доказательство пути B ветром (B > 0, картинка целая) + консервативность по построению.
* ✅ Камера «стена», K=4: `GPU.Frame` ↓ при `vis.method:0` (−6.4 %), с queries — в шуме (объяснено в
  ловушках); `gpuHzbDeferred`/`gpuHzbDrawnB` в readout.
* ✅ Валидатор: `cull validation PASS … camera A + deferred` (счётчик фазы 1 в CPU-эталоне).
* ✅ Смена уровня / resize / cut: `prevValid` по штампу+размеру+ревизии, форма кадра постоянна;
  `--scene-stress-gbv=20` на occlusion_test.

### Откат
`--set=gbuffer.hzb:0` — фаза 1 без HZB, три пасса не регистрируются, кадр = S4.

---

## S5b. Окклюжен со стороны СВЕТА: two-pass HZB для теневых вью (Legacy CSM по каскаду, VSM по странице) — S5b.1 (CSM) ✅ СДЕЛАН 2026-09-04 (commit abf0571, **дефолт OFF по замеру**, ручка `csm.hzbCull`); S5b.2 (VSM) ✅ СДЕЛАН 2026-09-04 (uncommitted, **дефолт OFF по замеру**, ручка `vsm.hzbCull`)

**Зависит от:** S1 (чанки в GPU-cull теней уже есть), S2 (`hzb_cull.hlsli` + self-test), indirect-путь
теней (Rung 0: `Main_ShadowCull` → `ExecuteIndirect`, есть). **Не зависит** от S3a/S3b/S4/S5 — и потому
идёт раньше S4: теневой путь уже GPU-driven, а G-buffer ещё нет. **Эффект:** кастер, целиком
спрятанный от света за более близким кастером (сотня объектов под стеной при низком солнце), не
рисуется ни в каскад, ни в страницу VSM — стена уже положила туда более близкую глубину. **Риск:**
средний — прямой Z теневых карт против reverse-Z библиотеки; HZB прошлого кадра живёт в ДРУГОМ
световом фрейме (каскад снапится и двигается с камерой), поэтому корректность даёт только
двухпроходная схема, а не «тест против прошлого кадра».

### Почему
Всё до этого шага — окклюжен глазами КАМЕРЫ, и правило UE (§2.5) запрещает применять его к теням:
кастер за спиной камеры обязан рисоваться, а латентный вердикт при повороте — дыра в тени. Но у
света своя глубина, и объект, целиком лежащий за стеной ОТ СВЕТА, в shadow map не пишет ничего.
UE делает ровно это для VSM: `r.Shadow.Virtual.UseHZB` (`VirtualShadowMapArray.cpp:363-367`,
«two pass occlusion culling for (Nanite) Virtual Shadow Maps») и `r.Shadow.Virtual.NonNanite.UseHZB`
(`:233-236`, «two-pass … with HZB from the current frame», дефолт 1). Механика: HZB строится по
ФИЗИЧЕСКИМ страницам пула (`HZBPhysicalArray`, `PF_R32_FLOAT`, размер = пул, `:761`, `:865`;
`BuildHZBPerPageCS` / `BuildHZBPerPageTopCS`, `:3807-3894`, шейдер
`VirtualShadowMapPhysicalPageManagement.usf`); валидна только с page table прошлого кадра
(`bPrevHZBValid = HZBPhysicalArray && PrevBuffers.PageTable`, `:3363`); не-Nanite кастеры тестируются
на этапе построения draw-команд (`VirtualShadowMapBuildPerPageDrawCommands.usf:244-322`,
`IsVisibleMaskedHZB` под `USE_HZB_OCCLUSION`); тест страницы — `VirtualShadowMapPageOverlap.ush:211-250`
(`SetupPageHZBRect`: уровень HZB клампится так, чтобы страница была ≥ 4×4 (`HZBLevel ≤
VSM_LOG2_PAGE_SIZE − 3`), `IsPageVisibleHZB`: тексели минус начало страницы, страница без записи в
`HZBPageTable` → по флагу `bTreatUnmappedAsOccluded`). Nanite-кастеры идут двухпроходно
(`bTwoPassOcclusion = UseHzbOcclusion()`, `:3367`; `CULLING_PASS_OCCLUSION_MAIN / POST`,
`NaniteCullRaster.cpp:38-39`, cvar `r.Nanite.Culling.TwoPass`, `:319-322`). Для Legacy CSM у Epic
HZB-cull'а нет (§2.5) — статику они закрывают кэшем; мы переносим VSM-механику на тайл каскада, потому
что каскады у нас основной режим и indirect-путь под ними уже есть.

### Код
1. **Орто-вариант в библиотеке.** `HzbBoxCullFrustumOrtho` = транскрипция `BoxCullFrustumOrtho`
   (`NaniteHZBCull.ush:414-442`): `RectMin/Max = centerClip ∓ (|ex·row0| + |ey·row1| + |ez·row2|)`,
   `crossesFar = RectMin.z < 0`, `crossesNear = RectMax.z > 1`, `isVisible = RectMax.z > 0 && (near
   clip ? RectMin.z < 1 : true)`, side-cull `any(RectMax.xy < −1 || RectMin.xy > 1)`; CPU-зеркало в
   `HzbCull.h`; self-test — ещё ≥ 6 кейсов с орто-проекцией (плоскость с дырой, бокс за/перед/на
   кромке/за far/вне тайла/угол с fold). Пункт §0.2: транскрибировать, не выводить.
2. **Прямой Z.** Теневые карты у нас с прямым Z (`ShadowGpuData.cpp:1536` `LESS_EQUAL`, clear 1.0),
   библиотека и self-test — reverse-Z как у UE. Не дублировать библиотеку под второй знак: **световая
   HZB хранит `1 − z`** (furthest = min, как и сейчас в `hzb_build_cs`), а ближайшая точка бокса
   подаётся как `1 − clipZ`. Тогда `HzbIsVisible` и self-test остаются буквально теми же; вариант с
   `max`-редукцией и перевёрнутым сравнением — записать как отвергнутый (второй набор формул =
   второй набор ошибок, [[vsm-smrt-plan]] уже платил за четыре флипа).
3. **Legacy CSM, по каскаду** (первым, S5b.1):
   * Ресурсы: `csmHzb[kCascades]` — R32_FLOAT пирамида на тайл (контент 2040² → мип 0 1020²,
     11 уровней; тот же `hzb_build_cs.hlsl` с новым `srcOffset` = начало тайла в атласе и `1 − z`),
     ОДНА на каскад, пишется раз в кадр (см. п. «одна сборка»); `prevLightViewProj[c]` + `prevValid[c]`
     (каскад той же конфигурации, Legacy, атлас не пересоздан); `deferredList[c]` (UAV, id кастеров)
     + `deferredArgs`.
   * Граф (`SceneRenderer_Graph.cpp`, вокруг `Main_ShadowCull` :305 / `Main_CSM` :328):
     `Main_ShadowCull` (`shadow_cull_cs.hlsl`) после фрустума/S14 получает второй тест — бокс кастера
     против `csmHzb[c]` **прошлого кадра с прошлыми матрицами каскада**: прошёл → visible list A
     (как сегодня); НЕ прошёл (был спрятан от света в прошлом кадре) → `deferredList[c]`, в A не
     попадает. `Main_CSM` (проход A) рисует A. Новый `Main_CsmHzb` (compute) строит `csmHzb[c]` из
     тайла после A. Новый `Main_ShadowCullPost` (compute) тестирует ТОЛЬКО `deferredList[c]` против
     свежей `csmHzb[c]` с ТЕКУЩИМИ матрицами → visible list B + args B. Новый `Main_CSM_Post`
     (`ExecuteIndirect` по B в те же тайлы, без clear).
   * **Одна сборка HZB на каскад в кадр — и почему это безопасно.** Следующему кадру нужна HZB
     полного тайла (после B), а проходу B — HZB после A. Использовать HZB после A как «прошлую» для
     следующего кадра **можно**: в ней нет кастеров прохода B, то есть она ГЛУБЖЕ, чем правда, и
     объект за ними не будет срезан — консервативно в нужную сторону, теряется только эффект в редких
     кадрах, когда B что-то нарисовал. Nanite строит две (промежуточную и финальную), нам хватает одной.
   * Ноль латентности: кастер, спрятанный в прошлом кадре и открывшийся в этом (стена сдвинулась,
     камера повернула, каскад переехал), лежит в deferred, проверяется против текущей HZB после A и
     рисуется в B **в том же кадре**. Кастер, открывшийся в прошлом кадре, уже в A. Дыры в тени нет по
     построению — это и есть причина двух проходов вместо «теста против прошлого кадра».
   * Чанки: cull теней уже почанковый (виртуальные группы `group*4+lod`), тест едет на чанк бесплатно.
   * Каскад 0 тестируется как остальные (правило «каскад 0 никогда» у UE — про occlusion КАМЕРОЙ).
   * Валидатор GPU-cull (`cull validation`, CPU-эталон = фрустум) с HZB-cull'ом обязан расходиться:
     гейт гоняется с `--set=csm.hzbCull:0`, как с `shadow.giIndirect:0`; readout `csm_readout.log`
     получает `hzbDeferred` и `hzbCut` на каскад.
4. **VSM, по странице** (вторым, S5b.2), транскрипция `VirtualShadowMapPageOverlap.ush` +
   `BuildHZBPerPageCS`:
   * `vsmHzbPhysical` — R32_FLOAT c мипами поверх ФИЗИЧЕСКОГО пула (4096² + мипы ≈ 85 МБ, как у UE;
     R16 не брать — конservативность решается на точных сравнениях), пирамида на страницу 128² →
     уровни 64…1 (`BuildHZBPerPageCS`: один тред-группа на страницу, `:3807-3894`), строится из
     страниц, отрисованных в этом кадре (после прохода A), `1 − z`.
   * Снимок page table прошлого кадра (`prevPageTable`, кольцо по слотам кадра как `Deferred[]`) —
     без него «та же виртуальная страница» не находится в пуле (`bPrevHZBValid`, `:3363`).
   * Тест — в `vsm_page_scatter_cs.hlsl` (пары (кастер, страница)): rect кастера в виртуальном
     пространстве уровня, минус начало страницы, уровень HZB ≤ `log2(128) − 3` (страница ≥ 4×4,
     `PageOverlap.ush:218-226`); страница, которой в прошлом кадре не было (не запрошена / не
     отрисована), → кастер виден (`bTreatUnmappedAsOccluded = false`; UE держит флаг ради статичной
     окклюзии кэшем — не наш случай при `cache off`).
   * Два прохода как у каскадов: A по парам, не срезанным прошлой HZB; сборка per-page HZB; B —
     отложенные пары против текущей. С кэшем страниц ON (когда включим) отложенных пар не бывает у
     кэшированных страниц — они не рисуются вовсе; шаг ортогонален [[vsm-page-caching-progress]].
5. Ручки: `--set=csm.hzbCull:0|1`, `--set=vsm.hzbCull:0|1` (дефолт ON после приёмки каждой части),
   галки в табах CSM / VSM, столбцы в readout'ах.

### Что сделано — S5b.1, Legacy CSM (2026-09-04, как построено, с отличиями от текста выше)
* **Библиотека.** `HzbBoxCullFrustumOrtho` в `shaders/hzb_cull.hlsli` (транскрипция
  `NaniteHZBCull.ush:414-442`, w ≡ 1: rect = центр ∓ сумма |полуэкстент × строка матрицы|,
  near/far прямо по z; `nearClip = false` — конвенция направленных теней у UE) + зеркало
  `hzb::BoxCullFrustumOrtho` в `HzbCull.h`. **Self-test расширен**: второй синтетический HZB под
  reverse-Z орто-проекцией (у орто своя шкала глубины, тот же экран и та же плоскость с дырой),
  8 орто-кейсов (перед/за/в дыре/пересекает near — pancaked/вне окна/за far/крупный на грубом
  уровне/5 мм перед плоскостью), `--hzb-cull-selftest` → **PASS 21 cases, gpu == cpu mirror**.
* **Прямой Z без второй библиотеки** (п. 2 плана, сделано): пирамида хранит `1 − z`
  (пермутация `HZB_LIGHT=1` шейдера `hzb_build_cs.hlsl`: источник — ПРЯМОУГОЛЬНИК атласа по
  `srcOffset`/`srcSize` с клампом внутри контента тайла, мип 0 = `1 − DepthTex`, одна min-цепь),
  а матрицы света идут в cull как `viewProj × FlipZ` (`CascadeHzb.cpp`: строка-вектор, `z' = w − z`)
  — clip z бокса выходит тем же `1 − z`. Библиотека и её тест не тронуты.
* **`sources/rendering/shadows/CascadeHzb.{h,cpp}`** (`render::CascadeHzb`): четыре R32-пирамиды на
  контент тайла (2040 → мип 0 1020², 10 уровней, свой non-shader-visible heap: SRV + 13 UAV на
  каскад, как у камерной пирамиды), `SetFrameViews` (матрицы кадра; прошлые становятся `prev`),
  `PrevValid(c)` = пирамида построена ровно в кадре N−1 при тех же матрицах, `RecordBuild` (по
  каскаду: origin тайла `((c%2)·tile + border, (c/2)·tile + border)`, 10 диспатчей с UAV-барьерами),
  `MarkBuilt`, `Invalidate` (смена уровня/размера). **Одна сборка на каскад в кадр** — как
  обосновано в п. «Одна сборка» выше.
* **Cull** (`shadow_cull_cs.hlsl`): второй CB `b1` = `CascadeHzbCB` (prev/cur матрицы ×4, маска
  валидности, rect тайла, размер мипа 0, `on`), `t5..t8` — пирамиды (при `on = 0` в слотах стоит
  SRV атласа — VOLATILE-таблица без дыр), `u2/u3` — deferred list + 8 счётчиков. После
  фрустум-теста каскадного вью: `HiddenLastFrame` = орто-cull **с пропуском бокового теста и без
  near clip** (`NaniteCullingCommon.ush:463-482`, «clamped rect HZB provides a better guess … post
  pass will clean up bad guesses») → rect → `HzbIsVisible(prev pyramid)`; спрятан → в deferred list
  (`InterlockedAdd` на счётчик каскада), иначе — как раньше в visible list. `shadow_cull_clear_cs`
  обнуляет 8 счётчиков каждый кадр (третий UAV, всегда привязан).
* **Post cull** (`shadow_cull_post_cs.hlsl`, новый): тред (i, каскад) по deferred list, орто-cull с
  ТЕКУЩИМИ матрицами (`:483-497`: только far-cap и тест глубины, near-crossing = виден) против
  свежей пирамиды → args B + visible list B (та же формула, что у главного cull'а) + счётчик
  «нарисовано в B».
* **Буферы** в `ShadowGpuData`: `deferredList_` (4 × кастеров), `deferredCount_` (8 uint, покой UAV),
  `indirectArgsB_` / `visibleListB_` (только строки каскадов, покой INDIRECT_ARGUMENT / VERTEX как
  у главной пары); `cullUav_` расширен до 7 наборов; `RecordIndirectShadowDraws(…, passB)` рисует
  любую из пар. Readback счётчиков (32 байта в кадр, кольцо на 3 слота) → `HzbDeferred(c)` /
  `HzbDrawnB(c)` — столбцы `hzbDef`/`hzbB` в `csm_readout` и строка в табе Render.
* **Граф** (`SceneRenderer_Graph.cpp`, ветка Legacy): `Main_CsmHzb` (атлас NON_PIXEL, пирамиды UAV →
  NON_PIXEL) → `Main_ShadowCullPost` (args B/list B UAV → INDIRECT/VERTEX, счётчики UAV →
  COPY_SOURCE → UAV) → `Main_CSMPost` (атлас DEPTH_WRITE, один CL на 4 каскада, тот же bias и
  scissor, что у прохода A); `gb.pShadow` переезжает на `Main_CSMPost`, чтобы локальные тени и
  освещение шли после прохода B. Все три пустые в кадре, где главный cull не тестировал.
* **Решение — в билдере главного cull'а** (`PrepareCullPass`): `dec.hzb = пирамиды живы ∧ кольца ∧
  post-PSO ∧ НЕ кадр валидатора` (на кадре одноразового readback'а CPU-эталон = чистый фрустум,
  и стадия HZB просто пропускает кадр — валидатор остаётся автоматическим, `cull validation PASS`
  в каждом прогоне). Ручка `--set=csm.hzbCull:0|1` (дефолт ON), галка в табе CSM.

**Отличия от текста плана.** (1) VSM (S5b.2) не сделан — см. ниже. (2) Проверка
`overlapsPixelCenter` не применяется ни в одном из проходов (у Nanite она есть: бокс между центрами
текселей не растрируется вовсе); цена — редкий субтексельный кастер, нарисованный лишний раз,
выгода — тот же консерватизм, что у S3b. (3) Стохастики/истории нет — у теней это чистый GPU-путь,
все вердикты в кадре.

### Ловушки
* **Билдер против записи.** `MarkBuilt(N)` вызывается в билдере `Main_CsmHzb`, а билдеры бегут ДО
  любой записи; CB главного cull'а, заполненный при записи, видел метку ЭТОГО кадра и `PrevValid`
  (N == N−1) был ложен всегда — первые прогоны показали `hzbDef = 0` при живых пассах. Снимок
  параметров (`hzbParams_`) делается в `PrepareCullPass`, до метки; запись только копирует.
  Строка-событие `cascade hzb cull: on=… prev-valid cascades …` в session-логе фиксирует переходы.
* Валидация одноразового readback'а видит только фрустум: стадия HZB отключается на этом кадре
  (по `readbackWanted`, посчитанному ДО деклараций базовой точки — они обязаны сидеть на ней).
* `casters` в readout — ОБЪЕКТЫ CPU-cull'а, `hzbDef` — caster-id GPU-пути (сабмеш-группы: 2769 id
  на 625 объектов), поэтому 180 отложенных при 87 объектах в каскаде — норма, не переполнение.
* Пирамида читается через `Texture2D<float> CsmHzb[4]` с индексом-счётчиком цикла (uniform) —
  без `NonUniformResourceIndex`; в post-проходе индекс = `dtid.y`, тоже uniform на группу.

### Замер 2026-09-04 (Release, `--shadow-mode=legacy --dlss=off --wind-freeze --set=exposure.autoExposure:0
--set=ocean.visible:0 --csm-readout`, `csm.hzbCull:0 → 1` в одном бинаре; уровень occlusion_test с ВТОРОЙ
стеной 26 м (x ≈ 50.8, z 62..106; 48 пальм в её тени по оценке; солнце (−0.50, −0.47, −0.72)))

| камера | каскад | `casters` (CPU) | `hzbDef` (GPU id) | `hzbB` | картинка off→on |
|---|---|---|---|---|---|
| A над первой стеной (4,14,44 → 38,3,70) | c1 / c2 | 87 / 108 | **180 / 159** | 0 / 0 | 0.029 % (пол 0.03–0.04 %) |
| C сверху (20,40,30 → 38,2,72) | c2 | 115 | **199** | 0 | 0.031 % |
| A в полёте (4 м/с +z, 6°/с yaw, кадр ~601) | c1 / c2 | 87 / 113 | 189 / 159 | 0 / 0 | 5.8 % = сдвиг камеры на 3 см между запусками; теневые пятна на земле совпадают, дыр нет (глазами) |

`hzbB = 0` на статике и в медленном полёте — ожидаемо: окклюдер (стена) статичен, а вердикт со
стороны света от камеры не зависит; проход B получит работу, когда кастер входит в тайл заново или
окклюдер двигается. Валидатор: `cull validation PASS: 46 views match CPU` в каждом прогоне.
GBV-гейт: `--scene-stress-gbv=20 --shadow-mode=legacy --level=…/occlusion_test.json` (Debug) →
`scene-stress verdict: CLEAN after 20 iterations`, переходы `on=1 prev-valid cascades 0123` в логе
на каждом уровне. Три конфигурации собраны; `check_shaders.py` 53/53.

### Стоимость на K=4 (`--profdump`, 30 с прогрева, Legacy, DLSS по умолчанию; ОДИН бинарь; пирамида четвертьразмерная)

| | камера A **off** (×2) | камера A **on** | камера C **off** (×2) | камера C **on** (×2) |
|---|---|---|---|---|
| `hzbDef` c1 / c2 / c3 | — | 180 / 163 / 173 | — | 0 / 199 / 189 |
| GPU `Pass_CSM` | 0.168 / 0.164 | **0.148** (−0.02) | 0.111 | **0.095 / 0.098** (−0.015) |
| GPU `Pass_CsmHzb` (4 пирамиды) | — | **0.110** | — | **0.129 / 0.129** |
| GPU `Pass_ShadowCullPost` + `Pass_CSMPost` | — | 0.008 + 0.006 | — | 0.008 + 0.009 |
| GPU.Frame, мс | 2.957 / 2.895 | 3.065 (**+0.13**) | 2.776 | 2.891 / 2.924 (**+0.12**) |
| CPU.Frame, мс | 2.958 / 2.905 | 3.072 | 2.782 | 2.896 / 2.931 |
| Whole Cycle, мс | 2.563 / 2.517 | 2.569 (ровно) | 2.156 | 2.170 / 2.181 (ровно) |
| CPU запись `Pass_CsmHzb` / `Pass_CSMPost` (воркеры) | — | 0.194 / 0.189 | — | 0.187 / 0.186 |

Читается так: стадия работает (160–200 кастеров на каскад срезано, картинка идентична), но на
сегодняшнем контенте **платит больше, чем экономит**: сборка четырёх пирамид — 0.11–0.13 мс GPU
(половинная пирамида 1020² стоила 0.15; четвертьразмерная 510² с 4×4-min на первом уровне сняла
лишь четверть — остаток упирается в ЧТЕНИЕ атласа, 4 × 2040² × R16 = 33 МБ в кадр, и никакая
резолюция пирамиды его не уменьшит), retest + проход B — 0.015 мс, а `Pass_CSM` на пальмах за
стеной худеет на 0.015–0.02 мс. CPU-запись (0.19 + 0.19 мс) идёт на воркерах, Whole Cycle ровно.
По критерию приёмки — **дефолт OFF**, ручка `csm.hzbCull:1` остаётся: окупится, когда спрятанные
кастеры дороги (город, интерьер, лес за скалой) — `hzbDef` в readout при включённой ручке скажет,
сколько их. Возможные удешевления, если контент потребует: пропуск сборки для каскадов с
`casters < 16` (c0 здесь — 5 кастеров, −25 % сборки), сборка на compute-очереди параллельно
G-buffer'у (проход B всё равно ждёт), один `ExecuteIndirect` на каскад через mega-буфер вместо
1776 вызовов в `Pass_CSMPost`.

**S5b.2 (VSM по странице) не начат** — та же механика на пул VSM стоит те же деньги плюс снимок page
table и 85 МБ R32 под пирамиду пула, и по замеру S5b.1 на сегодняшних уровнях окупаться ей нечем; делать,
когда появится контент, на котором S5b.1 со включённой ручкой уходит в плюс.

### Критерий приёмки
* ✅ (CSM) Тени **попиксельно** идентичны при `hzbCull:0/1` на двух ракурсах со второй стеной
  (дифф 0.029 / 0.031 % = пол) при 160–200 отложенных кастерах на каскад — консервативный cull не
  меняет ни одного пикселя. Остров/камера теней не гонялись: там нет окклюдера, стадии нечего
  откладывать (и это видно по `hzbDef` без запуска картинки).
* ❌→**дефолт OFF** (CSM): `hzbDef` падает на сотни, но `Pass_CSM` худеет на 0.02 мс против 0.13 мс
  стоимости — по этому же пункту шаг выключен, а не «оставлен на потом». Таблица выше.
* ✅ `--cam-fly` вдоль стены с yaw: кадр в движении без дыр (маска диффа — кромки пальм от сдвига
  камеры на 3 см между запусками, теневые пятна совпадают); `hzbDef` держится 189/159 в движении.
* ✅ `--scene-stress-gbv=20` CLEAN с активной стадией; `cull validation PASS` в каждом прогоне, при
  включённой ручке тоже — валидаторный кадр стадию пропускает сам.
* ⏳ VSM (S5b.2): не начат, см. выше.

### Откат
`csm.hzbCull:0` (дефолт) — три новых пасса не регистрируются, cull теней как сегодня; `vsm.hzbCull:0`
(дефолт) — см. S5b.2 ниже.

### Что сделано — S5b.2, VSM по странице (2026-09-04, как построено, с отличиями от п. 4 выше)
Транскрипция механики `r.Shadow.Virtual.NonNanite.UseHZB` на наш scatter-путь, ВНУТРИ одного пасса
`Main_VsmPageRender` (новых пассов графа нет — рендер страниц и так один список; цепочка = пять
новых барьерных точек после `pointConsume`).
1. **Пирамида пула** — `shaders/vsm_hzb_build_cs.hlsl` (`BuildHZBPerPageCS`): R32_FLOAT на
   ПОЛОВИНУ пула (2048², 7 мипов до 32²) = самодостаточная цепочка 64 → 1 на каждую 128-текельную
   страницу по её же смещению; хранит `1 − z` с min-редукцией (пул прямой Z, clear 1.0 → 0 =
   «дальше всех, не прячет ничего»). Одна тред-группа 16×16 на физическую страницу
   (`Dispatch(32, 32)` вручную, не через `RecordComputeDispatch`), четыре уровня через groupshared.
   Строится ТОЛЬКО по страницам, отрисованным в этом кадре (`PerPageDirty` после прохода A) —
   регион кэшированной страницы по-прежнему описывает её кэшированную глубину; `gFull = 1`
   (первая сборка, после смены уровня) строит все resident и обнуляет свободные. 22 МБ, живёт с
   пулом. Дельта от п. 4: полразрешения, не полное (85 МБ у UE не нужны: тест 4×4 на уровне ≥ 1).
2. **Снимок page table прошлого кадра** — `prevPageTable_` (55 КБ), копия в `RecordPageAllocate`
   ДО того, как аллокация перепишет таблицу (точка `snapshotCopy` в `PrepareRequestPass`) = ровно
   отображение «виртуальная страница → физическая» на момент прошлого рендера (UE
   `PrevBuffers.PageTable`).
3. **Тест в scatter'е** (`vsm_page_scatter_cs.hlsl`): один орто-cull на (кастер, уровень
   клипмапа) с ПРОШЛОЙ матрицей вью (`gPrevViewProj[42]` = `prevViewVp_` до перезаписи) и FlipZ,
   правило Nanite main pass (боковой фрустум пропущен, near = виден); в цикле по страницам rect
   перекладывается в клип страницы АНАЛИТИЧЕСКИ (`(rect − centre) · axis` — off-center проекция
   страницы только масштабирует xy), `PrevPageTable[pageId]` → физическая страница прошлого кадра →
   `HzbGetScreenRect` с viewRect = прямоугольник этой страницы в пуле → `HzbIsVisible`. Пары с
   футпринтом < 8×8 текселей не тестируются (16 загрузок ради draw'а, который почти ничего не
   растеризует; на грубых уровнях это все пары). Спрятанная пара → `DeferredPairs` (uint2
   (ведущий слот кастера, virtual page id), ёмкость 8 × кастеров; переполнение → рисуется в A и
   считается в `HzbCounters[2]`), не спрятанная — в список страницы как раньше. Только клипмап:
   локальные вью перспективные, post-пассу нужен раздельный view/proj — отложено (демо-уровень
   с локалами их не оправдывает).
4. **Post** — `shaders/vsm_hzb_post_cs.hlsl`: тред на пару, текущая проекция страницы против
   свежей пирамиды (`skipFrustum = false`, near = виден), пропуск страниц, не dirty в этом кадре
   (кэшированная не рисуется вовсе). Выживший добавляется **в тот же `PageVisibleList`** сразу
   ПОСЛЕ записей прохода A того же бакета: слот = `bucketBase + PageGroupCount[p, vg] (A) + rankB`,
   ранг из `PageGroupCountB` — кастер лежит ровно в одном из A/B на страницу, бакет размером в
   группу не переполняется. Так второй 45-мегабайтный список не нужен.
5. **Setup B** — `vsm_page_setup_cs.hlsl` в режиме `gPassB` (бывший `_pad6`): args B из
   `PageGroupCountB` (t11), `StartInstance = pageBase + bucketBase + countA`, u0/u4 = `pageDrawArgsB_` /
   `pageArgCountB_`; проекции, dirty-биты и brute-force списки не трогает (плейсхолдеры в таблице).
6. **Проход B** — тот же single-draw `ExecuteIndirect` по args B (viewport/цели/потоки прохода A
   ещё привязаны, перепривязывается только PSO после compute-диспатчей); счётчики → readback-кольцо
   → `VirtualShadowMap::HzbStats()` (`vsmHzbDeferred/DrawnB/Overflow` в строке `occlusion` у
   `--vis-readout`, строка в VSM-табе).
7. **Валидность prev-пирамиды** — в билдере `PrepareRenderPass`: `hzbLastBuildFrame_ ==
   lastRenderFrame_` (ни один рендер с прошлой сборки не прошёл без сборки — пропущенные
   «still»-кадры пул не меняют и валидность не ломают), `!hzbInvalidate_` (смена уровня →
   `InvalidateAllPages`), `prevViewVpValid_`; иначе фаза 1 ничего не откладывает, сборка + post
   идут (форма кадра от истории не зависит). Лог `vsm hzb cull: on= prev-valid= full= off-reason=`
   по смене состояния — `off-reason` называет ворота (scatter / single-draw / PSO / буферы).
8. **Решение** `d.hzb` = ручка ∧ scatter ∧ single-draw ∧ оба PSO ∧ буферы ∧ дескрипторы (в
   `ComputePageRenderDecisions`); точки `pointHzbBuild` (пул NPS, пирамида UAV, dirty NPS) →
   `pointHzbPost` (пирамида NPS, список/countsB/счётчики UAV, пары NPS) → `pointSetupB` (countsB
   NPS, argsB/argCountB/dirty UAV) → `pointDrawB` (пул DEPTH_WRITE, argsB INDIRECT, список VERTEX,
   счётчики COPY_SOURCE) → `pointHzbRestore` (счётчики UAV). Scatter-clear чистит и countsB, и
   счётчики (`gClearB`).
9. **Попутно починен `--shadow-mode=`:** `graphics_settings.json` (`shadows/mode = legacy` у
   владельца) применялся ПОСЛЕ флага и молча его перекрывал — все «VSM»-замеры этого шага до
   починки шли в Legacy (в профдампе `Pass_CSM`, ни одной строки VSM). Теперь флаг — boot-override
   на сессию (`render::g_shadowModeFromCli`), в файл пишется прежнее значение
   (`g_shadowModePersisted`). Ловушка в память: **проверять, что пасс вообще шёл, по scope'у
   профдампа**, прежде чем читать его счётчики.

### Ловушки — S5b.2
* **Пол паритета VSM с SMRT = 6.4 %**, не 0.03: `vsm.smrtTemporalDither` вращает набор сэмплов по
  НОМЕРУ КАДРА, а `--shot-delay` снимает по часам (кадры 2300 vs 2400 у двух запусков) — off/off
  расходятся на 6.4 % по контурам теней. Паритет снимать с `--set=vsm.smrtRayCount:0` (однотап):
  пол 0.03 %.
* Статичная камера + `--wind-freeze` = VSM-update ПРОПУЩЕН с 6-го кадра (`vsmSkipUpdate`): счётчики
  замерзают на значении кадра 4, профдамп меряет ничего. Замер только с ветром или `--cam-fly`.
* Первый `HiddenFromLightLastFrame` строил матрицу страницы и делал орто-cull НА КАЖДУЮ ПАРУ:
  scatter 0.093 → 0.164 мс; вынос cull'а на (кастер, уровень) с аналитическим remap'ом rect'а —
  0.143. Остаток (+0.05) — `PrevPageTable` + rect + загрузки на все resident-пары 10 уровней;
  рычаг на будущее: тестировать только уровни < `vsm.windMaxLevel` (там страницы dirty каждый кадр)
  или только страницы, dirty в прошлом кадре.
* На статичной камере `vsmHzbDrawnB = 0` всегда (как у S5); путь B доказан дрейфом камеры на
  стене (`--cam-fly=0.3,0`: 729 отложено, 4 дорисовано).

### Замер — S5b.2 (2026-09-04, Release, `--shadow-mode=vsm --dlss=off --wind-freeze
--set=vsm.smrtRayCount:0 --set=exposure.autoExposure:0 --set=ocean.visible:0`, ОДИН бинарь,
`--set=vsm.hzbCull:0|1`; VSM обновляется первые 5 кадров и замирает — снимок сравнивает пул, который
оставили один и два прохода)
| Камера | off vs on, % пикселей | readout `vsmHzbDeferred` (кадр 4) |
|---|---|---|
| A над стеной (occlusion_test) | 0.035 (пол 0.03) | 744 |
| камера теней (wind_test) | 0.031 | 46 |
| A, дрейф `--cam-fly=0.3,0` (без паритета) | — | 728 отложено, B 0–4, overflow 0; лог чист |
| C, дрейф | — | 326 |
| роща `80.08 6.32 40.58`, ветер ON | — | 19–20 |

Debug GBV `--scene-stress-gbv=20 --shadow-mode=vsm --set=vsm.hzbCull:1 --level=occlusion_test` →
`scene-stress verdict: CLEAN after 20 iterations (285 s)`; на каждой смене уровня лог показывает
`prev-valid=0 full=1` → `prev-valid=1` через кадр (снимок таблицы и пирамида перестраиваются, как задумано).

### Стоимость — S5b.2 (`--profdump`, 30 с прогрева, K=1, ОДИН бинарь; роща с ветром — страницы
ближних уровней перерисовываются каждый кадр; стена — дрейф камеры `--cam-fly=0.3,0`)
| | GPU.Frame | Pass_VsmPageRender | Scatter | Setup | HzbBuild | HzbPost | DrawB | CPU VsmPageRender |
|---|---|---|---|---|---|---|---|---|
| роща, hzb 0 (×2) | 2.016 / 2.015 | 0.193 / 0.189 | 0.097 / 0.093 | 0.019 / 0.018 | — | — | — | 0.080 / 0.081 |
| роща, hzb 1 (×3) | 2.133 / 2.149 / 2.129 | 0.319 / 0.338 / 0.308 | 0.159 / 0.164 / 0.143 | 0.021–0.024 | 0.034 | 0.020–0.022 | 0.002 | 0.114 / 0.131 |
| стена A дрейф, hzb 0 | 2.564 | 0.586 | 0.101 | 0.023 | — | — | — | 0.074 |
| стена A дрейф, hzb 1 (×3) | 2.585 / 2.588 / 2.566 | 0.603 / 0.598 / 0.594 | 0.131 / 0.127 / 0.125 | 0.021 | 0.033–0.036 | 0.028 | 0.003–0.004 | 0.107–0.113 |

Вердикт: на стене (729 пар/кадр спрятаны от солнца) two-pass **ровно окупает себя** (0.586 → 0.594,
кадр 2.564 → 2.566 — растр, который он снял, стоил столько же, сколько пирамида + retest + тест в
scatter'е); в роще (19 пар) — чистая цена **+0.11 мс (+5.5 % кадра)**. **Дефолт OFF по замеру**, как у
S5b.1: механика верна (паритет = пол, GBV CLEAN), ждёт контента, где спрятанные от света кастеры
дороги (город, стены с интерьерами), а не пальм под высоким солнцем. Ручка `--set=vsm.hzbCull:1`,
галка в VSM-табе.

### Критерий приёмки — S5b.2
* ✅ Паритет с `vsm.hzbCull:0` на стене и на камере теней (пол при однотапе; с SMRT-дизером пол
  сам по себе 6.4 %).
* ✅ Путь B живой (дрейф: B > 0), overflow 0, лог чист, `vsm hzb cull: on=1 prev-valid=1` со 2-го
  кадра, `prev-valid=0` ровно на кадре смены уровня.
* ✅ GBV CLEAN с принудительно включённой ручкой.
* ❌ «GPU.Frame ↓ на стене» — ровно; поэтому OFF.

### Откат — S5b.2
`vsm.hzbCull:0` (дефолт): scatter не откладывает, пирамида не строится, точки B не декларируются,
пасс = как до шага. Снимок page table (55 КБ копия) идёт только при включённой ручке.

---

## S6. Правила границ: тени, RT, редактор, гейты

**Зависит от:** S3a/S3b/S5. **Эффект:** система видимости не протекает туда, где ей нельзя.
**Риск:** низкий, но это то, что ломается молча.

* **Тени**: ни один теневой вью не читает результаты камеры (Debug-ассерт в `PrepareViewQueue`);
  `Legacy CSM`: каскад 0 — никогда (UE `:1149`), остальные — тоже никогда (дефолт UE 0); VSM — не
  трогать. Гейт: A/B `vis.occlusion`/`vis.hzb` при статичной камере теней — тени попиксельно те же.
* **RT**: `Main_BuildAS` строит AS по полному списку кастеров, не по видимым; отражения на камере
  «стена» показывают рощу за холмом в стекле/воде при любом состоянии тумблеров.
* **Редактор**: `ObjectId` readback и `GBuffer_Selected` — по S4-стриму; клик по объекту, скрытому
  окклюженом, не выделяет ничего (это корректно: его нет в кадре).
* **Гейты в док**: `--log-stress` 0/0, `cull validation PASS` (тени + камера), `--scene-stress-gbv=20`
  CLEAN, три конфига.

---

## S7 (опционально). GI-листва в GPU-driven камерный путь

`GpuInstancedModels`: cull + LOD-тир + fade на GPU для камеры (сегодня CPU, кап 256), теми же
кернелами S4/S5 (по-инстансные bounds уже есть в `boundsUnified_` для теней). Снимает кап 256
и даёт листве окклюжен. Делать, когда в уровнях появятся `gpuInstanced`-объекты (F2: сейчас их нет).

---

## 4. Рекомендованный порядок

```
S0   счётчики + scene.replicate + occlusion_test.json     [enabler, полдня]
S1   пер-вью маска чанков (frustum)                       [✅ 2026-09-03: камера 90→47, стена 90→26]
S3a  hardware queries + история + сферы локалов           [✅ 2026-09-04: стена K=4 −5.5 % GPU, дефолт ON]
S2   hzb_cull.hlsli + self-test                           [✅ 2026-09-03: 13/13, gpu == cpu]
S3b  HZB-тестер на истории S3a                            [✅ 2026-09-04: стена = queries (167/24), остров консервативнее (5 vs 26 чанков); ручка `vis.method:2`]
S5b  two-pass HZB для теневых вью (CSM по каскаду, VSM по странице) [S5b.1 CSM ✅ 2026-09-04: срезает 160–200 кастеров/каскад, картинка = пол, но −0.13 мс GPU на сегодняшнем контенте → дефолт OFF, ручка `csm.hzbCull`; S5b.2 VSM ✅ 2026-09-04: 728 пар/кадр на стене, паритет = пол, на стене ровно / в роще +0.11 мс → дефолт OFF, ручка `vsm.hzbCull`]
S4   G-buffer на ExecuteIndirect                          [✅ 2026-09-04: паритет = пол (стена, камера теней, кроссфейд 0.35), камерный ряд валидатора PASS, GPU ровно, воркеры 0.51→0.04 мс; дефолт ON, `gbuffer.indirect:0` откат]
S5   two-pass HZB внутри S4                               [✅ 2026-09-04: паритет = пол, валидатор `A + deferred`, стена K=4 при `vis.method:0` −6.4 % GPU (= путь queries без латентности), при queries в шуме; дефолт ON, `gbuffer.hzb:0` откат]
S6   правила границ + гейты                               [закрывает]
S7   GI-листва                                            [когда появится в уровнях]
```

Жёсткие зависимости: S5 ← S4 ← S0; S3a ← S0, S1; S3b ← S2, S3a (история); S5 ← S2; **S5b ← S1, S2**
(и indirect-путь теней, который уже есть — потому S5b раньше S4). S1, S2 и S3a независимы друг от
друга; S5b независим от S3a/S3b/S4. **S3a/S3b не обязательны для S5**, но остаются навсегда: для
transparent/glass/complex и для пропуска теневых пассов локальных источников.

Самые рискованные — S4 (паритет двух путей, см. предпосылку) и S5 (чтение через слоты кольца в
барьерном компиляторе). Перед S5 — прочитать `docs/enhanced_barriers_migration_plan.md` (декларации
`Prepare` авторитетны) и `async_compute_plan.md` (prereq `pHzb` у RTTrace).

---

## 5. Сверка с оригиналом UE — что транскрибируется дословно, что дельта

| Что | Файл UE | Статус |
|---|---|---|
| `BoxCullFrustumPerspective`, near-plane правило | `Shaders/Private/Nanite/NaniteHZBCull.ush:444-540` | транскрипция (S2) |
| `GetScreenRect`, `MipLevelForRect` | `NaniteHZBCull.ush:40-107` | транскрипция; **дельта:** не-pow2 HZB → реальные размеры мипа |
| `GetMinDepthFromHZB` 4×4, `IsVisibleHZB` | `NaniteHZBCull.ush:135-201` | транскрипция; сравнение `>=` без флипов (оба reverse-Z) |
| `RoundUpF16` против самоокклюжена | `HZB.usf:57-61`, `BuildInstanceDrawCommands.usf:200` | транскрипция (S2/S5) |
| two-pass: main против prev-HZB без prev-фрустума, post против cur | `NaniteCullingCommon.ush:445-512`, `NaniteCullRaster.cpp:6439-6505` | транскрипция (S5); **дельта:** муверы — объединение prev/cur бокса вместо `PrevLocalToWorld` |
| HW occlusion queries: батчеры 1 и 16, draw боксов, depth-test без записи | `SceneRendering.h:342-396`, `SceneOcclusion.cpp:335-534, 1243-1265` | транскрипция (S3a); **дельта:** `GREATER_EQUAL` = их `CF_DepthNearOrEqual` в нашем reverse-Z |
| латентность `NumBufferedOcclusionQueries`, `Map` ждёт GPU | `ConsoleManager.cpp:3989-3994`, `SceneOcclusion.cpp:107-133, 846-853` | S3a: `vis.queryLatency` 1 (стол) или `kFrameCount` |
| история и решающее дерево, стохастика ре-теста, «новый ⇒ виден», «near-plane ⇒ виден», «cut ⇒ сброс» | `ScenePrivate.h:108-266`, `SceneVisibility.cpp:2700-2935, 5367-5380` | дословно (S3a); `PrimitiveProbablyVisibleTime`/`MaxOcclusionPixelsFraction` — свойства UEngine, значения проверить по `Config/` |
| запросы сфер влияния spot/point; каскады выкл; VSM никогда | `SceneOcclusion.cpp:1109-1192, 283-306` | транскрипция (S3a.6) |
| HZB-тестер: bounds → тест → readback | `SceneOcclusion.cpp:946-1058`, `SceneVisibility.cpp:3133-3171` | по смыслу (S3b); **дельта:** structured buffer вместо 256×256-текстуры |
| `OCCLUSION_SLOP`, расширение недавно затестированных | `ScenePrivate.h:69`, `SceneVisibility.cpp:206-236, 2810-2823` | 1 см → `0.01f` (S3a) |
| сабпримитивы (HISM): виден ⇔ любой саб виден, никогда групповые | `SceneVisibility.cpp:2868, 2971-2980` | чанки (S1/S3a) |
| тени не кулятся камерой; каскад 0 никогда | `SceneOcclusion.cpp:56-62, :1149`, `ShadowSetup.cpp:4510-4519` | правило (S3a.6/S6) |

**Не сверено пока:** плоскостной `IsVisibleHZB(Rect, PlaneHZB)` (`NaniteHZBCull.ush:273-332`) —
кандидат на ужесточение теста для террейна, после S5.
