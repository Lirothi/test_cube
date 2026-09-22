# План: текстурный стриминг и виртуальные текстуры (транскрипция UE 5.6)

Дата: 2026-09-20. Статус: **PLANNED, не начат.** Референс — дроп `D:\Programming\ue_strip`
(оба дерева, `Source/` и `Shaders/`). Каждая ссылка на UE и на наш код — с `file:line`, сверено
2026-09-20 по файлам; что не сверено — помечено «(сверить)». Документ написан как задание для
исполнителя: шаг берётся целиком, без переразведки, кроме чтения файлов, перечисленных в шаге.

## 0. Правила работы (обязательно к прочтению)

1. **Line endings.** C++/HLSL/project-файлы — CRLF (`AGENTS.md:28-51`); смешанные концы строк
   после правки — нормализовать. Проверять байтами (python), не grep'ом.
2. **Один шаг = одна логическая правка.** Следующий шаг не начинать, пока «Критерий приёмки»
   текущего не выполнен. Коммиты делает владелец; исполнитель оставляет однострочник к
   потенциальному коммиту.
3. **Читать оригинал UE перед шагом.** В каждом шаге есть «UE-референс» — прочитать эти
   строки ДО написания кода; формулы и порядок операций транскрибировать, не выводить заново.
   Отступления от UE записывать в шапке файла и в §5 этого документа.
4. **Не трогать:** VSM (`sources/rendering/shadows/VirtualShadowMap.*`) — только образец;
   RT-bindless (`sources/rendering/rt/BindlessTable.*`) — VT растрового пути с ним не связан;
   редакторские превью (`AssetThumbnailCache`, `texdecode`) — их текстуры в стриминг не
   регистрируются.
5. **Гейты по типу правки** (память `gate-discipline-by-change-type`): новые ресурсы /
   очереди / фенсы / пассы / дескрипторные таблицы → полный набор: три конфига
   (`Release`, `Debug`, `Release_Editor`, команды в памяти `allowlist-build-command`), Release
   `--scene-stress`, Debug `--gbv` (окно НЕ свёрнуто), компаратор барьеров; шейдерная
   арифметика и ручки → `python tools/check_shaders.py <substr>` + глаза.
6. **Замеры** — same-binary A/B переключением ручки (`streaming.enabled` / `vt.enabled`),
   фиксированная экспозиция (`--set=exposure.autoExposure:0`), `--dlss=off`, `--wind-freeze`,
   шумовой пол ДО A/B (два одинаковых прогона), 2–4 запуска на шаг, окна видимые. Камеры:
   пляж `--cam-pos=-54.81,3.00,63.71 --cam-rot=-0.0571,0.4842,0.0317,0.8725`, кольцо пальм
   `--cam-pos=-37.61,2.50,-98.03 --cam-rot=-0.0997,0.2987,0.0314,0.9486`, полёт над островом —
   камера владельца.
7. **Новые файлы** — в `test_cube.vcxproj` И `test_cube.vcxproj.filters` (C++ прямые слеши,
   шейдеры и filters — обратные); новые compute-шейдеры — в `tools/check_shaders.py`;
   `[RootSignature]` обязателен; `RecordComputeDispatch` делит на 8 (`numthreads(8,8,1)`);
   CB-структура в C++ — зеркало cbuffer'а; `#pragma pack_matrix(row_major)` при матрицах.
8. **Диагностика** — только `LOG_*` в session-лог (категория `Render`/`Asset`, `LogCategory.h:14`), никаких новых
   `logs/<name>.log`; ридауты в dev-окне; ручки — через `--set=` диспетчер в `App.cpp` (блок
   рядом с `exposure.localHighlightContrast`, `App.cpp:1022`) и UI-контрол в РЕАЛЬНУЮ вкладку с
   Apply-группой (память `ui-control-location`).
9. **Ассеты не переимпортировать по своей инициативе** (`--import`, запись в `models/`,
   `import_staging/`) — только по согласованию с владельцем; шаги, где это нужно, помечены.
10. **Ручка, освобождающая ресурс** (стриминг off, VT off) — только в точке резидентности с
    GPU-idle; тестировать `--sweep=ручка:1,0,1,0` (память `runtime-knob-frees-resource`).

## 1. Текущее состояние (инвентарь 2026-09-20)

* **Загрузка.** Единственный 2D-загрузчик — `Texture2D` (`sources/materials/Texture2D.h:16`):
  свой DDS-парсер `CreateFromDDS_` (`Texture2D.cpp:208`; BC1/2/3/4/5/7, RGBA8, RG16F, RGBA16F,
  RGBA32F — таблица `MapDXGIToPair`, `:148-177`, `FormatPair{resTypeless, srvUnorm, srvSRGB, isBC,
  bytesPerBlockOrPixel}` `:143`) и WIC для PNG/JPG (`:70`, всегда RGBA8, мипы на CPU
  `BuildMipChainRGBA8_` `:790`). `x.png` подменяется на соседний `x.dds` (`:426`, `:534-553`).
  **Цепочка мипов грузится целиком**: `td.MipLevels = mipCount` (`:289`), футпринты всех мипов
  `GetCopyableFootprints` (`:302-307`), SRV с `MostDetailedMip 0`, `ResourceMinLODClamp 0`
  (`:1083-1084`). Ресурс — `GpuResource tex_` (`Texture2D.h:177`) с декларацией в реестре;
  свой CPU-хип на один SRV `srvHeapCPU_/srvCPU_` (`:185-186`), per-frame копия в кольцо
  `GetSRVForFrame` (`:92`, `Texture2D.cpp:737-761`, кэш по `stagedFrame_`).
* **Аплоад.** `UploadBatch` (`sources/rendering/core/UploadBatch.h:21`): UPLOAD-буфер на
  текстуру (`Texture2D.cpp:309-345`), барьер COPY_DEST→read на списке вызывающего (`:375-382`,
  канон = `PIXEL | NON_PIXEL`, декларация `:388`), `SubmitAndWait` = стол (есть и неблокирующий
  `Submit`, `UploadBatch.h:34`). Бут `App.cpp:1208/1281-1282`; смена уровня `App.cpp:1545-1562` после
  `renderer.WaitForPreviousFrame()` — **CPU и GPU стоят**.
* **Async есть только у редактора и только CPU**: `texdecode` (`TextureDecodeCache.h:25`,
  `SubmitDetach`, ≤2 декода), `Texture2D::DeferDecodeScope` (`Texture2D.h:113`), потребитель
  `AssetThumbnailCache.cpp:921/1526`.
* **Общий кэш текстур** по ключу (путь, usage, normalIsRG, alphaCutoff) — `Texture2D.cpp:488-491`,
  `:556-560`; **LOD-компонента в ключе нет**.
* **Дескрипторы.** Растр не bindless: каждый материал каждый кадр копирует 3 SRV
  (`MaterialData::StageGBufferBindings`, `MaterialData.cpp:146-169`); кольцо кадра
  **4096 CBV/SRV/UAV** (`FrameScheduler.cpp:146-147`, bump с исключением `DescriptorHeapGPU.h:38-57`).
  Bindless только у RT (`BindlessTable.h:66`).
* **Материал** = 3 текстуры (`MaterialData.h:158-160`, `kGBufferSrvCount = 3` `:152`), JSON-ключи
  `albedo/mr/normal/emissive/shader/normalIsRG` (`MaterialDataManager.cpp:28-33`); семплер один
  `AnisoWrap(16)`, `MinLOD 0`, `MaxLOD FLT_MAX` (`SamplerManager.cpp:169-177`), единственный
  LOD-bias — DLSS (`MaterialData.cpp:167`, `Renderer.h:423-430`). Пер-материальные параметры —
  `MaterialSurfaceParams` (`MaterialData.h:37-60`, аплоад `MaterialData.cpp:192-201`).
* **Манифест** `models/<name>.mesh.json` читает `SceneObjectFactory.cpp:82` (`materials[]`
  `:180-182`, `texOffsScale` `:265`); списка текстур и плотности UV нет.
* **Импортёр** `sources/assets/AssetImporter.h:16`: `maxTextureSize = 2048` (`:20`),
  `bc5Normal = false` (`:24`), полная цепочка мипов (`FinishTextureDds`, `AssetImporter.cpp:322-360`),
  форматы `:507-509`, вывод `models/<name>/textures/*.dds`.
* **Границы объектов**: `RenderableObject::GetWorldBounds()` (`RenderableObject.h:115`, кэш
  `:313`); инстансные группы — `GpuInstancedModels::instancedWorldBounds_`
  (`GpuInstancedModels.h:96`, объединённый AABB группы).
* **Очереди.** `GraphicsDevice`: `queue_` + `computeQueue_` (`GraphicsDevice.h:102-103`), COPY
  аллокатор-слот предусмотрен (`FrameResource.h:57`); кросс-очередные точки —
  `Renderer::SetSubmitBatchCrossQueueWait` (`Renderer.h:371`), `FrameScheduler::CrossQueuePoint`.
  `TaskSystem::Get().SubmitDetach` (`sources/core/task/TaskSystemEnki.h:47`).
* **Учёт памяти.** `RegisterMemoryProvider(name, fn, self)` (`MemoryReport.h:35`), провайдеры
  `sky.luts`, `cloud.textures`, `rt.as*`; **текстурного нет, `TickMemoryReport` закомментирован**
  (`App.cpp:1537`). Имя ресурса = путь (`Texture2D.h:178-181`).
* **Образец резидентности** — VSM: `VirtualShadowMap.h:27-91` (`kPageSize 128`, пул 4096² =
  1024 страниц, запись `bit31 resident | bits0..15 phys page` `:155-158`, освобождение после N
  кадров `:175`), сверка `Scene.cpp:2370-2567`, HUD `DeveloperWindow.cpp:2780-2798`.
* **Диск.** DDS в `textures/ + models/ + data/`: 100 файлов, 244 МБ (BC7_SRGB 129 / BC7_UNORM
  56 / BC1 11 / RGBA16F 7 / BC6H 6 / BC4 2 по всему репо); 1024² ×46, 2048² ×25. PNG: 88,
  198 Мпикс, шесть 4096² камней (`models/rocks/textures/*.png`, 12–25 МБ) — импортёр давит их до
  2K. Уровень `wind_test`/`demo`: 16 материалов, 47 текстур, **76.6 МБ**; `atoll` 39.3 МБ.
  Террейн: `import_staging/{sandy_gravel,sandstone_cracks}_1k` (1024²×3), `{coast_sand_01,
  marble_cliff_05}_2k` (2048²×3); бомбинг — 3 тапа `SampleGrad` на карту
  (`terrain_tiling.hlsli:242-244, 271-275, 319`). Океан: шесть PNG без DDS (WIC).
* **GPU** — RTX 4090: Tiled Resources tier 4, Sampler Feedback tier 1.0, DirectStorage.

Вывод: стриминг нужен не ради VRAM (уровень — 40–80 МБ), а ради стола на загрузке, 4K+
исходников без даунскейла и как ядро для VT/RVT.

## 2. Целевая архитектура

```
Часть A (мип-стриминг)                       Часть B (SVT)                      Часть C (RVT)
Texture2D(streamable, resident mips)         VirtualTextureSpace (page table)   RvtProducer (ortho render → BC → pool)
  ↑ swap на границе кадра                    PhysicalSpace (пулы BC7 ×3)        terrain_tiling: RVT + Replace вблизи
TextureStreamingManager (wanted/budget)      VirtualTextureSystem (feedback)    инвалидация dirty-rect
  ↓ запросы                                    ↓ запросы тайлов
TextureStreamingIo (overlapped ReadFile) → TextureUploadRing (persistent-mapped UPLOAD)
  ↓                                            ↓
Main_TextureStreaming (первый пасс кадра, графическая очередь): копии ring→new, old→new (общие мипы),
                                              tile→pool, scatter page table; ридбэки feedback
```

Порядок кадра: `Main_PrologueClear → Main_TextureStreaming → … → Main_GBuffer (пишет VT feedback)
→ Main_VtFeedbackCompact → … → Main_Tonemap`. Все GPU-копии стриминга — на графической очереди
внутри графа (декларации барьеров как у остальных пассов); COPY-очередь — отступление на потом
(см. §5), потому что UE копирует общие мипы на рендер-потоке (`RHI.cpp:2297-2320`), а наш
барьерный реестр живёт в графе.

Части независимы по коду, но B использует IO-ринг, аплоад и retire-bin части A; C использует
page table/пул/feedback части B. Порядок: A1→A6, затем B1→B4 + C1→C3 вместе (или ни то, ни
другое — см. «Часть B — статус» перед B1). A6 (bindless для растра) стоит перед B, потому что
снимает единственный не-VT аргумент за SVT — батчинг indirect по материалам.

## 3. Шаги

### A1. Импортёр и метаданные мипов — 2 дня

**Цель.** DDS становится стримящимся контейнером без изменения формата: смещения мипов
считаются из заголовка; импортёр пишет плотность UV в манифест; текстуры могут создаваться с
частичной цепочкой (хвост).

**UE-референс.** Кук: `Engine/Private/TextureDerivedData.cpp:2879-2884` (срез верхних мипов),
`:2930-2934` (`FirstInlineMip = NumMips − NumNonStreamingMips`; `NumNonStreamingMips` не
сериализуется, рантайм = «число inline-мипов»), `:2568-2610` (`GetNumNonStreamingMips`),
`TextureDerivedDataTask.h:31` (`NUM_INLINE_DERIVED_MIPS = 7`), `TextureDerivedData.cpp:4369`
(`GMinTextureResidentMipCount = 7`), `Texture2DResource.cpp:261-270` (мип обязан быть плотно
упакован). Плотность UV: `TextureStreamingTypes.h:151-157` (`TexelFactor` = мировой размер
квадрата единичных UV), `TextureStreamingBuild.cpp:666-679` (`TextureDensity × ComponentScale`,
фолбэк `LocalUVDensities[0]`); расчёт плотности по мешу — `FStaticMeshRenderData::ComputeUVDensities`
(`Engine/Private/StaticMesh.cpp:3970-4029`, вызов `:4242`; арифметика —
`Engine/Private/Streaming/UVChannelDensity.h:13-81`).

**Файлы.** Изменить: `sources/materials/Texture2D.h/.cpp`, `sources/assets/AssetImporter.h/.cpp`,
`sources/app/scene/SceneObjectFactory.cpp`, `sources/app/main.cpp` (CLI). Создать:
`sources/rendering/streaming/DdsMipTable.h`.

**Конструкция.**
1. `DdsMipTable` (`DdsMipTable.h`): `struct Mip { UINT64 fileOffset; UINT width, height, rowPitchBytes,
   sliceBytes; }`, `std::vector<Mip> mips; UINT mipCount; DXGI_FORMAT format;` — заполняется в
   `CreateFromDDS_` из заголовка (DX10 `:236`) и `FormatPair::bytesPerBlockOrPixel/isBC`
   (`Texture2D.cpp:143`): для BC `rowPitch = max(1, (w+3)/4) × bytesPerBlock`, `slice = rowPitch ×
   max(1, (h+3)/4)`; для linear `w × bpp`. `fileOffset` — накопительно от конца заголовка (128
   или 148 байт). **Сверка**: сумма `sliceBytes` == размер файла − заголовок (assert при загрузке),
   и `slice` == `GetCopyableFootprints` `rowSizes×numRows` (`:302-307`) — иначе `LOG_ERROR`, текстура
   помечается `nonStreamable`.
2. `Texture2D::CreateDesc` (`Texture2D.h`, сверить поле) получает `bool streamable = false; UINT
   residentMips = 0;` (0 = все). `CreateFromDDS_`: при `streamable` создаёт ресурс с
   `td.MipLevels = residentMips` и грузит только мипы `[mipCount − residentMips, mipCount)`
   (хвост; `MostDetailedMip 0` ресурса = самый крупный из резидентных). Правило хвоста — как
   UE: `residentMips = min(mipCount, 7)` = `kNonStreamingMips`. На этом шаге по умолчанию
   `residentMips = mipCount` (всё резидентно) — изменение поведения приходит в A3.
3. Хранить на текстуре: `DdsMipTable mipTable_`, `std::wstring sourcePath_` (уже есть как
   `debugName_`/resolved path), `UINT residentMips_`, `int streamingIndex_ = -1`.
4. Импортёр: опция `ImportOptions::uvDensity = true` — по каждому материалу (примитиву glTF)
   как UE `FUVDensityAccumulator` (`Engine/Private/Streaming/UVChannelDensity.h:30-55`): на
   треугольник `d_i = sqrt(area_world_i(bakeScale) / area_uv_i)`, вес `w_i = sqrt(area_world_i)`;
   отсортировать по `d_i`, отбросить по 10 % сверху и снизу, `density = Σ w_i·d_i / Σ w_i`;
   запись в `mesh.json`:
   `"uvDensity": [d0, d1, …]` (порядок = `materials[]`). CLI `--import-max-tex=<n>` поверх
   `maxTextureSize` (`AssetImporter.h:20`); `bc5Normal` — оставить дефолт, включать флагом
   `--import-bc5` (не менять существующие ассеты втихую).
5. `SceneObjectFactory.cpp` (рядом с `texOffsScale`, `:265`): читать `uvDensity` в описание
   объекта; при отсутствии — `LOG_WARNING_ONCE` (`Log.h:806`) и дефолт 1.0 (`TexelFactor` тогда = мировой размер
   AABB объекта — грубая замена, как UE «unknown ref»).

**Ридаут/ручки.** Нет.

**Критерий приёмки.** (1) Все DDS уровня `wind_test` проходят самосверку таблицы мипов
(`LOG_INFO` со счётчиком «streamable N / nonStreamable M / png K»); (2) `--import` тестового
ассета (по согласованию с владельцем; можно на копию в scratchpad) пишет `uvDensity`, значение
для плоского квада с UV 0..1 и стороной 2 м = 2.0 (ручная проверка инварианта); (3) картинка
бит-в-бит с HEAD (все мипы резидентны).

**Гейт.** Три конфига; Release `--scene-stress`; без GBV (ресурсы не менялись).

**Откат.** `streamable = false` везде (код мёртвый).

### A2. IO-ринг, аплоад-ринг, пасс `Main_TextureStreaming`, подмена ресурса — 3 дня

**Цель.** Инфраструктура, по которой мипы приезжают с диска в GPU без стола: чтение на
воркере в persistent-mapped UPLOAD-ринг, копии на графической очереди первым пассом кадра,
подмена `ID3D12Resource` в `Texture2D` на границе кадра, старый ресурс в retire-bin.

**UE-референс.** Путь AsyncCreate: `Texture2DStreamIn_IO_AsyncCreate.cpp:21-56`
(`AllocateAndLoadMips → AsyncCreate → Finalize`), `Texture2DStreamIn.cpp:30-46` (буферы новых
мипов), `:107-118` + `RHI.cpp:2297-2320` (`CopySharedMips`: SRV→CopySrc/CopyDest, `CopyTexture`
общего хвоста), `D3D12Texture.cpp:851-907` (`RHIAsyncCreateTexture2D`: новые мипы из данных,
остальные нулятся), `:1287-1365` (`AsyncReallocateTexture2D_RenderThread`: `NumSharedMips =
min(old,new)`, per-mip `CopyTextureRegion` под барьерами), `Texture2DUpdate.cpp:158-172` +
`StreamableTextureResource.cpp:252-276` (`FinalizeStreaming`: `NumRequestedLODs = новое`,
`RHIUpdateTextureReference`), IO: `Texture2DStreamIn_IO.cpp:53-143` (батч-чтение прямо в целевую
память, `AIOP_FLAG_HW_TARGET_MEMORY`), `:173-196` (отмена), `StreamingManagerTexture.cpp:1610-1628`
(очередь `PendingMipCopyRequests`) + `:1679-1717` (`ProcessPendingMipCopyRequests`: N текстур в кадр).

**Файлы.** Создать: `sources/rendering/streaming/TextureUploadRing.h/.cpp`,
`TextureStreamingIo.h/.cpp`, `TextureRetireBin.h/.cpp`, `TextureStreamingPass.h/.cpp`;
изменить: `sources/rendering/core/RenderPass.h` (enum `Main_TextureStreaming` после
`Main_PrologueClear`), `sources/core/profiling/ProfilerScopes.h/.cpp` (`kPassTextureStreaming`),
`sources/app/scene/SceneRenderer_Graph.cpp` (регистрация пасса), `SceneRenderer.h`
(`TextureStreamingPoints`), `sources/materials/Texture2D.h/.cpp` (`AdoptResource`),
`sources/rendering/core/Renderer.h/.cpp` (владение подсистемой, тик на границе кадра),
`sources/app/App.cpp` (`--set=streaming.*`), vcxproj/filters.

**Конструкция.**
1. **`TextureUploadRing`**: один UPLOAD-хип `streaming.tempMemoryMB` (50, как
   `MaxTempMemoryAllowed`), persistent `Map`, кольцевой sub-аллокатор с выравниванием 512 байт
   (`D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT`) и 256 на ряд (`D3D12_TEXTURE_DATA_PITCH_ALIGNMENT`
   — при копировании из буфера в текстуру `rowPitch` в буфере должен быть кратен 256, поэтому
   IO читает мип построчно в выровненный layout, либо мип целиком, если `rowPitch` DDS уже
   кратен 256 — у BC7 2048² ряд = 512·16 = 8192 ✓, у 64² = 16·16 = 256 ✓; общий случай —
   построчная раскладка на воркере). Освобождение по фенсу графической очереди (значение
   фенса кадра, в котором копия записана + `kFrameCount`).
2. **`TextureStreamingIo`**: один поток-воркер (позже пул), очередь заявок `{texture, mipFirst,
   mipLast, ringOffset, cancelFlag, priority}`; `CreateFile(FILE_FLAG_OVERLAPPED)`, `ReadFile` по
   диапазону мипов (у DDS они подряд — одно чтение), в ринг; готовность — атомарный флаг +
   `completedFrame`. Отмена — флаг до старта чтения (как `FCancelIORequestsTask`); после старта —
   дочитать и выбросить. Файлы держать открытыми в LRU на 64 хендлов.
3. **`TextureRetireBin`**: `{GpuResource, retireFrame = frame + kFrameCount}`; дренаж на границе
   кадра, когда `retireFrame ≤ frame` (шаблон `rt.as.retired`: штамп `RtSceneAs.cpp:547`, освобождение
   `AccelerationStructure.cpp:538-542`).
   Провайдер памяти `tex.ret`.
4. **Пасс `Main_TextureStreaming`** (графическая очередь, сразу после `Main_PrologueClear`,
   `AddPass2` с билдером): для каждой заявки со статусом «данные в ринге»: (a) новый ресурс
   `render::CreateCommittedTexture(...)` с `MipLevels = newResident`, начальное состояние
   `COPY_DEST`, GpuResource с декларацией (имя = путь + `:s<N>`); (b) `CopyTextureRegion`
   ring→new для новых мипов (`D3D12_PLACED_SUBRESOURCE_FOOTPRINT` из `GetCopyableFootprints` по
   новому ресурсу, `:302-307` образец); (c) `ctx.Use(old, COPY_SOURCE)` → `CopyTextureRegion`
   old→new для общих мипов (`NumShared = min(oldResident, newResident)`, индексы со сдвигом,
   как `D3D12Texture.cpp:1313-1315`); (d) `NextPoint`: old → его канонический read state, new →
   `PIXEL_SHADER_RESOURCE | NON_PIXEL` (канон материальных текстур — см. `Texture2D.cpp:375-382`).
   Stream-out — то же с `newResident < oldResident` (только (a),(c),(d)). Лимит N заявок на кадр
   (`streaming.maxPerFrame`, 8) — амортизация UE.
5. **Подмена** — в serial-фазе билдера СЛЕДУЮЩЕГО кадра (копии этого кадра уже на GPU, их
   ждёт обычная синхронизация кадров): `Texture2D::AdoptResource(GpuResource&& newTex, UINT
   residentMips)`: `tex_` → retire-bin, `tex_ = new`, `mipLevels_ = residentMips`, пересоздать SRV
   в `srvHeapCPU_` (`Texture2D.cpp:1069-1084`), сбросить `stagedFrame_` тем же сентинелом, что три
   существующих места (`Texture2D.cpp:400/698/734` пишут `UINT(-1)` в uint64-поле `Texture2D.h:189` —
   привести все четыре к одному значению), тогда кольцо подхватит новый дескриптор в этом же кадре. Ключ общего кэша не меняется (ресурс один на всех
   потребителей, как у UE).
6. **Ручка** `streaming.forceMips:N` (тест без менеджера): каждой streamable-текстуре wanted =
   clamp(N); `--sweep=streaming.forceMips:12,4,12,4` гоняет подмену в обе стороны.

**Ридаут/ручки.** `streaming.tempMemoryMB`, `streaming.maxPerFrame`, `streaming.forceMips`;
`LOG_INFO` раз в 5 с: заявок в полёте / байт в ринге / retired.

**Критерий приёмки.** (1) `--sweep=streaming.forceMips:12,4,12,4` на `wind_test`: картинка при
12 == HEAD бит-в-бит после установления, при 4 — заметно размытые текстуры без артефактов;
(2) кадр без столов: `GPU.Frame` в профдампе при подмене ≤ +5 % к baseline; (3) реестр
состояний без FATAL, компаратор барьеров молчит; (4) утечек нет: `tex.ret` возвращается
к 0 (`tex.ret` в `mem:`), `[texcache]` при выходе без роста.

**Гейт.** Полный набор (новый пасс, ресурсы, декларации): три конфига, Release `--scene-stress`,
Debug `--gbv` (окно не свёрнуто), компаратор барьеров, `--sweep` выше в Release и Debug.

**Откат.** `streaming.enabled = 0` → пасс не регистрируется, `forceMips` игнорируется, все
текстуры создаются полными.

### A3. Менеджер: границы, wanted mips, бюджет, приоритеты, воркер — 3 дня

**Цель.** Транскрипция `FRenderAssetStreamingManager` + `FRenderAssetStreamingMipCalcTask`:
каждой streamable-текстуре — wanted mips из геометрии сцены и камеры, бюджет пула, приоритеты,
отмены; 5-кадровый конвейер с одной фоновой задачей.

**UE-референс.** Кадр: `StreamingManagerTexture.cpp:1898-2053` (стадии), `:1441-1531`
(нарезка), `:1592-1628` (выдача), `:753-807` (`UpdatePendingStates`, `PrepareAsyncTask`),
`:1742-1770` (пул). Задача: `AsyncTextureStreaming.cpp:1015-1065` (`DoWork`), `:77-103`
(`ComputeViewInfoExtras`: `ScreenSize = min(MaxEffective, width) × 0.5 × Boost`), `:139-360`
(wanted + форс-кейсы), `:569-861` (бюджет), `:409-467` (`TryDropMaxResolutions`), `:469-528`
(`TryDropMips`), `:530-567` (`TryKeepMips`), `:908-995` (load/cancel). Границы:
`TextureInstanceView.cpp:286-449` (ядро), `:451-496` (`ProcessElement`, `TexelFactor` FLT_MAX /
отрицательный), `TextureInstanceView.h:23-75` (`FBounds4`). Формулы: `StreamingTexture.cpp:306-333`
(`GetWantedMipsFromSize`), `:336-377` (`SetPerfectWantedMips_Async`), `:172-262` (биасы и пределы),
`:383-412` (retention), `:493-540` (load-приоритет, split-request), `:282-304` (`GetExtraBoost`).
Настройки: `TextureStreamingHelpers.cpp:73-324` (cvars), `:331-392` (`Settings::Update`).

**Файлы.** Создать: `sources/rendering/streaming/TextureStreamingManager.h/.cpp`,
`StreamingTexture.h`, `StreamingBounds.h/.cpp`, `StreamingSettings.h`. Изменить: `Scene.cpp`
(регистрация границ при построении очередей, `SceneRenderQueue`), `GpuInstancedModels.h/.cpp`
(экспорт `instancedWorldBounds_` + флаг «рисовалась в прошлом кадре»), `Renderer` (тик),
`App.cpp` (`--set`), `GraphicsSettings.h/.cpp` (группа Streaming), `DeveloperWindow.cpp` (вкладка).

**Конструкция.**
1. **`StreamingTexture`** (= `FStreamingRenderAsset`, поля 1:1): `residentMips, requestedMips,
   wantedMips, visibleWantedMips, hiddenWantedMips, budgetedMips, maxAllowedMips, minAllowedMips,
   numNonStreamingMips (=7), budgetMipBias, numMissingMips, lastRenderTime, maxSize, maxSizeVisibleOnly,
   bForceFullyLoad, bIsTerrain, bytesPerMip[]` + `retentionPriority, loadOrderPriority`.
2. **`StreamingBounds`**: записи `{AABB, texelFactor, lastRenderTime, minDistance, textures[]}`:
   статические объекты — по объекту × материал × 3 текстуры, `texelFactor = uvDensity[material]
   × modelScale / max(texOffsScale.z, texOffsScale.w)` (`texOffsScale = (offsU, offsV, scaleU,
   scaleV)`, `gbuffer_common.hlsli:114`; тайлинг уменьшает мировой размер единичного UV);
   инстансные группы — `instancedWorldBounds_` × те же материалы (консервативно, как крупный
   компонент у UE); `lastRenderTime` — время последнего кадра, где объект/группа прошли кулл
   (CPU-история или GPU-счётчик `vis`). SIMD по 4 не требуется (сотни записей).
3. **Расчёт (воркер)** — дословно: `ScreenSize = displayWidth × 0.5 × streaming.boost`, где
   `displayWidth` — ВЫХОДНОЕ разрешение (`Renderer::GetWidth()`), не рендер-разрешение под DLSS
   (UE подаёт `UnscaledViewRect.Width()`, `GameViewportClient.cpp:1769`; апскейл уже учтён тем,
   что семплер под DLSS выбирает мип как на дисплейном разрешении);
   `distSq = dist² до AABB` (`TextureInstanceView.cpp:350-370`), `≥ 1`; `normSize = ScreenSize ×
   rsqrt(distSq)`; `maxSize = max(texelFactor × normSize)`, `_VisibleOnly` только по записям с
   `lastRenderTime > now − 0.5 с` (`StreamingManagerTexture.cpp:1799`); оба размера умножаются на
   `BoostFactor = 0.71` группы (`GetExtraBoost`, `StreamingTexture.h:76-80`, применение
   `AsyncTextureStreaming.cpp:287-288`); `wanted = clamp(ceil(1 + log2(max(1, size))), min, max)`; `hidden = size × 0.5` (`HiddenPrimitiveScale`); `perfect = max(visible, hidden)`;
   `LODBias = budgetMipBias + (streaming.perTextureBias ? 0 : streaming.mipBias)` — глобальный
   bias у UE входит только при выключенном per-texture bias (`StreamingTexture.cpp:196-201`);
   `maxAllowed = clamp(mipCount − LODBias, numNonStreamingMips, mipCount)` (`:229-236`);
   `dlssBias` в стриминг НЕ входит (кап не может вернуть мип, которого не запросил размер);
   форс: `bForceFullyLoad` → `FLT_MAX`; `streaming.fullyLoadUsed` → всё, что рендерилось < 300 с.
4. **Бюджет**: `pool = streaming.poolSizeMB > 0 ? … : 0.7 × dedicatedVRAM`
   (`QueryVideoMemoryInfo`, `MemoryReport.cpp:185`); `available = pool − nonStreaming − margin`;
   бюджет сжимается сразу, растёт при запасе > `temp + margin` (`:654-665`); перебор →
   сортировка retention → `TryDropMaxResolutions` (per-texture bias, пока `streaming.perTextureBias`)
   → `TryDropMips` по одному с хвоста; недобор → `TryKeepMips` (без IO). Отмены in-flight:
   `requested > max(resident, wanted+1)` или `requested < min(resident, wanted)` (при «missing too
   many mips» — `< wanted`; `AsyncTextureStreaming.cpp:935-943`); stream-out ставится ПЕРЕД stream-in;
   один сверхбольшой запрос всегда пропускается.
5. **Конвейер**: стадия 0 — снимок (камера non-jittered, `renderWidth`, границы, `now`) +
   `TaskSystem::Get().SubmitDetach` (правило: детач + троттл, не ждать в кадре); стадии 1..3 —
   инкрементальное обновление `lastRenderTime`/границ по четвертям массива; стадия 4 — если
   задача готова: `StreamRenderAssets`: заявки в `TextureStreamingIo` (A2) по `loadOrderPriority`,
   split-request (`visible` первым при `budgeted ≥ streaming.minMipForSplit` и не террейн);
   иначе ждать следующего кадра (не блокировать). `streaming.framesForFullUpdate` (5).
6. **Инвариант для теста** (память `transcription-half-a-pair`): текстура 2048² (12 мипов),
   `texelFactor 10 м`, дистанция 20 м, ширина дисплея 2560, boost 1 → `size = 10 × 1280 / 20 × 0.71
   = 454` → `wanted = ceil(1 + log2 454) = 10` → резидентно 512². Зашить как `LOG_INFO` self-test при
   старте менеджера с `--set=streaming.selftest:1`.

**Ридаут/ручки.** `streaming.enabled, poolSizeMB, mipBias, boost, hiddenScale,
framesForFullUpdate, tempMemoryMB, minMipForSplit, perTextureBias, fullyLoadUsed, dropMips`;
вкладка dev-окна «Streaming» (`ResetStreaming` + непрерывный диапазон в `GraphicsControl`, как
`ResetFog` и остальные, `GraphicsSettings.h:15, 159-169`): пул/занято/бюджет, in/out МБ за кадр, гистограмма `wanted − resident`, список
текстур (путь, resident/wanted/budgeted, приоритет, lastRender). `LOG_INFO` раз в 5 с те же числа.

**Критерий приёмки.** (1) Инвариант п. 6 сходится; (2) смена уровня без стола: время от строки
`session cmdline=` до первого `screenshot saved` в session-логе при `--shot-delay=0` ≤ 0.3× HEAD
(baseline замерить ДО, 3 прогона); (3) `--set=streaming.poolSizeMB:24` на `wind_test`: менеджер
держит бюджет (ридаут «занято ≤ пул» после 30 кадров), нет пляски — суммарный in/out за
секунду при неподвижной камере → 0; (4) установившийся кадр при `poolSizeMB:-1` == HEAD на
трёх камерах в пределах пола шума (все нужные мипы приехали); (5) облёт (`--cam-fly=x,z --cam-fly-yaw=…`, `main.cpp:651-656`) — в
ридауте нет текстур с `wanted − resident > 2` дольше 1 с.

**Гейт.** Три конфига; Release `--scene-stress` (уровни переключаются под стримингом — главное
место для use-after-free); Debug `--gbv` один прогон; `--sweep=streaming.enabled:1,0,1,0`.

**Откат.** `streaming.enabled = 0`.

### A4. Fade мипов, провайдер памяти, debug — 1 день

**UE-референс.** `RenderCore/Public/RenderResource.h:289-334` (`FMipBiasFade`: `SetNewMipCount`,
`CalcMipBias`), `RenderResource.cpp:617-620` (`GMipFadeSettings {0.3, 0.1} Normal, {2.0, 1.0}
Slow`), `:633-690` (расчёт скорости), `StreamableTextureResource.cpp:227, 262` (вызовы при
финализации).

**Конструкция.** `MipBiasFade` (транскрипция) на `StreamingTexture`; каждый кадр
`Texture2D::SetMinLodClamp(float)` → пересоздание SRV в `srvHeapCPU_` с `ResourceMinLODClamp`
(`Texture2D.cpp:1084`) только у текстур в фейде (флаг), `stagedFrame_` сбросить. Провайдер
`tex` в `MemoryReport` (сумма `sliceBytes` резидентных мипов по реестру) + вернуть
`render::TickMemoryReport` в `App.cpp:1537`. `streaming.dropMips` (как `r.Streaming.DropMips`:
принудительно −N от `budgeted`) для визуальной проверки. `streaming.mipFade 0|1`, `mipFadeIn 0.3`,
`mipFadeOut 0.1`.

**Критерий приёмки.** Серия кадров при облёте с `mipFade:0` vs `:1` — при 1 подгрузка мипа не
читается как поп (GIF/серия, глазами); строка `mem:` (категория `Core`, `MemoryReport.cpp:210`)
содержит `tex X MB` и совпадает с суммой из ридаута ± 1 %. Имена провайдеров короткие (`tex`,
`tex.ret`, `vt`, `rvt`): буфер строки 512 байт с отсечкой при 40 байтах остатка
(`MemoryReport.cpp:204-206`).

**Гейт.** dxc не нужен; три конфига; глаза.

**Откат.** `streaming.mipFade = 0`; провайдер безвреден.

### A5. Первый потребитель: камни 4K без даунскейла — 0.5 дня (по согласованию с владельцем)

Переимпорт `models/rocks` с `--import-max-tex=4096` (и `--import-bc5` для нормалей, если
владелец согласен на смену формата); сравнить `mem:` до/после и close-up камня. Приёмка:
при `poolSizeMB:-1` камень вблизи 4K-резкий, вдали резидентно ≤ 1K (ридаут); память уровня
выросла не более чем на размер резидентных мипов камней.

### A6. Bindless для растра — 3 дня (решение 2026-09-22, перед частью B)

**Цель.** Убрать per-draw бинд текстур материала в растровом пути: текстуры адресуются
индексами в одном персистентном shader-visible хипе через SM6.6 `ResourceDescriptorHeap[]`
(как уже делает RT), per-draw копирование трёх SRV в кольцо кадра исчезает, а indirect-группы
с разными материалами могут идти одним `ExecuteIndirect` — материальные индексы едут
root-константами в команде. Это то, что мегатекстура давала «побочно»; здесь — напрямую и
без цены VT-семпла.

**Референс (наш код и UE).** RT-путь: `sources/rendering/rt/BindlessTable.h:55-135` (раскладка
хипа: `heap[f]` geometry-info, `kSceneBase + f·kScenePerFrame + i` per-frame регион,
`kGeoBase + 4·d` иммутабельные наборы, `kMaxDescriptors 8192`, `GetOrUpdateMesh`,
`WriteSceneDescriptor`), RT-диспатчи биндят ТОЛЬКО этот хип
(`SceneRenderer_Reflections.cpp:311/375/543/822`, `rtAs_.Bindless().Heap()`), проверка
возможностей `RtSmoke.cpp:97-107` (`ResourceBindingTier 3`, `HighestShaderModel ≥ 0x66`).
Сегодняшний растр: `MaterialData::StageGBufferBindings` (`MaterialData.cpp:146-169`) копирует
3 SRV на КАЖДЫЙ draw — вызовы `GBufferRenderable.cpp:669`, `InstancedDrawBatch.cpp:148/232`,
`ShadowGpuData.cpp:1070`; кольцо кадра 4096 (`FrameScheduler.cpp:146-147`); indirect G-buffer —
`t0..t2` в RS (`gbuffer_indirect.hlsl:21-23`), «one ExecuteIndirect per (group, LOD)»
(`SceneRenderer_Geometry.cpp:267`, ~1000 вызовов, CPU 0.19–0.28 мс `:277`); command signature —
только `DRAW_INDEXED`, без root signature (`Renderer.cpp:2111-2132`); маскированные тени —
массив `gMaskAlbedo[16]` + `NonUniformResourceIndex` (`shadow_indirect_csm.hlsl:250-314`,
кап `kMaxMaskedGroups = 16`, `ShadowGpuData.h:415`). `InstancePerObject` — 224 байта и **расти не
может** (`InstanceTypes.h:38-62`, static_assert; shadow stride, память `lod-crossfade`) —
индексы материала в инстанс НЕ кладём. UE: `D3D12RHI/Private/D3D12BindlessDescriptors.h/.cpp`
(менеджер ресурсных дескрипторов `:164`, семплеров `:74`, контекст `:102`; «has to handle
renames on command lists» — их вариант per-frame ретайра слотов; читать структуру, не API).

**Файлы.** Создать: `sources/rendering/descriptors/BindlessHeap.h/.cpp` (общий персистентный
хип; `BindlessTable` RT становится клиентом с зарезервированным регионом). Изменить:
`FrameScheduler.cpp:146-147` (кольцо кадра — партиция того же хипа), `Renderer.cpp:1163-1172`
(`BindDescriptorHeaps` биндит один CBV_SRV_UAV хип + семплерный), `BindlessTable.h/.cpp`
(`kSceneBase/kGeoBase` от базы региона), `Texture2D.h/.cpp` (`bindlessIndex_`, `EnsureBindless`),
`MaterialData.h/.cpp` (индексы в `SurfaceParams`), `gbuffer.hlsl` / `gbuffer_inst.hlsl` /
`gbuffer_indirect.hlsl` / `gbuffer_instcb.hlsl` (пермутация `GBUFFER_BINDLESS=1`),
`shadow_indirect_csm.hlsl` + `ShadowGpuData.cpp:1070/1216` (маска по индексу вместо массива 16),
`Renderer.cpp:2111` (вторая command signature `{CONSTANT ×3, DRAW_INDEXED}` с root signature),
CS, пишущий indirect-аргументы (`shadow_cull_cs.hlsl` / G-buffer args builder — добавить
3 uint индексов группы в команду), `SceneRenderer_Geometry.cpp:262-300` (один `ExecuteIndirect`
на LOD-тир × PSO), `App.cpp` (`raster.bindless`), vcxproj/filters, `check_shaders.py`.

**Конструкция.**
1. **Один shader-visible CBV_SRV_UAV хип** (tier 3 — до 1M дескрипторов; берём 65536):
   `[кольцо кадра 3 × 4096][RT-регион 8192][слоты текстур …]`. Биндится один раз на список —
   D3D12 держит один CBV_SRV_UAV хип одновременно, поэтому кольцо кадра и bindless-слоты
   ОБЯЗАНЫ жить в одном хипе; `DescriptorAllocator` кольца работает в своей партиции без
   изменений интерфейса.
2. **Слот текстуры иммутабелен.** `Texture2D::EnsureBindless()` при первом использовании пишет
   SRV в новый слот и запоминает индекс; **подмена ресурса (A2 `AdoptResource`) = НОВЫЙ слот +
   новый индекс**, старый слот уходит в retire-bin вместе со старым ресурсом и переиспользуется
   через `kFrameCount` кадров. Это снимает гонку «кадр N−1 на GPU ещё читает слот, который кадр N
   переписал» — правило ABA из памяти `gpu-helpers-have-fixed-shapes`, применённое к дескрипторам.
3. **Материал** несёт `uint albedoIdx, mrIdx, normalIdx` в `SurfaceParams`
   (`gbuffer_indirect.hlsl:35`, per-frame CB — индексы обновляются каждый кадр вместе с ним, так
   что новый слот после подмены виден в следующем кадре); семплер остаётся статическим
   `AnisoWrap(16)` (с DLSS-bias) — bindless-семплеры не нужны. Пермутация `GBUFFER_BINDLESS=1`:
   `Texture2D t = ResourceDescriptorHeap[NonUniformResourceIndex(idx)]`; `NonUniformResourceIndex`
   обязателен там, где индекс различается внутри волны (слитые indirect-группы).
4. **Indirect.** Command signature `{D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT (root-слот констант,
   3 uint), DRAW_INDEXED}`, stride 32 байта, `CreateCommandSignature(&desc, rootSignature, …)` —
   с root-аргументами сигнатура требует root signature; кулл-CS пишет индексы материала группы в
   первые 12 байт команды. Группы одного PSO/LOD-тира сливаются в один `ExecuteIndirect` с общим
   count-буфером (список аргументов — уже сплошной по группам, `MeshManager.cpp:2109`).
   Маскированные тени: `gMaskAlbedo[16]` → индекс из тех же констант, кап 16 групп снимается.
5. **Фолбэк** — `raster.bindless = 0` (или tier < 3 / SM < 6.6 по `RtSmoke`-проверке): staged
   tables как сегодня; обе ветки живут, пока bindless не принят по умолчанию.

**Ридаут/ручки.** `raster.bindless 0|1`; `LOG_INFO` при старте: tier/SM, размер хипа, слотов
занято; в профдампе — число `ExecuteIndirect` на кадр и CPU-время записи indirect-списка.

**Критерий приёмки.** (1) Картинка бит-в-бит `raster.bindless:1` vs `:0` на трёх камерах
(те же SRV, тот же семплер); (2) `wind_test`: `ExecuteIndirect` на кадр ~1000 → ≤ 50, CPU-запись
indirect-списка ниже нынешних 0.19 мс (`SceneRenderer_Geometry.cpp:277`), `Pass_GBuffer` GPU не
хуже; (3) Debug `--gbv` чистый (GBV ловит выход индекса за хип); (4) RT- и VSM-паритет (RT-регион
хипа переехал, `kSceneBase/kGeoBase` пересчитаны); (5) `--sweep=raster.bindless:1,0,1,0`.

**Гейт.** Полный набор (новый хип, RS, command signature, шейдерные пермутации).

**Откат.** `raster.bindless = 0`.

---

### Часть B — статус (решение 2026-09-22)

Аргументы за SVT сами по себе закрыты: память и загрузку без стола даёт часть A, «один бинд
на всё» и батчинг indirect даёт A6 дешевле и без VT-семпла, а наши текстуры малы относительно
видимого (резидентный набор ≈ вся текстура, см. пример с пальмами: 600 инстансов на всех
дистанциях просят все мипы 1K-атласа). **B делается только как ядро для C** — page table,
пул, feedback, аплоад тайлов нужны RVT террейна; B1 (`.vt`-импортёр) и SVT-материалы —
только если появятся ассеты 8K+. Решение «B+C или ничего» — по замеру бомбинга: профдамп
`Pass_GBuffer` террейна с бомбингом против одного тапа на пляжной висте; ниже ~0.2 мс — не
окупает 3.5 недели, 0.5 мс и выше плюс желание мирового слоя под декали/мокрость — делать.

### B1. Импортёр `.vt` и офлайн-валидатор — 2 дня

**Цель.** Контейнер стримящихся VT-тайлов от импортёра — транскрипция
`FVirtualTextureBuiltData` в минимальной живой форме (только `RawGPU`).

**UE-референс.** `Engine/Private/VT/VirtualTextureBuiltData.h:15-30` (пределы, кодеки —
живы константные и `RawGPU`), `:37-75` (`FVirtualTextureDataChunk`), `:78-127`
(`FVirtualTextureTileOffsetData`: разреженный Morton→offset), `:130-237` (`FVirtualTextureBuiltData`:
`TileSize`, `TileBorderSize` — **бордер запечён в тайл**, `GetPhysicalTileSize = Tile + 2·Border`
`:207`, чанк на мип для верхних, один на все нижние `:153-157`, `GetTileOffset` `:225`),
`VirtualTextureBuildSettings.cpp:9-30` (`r.VT.TileSize 128`, `TileBorderSize 4`, клампы).

**Файлы.** Изменить: `AssetImporter.h/.cpp` (режим `--import-vt`), создать:
`sources/rendering/vt/VtBuiltData.h` (заголовок формата, общий с рантаймом),
`tools/vt_validate.py`.

**Конструкция.** Файл `<name>.vt`: заголовок `{magic 'VTX1', tileSize 128, border 4, numLayers,
layerFormat[8] (DXGI), layerFallback[8] (RGBA8), width, height (текселей мипа 0), numMips,
chunk[numMips] {offset, size}, tileBytesPerLayer[8]}`; тайл = `(128+8)² ` блоков формата слоя,
слои подряд; тайлы мипа — в Morton-порядке (`vAddress`), плотно (`TileOffsetData` — только
identity на первом срезе: `offset = vAddress × tileStride`); бордер берётся у соседей по
адресному режиму (WRAP для тайлящихся, CLAMP иначе — параметр импорта); уровни от «один
тайл» (128²) и ниже — один финальный чанк (у UE `MaxLevel = CeilLog2(min(WidthInTiles,
HeightInTiles))`, `AllocatedVirtualTexture.cpp:79`, ниже одного тайла ничего нет). Энкодинг слоёв — тот же DirectXTex BC7/BC5 GPU-путь импортёра
(`AssetImporter.cpp:114-154`), по тайлам. `vt_validate.py`: перечитать заголовок, проверить
`Σ tiles × stride == chunk.size`, Morton-обход, бордеры (сравнить края соседних тайлов с
исходником).

**Критерий приёмки.** `--import-vt` на `models/rocks` (копия в scratchpad, без записи в
`models/` до согласования): валидатор зелёный, размер файла = Σ чанков; для 4096² при 128:
32×32 тайла мипа 0, 6 уровней (0..5), уровень 5 = один тайл, хвоста нет.

**Гейт.** Только валидатор + сборка трёх конфигов.

### B2. VT-ядро без feedback: page table, пулы, `vt_common.hlsli`, пермутация G-buffer — 4 дня

**Цель.** Семплинг VT в G-buffer с обновлением page table по CPU-эвристике части A (те же
wanted mips → все страницы уровня в кадре) — отделить семплинг от feedback.

**UE-референс.** Данные: `VirtualTexturing.h:55-60, 66-102, 145-190, 356-373`,
`VirtualTextureSpace.cpp:37-48` (формат page table), `:307-336` (мипы, рост),
`AllocatedVirtualTexture.cpp:74-81` (`MaxLevel`), `:89-99` (16/32 бит), `:121-128` (Morton),
`:493-556` (**упаковка юниформ**), `VirtualTexturePhysicalSpace.h:16-24, 96-102`,
`VirtualTextureProducer.cpp:74-94` (`TileSize = Tile + 2·Border`), `VirtualTextureSystem.cpp:966-1034`
(размер пула, кап 181²), `PageTableUpdate.usf:67-83` (**запись**), `TexturePageMap.h:55-64` (vLogSize
vs vLevel — читать первым), `TexturePageMap.cpp:113-141, 485-531` (маппинг предка вниз, перекраска
при анмапе), `TexturePagePool.cpp:9-10, 204-216, 276-338` (reserved 0, free heap, alloc/lock).
Шейдер: `VirtualTextureCommon.ush:113-129` (адресный режим после LOD), `:140-152` (`MipLevelAniso2D`),
`:240-247` (IGN-шум), `:258-286` (LOD), `:324-370` (`Load` page table), `:756-808` (`VTUniform_Unpack`,
`VTComputePhysicalUVs`), `:811-856` (`SampleGrad`, fallback).

**Файлы.** Создать: `shaders/vt_common.hlsli`, `shaders/vt_pagetable_update_cs.hlsl`,
`sources/rendering/vt/{VirtualTextureSpace, VirtualTexturePhysicalSpace, TexturePagePool,
TexturePageMap, VtProducerSvt, VirtualTextureSystem}.h/.cpp`; изменить: `gbuffer.hlsl` (и
`gbuffer_inst.hlsl` / `gbuffer_indirect.hlsl` / `gbuffer_instcb.hlsl` — те, что рисуют
VT-материалы; сначала статический `gbuffer.hlsl`), `MaterialData.h/.cpp`, `MaterialDataManager.cpp`
(ключ `"vt"`), `RenderTargetManager` (не нужен: page table и пулы — персистентные ресурсы
подсистемы, как VSM-пул), `SceneRenderer_Graph.cpp` (`Main_TextureStreaming` получает
VT-работу: scatter page table + аплоад тайлов), `check_shaders.py`, vcxproj/filters.

**Конструкция.**
1. **Space**: page table `R32_UINT` (позже R16 при пуле ≤ 64² тайлов), размер = аллоцированный
   quadtree-экстент (мин 32²), полная цепочка мипов, UAV+SRV, GpuResource с канонным NPS
   (обновление — UAV в `Main_TextureStreaming`). Аллокатор виртуальных адресов — quadtree с
   Morton (`VirtualTextureAllocator.h:104-163` — транскрибировать в объёме `Alloc/Free`).
2. **PhysicalSpace**: группа из 3 слоёв (`albedo BC7_SRGB`, `normal BC7_UNORM/BC5`, `mr
   BC7_UNORM`), тайл **136**, `vt.poolSizeMB` (48 на группу) → `tilesPerSide = floor(sqrt(bytes /
   (3 × tileBytes)))`, кап 181; по одной 2D-текстуре на слой, SRV sRGB/linear.
3. **Запись page table** (`vt_pagetable_update_cs.hlsl`, транскрипция `PageTableUpdate.usf:67-83`):
   `vLevel[0:3] | pX[4:11] | pY[12:19]`; `pAddress 0` = «нет»; scatter из structured buffer
   `{vAddress, vLevel, vLogSize, pTile}` по мипам `[vLogSize..0]` квадами `2^(vLogSize−mip)`
   (семантика `ExpandPageTableUpdateMasked`, `VirtualTextureSpace.cpp:394-416`) — наше
   отступление в механизме (compute вместо инстансных квадов), не в результате.
4. **`vt_common.hlsli`**: `VTPageTableUniform` (2×uint4), `VTUniform` (uint4/слой) — упаковка
   как `AllocatedVirtualTexture.cpp:493-556`; `TextureComputeVirtualMipLevel` (анизо-формула +
   `vt.noise` IGN + `dlssBias`), `TextureLoadVirtualPageTable` (`Load` на уровне),
   `VTComputePhysicalUVs` (`UVScale = (4096 >> vLevel)/4096`, бордер), `TextureVirtualSample`
   (`SampleGrad`, fallback при 0). Семплер — существующий `AnisoWrap(16)` → для VT нужен CLAMP
   (адресация внутри пула!) — новый пресет `AnisoClamp(16)` в `SamplerManager`.
5. **Материал**: JSON `"vt": "models/rocks/textures/Rock01.vt"` → `MaterialData::vt_` (хендл в
   `VirtualTextureSystem`), биндинги G-buffer: page table SRV + 3 пула вместо 3 текстур — в пермутации `GBUFFER_VT=1`
   таблица `t0` (`gbuffer.hlsl:26`, `numDescriptors=3`) расширяется до 4 дескрипторов,
   `kGBufferSrvCount` (`MaterialData.h:152`) становится пермутационным (`StageGBufferBindings`,
   `MaterialData.cpp:154-161`); юниформы в `MaterialSurfaceParams`.
6. **CPU без feedback**: `VirtualTextureSystem::UpdateHeuristic` — для каждого VT-материала по
   wanted mips из A3 запросить все тайлы уровня `mipCount − wanted` в видимой области (на первом
   срезе — все тайлы уровня; для 4K/128 на уровне 1024² это 64 тайла), маппинг предков, LRU-пул
   (`TexturePagePool`), корневые страницы залочены и произведены синхронно при загрузке. Тайлы
   читаются через `TextureStreamingIo` (A2) и копируются `CopyTextureRegion` ring→pool в
   `Main_TextureStreaming` со `SkipBorder = 0` (бордер запечён).

**Ридаут/ручки.** `vt.enabled, vt.poolSizeMB, vt.noise, vt.aniso, vt.maxAniso (8), vt.maxUploads
(32)`; `LOG_INFO`: страниц отображено / резидентно / пул занят.

**Критерий приёмки.** (1) Камень 4K через VT vs прямой семплинг 4K DDS (A5) на close-up:
разность в пределах шума стохастического трилинейного (`vt.noise:0` → паритет по глазам,
разность < 2/255 на 99 % пикселей); (2) при `vt.poolSizeMB:4` пул переполняется и картинка
деградирует по мипам (предки), не дырами и не `FallbackValue`; (3) `outOfBounds`
(семплы, попавшие в запись 0) = 0 после установления.

**Гейт.** Полный набор (новые ресурсы/UAV/root-слот); `check_shaders vt`; `--sweep=vt.enabled:1,0,1,0`.

**Откат.** `vt.enabled = 0` → материал биндит DDS-фолбэк (тот же `.dds` рядом, 2K).

### B3. Feedback, `VirtualTextureSystem`, приоритеты, префетч — 4 дня

**UE-референс.** Запись: `VirtualTextureCommon.ush:29-58` (выбор одного семпла на пиксель),
`:361-370` / `:409-417` (упаковка `SpaceID<<28 | vPageX | vPageY<<12 | (vLevel+1)<<24`),
`:60-77` (джиттер `(SvPos & TileMask) == JitterOffset` и `InterlockedAdd` на счётчик),
`BasePassPixelShader.usf:2378-2384`; ресурс: `VirtualTextureFeedbackResource.cpp:16-37,
76-104, 106-246, 305-322, 332-365` (размер `ceil(W/16)·ceil(H/16)·2`, компакция хэш-таблицей,
последовательность джиттера); ридбэк: `VirtualTextureFeedback.cpp:9-14, 119-128, 196-310`
(ринг 8, латентность 3); CPU: `UniquePageList.h:25-84`, `UniqueRequestList.h:148-152, 379-501`,
`VirtualTextureShared.h:88-123` (приоритет), `VirtualTextureSystem.cpp:197-211` (бюджеты),
`:1436-1469` (анализ), `:1551-1600` (LRU-touch батч), `:1602-2008` (**gather**: декод, предок,
лестница `L − min(2, L)`, load/mapping), `:2229-2292` (`SubmitThrottledRequests`), `:2597-2655`
(`FinalizeRequests`), `:2778-2783` (`PendingFrameDelay 3`), `:2852-3083` (Begin/End),
`TexturePagePool.cpp:126-184` (`EvictPages`), `VirtualTexturePhysicalSpace.cpp:21-55, 242-286`
(residency bias); `GPUFeedbackCompaction.usf` (транскрибировать).

**Файлы.** Создать: `shaders/vt_feedback_compact_cs.hlsl`, `sources/rendering/vt/{VtFeedback,
UniquePageList, UniqueRequestList}.h/.cpp`; изменить: `RenderTargetManager` (per-frame
`vtFeedback` structured uint `ceil(W/16)·ceil(H/16)·2 + 1`, `vtFeedbackCompact`, READBACK-ринг
`kFrameCount + 2` = 5 слотов), `gbuffer.hlsl` (`RWStructuredBuffer<uint> VtFeedback : u0` в RS пермутации `GBUFFER_VT`),
`SceneRenderer_Graph.cpp` (пасс `Main_VtFeedbackCompact` после `pGbufDone`: компакция →
`CopyResource` в READBACK слот; декларации UAV/COPY), `VirtualTextureSystem`.

**Конструкция.** Транскрипция §4.2–4.3 справочника: буфер feedback очищается в
`Main_PrologueClear` (элемент 0 = счётчик), PS пишет запрос при совпадении джиттера,
`InterlockedAdd` на счётчик; компакция → `(pageId, count)`; ридбэк с фенсами кадров
(READBACK-ринг `kFrameCount + 2` = 5 слотов: при `kFrameCount = 3` слот кадра N переписывается
кадром N+3 до чтения, запаса нет; UE держит `MaxTransfers = 8`, `VirtualTextureFeedback.h:61`;
латентность = 3 кадра по `PendingFrameDelay`); CPU: `UniquePageList` (8K) → `GatherRequests` (декод, резидентная →
LRU-touch, нерезидентная → `FindNearestPageAddress` → немедленный маппинг предка + лестница
префетча + `AddLoadRequest`) → `UniqueRequestList::Sort` по приоритету `Locked > Streaming >
Producer > Count·(1+L)` с бюджетами `vt.maxUploads 32`, `vt.maxProduced 32` → заявки IO → в
`Main_TextureStreaming` аплоад тайлов + scatter page table. `PageFreeThreshold 15`, residency
bias при заполнении > 0.95 (`vt.residency.*`). Эвристика B2 остаётся как `vt.feedback:0`
(отладочный режим).

**Критерий приёмки.** (1) Ридаут: запрошено/резидентно/промахов; при неподвижной камере
промахи → 0 за ≤ 5 кадров после прихода IO; (2) облёт вокруг камня: поп ограничен лестницей
(в серии кадров нет `FallbackValue`, переходы только между соседними мипами); (3)
`vt.poolSizeMB:8` — residency-bias поднимает mip bias, ридаут это показывает, дыр нет;
(4) стоимость: `Main_VtFeedbackCompact` ≤ 0.05 мс, запись feedback в G-buffer ≤ +2 % `Pass_GBuffer`.

**Гейт.** Полный набор (UAV в G-buffer RS, READBACK, новый пасс).

**Откат.** `vt.feedback = 0` (эвристика B2), `vt.enabled = 0`.

### B4. Debug и UI — 1 день

`vt.borders` (розовая рамка в тайлах при импорте — как `r.VT.Borders`, флаг импортёра),
debug-view «раскраска по уровню page table» в `TextureDebugViewer`/`debug.texMode`, `vt.flush`
(эвикт всего), `vt.dump` (`LOG_INFO` таблицы страниц), провайдер `vt`,
вкладка «VT» dev-окна; провайдеры `vt` (пулы + page table). Приёмка: команды работают, `mem:` показывает пулы.

### C1. RVT террейна: продюсер, ортокамера, BC-компрессия, семплинг с `Replace` — 3 дня

**Цель.** Террейн печётся в страницы VT (albedo + normal/roughness) один раз и живёт, пока не
инвалидирован; G-buffer террейна семплит стек RVT одной page-table-выборкой вместо 3×3 тапов
бомбинга; вблизи — прямой бомбинг (`Replace`).

**UE-референс.** `RuntimeVirtualTextureProducer.cpp:41-120, 166-206` (батч ≤ 8, `UVRange` с
бордером, `Saturated` пока сцена не готова), `RuntimeVirtualTextureRender.cpp:1027-1306`
(`FRenderGraphSetup::Init`: какие пассы), `:2034-2134` (`RenderPage`: ортографическая
reversed-Z камера вниз по Z объёма, `MipLevel`, `PackHeight`), `:2218-2306` (копия/компрессия в
пул, `DirectCompress`), `Shaders/Private/VirtualTextureMaterial.usf:53-63, 120-141` (что пишет
материал, упаковка нормали/высоты), `VirtualTextureCompress.usf` (BC1/BC5 compute — транскрибировать),
`Engine/Private/VT/RuntimeVirtualTexture.cpp:316-370` (описание продюсера, соотношение сторон
объёма), `:439-494` (форматы), `:583-604` (приоритеты), `:606-616` (`WorldToUV`),
`VirtualTextureCommon.ush:875-926` (`VirtualTextureWorldToUV`), `:928-988` (распаковка),
`MaterialExpressionRuntimeVirtualTextureSample.h:16-66` (mip-режимы, `Replace`).

**Файлы.** Создать: `sources/rendering/vt/VtProducerRvt.h/.cpp`, `shaders/rvt_page.hlsl`
(VS/PS: террейн-материал с `RVT_PAGE=1`, MRT0 albedo, MRT1 `(Nx, Ny, 0, 0)`, MRT2 `(rough, 0, 0, 0)`),
`shaders/vt_compress_bc1_cs.hlsl`, `shaders/vt_compress_bc5_cs.hlsl`, `shaders/vt_compress_bc4_cs.hlsl`;
изменить:
`terrain_tiling.hlsli` (ветка `TERRAIN_RVT`: `SampleRvt(worldXZ, ddx, ddy)` + `Replace` по
дистанции), `gbuffer*` террейна, `SceneRenderer_Graph.cpp` (пасс `Main_RvtProduce` перед
`Main_GBuffer`, графическая очередь: рендер страниц в MRT 136² (texture2DArray на батч ≤ 8) →
compute BC → `CopyTextureRegion` в пул), `Scene` (объём RVT = объединённый AABB объектов
`renderLayer: Terrain`), `GraphicsSettings`/`App.cpp` (`rvt.*`).

**Конструкция.** Тип слоёв — как UE `BaseColor_Normal_Roughness` (у UE это ДВА слоя: DXT1 +
DXT5 `(Nx, Rough, Ny)`, `VirtualTextureMaterial.usf:127`, `RuntimeVirtualTexture.cpp:445-448`);
наше отступление — **три слоя одной группы: BC1 sRGB albedo + BC5 `(Nx, Ny)` + BC4 roughness**,
три MRT и три энкодера (BC1/BC5/BC4 проще BC7/DXT5 на GPU). Виртуальное разрешение:
`rvt.virtualSize` (16384 при острове ~1 км → тексель 6 см, 128 тайлов, 8 уровней); `Replace`:
`rvt.nearReplaceDistance` (подобрать; за ней бомбинг 2 см). Производство: `RvtProducer::
RequestPageData → Available`; `RenderFinalize` батчит ≤ 8; на страницу — ортопроекция над
`UVRange` страницы С бордером, reversed-Z, глубина по террейну (Z вниз), VS/PS террейна как в
G-buffer, но без освещения; после батча — BC-компрессия compute прямо в пул (UAV-алиасинг
`R32G32_UINT` на BC1, `R32G32B32A32_UINT` на BC5 — как `DirectCompress`); бюджет
`rvt.maxUploads 2` (редактор 32). Корневые уровни (≤ 1024² виртуальных) производятся при
загрузке уровня синхронно и залочены. Нижние мипы RVT — из тех же страниц (рендер на
уровне), стриминга нижних мипов (UE `StreamLowMips`) — нет.

**Критерий приёмки.** (1) Скриншот-паритет с бомбингом на средней/дальней дистанции
(`rvt.enabled:1` vs `:0`, пол ДО): разность < 4/255 на 99 % пикселей террейна дальше
`nearReplaceDistance`; вблизи — бит-в-бит (Replace); (2) `Pass_GBuffer` террейна: −30 % и более
на пляжной висте (3 тапа → 1 стек); `Main_RvtProduce` при неподвижной камере → 0 мс после
установления, при облёте ≤ 0.3 мс; (3) `rvt.poolSizeMB` в ридауте, переполнение → residency
bias, не дыры.

**Гейт.** Полный набор (новый пасс, MRT-массив, компрессия в пул). **BC-форматы не UAV-able**,
и typeless-каст в D3D12 не выходит за семейство формата: SRV BC1 на `R32G32_TYPELESS` отвергается
в `CreateShaderResourceView`. UAV-алиасинг BC, который делает UE (`GRHISupportsUAVFormatAliasing`),
в D3D12 возможен только через `CreateCommittedResource3` с `CastableFormats = {BC1_UNORM,
R32G32_UINT}` при `OPTIONS12.RelaxedFormatCastingSupported && EnhancedBarriersSupported`
(`D3D12RHI/Private/Windows/WindowsD3D12Device.cpp:1806-1821`; фолбэк — heap-aliasing двух
placed-ресурсов, `:1823`), а у нас enhanced-путь `CreateCommittedResource3` выключен по
умолчанию (`sources/rendering/core/TextureCreate.h:13-15`). **Первый срез — запасной путь UE**:
compute пишет блоки в промежуточную UINT-текстуру (`R32G32_UINT` для BC1/BC4, `R32G32B32A32_UINT`
для BC5), затем `CopyTextureRegion` в BC-пул (как `CopyPagesToOutput`,
`RuntimeVirtualTextureRender.cpp:2218-2248`); прямая запись — отдельный шаг после включения
enhanced barriers. Проверить GBV.

**Откат.** `rvt.enabled = 0` → террейн бомбится как сегодня.

### C2. Инвалидация и редактор — 1 день

`RvtProducer::Invalidate(worldRect, priority)` → `EvictPages(rect, MaxLevelToEvict,
DirtyPagesKeptMappedFrames 8)` (`TexturePagePool.cpp:126-184`): видимые остаются отображены и
перепекаются, остальные выкидываются; триггеры: правка `MaterialSurfaceParams` террейн-материала
в Inspector (весь объём), смена ассета террейна (весь объём), `--set=rvt.flush`. Приёмка:
правка ручки бомбинга в редакторе видна на террейне через ≤ 8 кадров без «дырок»; ридаут
«грязных страниц».

### C3 (опц.). Мокрый песок и каустика в слой RVT

`Main_ShoreWetness` пишет маску в 4-й слой (Mask4, BC4) вместо экранного пасса; террейн
читает её из того же стека. Только после C1/C2 и отдельного замера.

## 4. Справочник UE (что транскрибируется; для чтения по шагам)

### 4.1. Мип-стриминг

* Менеджер `FRenderAssetStreamingManager` (`StreamingManagerTexture.h:30`), состояние
  `FStreamingRenderAsset` (`StreamingTexture.h:21`, поля по категориям потоков `:188-195`).
* Кадр: `UpdateResourceStreaming` (`StreamingManagerTexture.cpp:1898`) — стадии по
  `r.Streaming.FramesForFullUpdate` (5): стадия 0 `UpdatePendingStates` + `PrepareAsyncTask` + запуск
  `FRenderAssetStreamingMipCalcTask` (`:1972-1974`); стадии 1..N `UpdateStreamingRenderAssets`
  (`:1441`, окно `[i·Num/N, (i+1)·Num/N)`) + `IncrementalUpdate`; финал `StreamRenderAssets` (`:1592`).
  `DoWork` (`AsyncTextureStreaming.cpp:1015`): `ComputeViewInfoExtras → UpdateBoundSizes_Async →
  per-asset UpdateOptionalMipsState/UpdatePerfectWantedMips → UpdateBudgetedMips → UpdateLoadAndCancelationRequests
  → UpdatePendingStreamingStatus`.
* Wanted: `ScreenSize = min(MaxEffectiveScreenSize, viewWidth) × 0.5 × Boost`
  (`AsyncTextureStreaming.cpp:90-103`, ширина `GameViewportClient.cpp:1769`); `distSq` до бокса
  (`TextureInstanceView.cpp:350-370`); `MaxSize = max(TexelFactor × ScreenSize × rsqrt(distSq))`
  (`:405-408, 467-468`); `_VisibleOnly` по `LastRenderTime > WorldTime − 0.5` (`:415-419`,
  `StreamingManagerTexture.cpp:1799`); **`WantedMips = clamp(ceil(1 + log2(max(1,Size))), Min, Max)`**
  (`StreamingTexture.cpp:306-333`); `Hidden = Size × 0.5` (`:366-376`); биасы `:193-202`;
  `MaxAllowed/MinAllowed` `:229-248`; extra boost 0.71 (`StreamingTexture.h:76-80`, применяется к обоим размерам `AsyncTextureStreaming.cpp:287-288`).
* Бюджет (`AsyncTextureStreaming.cpp:569-861`): `Available = Pool − NonStreaming − Margin`
  (`:634-635`), рост только при запасе (`:654-665`), retention-сортировка
  (`AsyncTextureStreaming.h:96-115`; веса `StreamingTexture.cpp:383-412`: +4096 меш, +2048 keep,
  +1024 visible, +512 <8 МБ, +256 hi-prio/<200 КБ, +clamp(255 − LastRender)), `TryDropMaxResolutions`
  (`:409-467`, `StreamingTexture.cpp:420-449`), `TryDropMips` (`:469-528`), `TryKeepMips`
  (`:530-567`). Load/cancel (`:908-995`): temp-бюджет, отмена при `Requested > max(Resident,
  Wanted+1)`, stream-out перед stream-in, один большой пропускается.
* IO/GPU: выбор `Texture2D.cpp:1937-2071`; generic `TextureStreamIn.cpp:264-526`; чтение
  `Texture2DStreamIn_IO.cpp:53-143`; AsyncCreate `Texture2DStreamIn_IO_AsyncCreate.cpp:21-56`,
  `Texture2DStreamIn.cpp:107-175`, `RHI.cpp:2297-2320`; D3D12 `D3D12Texture.cpp:851-907, 1287-1365`;
  финал `StreamableTextureResource.cpp:252-276`; fade `RenderResource.h:289-334`,
  `RenderResource.cpp:617-690`. **RHI virtual streaming в D3D12 не реализован**
  (`DynamicRHI.cpp:571-579`, `Core/Public/HAL/Platform.h:318-319`, `StreamableTextureResource.cpp:38-56`).
* Cvars (`TextureStreamingHelpers.cpp`): `UseNewMetrics 1 :73`, `Boost 1 :79`,
  `MaxEffectiveScreenSize 0 :95`, `PoolSize −1 :119`, `MaxTempMemoryAllowed 50 :138`,
  `DropMips 0 :146`, `HiddenPrimitiveScale 0.5 :180`, `MipBias 0 :190`, `UsePerTextureBias 1 :200`,
  `FullyLoadUsedTextures 0 :207`, `UseAllMips 0 :219`, `NumStaticComponentsProcessedPerFrame 50 :243`,
  `MinMipForSplitRequest 10 :256`, `FramesForFullUpdate 5 :282`; `AmortizeCPUToGPUCopy 0` /
  `MaxNumTexturesToStreamPerFrame 0` (`StreamingManagerTexture.cpp:1568/1575`).

### 4.2. VT: данные и шейдер

* Пределы `VirtualTexturing.h:55-60`; page table по слоям `R16/32*_UINT` (`VirtualTextureSpace.cpp:37-48`),
  полная цепочка мипов (`:307-320`); 16 бит при пуле ≤ 64² (`AllocatedVirtualTexture.cpp:89-99`).
* Запись `vLevel[0:3] | pX[4:9|4:11] | pY[10:15|12:19]` (`PageTableUpdate.usf:67-83`);
  `pAddress 0` reserved (`TexturePagePool.cpp:9-10`); валидность `(Packed >> 4) != 0`
  (`VirtualTextureCommon.ush:781-786`).
* Юниформы: 2×uint4 page table + uint4/слой (`AllocatedVirtualTexture.cpp:493-556`); распаковка
  `VirtualTextureCommon.ush:154-202, 756-779`.
* Пул: `TileSize = Tile + 2·Border` (`VirtualTextureProducer.cpp:76`), одна текстура на слой,
  размер `floor(sqrt(bytes/(count·tileBytes)))`, `SplitPhysicalPoolSize`, кап 181²
  (`VirtualTextureSystem.cpp:966-1034`), дефолт 64 МБ (`VirtualTexturePoolConfig.h:64`).
* Шейдер: LOD анизо + IGN (`:140-152, 240-247, 259-287`), адресный режим после LOD (`:113-129`),
  `Load` page table на уровне (`:324-370`), физический UV (`:789-808`), `SampleGrad` (`:811-856`),
  предки вниз по мипам (`TexturePageMap.cpp:485-531`, анмап `:113-141`), корни залочены
  (`AllocatedVirtualTexture.cpp:200-258`).
* Feedback: `VirtualTextureCommon.ush:29-77` (один семпл на пиксель, джиттер, запись), упаковка
  запроса `:361-370`,
  `VirtualTextureFeedbackResource.cpp:305-365` (размер, последовательность), компакция
  `:106-246` + `GPUFeedbackCompaction.usf`; ридбэк `VirtualTextureFeedback.h:61` (`MaxTransfers = 8`),
  `.cpp:9-14` (латентность 3).

### 4.3. VT: CPU, продюсеры, диск

* `Update = Begin → End → Finalize` (`VirtualTextureSystem.cpp:3072-3083`), async-задача
  (`:2852-2893`), финал (`:2597-2655`); dirty-RVT только при доступном feedback (`:2976-2987`).
* `UniquePageList` 8K (`UniquePageList.h:25-29`); `PendingFrameDelay 3` (`:2778-2783`);
  gather (`:1602-2008`): предок `FindNearestPageAddress` (`:1822-1874`), лестница `L − min(2,L)`
  (`:1877-1951`); `UniqueRequestList` (`UniqueRequestList.h:148-152, 379-501`), приоритет
  (`VirtualTextureShared.h:88-123`); бюджеты (`:197-211`); continuous-updates случайно (`:2010-2036`).
* `TexturePagePool`: free heap `(Frame<<4)|L` (`:204-216`), `PageFreeThreshold 15`,
  `EvictPages` (`:126-184`), residency bias (`VirtualTexturePhysicalSpace.cpp:242-286`).
* `ApplyUpdates` (`VirtualTextureSpace.cpp:371-581`): квады, volatile VB («flushes the RHI
  thread!» `:460`), отложенное обновление при реаллокации (`:380-389`).
* Продюсеры: контракт `VirtualTexturing.h:286-354` (статусы `:213-232`, две фазы финализатора
  `:194-211`); SVT `UploadingVirtualTexture.cpp:98-146, 326-439`; чанки
  `VirtualTextureChunkManager.cpp:64-163` (один read на все слои, `Saturated`); транскод
  `VirtualTextureTranscodeCache.cpp:78-240, 355-459`; аплоад `VirtualTextureUploadCache.cpp:172-211,
  279-525, 589-612` (страницы 4 МБ, `SkipBorder`, освобождение через 2 кадра, бюджет 64 МБ/2000).
* RVT: `RuntimeVirtualTextureProducer.cpp`, `RuntimeVirtualTextureRender.cpp:1027-1306, 2034-2134,
  2218-2306`, `VirtualTextureMaterial.usf:120-141`, `RuntimeVirtualTexture.cpp:316-370, 439-494,
  583-616`, `RuntimeVirtualTextureSceneProxy.cpp:168-248`, Landscape `LandscapeProxy.h:598-641`,
  `LandscapeRender.cpp:1580-1588, 2520-2543`; adaptive `AdaptiveVirtualTexture.h:15-21`,
  `VirtualTextureCommon.ush:289-321`.
* Cvars: `r.VT.TileSize 128` / `TileBorderSize 4` (`VirtualTextureBuildSettings.cpp:9-19`),
  `MaxUploadsPerFrame 2` / `.Streaming 32` / `MaxTilesProducedPerFrame 32` /
  `MaxContinuousUpdatesPerFrame 1` / `FeedbackFactor 16` / `PageFreeThreshold 15` /
  `DirtyPagesKeptMappedFrames 8` / `MaxAnisotropy 8` (`VirtualTextureScalability.cpp:12-138`),
  `PoolSizeScale 1` / `SplitPhysicalPoolSize 0` (`VirtualTexturePoolConfig.cpp:20-41`),
  `UploadMemoryPageSize 4` / `MaxUploadMemory 64` / `MaxUploadRequests 2000`
  (`VirtualTextureUploadCache.cpp:13-39`), `FeedbackLatency 3` (`VirtualTextureFeedback.cpp:9-14`),
  `Residency.* 4/0.95/0.95/0.65/0.2` (`VirtualTexturePhysicalSpace.cpp:21-55`).

### 4.4. Ограничения UE, о которых знать

Отложенный `ApplyUpdates` при реаллокации page table (`VirtualTextureSpace.cpp:382-388`);
аплоад page table стопорит RHI-поток (`:460`); feedback устаревает на 3 кадра; корни не
гарантированно резидентны (`VirtualTextureSystem.cpp:222-226`, `todo[VT]`); пул ≤ 181²; RVT
рендерит все слои при частичном запросе (`RuntimeVirtualTextureProducer.cpp:191-193`);
persistent-buffer аплоад обходит RHI (`VirtualTextureUploadCache.cpp:198-200`); 3D VT не доделан.

## 5. Отступления от UE

| UE | Мы | Почему |
|---|---|---|
| RHI virtual texture streaming (tiled) | нет | в D3D12 у UE не реализовано |
| IoStore / bulk data / DDC | DDS (смещения из заголовка) + свой `.vt` | без ассет-пайплайна UE |
| Копия общих мипов на рендер-потоке (`RHI.cpp:2297`) | копия в графе, пасс `Main_TextureStreaming`, графическая очередь | барьерный реестр живёт в графе; COPY-очередь — потом |
| `FMipBiasFade` через bias семплера | `ResourceMinLODClamp` SRV на кадр | SRV и так копируется каждый кадр |
| Инстансные квады для page table | compute-scatter | «This flushes the RHI thread!» |
| zlib/Crunch | только `RawGPU` | у UE deprecated |
| DXT5 `(Nx, Rough, Ny)` для RVT | BC1 + BC5 + BC4 | нужен только простой GPU-энкодер |
| Ринг ридбэка `MaxTransfers = 8` (`VirtualTextureFeedback.h:61`) | `kFrameCount + 2` = 5 слотов + фенс кадра | латентность 3 с запасом 2 кадра |
| Adaptive / 3D / lightmap VT, `StreamLowMips` RVT | нет | нет потребителя |
| Меши в стриминг-менеджере | нет | свой контракт LOD |
| Лимит подмен в кадр 0 (без лимита) | `streaming.maxPerFrame` 8 всегда | амортизация копий общих мипов в графе |
| DirectStorage / tiled resources / Sampler Feedback | возможные отступления на потом | не путь UE; вернуться, если копия общих мипов дорога |

## 6. Ручки: UE → `--set`

| UE | наша | дефолт |
|---|---|---|
| `r.TextureStreaming` | `streaming.enabled` | 1 |
| `r.Streaming.PoolSize` | `streaming.poolSizeMB` | −1 (70 % VRAM) |
| `r.Streaming.MipBias` | `streaming.mipBias` | 0 |
| `r.Streaming.Boost` | `streaming.boost` | 1.0 |
| `r.Streaming.HiddenPrimitiveScale` | `streaming.hiddenScale` | 0.5 |
| `r.Streaming.FramesForFullUpdate` | `streaming.framesForFullUpdate` | 5 |
| `r.Streaming.MaxTempMemoryAllowed` | `streaming.tempMemoryMB` | 50 |
| `r.Streaming.MinMipForSplitRequest` | `streaming.minMipForSplit` | 10 |
| `r.Streaming.UsePerTextureBias` | `streaming.perTextureBias` | 1 |
| `r.Streaming.FullyLoadUsedTextures` | `streaming.fullyLoadUsed` | 0 |
| `r.Streaming.DropMips` | `streaming.dropMips` | 0 |
| `r.Streaming.MaxNumTexturesToStreamPerFrame` (UE: 0 = без лимита, только при `AmortizeCPUToGPUCopy`) | `streaming.maxPerFrame` | 8 (отступление, §5) |
| `GEnableMipLevelFading`, `GMipFadeSettings` | `streaming.mipFade`, `mipFadeIn/Out` | 1 / 0.3 / 0.1 |
| — | `streaming.forceMips`, `streaming.selftest` | тест |
| — (UE bindless RHI, `D3D12BindlessDescriptors`) | `raster.bindless` | 1 при tier 3 / SM 6.6, иначе 0 |
| `r.VT.TileSize` / `TileBorderSize` | импорт: `vt.tileSize` / `vt.border` | 128 / 4 |
| `VirtualTexturePoolConfig` | `vt.poolSizeMB` (на группу) | 48 |
| `r.vt.FeedbackFactor` / `FeedbackLatency` | `vt.feedbackFactor` / `vt.feedbackLatency` | 16 / 3 |
| `r.VT.MaxUploadsPerFrame.Streaming` / `MaxUploadsPerFrame` | `vt.maxUploads` / `rvt.maxUploads` | 32 / 2 |
| `r.VT.MaxTilesProducedPerFrame` | `vt.maxProduced` | 32 |
| `r.VT.PageFreeThreshold` | `vt.pageFreeThreshold` | 15 |
| `r.VT.MaxAnisotropy` / `AnisotropicFiltering` | `vt.maxAniso` / `vt.aniso` | 8 / 1 |
| стохастический трилинейный | `vt.noise` | 1 |
| `r.VT.Residency.*` | `vt.residency.*` | 0.95/0.95/0.65/0.2/4 |
| `r.VT.EnableFeedback` | `vt.feedback` | 1 |
| `r.VT.RVT.DirtyPagesKeptMappedFrames` | `rvt.dirtyKeptFrames` | 8 |
| `RuntimeVirtualTextureReplace` | `rvt.nearReplaceDistance` | подобрать |
| — | `rvt.virtualSize` | 16384 |
| — | `rvt.enabled` / `rvt.poolSizeMB` / `rvt.flush` | 1 / 32 / debug |
| `r.VT.Borders` / `Flush` / `Dump` | `vt.borders` / `vt.flush` / `vt.dump` | debug |

## 7. Риски

* **Копия общих мипов при каждой подмене** — трафик при колебаниях wanted; гасится `TryKeepMips`
  и `PageFreeThreshold`-подобной выдержкой; если дорого — tiled resources (§5).
* **`rowPitch` 256 в UPLOAD-ринге** — DDS-мипы не всегда выровнены → построчная раскладка на
  воркере (A2 п.1), проверить на 64×1024-полосках листвы.
* **Барьеры/реестр**: подмена только в serial-фазе, retire-bin обязателен (ABA-ловушка
  per-frame кэшей — память `gpu-helpers-have-fixed-shapes`), имя ресурса с суффиксом
  поколения. **Каждый `DeclareCreated` бампает `CanonicalStateRegistry::generation_` и
  сбрасывает кэш компиляции барьеров** (`ResourceDeclarations.h:99`): при 8 подменах в кадр кэш
  бьётся каждый кадр — регистрировать новые ресурсы одной пачкой раз в кадр и замерить CPU
  компиляции графа до/после; если дорого — отдельная «внеграфовая» регистрация без бампа
  поколения (сверить, что кэш действительно per-generation).
* **Кольцо дескрипторов 4096/кадр** — VT-материал биндит 4 SRV вместо 3; при сотнях
  VT-материалов пересмотреть (bindless для растра — отдельный шаг).
* **Ключ кэша `Texture2D`** — ресурс один, wanted = max по потребителям (как UE); ключ не
  расширять.
* **DLSS** — `ScreenSize` части A считается по ширине ДИСПЛЕЯ (иначе стриминг недогрузит под
  апскейлом); в LOD VT (B2) `GetDlssMipBias` входит явно, потому что производные там
  рендер-разрешения.
* **Feedback-латентность 3 + IO** → поп; лестница префетча + предки + fade.
* **BC-энкодер на GPU** (C1) и **UAV-алиасинг BC** — проверить на GBV; запасной путь через
  UINT-текстуру + копию.
* **RVT-разрешение против 2 см бомбинга** — `Replace` вблизи; adaptive — потом.
* **Память**: пул SVT 48 + RVT 32–48 + ринг 50 + page tables — больше сегодняшнего уровня;
  оправдание — 4K+ ассеты и амортизированный террейн, замерять `mem:`.
* **Редакторские превью** не регистрируются (`nonStreamable`).

## 8. Порядок и оценка

A1 (2) → A2 (3) → A3 (3) → A4 (1) → A5 (0.5) → A6 (3) ≈ 2.5 нед.; затем решение по замеру
бомбинга: B1 (2) → B2 (4) → B3 (4) → B4 (1) ≈ 2.5 нед. + C1 (3) → C2 (1) ≈ 1 нед., либо ничего
(B без C не делается; B1 — только под ассеты 8K+). C3 — опционально. Первый видимый результат —
A3 (смена уровня без стола); первый перф-выигрыш — A6 (~1000 → ≤ 50 `ExecuteIndirect`), затем
C1 (террейн без 3× бомбинга).

## 9. Файлы UE для дословного чтения

**A:** `Engine/Private/Streaming/StreamingManagerTexture.cpp` (`:1898`, `:1441`, `:1592`, `:1742`,
`:381`), `AsyncTextureStreaming.cpp` (`:139`, `:409-861`, `:908`, `:1015`), `StreamingTexture.cpp/.h`
(`:306`, `:336`, `:383`, `:493`, `:172`), `TextureInstanceView.cpp/.h` (`:286`, `:451`),
`TextureStreamingHelpers.cpp/.h`, `Engine/Private/Texture2D.cpp:1937-2109`,
`Texture2DStreamIn_IO.cpp` + `Texture2DStreamIn.cpp` + `Texture2DUpdate.cpp`,
`Texture2DMipDataProvider_IO.cpp` + `Texture2DMipAllocator_AsyncCreate.cpp`,
`TextureDerivedData.cpp:2568-2644, 2845-3347`, `D3D12RHI/Private/D3D12Texture.cpp:851-907,
1287-1365, 430-479`, `RenderCore/Public/RenderResource.h:289-334`,
`Public/Streaming/StreamableRenderResourceState.h`.

**B/C:** `RenderCore/Public/VirtualTexturing.h`, `Shaders/Private/VirtualTextureCommon.ush`,
`Shaders/Private/PageTableUpdate.usf`, `Renderer/Private/VT/AllocatedVirtualTexture.cpp`,
`VirtualTextureSystem.cpp` (`:77-211`, `:966-1111`, `:1602-2008`, `:2229-2655`, `:2852-3083`),
`VirtualTextureSpace.cpp`, `TexturePagePool.cpp/.h`, `TexturePageMap.cpp/.h`, `UniqueRequestList.h`,
`UniquePageList.h`, `VirtualTextureShared.h`, `VirtualTextureFeedbackResource.cpp`,
`VirtualTextureFeedback.cpp`, `Shaders/Private/GPUFeedbackCompaction.usf`,
`RuntimeVirtualTextureRender.cpp`, `RuntimeVirtualTextureProducer.cpp`,
`Engine/Private/VT/VirtualTextureUploadCache.cpp`, `VirtualTextureTranscodeCache.cpp`,
`VirtualTextureBuiltData.h`, `Engine/Private/VT/RuntimeVirtualTexture.cpp`,
`Renderer/Private/VT/RuntimeVirtualTextureSceneProxy.cpp`, `Shaders/Private/VirtualTextureMaterial.usf`,
`Shaders/Private/VirtualTextureCompress.usf`, `Landscape/Private/LandscapeRender.cpp:1580-1588,
2520-2543`.
