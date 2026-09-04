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
| Двухпроходный cull против prev-HZB + cur-HZB | **да, как S5** | Ноль латентности и ноль поппинга по построению; prev-HZB у нас бесплатна (F4). Требует GPU-driven G-buffer (S4). |
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

## S4. G-buffer на `ExecuteIndirect` (GPU-driven камерный путь)

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

### Критерий приёмки
* Камера теней и «стена», `--dlss=off`, океан выкл: G-buffer **попиксельно идентичен** CPU-пути
  (дифф = пол) — включая masked-материалы и LOD-crossfade; `objectId` readback совпадает (клик по
  объекту в редакторе — Release_Editor).
* K=4: `RenderObjectBatch.Async` и `Pass_GBuffer` CPU ↓ кратно; `GPU.Frame` не хуже; `cull validation
  PASS` расширен на камерные args (тот же валидатор, слот «камера»).
* `--scene-stress-gbv=20` CLEAN (новые ресурсы/пассы/дескрипторы — полный набор гейтов).

### Откат
`gbuffer.indirect:0`.

---

## S5. Двухфазный HZB-occlusion внутри GPU-driven G-buffer'а (Nanite two-pass)

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

### Критерий приёмки
* **Попиксельная идентичность** с S4 без HZB (`vis.hzb:0`) на статичных камерах И на `--cam-fly`
  серии через холм (покадрово); это и есть доказательство консервативности двух фаз.
* Камера «стена», K=4: `instancesDrawn` фазы A+B < S4; `GPU.Frame` ↓; `RenderObjectBatch` GPU ↓ —
  записать; `occludedList` после фазы 2 в readout (сколько «плохих догадок»).
* Валидатор: CPU-зеркало (S2) над теми же боксами и той же HZB (readback HZB мипа) даёт те же
  вердикты, что GPU — PASS в логе (расширение `cull validation`).
* Смена уровня / resize / камера-cut: нет кадра с дырами (проверка `--scene-stress`).

### Откат
`vis.hzb:0` — фаза 1 без HZB, фаза 2 пуста.

---

## S5b. Окклюжен со стороны СВЕТА: two-pass HZB для теневых вью (Legacy CSM по каскаду, VSM по странице)

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

### Критерий приёмки
* Тени **попиксельно** идентичны при `hzbCull:0/1` на трёх ракурсах (стена, камера теней, остров), с
  низким солнцем и `--set=ocean.visible:0` — консервативный cull не меняет ни одного пикселя.
* Стена K=4, низкое солнце: `casters` по каскадам в `csm_readout` падает на сотни, `Pass_CSM` GPU
  меньше на бо́льшую величину, чем стоят `Main_CsmHzb + Main_ShadowCullPost + Main_CSM_Post` (иначе
  шаг выключается по дефолту, а не «оставляется на потом»).
* `--cam-fly` вдоль стены: кадр в движении без дыр в тени (A/B off/on, дифф = пол) — двухпроходность
  проверяется, не постулируется.
* VSM: то же по `Pass_VsmPageRender` и картинке; `--scene-stress-gbv=20` CLEAN (новые ресурсы,
  барьеры, пассы — полный набор гейтов по типу правки); `cull validation PASS` при `csm.hzbCull:0`.

### Откат
`csm.hzbCull:0` / `vsm.hzbCull:0` — новые пассы не регистрируются, cull теней как сегодня.

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
S5b  two-pass HZB для теневых вью (CSM по каскаду, VSM по странице) [indirect-путь теней уже есть; сотня объектов под стеной]
S4   G-buffer на ExecuteIndirect                          [крупная работа, паритет картинки]
S5   two-pass HZB внутри S4                               [цель документа]
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
