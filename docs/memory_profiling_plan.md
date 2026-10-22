# Профайлинг аллокаций памяти (CPU + GPU) — план

Написан 2026-10-22 по просьбе владельца («запланируй создание профайлинга аллокаций памяти. как пример анрил
в D:\zalupa»). **Статус: M0 `9f5ba576`, M1 `044b2b99`, M2 `31e7703d`, M3 `5355e8d6`, M4 `9cdb24b4` закоммичены 2026-10-22; M5
сделан (не закоммичен); следующий — M6.** Заменяет черновик «На будущее:
трасса памяти» из `docs/bug_rt_retire_bin_leak.md:83-103`. Референс — UE 5.8.2 в `D:\zalupa\Engine`; все ссылки на
UE ниже даны от этого корня и сверены 2026-10-22 (выборочно — по файлам; что не сверено — помечено «(сверить)»).

Документ — задание для исполнителя: шаг берётся целиком, без переразведки, кроме чтения файлов, перечисленных в
шаге. Решения владельца — §8 (Q1-Q5 приняты 2026-10-22).

---

## 0. Правила

1. **Сначала UE.** Перед шагом прочитать его «UE-референс»; форматы событий, упаковку, правила запросов
   транскрибировать, не изобретать. Отступление — в §7 с причиной.
2. **Один бинарник.** Учёт компилируется во всех трёх конфигурациях (Debug, Release, Release_Editor) и включается
   ключом; цена выключенного учёта замеряется на каждом шаге (A/B в одном бинарнике, шумовой пол до A/B).
3. **Диагностика = события.** Отчёты, сводки, вердикты гейтов — `LOG_*` в session log
   (`AGENTS.md` → «Diagnostics are events, not files»). Единственный новый файл плана — бинарная трасса M4 в `traces/`
   (разрешено владельцем, Q1).
4. **Сборка/файлы.** CRLF в C++/проектах (проверка байтами), новые файлы — в `test_cube.vcxproj` И `.filters`,
   сборка всех трёх конфигураций, `python tools/check_logging.py` перед сдачей шага.
5. **Гейты по типу правки.** Учёт GPU-ресурсов (M1) меняет только код создания/освобождения — Debug с отладочным
   слоем + `--scene-stress` (смена уровней = массовое создание/освобождение). Проходов, барьеров и состояний план не
   трогает, GBV не нужен. Новые MCP-инструменты — кейс в `tools/intent_regression.cpp` `TestMcpDrivesTheSameRoad`.
6. **Замеры** — живой редактор через MCP с закрытым UI (F2) там, где это картинка/readout; headless — для гейтов.
   2–4 запуска на шаг. Камера утечки — орбита над рощей: `--level=data/levels/occlusion_test.json --cam-pos=0,8,0
   --cam-orbit=35,14` (рецепт из `docs/bug_rt_retire_bin_leak.md:37-39`).
7. **Ассеты и уровни не трогаем** (никаких `--import/--bake`, записей в `data/`).
8. **Сначала доказать, что ловит.** Каждый детектор (трасса, гейт) проверяется на нарочно внесённой утечке
   (`--mem-selftest`, M6) до того, как его «тишине» поверят — как `--gbv-selftest`.

---

## 1. Зачем

Что сейчас нельзя узнать:

1. **Кто держит VRAM.** `mem:` знает 7 провайдеров (`tex`, `tex.ret`, `sky.luts`, `rt.as`, `rt.as.bin`,
   `rt.as.retired`, `cloud.textures`) — это 5 подсистем из ~30, создающих ресурсы. Остальное — одна цифра
   `vram local`.
2. **Кто держит CPU-кучу.** `private ≈ mi commit ≈ 2.5 ГБ` уже на старте (`docs/bug_rt_retire_bin_leak.md:72-73`) —
   «не утечка, но разложить по владельцам когда-нибудь». Разложить нечем. **M0 (2026-10-22): посылка ложная** —
   `mi_process_info` отдаёт как «commit» private всего процесса; сам mimalloc держит ~160 МБ, а 2 ГБ из 2.9 —
   write-combined страницы GPU-отображений (итоги M0 ниже).
3. **Какая аллокация растёт.** Утечку корзины RT (2026-09-03) нашли снапшотами Visual Studio, и то по случайности:
   VS видит HeapAlloc рантайма D3D12, но не mimalloc и не VRAM (`bug_rt_retire_bin_leak.md:24-26`). Строка `mem:`
   говорит «сколько и примерно кто», не «какая аллокация и с какого стека» (`MemoryReport.h:21-23`).
4. **Сколько аллокаций в кадре.** Число `new/delete` за кадр по потокам — перф-метрика, которой нет вовсе.
5. **Возвращается ли память после смены уровня.** `--scene-stress` гоняет уровни, но память не сверяет.

Цель — то, что в UE дают LLM + Memory Insights + `rhi.DumpResourceMemory` + MemReport, в масштабе нашего движка:

| Вопрос | Инструмент UE | Наш шаг |
|---|---|---|
| Кто держит VRAM, по ресурсам | `rhi.DumpResourceMemory` | M1 + M2 |
| Кто держит память, по владельцам (дерево тегов) | LLM, `stat LLM`, `-llmcsv` | M1 (GPU), M3 (CPU) |
| Что выросло между A и B, с какого стека | Memory Insights, правила `AaBf`/`AaBCf` | M4 + M5 |
| Сколько аллокаций в кадре, кто churn | `MallocFrameProfiler`, правило `AafB` | M0, M5 |
| Утечка за смену уровня / за минуту полёта | MemReport до/после руками | M6 (гейты) |

---

## 2. Как у UE (сводка, проверено по коду 2026-10-22)

### 2.1 LLM — счётчики по тегам (`Source/Runtime/Core/Private/HAL/LLM/LLM.cpp`, 7720 строк)

- **Не прокси аллокатора**: хуки стоят в инлайн-обёртках `FMemory` над `GMalloc`
  (`Core/Public/HAL/FMemory.inl:39,152,221`); кто зовёт `GMalloc` напрямую — мимо LLM. Realloc = free + alloc,
  новый блок наследует тег старого (`FMemory.inl:108-109,133`).
- **Два трекера** (`Core/Public/HAL/LowLevelMemTracker.h:104-119`): `Platform` — страницы ОС (хуки
  `BinnedAllocFromOS`, `WindowsPlatformMemory.cpp:524-531`; для mimalloc — свои `VirtualAlloc`/`VirtualFree` с
  битовой картой commit, `WindowsPlatformMemory.cpp:801-906`), `Default` — логические аллокации.
- **Теги**: `ELLMTag` — `uint8` (`LowLevelMemTracker.h:169-318`), плюс `LLM_DEFINE_TAG(A_B_C, ...)`, где `_` → `/`
  и родитель берётся из пути (`LLM.cpp:4285-4290`, `:3045-3089`). Скоупы `LLM_SCOPE` — стек тегов на поток
  (`LLMPrivate.h:395`, `LLM.cpp:6623-6643`); `LLM_SCOPE` заодно ставит `UE_MEMSCOPE`, то есть тот же тег уходит в
  трассу (`LowLevelMemTracker.h:351-409`).
- **Карта указателей** `LLMMap` (`LLMPrivate.h:474-491`, `MemoryUtils.h:661-853`): ключ `(ptr<<16)|sizeHigh16`,
  значение — младшие 32 бита размера + индекс группы тегов; «20 байт на аллокацию → 17»
  (`LLMPrivateDefines.h:11-14`). 16 полос с мьютексами по старшим битам хеша, линейное пробирование при загрузке
  ≤30 %, хранилище страницами по 16 КБ.
- **Своя память без рекурсии**: `FLLMAllocator` на голом `VirtualAlloc` (`LLM.cpp:4440-4780`,
  `WindowsPlatformMemory.cpp:755-791`), его объём — отдельный тег накладных.
- **Поток → кадр**: каждый поток копит дельты в своём `FLLMThreadState` под своим (почти всегда свободным)
  мьютексом; раз в кадр `UpdateStatsPerFrame` их забирает и поднимает по дереву тегов
  (`LLM.cpp:1990-2035`, `:5530-5807`). Пики — только на этом обновлении (`:5522-5526`).
- **Производные строки** (`LLM.cpp:2091-2126`): `Untracked` = память процесса − учтённое; `FMallocUnused` =
  страницы аллокатора − логические байты (это и есть фрагментация/кэш аллокатора).
- **GPU**: «On Windows there is no way to hook into the low level d3d allocations and frees» — каждая текстура
  репортится вручную в оба трекера (`Source/Runtime/D3D12RHI/Private/D3D12Texture.cpp:101-107`); ключ — фиктивный
  нечётный адрес `this+1` (`D3D12Resources.h:749-753`). И сам UE признаёт двойной учёт в одном из пулов
  (`D3D12Allocation.cpp:527-535`).
- **Цена**: «LLM_MEMORY_OVERHEAD 600 МБ на ~4 млн аллокаций» (`LowLevelMemTracker.h:96-99`); LLM глушит детектор
  хитчей, потому что «already slow enough» (`LLM.cpp:2491`). Без `-llm` выключается и чистит данные, включить
  обратно нельзя (`LLM.cpp:2377`).

### 2.2 Memory Insights — трасса аллокаций (`Core/Private/ProfilingDebugging/`)

- **Обёртка** `FMallocWrapper` — самый внутренний прокси (`MemoryTrace.cpp:181-274`); включается один раз, при
  создании `GMalloc`, по сырому `GetCommandLineW()` (`Microsoft/WindowsMemoryTrace.cpp:225-243`). Каналы
  `memalloc`/`callstack`/`module` read-only — только из командной строки (`MemoryTrace.cpp:16-17`); пресеты
  `memory = memtag,memalloc,callstack,module`, `memory_light = memtag,memalloc` (`TraceAuxiliary.cpp:139-140`).
  **Позднее подключение не работает**: free неизвестного указателя → «fake alloc size 0»
  (`TraceServices/Private/Model/AllocationsProvider.cpp:1699-1716`).
- **События** (`MemoryTrace.cpp:66-173`, упаковка `:491-539`): `Alloc{u64 Address, u32 CallstackId, u32 Size>>3,
  u8 log2(align)<<3 | Size&7, [u8 RootHeap]}`, отдельные `AllocSystem`/`AllocVideo` без байта кучи, `Free`,
  `ReallocAlloc/Free`, `UpdateAlloc`, `HeapSpec{Id, ParentId, Flags, Name}`, `HeapMarkAlloc/Unmark`. **Времени в
  событии нет**: `Marker{Cycle}` раз в 4095 событий (`MemoryTrace.cpp:33-44`); порядок между потоками — глобальный
  серийный номер TraceLog.
- **Кучи** (`Core/Public/ProfilingDebugging/MemoryTrace.h:30-61`): корни 0 = System, 1 = Video, 0-15
  зарезервированы. Пул = аллокация + `MarkAllocAsHeap`, его суб-аллокации — дети; анализатор вычитает
  помеченный блок из итога, двойного счёта нет (`AllocationsProvider.cpp:2205-2227`).
- **Теги в трассе** — не в событии аллокации, а скоупом `MemoryScope{Tag}` на поток (`TagTrace.cpp:28-113`).
- **Стеки** (`Microsoft/WindowsCallstackTrace.cpp`): свой раскрутчик по `.pdata` с lock-free кэшем «адрес возврата
  → размер кадра» (`:183-245`), запасной путь — `CaptureStackBackTrace` (`:942-995`); дедуп по хешу, каждый новый
  стек уходит один раз дельта-varint'ом (`CallstackTracePrivate.h:58-146`); символы — оффлайн по
  `ModuleLoad{Name, Base, Size, PDB GUID+Age}` (`Microsoft/WindowsModuleDiagnostics.cpp:164-203`).
- **Потоки**: у каждого свой TLS-буфер, отдельный поток сливает (`TraceLog/Private/Trace/TlsBuffer.cpp:49`).

### 2.3 GPU-память D3D12 (`Source/Runtime/D3D12RHI/Private/`, `Source/Runtime/RHI/`)

- В трассу: ключ — GPU VA; у текстур его нет, поэтому при включённом `memalloc` UE кладёт «committed» текстуры в
  собственную кучу, чтобы VA появился (`D3D12Adapter.cpp:1528-1533`, `D3D12PoolAllocator.cpp:471-501`), иначе
  текстуры в трассу не попадают (`D3D12Texture.cpp:94`). Решается при инициализации адаптера.
- **Таблица живых ресурсов** (`RHI_WANT_RESOURCE_INFO`, всё кроме Shipping): `FRHIResourceInfo{Name, Type,
  VRamAllocation, IsTransient, bResident}` (`RHI/Public/RHI.h:532-540`), команда
  `rhi.DumpResourceMemory [N|all] [summary] [Name=] [Type=] [Transient=] [csv]` (`RHI/Private/RHI.cpp:879-1096`) —
  по убыванию размера, флаги RT/DS/UAV/RTAS/Streaming/Transient.
- **Освобождённые VA за 100 кадров** для разбора page fault (`TRACK_RESOURCE_ALLOCATIONS`,
  `D3D12Adapter.cpp:1953-2025`, `D3D12.DumpTrackedAllocations`, `:2095-2256`).
- Бюджет: `QueryVideoMemoryInfo` каждый кадр (`RHICore/Private/DXGIUtilities.cpp:56-134`,
  `D3D12Adapter.cpp:1900-1942`).

### 2.4 Анализ в Insights (`Source/Developer/TraceServices/`, `TraceInsights/.../MemoryProfiler/`)

- **Правила запросов** — предикаты над временем аллокации `a` и освобождения `f` относительно меток A/B/C/D
  (`TraceServices/Public/TraceServices/Model/AllocationsProvider.h:25-52`):

  | Правило | Смысл | Условие |
  |---|---|---|
  | `aAf` | живые в A | a ≤ A ≤ f |
  | `AaBf` | рост | A ≤ a ≤ B ≤ f |
  | `aAfB` | спад | a ≤ A ≤ f ≤ B |
  | `AafB` | короткоживущие (churn) | A ≤ a ≤ f ≤ B |
  | `aABf` | долгоживущие | a ≤ A, f ≥ B |
  | `AaBCf` | **утечки** | A ≤ a ≤ B, f ≥ C |
  | `AaBfC` | ограниченное время жизни | A ≤ a ≤ B ≤ f ≤ C |

- Группировки: размер, дерево тегов, стек аллокации/освобождения (прямой и инвертированный), куча, любой
  столбец (`MemoryProfiler/Widgets/SMemAllocTableTreeView.cpp:201-222`). Таблица тегов: Size A, Size B, Diff
  (`MemTagTable.cpp:289-497`).
- Индекс по времени — SbTree (`TraceServices/Private/Model/SbTree.h:59-105`) — нужен на 10^8 событий; живые
  аллокации UE и сам сканирует линейно (`AllocationsQuery.cpp:193-430`).

### 2.5 Отладочные прокси и MemReport (`Core/Public/HAL/`)

- `FMallocLeakDetection` — стек на каждую аллокацию, ~300 Б на живой указатель под одним глобальным локом, МНК-
  наклон «байт за кадр» (`MallocLeakDetection.cpp:270-290`, `:458-512`). Очень дорого.
- `MallocFrameProfiler` — топ стеков по churn за N кадров; `DoubleFreeFinder`; `MallocPoisonProxy` (0xCD/0xDD).
- `MemReport` (`Engine/Private/UnrealEngine.cpp:8169-8275`): GC + flush, затем список команд из
  `[MemReportCommands]` (`Engine/Config/BaseEngine.ini:2371-2455`): `rhi.DumpMemory`, `rhi.DumpResourceMemory`,
  `obj list`, `listtextures`, `r.DumpRenderTargetPoolMemory`, …

---

## 3. Как у нас (разбор 2026-10-22, по коду)

| | Факт | Где |
|---|---|---|
| Создание GPU-ресурсов | **~108 мест**: 47 через `render::CreateCommittedTexture`, 10 через `UploadManager::CreateBufferWithData` (пара default+upload), **51 — прямой `CreateCommittedResource` в 26 файлах** (в основном буферы: VSM, ShadowGpuData, RT AS, readback, кольца) | `rendering/core/TextureCreate.h:33`, `UploadManager.h:17-51` |
| Прочее | 52 `CreateDescriptorHeap` в 29 файлах, 3 `CreateQueryHeap`, 4 пула command allocator; ImGui-бэкенд — 5 своих созданий (`third_party/imgui/backends/imgui_impl_dx12.cpp`) | — |
| Модель | **Всё committed**: 0 placed/reserved/`CreateHeap`, нет D3D12MA. Каждый ресурс — своя неявная куча (буфер — минимум 64 КБ по правилу выравнивания D3D12; сколько это стоит — измерит M1) | — |
| Имена | Непоследовательно: имена есть у `GpuResource` и RenderTargetManager (`RenderTargetManager.cpp:828`), **нет** у буферов AS, кольца кадра, upload-промежуточных, части VSM/ShadowGpuData, BindlessTable, TextManager, Screenshot, профайлера, редакторских | — |
| Освобождение | `ComPtr`; ≥6 своих корзин ретайра (AS `retireBin_`, `TextureRetireBin`, океан, текст, превью, материалы). `CanonicalStateRegistry::Forget` **ненадёжен как хук освобождения**: «Not every release path unregisters (measured)» | `ResourceDeclarations.h:55-58`, `:142` |
| Реестр имён | `CanonicalStateRegistry` (барьеры) уже хранит имя и счётчик живых по имени `NetByName` для ~80 `GpuResource` | `ResourceDeclarations.h:26-305` |
| CPU-куча | mimalloc **2.2.5 prebuilt** (`third_party/mimalloc/lib/{Debug,Release}`), без исходников, без `MI_TRACK_ETW`; подменяет только `new/delete` (`app/main.cpp:7` `mimalloc-new-delete.h`, 20 операторов). `malloc` идёт в CRT-кучу (WIC, `Material.cpp:1179`, рантайм D3D, драйвер) | — |
| Своих аллокаторов | нет (ни арен, ни `pmr`); GPU-кольца `FrameResource::AllocDynamic`, `TextureUploadRing` | `FrameResource.cpp:50` |
| Потоки-аллокаторы | main, воркеры `TaskSystemLockFree` (0.75 × HW), `TextureStreamingIo`, LogWriter, ProfilerTraceWriter, EditorMcp, ImportPanel, LLM-потоки | `App.cpp:2077` |
| Строка `mem:` | раз в 5 с: private/ws, vram local/budget/nonlocal, `mi commit`, провайдеры | `MemoryReport.cpp:168-211`, вызов `App.cpp:2210` |
| Профайлер | Chrome-trace JSON в `traces/` — только `"ph":"X"`, счётчиков нет | `core/profiling/Profiler.cpp:954-1017` |
| MCP | 15 инструментов; `get_profile` → `RuntimeHost::Profile` → `McpRuntimeHost` (`App.cpp:1850-1886`) — образец для `get_memory` | `editor/intent/EditorMcpServer.cpp:340`, `:488`, `:874-887` |
| Стеки | `CaptureStackBackTrace(2,24)` + модуль+смещение без dbghelp (`--barrier-msg-trace`); dbghelp `StackWalk64`/`SymFromAddr` в крэш-логгере | `GraphicsDevice.cpp:93-118`, `app/diagnostics/SceneStress.cpp:34-152` |
| Сборка | x64; `/Oy` на x64 не бывает — раскрутка по `.pdata` работает во всех конфигурациях; LTCG в Release/Release_Editor склеивает инлайненные кадры | `test_cube.vcxproj:67,74` |
| UI | DeveloperWindow: вкладки `enum Tab` (`DeveloperWindow.h:45`), Streaming уже показывает пул/бюджет (`DeveloperWindow.cpp:3172`) | — |

---

## 4. Архитектура

Три слоя, как у UE, но меньше:

```
Слой A  ПЕРЕПИСЬ (аналог LLM)          всегда есть; счётчики по тегам, CPU и GPU, раз в кадр
          GPU: каждая аллокация через gpu::Create*(…, tag, name) → таблица живых ресурсов
          CPU: свой operator new/delete → mi_malloc + (при --memtrack) карта ptr→тег
Слой B  ТРАССА (аналог Memory Insights) по ключу на старте; события + стеки → бинарный файл → tools/memtrace.py
Слой C  ВЫВОД И ГЕЙТЫ                   mem:, memreport, MCP get_memory, вкладка Memory, счётчики в --trace, гейты
```

### 4.1 Теги (общие для CPU и GPU)

`sources/core/memory/MemTags.h` — X-macro, `uint8`, закрытый набор (как `ELLMTag` без динамических тегов):

```cpp
// MEM_TAG(Id, "name", Parent)
MEM_TAG(Untagged,     "untagged",      None)
MEM_TAG(Frame,        "frame",         None)
MEM_TAG(FrameGbuffer, "frame.gbuffer", Frame)
MEM_TAG(Shadow,       "shadow",        None)
MEM_TAG(ShadowVsm,    "shadow.vsm",    Shadow)
...
```

**Список согласован владельцем 2026-10-22 (Q4).** Правила:

1. **Тег = владелец по смыслу.** Upload-промежуточные, readback, кучи дескрипторов и запросов идут в тег того, кто
   их создал. ВИД (текстура / буфер / куча дескрипторов / куча запросов) и КУЧА (`gpu.local` / `gpu.nonlocal`) —
   колонки переписи, не теги.
2. **Цели RenderTargetManager — по фичам**: RTM передаёт тег фичи в каждое создание, общего `targets` нет.
3. **Тени — пять тегов** (режим `shadow.mode` переключается на лету — видно, отдаёт ли выключенный режим память).
4. **Имена короткие**, в стиле нынешних провайдеров `mem:` (`rt.as`, `tex.stream`); родитель — префикс до точки
   (как UE берёт родителя из пути, `LLM.cpp:3045-3089`); не больше двух уровней.
5. **Один тег — обе стороны**: строка тега несёт колонки CPU и GPU (у `ocean` — h0 на CPU и симуляция на GPU).

| Тег | GPU (M1) | CPU (M3) |
|---|---|---|
| `frame.gbuffer` | GB0-2, GBVelocity, GBAux, Depth, DepthCopy, ObjectID, Hzb, HzbClosest (`RenderTargetManager.cpp:836-876`) | — |
| `frame.color` | Light, Scene, SceneOpaque, DlssOutput, Tonemap, Fxaa, DebugPreview | — |
| `frame.upload` | кольцо `FrameResource` + overflow-чанки (`FrameResource.cpp:24,135`) | — |
| `frame.swapchain` | бэкбуферы (DXGI, учёт вручную) | — |
| `tex.material` | Texture2D / TextureCube с upload-промежуточными | — |
| `tex.stream` | пул стриминга (`TextureStreaming.cpp:444`), `TextureUploadRing` | менеджер стриминга, поток IO |
| `tex.decode` | — | `TextureDecodeCache`, буферы декода `Texture2D` |
| `mesh` | VB, IB, LOD IB (`Mesh.cpp:47,62,284`) | данные мешей (загрузка, бейк) |
| `mesh.instances` | `InstanceBuffer`, `GpuInstancedModels` | — |
| `shadow.csm` | атлас каскадов (`RenderTargetManager.cpp:931`), `CascadeHzb` | — |
| `shadow.sdsm` | `SdsmShadows` | — |
| `shadow.vsm` | `VirtualShadowMap` | — |
| `shadow.local` | атласы spot/point (`RenderTargetManager.cpp:995,1068`), `LocalShadowCache` | — |
| `shadow.gpudata` | `ShadowGpuData`: таблица кастеров, общий VB/IB, indirect (по ней же рисуется GPU-driven G-buffer камеры) | — |
| `rt.as` | BLAS/TLAS/scratch, wind-слоты (`AccelerationStructure.cpp`) | — |
| `rt.bindless` | `BindlessTable` | — |
| `reflect` | Reflection, ReflectionScratch/History, OceanReflection(+History), GlassRefl* | — |
| `ao` | Gtao, GtaoFiltered/History/Upsampled | — |
| `bloom` | BloomFft*, LensFlareAccum, StreakPyramid*, BloomHistory/Down/Up, ядра `BloomRenderer` | — |
| `fog` | FogScatter, FogIntegrated (froxel-объём height fog) | — |
| `shafts` | LightShaftA/B/History | — |
| `sky` | LUT'ы `SkyAtmosphere`, `Skybox` | — |
| `cloud` | текстуры `VolumetricCloud`, CloudTrace(+Depth), CloudResolved(+Depth) | — |
| `ocean` | `OceanSimulation`, `OceanSurfSim`, `OceanWetness`, `OceanReadback` | h0 и данные волн |
| `lights` | буферы `LightManager` | — |
| `exposure` | `ExposureMetering` | — |
| `dlss` | `DlssHandler` (внутренности Streamline/NGX — в `gpu.untracked`) | — |
| `vis` | `OcclusionQueries`, `HzbOcclusionTester`, `HzbCullSelfTest` | — |
| `vfx` | частицы (`ParticleEmitterObject`) | — |
| `text` | `TextManager` | текст |
| `ui` | `ImGuiLayer` (бэкенд ImGui в `third_party` — в `gpu.untracked`) | ImGui |
| `editor` | превью, миниатюры, pick-readback | редактор |
| `editor.import` | — | поток импорта |
| `editor.intent` | — | intent-модель, MCP |
| `renderer` | прочее из `Renderer.cpp`, `Screenshot` | запись команд, рендер-граф, PSO и материалы |
| `scene` | — | загрузка уровня, объекты |
| `tasks` | — | `TaskSystem` |
| `log` | — | кольцо лога, `LogWindow` |
| `profiler` | query heap и readback таймстемпов | `TraceDumpData` |
| `memtrack` | — | накладные самого учёта (как `LLMOverhead`) |
| `untagged` | — (GPU без тега не бывает: тег — параметр обёртки) | всё вне скоупов |

Новый владелец ресурсов получает тег в этой таблице до первого `gpu::Create*`; `check_gpu_alloc.py` тег не
проверяет — проверяет ревью.

Трекеры (измерения, а не теги): `cpu` (логические байты mimalloc), `gpu.local` (DEFAULT-кучи), `gpu.nonlocal`
(UPLOAD/READBACK) и платформенные счётчики — private, `mi commit`, NT-кучи (`HeapSummary` по `GetProcessHeaps`,
есть в `heapapi.h` SDK 10.0.22621), `vram local/nonlocal` (`QueryVideoMemoryInfo`). Производные строки — по UE
(`LLM.cpp:2091-2126`):

- `gpu.untracked` = `vram local CurrentUsage` − Σ `gpu.local` (драйвер, DLSS/Streamline, ImGui-бэкенд);
- `cpu.slack` = `mi commit` − Σ `cpu` (кэш и фрагментация mimalloc, аналог `FMallocUnused`);
- `cpu.untagged` — тег по умолчанию; `private.other` = private − `mi commit` − NT-кучи (образы, стеки, маппинги
  драйвера).

**Никогда не сравнивать перепись с NVML** (`GpuMemoryTotals`): там вся карта, включая llama-server.

### 4.2 GPU-перепись

- `sources/rendering/core/GpuAlloc.{h,cpp}`: `gpu::CreateCommittedTexture/Buffer(device, heapProps, flags, desc,
  state, clear, tag, name, out)`, `gpu::CreateDescriptorHeap(…, tag, name)`, `gpu::CreateQueryHeap(…)`; нынешний
  `render::CreateCommittedTexture` (энхансд-барьеры) остаётся внутри. Каждая обёртка: создать → `SetName` →
  `GetResourceAllocationInfo(0, 1, &desc)` (реальный размер неявной кучи) → запись в перепись `{tag, name,
  allocBytes, descBytes, heapType, dimension/format/size, flags RT/DS/UAV, кадр создания}`.
- **Освобождение — sentinel**: крошечный `IUnknown`, прикреплённый `SetPrivateDataInterface(kMemSentinelGuid, …)`;
  его последний `Release` происходит при уничтожении ресурса, каким бы путём тот ни умер (любая корзина, любой
  `ComPtr`, keep-alive вектор) — правок в корзинах не нужно. Адрес ресурса освобождается после записи free, так что
  ключ `ID3D12Resource*` не путается при переиспользовании адреса. Ключ трассы — сам указатель (аналог UE `this+1`),
  GPU VA не нужен, текстуры класть в кучи не надо (в отличие от `D3D12Adapter.cpp:1528-1533`).
- Swapchain-буферы создаёт DXGI — записываются вручную по `GetResourceAllocationInfo` (тег `frame.swapchain`).
- Дескрипторные кучи: размер = `NumDescriptors × increment` — оценка, помечается `est`.
- `tools/check_gpu_alloc.py` (по образцу `check_logging.py`): прямой `CreateCommittedResource*`/
  `CreateDescriptorHeap`/`CreateQueryHeap` вне `GpuAlloc.cpp` = находка, код выхода = число находок.

### 4.3 CPU-перепись

- Свой TU `sources/core/memory/MemNew.cpp` вместо `#include "mimalloc-new-delete.h"` (`main.cpp:7`): те же 20
  операторов, те же вызовы `mi_*` — байт-в-байт эквивалент, плюс учёт.
- **Счётчики на поток** (всегда): `thread_local` блок `{allocs, frees, bytes}` — один писатель, атомики `relaxed`
  без `lock`-префикса (load+store), блок регистрируется в глобальном списке и не освобождается никогда (потоков
  мало; UE так же копит `PendingThreadStates`, `LLM.cpp:5108-5151`). Размер при free — `mi_usable_size(p)`.
- **Теги** (по `--memtrack`, Q2): `MEM_SCOPE(MemTag::X)` — стек из 16 `uint8` на поток; карта `ptr → тег`
  (размер не храним — `mi_usable_size` даёт его при free): 16 полос по старшим битам хеша, открытая адресация,
  страницы из `VirtualAlloc` (никогда не `new` — рекурсия; плюс `thread_local` флаг-сторож, как
  `GDoNotAllocateInTrace`, `MemoryTrace.cpp:44-51`). Дельты по тегам — в поточном блоке, сумма раз в кадр.
  Free неизвестного указателя (выделен до включения) — счётчик `unknown frees`, не ошибка.
- Включение — один раз, при первом `operator new`, по сырому `GetCommandLineW()` (как
  `WindowsMemoryTrace.cpp:225-243`): аллокации статической инициализации идут раньше `WinMain`.
- Задачи наследуют тег: `TaskSystem` при постановке запоминает текущий тег (1 байт), воркер ставит его на время
  задачи. Это наше решение — у UE отдельные `LLM_SCOPE` в телах задач.

### 4.4 Трасса (слой B)

- Ключ на старте: `--memtrace[=gpu|cpu|all][,stacks=all|big|1/N]`. GPU-трассу можно начать и позже: в момент
  старта перепись выдаёт всех живых как синтетические `Alloc` (стека нет) — то, чего UE не умеет
  (`AllocationsProvider.cpp:1699-1716`); то же для CPU при включённом `--memtrack`.
- События (по `MemoryTrace.cpp:66-173`): `Header{magic, version, qpcFreq}`, `Module{base, size, PDB GUID+age,
  path}`, `TagSpec{id, parent, name}`, `HeapSpec{id, parent, name}` (корни: 0 cpu, 1 gpu.local, 2 gpu.nonlocal),
  `Alloc{u64 key, size>>3 | align, u8 tag, u8 heap, u32 stack, u16 thread}`, `Free{u64 key, u8 heap}`,
  `Name{key, utf8}` (имя GPU-ресурса — вместо UE-метаданных), `Marker{qpc, frame}` — **на границе кадра** и каждые
  4095 событий, `Stack{id, n, frames delta-varint}` один раз на уникальный стек.
- Стеки: `RtlCaptureStackBackTrace(skip, 32, frames, &hash)` — хеш он отдаёт сам, это ключ дедупа; GPU-аллокации
  — всегда (их сотни), CPU — `stacks=big` (≥ 64 КБ) или 1/N. Свой раскрутчик UE (`WindowsCallstackTrace.cpp:183-245`)
  — только если замер M4 покажет, что `RtlCaptureStackBackTrace` дорог.
- Буферы по 64 КБ на поток, сливает отдельный поток-писатель (как LogWriter — не `TaskSystem`); при закрытии —
  символизация в процессе через dbghelp по уникальным адресам, таблица `Symbol{addr, func, file:line}` в конец
  файла. При крэше таблицы нет — `memtrace.py` символизирует сам (dbghelp через `ctypes`, PDB рядом с exe).

---

## 5. Шаги

### M0. Базовая линия: платформенная раскладка + аллокаций/кадр — [полдня]

- **Что**: `MemNew.cpp` вместо `mimalloc-new-delete.h` (без тегов) + поточные счётчики; в `mem:` добавить
  `heaps X` (`HeapSummary` по `GetProcessHeaps`), `other Y`, `allocs/frame N (main M)`.
- **UE-референс**: `LLM.cpp:2091-2126` (производные строки), `FMemory.inl:39,152,221` (где хук).
- **Критерий приёмки**: сумма столбцов = private с остатком, названным в отчёте; аллокаций/кадр замерено на
  wind_test (F2, камера владельца) и occlusion_test-орбите, по потокам; A/B старого и нового `new` — время кадра в
  шумовом полу. Эти числа задают размеры карты и бюджет накладных для M3 — записать сюда.
- **Гейт**: три конфигурации, `--log-stress`, `--scene-stress` (Debug — отладочный слой есть только там).

**СТАТУС: СДЕЛАНО 2026-10-22, закоммичено `9f5ba576`.** Файлы: `sources/core/memory/MemNew.cpp` (20 операторов
`mimalloc-new-delete.h` один в один + счётчики), `MemCounters.h` (блоки по потокам, 1024 + общий overflow),
`diagnostics/MemBench.{h,cpp}` (`--mem-bench`), `rendering/core/MemoryReport.{h,cpp}` (новые столбцы, Debug-строки
`mem threads:` и `mem regions:`), `app/main.cpp` (без `mimalloc-new-delete.h`, диспетчер `--mem-bench`).

Итоги (Release, wind_test, камера `-59.50,4.25,52.73` / `-0.0392,0.9604,-0.1793,-0.2098`, ~304 к/с = 3.3 мс):

| Что | Число |
|---|---|
| `new`/кадр | **~1510** (main **1430 = 95 %**), 0.20 МБ запрошено/кадр; 26 потоков, воркеры по ~4/кадр |
| Живых аллокаций | ~29 000, стабильно (орбита: 29.1 → 30.5 тыс. за первые 20 с прогрева, дальше ровно) |
| Цена счётчиков (`--mem-bench`) | **≤ 1.45 нс/пара** в 1 потоке (5.26 против 3.81 у голого `mi_new`), ≤ 2.46 нс на 8 потоках; это верхняя граница — в разницу входит и вызов-обёртка `operator new → mi_new`, который был и в заголовке mimalloc |
| Цена на кадр | 1510 × 1.45 нс ≤ **2.2 мкс = ≤ 0.07 % кадра** — ниже шумового пола; A/B время кадра не делали (2.2 мкс им не видно) |
| Орбита 35 м / 14°/с, 60 с | `new`/кадр 1464–1528 — те же; private ±10 МБ, vram ±10 МБ (плоско), `rt.as.retired` 8 → 398 МБ (кумулятивный, ожидаемо) |

**Платформенная раскладка и главная находка.** До M0 `mi commit` брался из `mi_process_info`, а его «commit» на
Windows — это `PagefileUsage`, то есть тот же private байт в байт (замер: `mi commit 2952` при `private 2952`,
`other = −heaps`). Столбец всегда дублировал private, и вывод «почти вся память процесса в куче mimalloc» был
прочитан с него. Теперь `mi commit` = `mi_stats_get().committed.current`:

```
mem: private 2889 MB | vram local 1804 nonlocal 247 | mi commit 154 MB | heaps 202 MB (9) | other 2533 MB | new/frame 1516 (main 1435) 0.22 MB live 28996
mem regions: private commit 2755 MB in 411 reservations (walk 24.7 ms) | mimalloc 159 | write-combined 2080 | exec 0 | rest 516 MB (NT heaps 202 of it)
             biggest: 265 MB, 159 MB mi, 105 MB wc, 64 MB wc, 64 MB wc, 50 MB wc
```

- **mimalloc — 154–160 МБ**, не 2.5 ГБ. NT-кучи (CRT, рантайм D3D12, драйвер) — 202 МБ в 9 кучах.
- **Остаток назван: 2080 МБ write-combined приватных страниц** — CPU-отображения памяти, которую видит GPU
  (UPLOAD/READBACK-кучи, shader-visible дескрипторы, кольца драйвера), кусками по 50–105 МБ. DXGI `nonlocal`
  при этом 247 МБ — значит, либо DXGI не считает большую часть этих страниц, либо они не наши ресурсы. Уже на
  кадре 0 (до загрузки уровня) private 1853 МБ при mimalloc 32 МБ. **Это вопрос к M1**: перепись по куче
  (`gpu.nonlocal`, UPLOAD/READBACK по владельцам) скажет, сколько из 2 ГБ — наши ресурсы.
- Наши дескрипторные кучи этого не объясняют (крупные только bindless и `BindlessTable`).
- Обход адресного пространства стоит 25 мс на главном потоке — поэтому только Debug и раз в минуту.

Гейты: три конфигурации собраны; `--log-stress` 0 провалов; `--scene-stress=150` (Debug) CLEAN, 72 с, без
ERROR/FATAL; `check_logging.py` 0. Запусков: 6 (бенч, wind_test ×2, орбита, два гейта).

### M1. GPU-перепись — [2 дня: M1a, M1b, M1c]

- **M1a** — `MemTags.h`, `GpuAlloc.{h,cpp}`, sentinel, таблица переписи, ручной учёт swapchain; перевести 47
  `CreateCommittedTexture` и `UploadManager` (тег — параметр). В `mem:` — `gpu local X nonlocal Y untracked Z`.
- **M1b** — 51 прямой `CreateCommittedResource` по подсистемам (список — §3 и отчёт разведки: shadows/, rt/,
  ocean/, lighting/, core/, visibility/, editor/), 52 дескрипторные кучи, 3 query-кучи. Файловые хелперы
  (`AccelerationStructure.cpp:18,52`, `SdsmShadows.cpp:45`, `ExposureMetering.cpp:43`) меняются одной строкой.
  Каждому ресурсу — имя (сейчас безымянные — см. §3).
- **M1c** — `tools/check_gpu_alloc.py` (0 находок в `sources/`); сверка с провайдерами ±1 %: `sky.luts` = LUT'ы
  внутри `sky`, `cloud.textures` = текстуры `VolumetricCloud` внутри `cloud` (без целей CloudTrace/Resolved),
  `tex` = `tex.stream`, тег `rt.as` = провайдеры `rt.as` + `rt.as.bin` (перепись видит корзину живой; входит ли
  корзина уже в провайдер `rt.as` — сверить в `RtSceneAs.cpp:107-115`); сверять по именам
  ресурсов, где тег шире провайдера. Затем дублирующие «живые» провайдеры удалить (Q3), кумулятивные
  (`rt.as.retired`, `tex.ret`) остаются.
- **UE-референс**: `D3D12Texture.cpp:93-133` (ручной учёт), `D3D12Resources.h:749-753` (фиктивный ключ),
  `RHI.cpp:879-1096` (что показывает дамп), `D3D12Allocation.cpp:527-535` (как НЕ надо: двойной учёт).
- **Критерий приёмки**: `gpu.untracked` назван и объяснён (ожидаем DLSS/драйвер/ImGui — проверить
  выключением DLSS); **2080 МБ write-combined из M0 разложены: сколько — наши UPLOAD/READBACK по тегам, сколько —
  чужое** (и почему DXGI `nonlocal` показывает 247); после `--scene-stress` теги возвращаются к значению первого
  визита уровня; столбец
  `alloc − desc` (цена committed-выравнивания) посчитан — если он заметен, это повод для отдельного плана пулов,
  не для этого.
- **Гейт**: три конфигурации; Debug с отладочным слоем — sentinel срабатывает и под ним (проверить, что слой не
  держит ресурсы дольше; `live objects at exit: none` по-прежнему); `--scene-stress`.

**СТАТУС: СДЕЛАНО 2026-10-22, закоммичено `044b2b99`.** Что вошло:
- `core/memory/MemTags.h` (таблица §4.1), `rendering/core/GpuAlloc.{h,cpp}` (обёртки, запись-sentinel на private
  data, перепись, `SnapshotRecords`), `tools/check_gpu_alloc.py` — **0 находок**: все ~108 созданий ресурсов, 54
  дескрипторные кучи, 3 query-кучи и 11 `UploadManager::CreateBufferWithData` (тег + имя, staging — `<name>.Upload`)
  идут через перепись; буферы свопчейна — `TrackExternalResource`. Миграцию делали 4 агента по непересекающимся
  наборам файлов; безымянные раньше объекты (буферы AS, кольцо кадра, upload-копии, readback'и) получили имена.
- `Texture2D::SetMemTag` (по умолчанию `tex.material`; кэш текстур: тег того, кто загрузил файл первым):
  ядра bloom → `bloom`, шрифт → `text`, текстуры океана → `ocean`, BRDF LUT → `sky`, спрайт частиц → `vfx`,
  иконки → `editor`/`ui`, загрузки миниатюр → `editor` (если миниатюра грузит файл раньше уровня, текстура так и
  останется записанной на редактор). `TextureCube` (IBL неба) — пока `tex.material`.
- `mem:`: `gpu local X nonlocal Y MB (N res) untracked Z`; Debug-строка `mem gpu:` (по тегам, раз в минуту);
  **`--memreport[=<с>]`** — разовый отчёт в session log: платформа + обход адресного пространства, дерево тегов,
  все GPU-объекты по убыванию (часть M2 сделана здесь — без неё не сверить провайдеры по именам).
- `--scene-stress`: строка итерации несёт `gpu L/N MB, res, heaps, live`.

Итоги (Release, wind_test, камера владельца, DLSS вкл. / выкл.):

| | DLSS вкл | DLSS выкл |
|---|---|---|
| Перепись local / non-local, МБ | 1524 / 76 (823 рес. + 201 куча) | 2316 / 76 |
| DXGI local / non-local, МБ | 1804 / 245 | 2495 / 235 |
| **untracked** local / non-local | **279** / 169 | **178** / 159 |
| write-combined (обход адресов) | 2071 | 2704 |

- `untracked` local: ~100 МБ — DLSS (уходит с `--dlss=off`), ~178 МБ — драйвер; non-local ~160–170 МБ — тоже не
  наши объекты. Бэкенд ImGui (`third_party`) в перепись не входит — он там же.
- **2 ГБ write-combined из M0 объяснены: это видеопамять процесса, отражённая в его private.** WC ≈ DXGI
  local + non-local в пределах ±1.3 % в обоих прогонах, и DLSS-off сдвинул VRAM на +681 МБ, WC на +633. Наших
  UPLOAD/READBACK — 76 МБ. Значит, private на этой машине считает VRAM второй раз; собственно CPU-память процесса ≈
  private − WC ≈ 0.8 ГБ (mimalloc 183, NT-кучи 204, прочее ~316).
- Крупнейшие теги (DLSS вкл): `bloom` **376 МБ** — из них **288 МБ = `Deferred[0..2].BloomFftA/B/Kernel` по 32 МБ на
  каждый кадр в полёте** (ядро между кадрами не меняется — **сведено к одному набору 2026-10-22**, см. ниже); `frame.color` 284
  (`DlssOutput`/`Tonemap`/`Fxaa` ×3), `frame.gbuffer` 149, `shadow.local` 128 (`LocalShadowCache` 64, `PointShadow`
  48, `SpotShadow` 16), `reflect` 115, `shadow.gpudata` 66+7, `rt.as` 64, свопчейн 60, `cloud` 51, `ocean` 46+4.5,
  `shadow.csm` 32, `mesh` 31 (195 буферов), `tex.stream` 31 + **50 non-local** (`Streaming.UploadRing`),
  `tex.material` 20.
- Цена committed-выравнивания (`buffer slack`, гранула 64 КБ) — **19.4 МБ**: повода для плана пулов нет.
- **Сетки FFT bloom — один набор на все кадры в полёте (2026-10-22).** Их трогает только bloom внутри `Main_Tonemap`
  (графическая очередь, `SceneRenderer_Graph.cpp` `AddPass2(RenderPass::Main_Tonemap, ...)` без очереди = Graphics),
  от записи до чтения в пределах одного кадра; A/B — рабочие буферы прохода, ядро кэшируется по ключу. Теперь
  `RenderTargetManager` владеет `bloomFftA_/B_/Kernel_`, а `Deferred[f].bloomFft*` — невладеющие алиасы, как у атласа
  CSM; ключ ядра в `BloomRenderer` один вместо трёх по слотам. UE так же: одно закэшированное ядро на вид
  (`PostProcessFFTBloom.cpp:338-358`, сохранение `:695-710`), рабочие буферы — транзиенты RDG
  (`GPUFastFourierTransform.cpp:1359`, `:1369`). Замер (Release, wind_test, `--dlss=off`, камера `-59.50,4.25,52.73`):
  `bloom` **391.1 → 199.1 МБ** (−192), перепись local 2316.4 → 2124.4, DXGI local 2495 → 2302; кадр с
  `--wind-freeze --set=exposure.autoExposure:0` в шумовом полу (до/до 1.38 % пикселей, после/до 1.36–1.48 %; контроль
  `bloom.enabled:0` — 28.6 %, ореол луны r8-16 52.69 → 50.91, после 52.686). Гейты (Debug): `--gbv --gbv-passes=Tonemap`
  на wind_test чисто (`live objects at exit: none`; пересоздание целей посреди прогона → ровно одна пересборка ядра на
  новом поколении); `--scene-stress-gbv=14` CLEAN, 0 `render.validation`, 23 пересоздания целей — но свёртка на его
  уровнях не включалась ни разу (0 пересборок ядра), так что он покрыл создание/удаление общего набора, а не FFT;
  `check_gpu_alloc.py` 0, `check_logging.py` 0; три конфигурации 0 ошибок / 0 предупреждений.

Сверка с провайдерами (M1c) — по именам ресурсов в `--memreport`:

| Провайдер | Значение | Перепись | Итог |
|---|---|---|---|
| `sky.luts` | 17.1 | `SkyAtmosphere.*` + `SkyEnvironment.*` = 16.31 + 0.75 readback = 17.06 | совпало; удалён |
| `cloud.textures` | 39.1 | `VolumetricCloud.*` = 39.06 (цели `Deferred[f].Cloud*` — сверх) | совпало; удалён |
| `rt.as` | 57.3 | `rt.as` 64.6: +7.3 МБ — гранула 64 КБ у 214 буферов (провайдер считал запрошенные байты) | перепись точнее; удалён |
| `tex` | 31.9 | `tex.stream` local 31.4; 0.5 МБ — зарегистрированные в стриминге текстуры, ещё на ресурсе `Texture2D` (`tex.material`) | удалён |
| `rt.as.bin` | 0.0 | — | **оставлен**: перепись не отличает буфер в корзине ретайра от живого, а это сигнал утечки 2026-09-03; Q3 исходил из того, что он дублирует |

Вместе с провайдерами ушёл мёртвый учёт `ownedBytes_` в `SkyAtmosphere`/`VolumetricCloud`. Остались кумулятивные
`tex.ret`, `rt.as.retired` и `rt.as.bin`.

Гейты: три конфигурации; `check_gpu_alloc.py` 0, `check_logging.py` 0; `--scene-stress=150` (Debug) CLEAN, 67.6 с,
число ресурсов на сменах уровня 696–804 без тренда (записи освобождаются); Debug-прогон без `render.validation`;
`--gbv --gbv-passes=Compose`: `live objects at exit: none`. Запусков: 7 (wind_test ×3, DLSS-off, scene-stress, Debug,
GBV).

### M2. Вывод — [1 день]

- **`memreport`** — **GPU-часть и платформа сделаны в M1** (`--memreport[=<с>]`, `render::RequestMemoryReport`);
  остаются кнопка, MCP, CPU-теги после M3 и пик/Δ к прошлому дампу. Исходная формулировка:
  одноразовый многострочный дамп в session log (`WriteRawLines`, Info,
  `core`): дерево тегов CPU/GPU (байты, число, пик, Δ к прошлому дампу), таблица GPU-ресурсов по убыванию
  (имя, тег, тип, формат, размеры, куча, байты, кадр создания, флаги — как `rhi.DumpResourceMemory`),
  платформенная раскладка. Вызов: кнопка в DeveloperWindow, `--memreport` (после `--shot-delay`), MCP.
- **MCP `get_memory {filter, top, snapshot: "A", diff: "A", report}`** — JSON: платформа, теги, топ ресурсов;
  `snapshot`/`diff` — таблица тегов «Size A / Size B / Diff» как в Insights (`MemTagTable.cpp:289-497`). Кейс в
  `TestMcpDrivesTheSameRoad`; собрать Release_Editor для живого использования.
- **Вкладка «Memory»** в DeveloperWindow: дерево тегов (текущее, пик, Δ от снимка, allocs/frame), таблица
  GPU-ресурсов с сортировкой/фильтром, полоса VRAM/бюджет; стоит только когда открыта. `--mem-window` (как
  `--log-window`) — чтобы её можно было сфотографировать `--shot`.
- **Счётчики в Chrome-трассе**: при `--trace` — события `"ph":"C"` раз в кадр по верхним тегам и платформе;
  память ложится дорожками рядом с CPU/GPU-скоупами (`Profiler.cpp:954-1017`; тот же артефакт, новых файлов нет).
- **Критерий приёмки**: скриншот вкладки (`--mem-window --shot`), дамп `memreport` в логе, MCP-гейт зелёный.

**СТАТУС: СДЕЛАНО 2026-10-22, закоммичено `31e7703d`.** Что вошло:
- `render::ReadMemoryPlatform` (одно место чтения платформы для строки `mem:`, отчёта, вкладки и MCP),
  именованные снимки `SaveMemorySnapshot` / `FindMemorySnapshot`, `render::MemoryJson`.
- **MCP `get_memory {filter, top, snapshot, diff, report}`** (`EditorMcpServer`, `RuntimeHost::Memory`, App →
  `MemoryJson`): платформа, перепись, теги, `top` объектов (по умолчанию 20, ≤ 500), `diff` по тегам
  («Size A / Size B / Diff»), diff считается ДО snapshot — `{diff:"A", snapshot:"A"}` читается «с прошлого вызова».
  Кейс в `TestMcpDrivesTheSameRoad`. Живая проверка (Release_Editor, wind_test): снимок A, через 8 с diff A —
  0 изменений по тегам, private +0.5 МБ, live +42; `filter: bloom, top: 3` — три FFT-текстуры по 32 МБ.
  Документные инструменты (и этот) выполняет кадровый цикл редактора: без `--editor` вызов ждёт 60 с и снимается.
- **Вкладка «Memory»** (`app/ui/MemoryTab.{h,cpp}`, DeveloperWindow): процесс, полоса VRAM/бюджет, перепись,
  дерево тегов (родитель = сумма детей) с колонками Δ от снимка, таблица всех GPU-объектов с фильтром и сортировкой
  (ImGuiListClipper), кнопки «Snapshot» и «Report to session log». Данные — дважды в секунду и только пока вкладка
  нарисована. `--mem-window` открывает её на старте (снимок Debug: рисуется, таблицы ниже края окна по умолчанию).
  Пик и allocs/frame по тегу — после M3 (CPU-теги).
- **Счётчики в Chrome-трассе**: `Profiler::RecordTraceCounters` / `IsTraceCapturing` (`TraceDumpData::counters`,
  события `"ph":"C"`); `MemoryReport` каждый кадр захвата пишет трек «memory MB» (private, vram local/nonlocal,
  перепись local/nonlocal) и «gpu tags MB» (верхние теги, дети суммированы). Проверено: `--trace=60` — по 60
  образцов на трек, JSON валиден. Вне захвата — одна атомарная загрузка в кадр.

**Ловушка, которая стоила часа (2026-10-22):** после вставки полей в середину класса `Profiler` Release и
Release_Editor падали на 8-м кадре (AV на чтении ~0x169 сразу во всех рабочих потоках, в деструкторе `ScopedGpu`
каждого прохода), Debug — нет. Код был ни при чём: **инкрементальный LTCG** («N of M functions compiled, the rest
were copied from previous compilation») оставил встроенные копии `Begin/EndGpuSample` со СТАРЫМИ смещениями полей.
Удаление `test_cube/x64/<cfg>/test_cube.iobj` + `.ipdb` (полная кодогенерация) — и тот же код работает. Правило:
после изменения раскладки широко встраиваемого класса — удалить `.iobj/.ipdb` Release-конфигураций перед сборкой.
Как нашлось: событие 1000 журнала Application (модуль + смещение) → dbghelp по PDB (`scratchpad/symbolize.py`) →
стек из `--scene-stress` (`logs/crash_stack.txt`, его фильтр необработанных исключений).

Гейты: три конфигурации (Release/Release_Editor — с полной кодогенерацией); `intent_regression` PASS (новые проверки
`get_memory`: аргументы, `top` ≤ 500, значения по умолчанию, инструмент в списке); Release wind_test — чистый выход;
`check_logging.py` 0, `check_gpu_alloc.py` 0. Ресурсы, проходы и барьеры не менялись — GBV не нужен.

### M3. CPU-теги — [1.5 дня] (Q2: по ключу `--memtrack`)

- **Что**: §4.3 целиком — `MEM_SCOPE`, карта, `--memtrack`, наследование тега задачами; скоупы на владельцах:
  загрузка мешей/текстур (`TextureDecodeCache` уже знает `BytesResident()`, `.h:98`), загрузка уровня, запись
  команд, рендер-граф, редактор, логирование, профайлер, intent-модель, ImGui. Строки `cpu.slack`,
  `cpu.untagged`, `memtrack` (накладные).
- **UE-референс**: `LLM.cpp:5185-5328` (alloc/free/move), `MemoryUtils.h:661-853` (полосатая карта),
  `LLM.cpp:4440-4780` (свой аллокатор), `LLM.cpp:5530-5807` (сбор по кадру), `LowLevelMemTracker.h:351-409`.
- **Критерий приёмки**: **~160 МБ mimalloc разложены по тегам** (M0: «2.5 ГБ кучи» был ложный столбец —
  см. итоги M0); `untagged` < 10 % учтённого в установившемся кадре wind_test; цена `--memtrack` записана
  `--mem-bench` (нс на new/delete; на кадр — × ~1510 new/кадр из M0), цена выключенного — шум. Карта: ~29–31 тыс.
  живых аллокаций на wind_test — размер стартовой таблицы.
- **Гейт**: три конфигурации, `--scene-stress` с `--memtrack`, `--log-stress`.

**СТАТУС: СДЕЛАНО 2026-10-22, `5355e8d6`.** Что вошло:
- `core/memory/MemScope.h`: `MEM_SCOPE(MemTag::X)` — стек тегов на поток (16 уровней, статический TLS).
- `MemNew.cpp`: `--memtrack` читается из сырой `GetCommandLineW()` при ПЕРВОЙ аллокации процесса; карта `ptr → тег`
  — **256** полос под SRWLOCK, линейное пробирование со сдвигом при удалении, таблицы на `VirtualAlloc`, размер не
  хранится (`mi_usable_size` при free); дельты по тегам — в блоке потока (как у M0, один писатель). Без ключа — одна
  relaxed-загрузка на вызов.
- `TaskWithDeps::memTag_`: тег захватывается при создании задачи, воркер выполняет тело под ним (UE так не делает).
- Скоупы: `App::Run` (создание систем, окно/устройство, профайлер, задачи, материалы, текст, загрузка уровня,
  тики сцены/UI/рендера, `Scene::Render` — его задачи наследуют), `MeshManager` (4 точки входа), `TextureDecodeCache`
  (задача декода наследует), стриминг (IO-поток, менеджер, `TickManager`), `OceanSimulation`, поток писателя лога и
  `LogSession` (кольца логгера — 13.3 МБ в трёх аллокациях), `LogWindow`, трасса/`EndFrame` профайлера, MCP-поток,
  потоки LLM, воркер импорта, отрисовка редактора.
- Вывод: `mem:` (` | cpu X MB untagged Y MB (Z%) slack S MB map M MB`), Debug `mem cpu:`, `--memreport` (CPU-колонки
  в дереве тегов + строка cpu), `get_memory` (`cpu{}`, колонки `cpuMB`/`cpuAllocations`, `dCpuMB` в diff; `null`
  без ключа), вкладка Memory (строка CPU и колонки CPU / d CPU), трек «cpu tags MB» в `--trace`, `--mem-bench`
  пишет `memtrack ON/off`.

Итоги (Release, wind_test, камера владельца):

| | |
|---|---|
| Живое через `operator new` | **33 МБ** в ~30.3 тыс. аллокаций |
| По тегам | `log` 13.3 (3), `renderer` 11.0 (3211), `scene` 5.3 (21084), `tex.stream` 1.8 (3560), `profiler` 0.5, `text` 0.2, `tasks` 0.1; `mesh`/`ocean` ≈ 0 (CPU-данные отпускаются после загрузки) |
| **untagged** | **1.1 МБ = 3 %** (до скоупов на `Systems` и логгере было 15 МБ = 45 %) — критерий < 10 % выполнен |
| mimalloc commit | 155–183 МБ → **slack 121–150 МБ**: освобождённое после загрузки, которое mimalloc держит у себя (кандидат: `mi_collect(true)` после загрузки уровня — не проверено) |
| Карта | 1.1 МБ на 30 тыс. записей; unknown frees 0 |
| Цена `--mem-bench` | без ключа +2.5 нс/пара (было 1.45 до проверки ключа); с ключом +27 нс в 1 потоке, **+131 нс на 8 потоках** (с 16 полосами было +356 — отсюда 256) |
| Цена на кадр | `Whole Cycle` 1.744 → 1.816 мс (+0.07, ~4 % главного потока), `Scene::Render` +0.08; кадр GPU-bound (3.3 мс) не изменился; по одному прогону |

Так закрыт вопрос «2.5 ГБ кучи» из M0 до конца: mimalloc держит ~160 МБ, из них живого ~33 МБ, остальное — его кэш.

Гейты: три конфигурации (Release/Release_Editor — полная кодогенерация: менялись раскладки `TaskWithDeps` и
`MemorySnapshot`); `intent_regression` PASS и с `--memtrack`; `--scene-stress=150 --memtrack` (Debug) CLEAN, 46 с;
`--log-stress` 0; Debug-прогон вкладки с `--memtrack` — чистый выход; `check_logging.py` 0, `check_gpu_alloc.py` 0.

### M4. Трасса — [2 дня] (Q1: `traces/`)

- **Что**: §4.4 целиком; файл `traces/memtrace_<stamp>_<pid>.mtrace` (решение Q1), хранятся последние 10.
- **UE-референс**: `MemoryTrace.cpp:33-44,66-173,491-539,696-716`, `CallstackTracePrivate.h:58-146`,
  `WindowsModuleDiagnostics.cpp:164-203`, `TlsBuffer.cpp:49`.
- **Критерий приёмки**: воспроизведение трассы до конца даёт ровно перепись (Σ живых по тегу = таблица M1/M3,
  байт в байт для GPU); цена записана (`gpu` — незаметно; `cpu,stacks=big` и `1/N` — мс кадра); поздний старт
  GPU-трассы проверен.

**СТАТУС: СДЕЛАНО 2026-10-22, `9cdb24b4`.** Что вошло:
- `core/memory/MemTrace.{h,cpp}`: ключ `--memtrace[=all|cpu|gpu][,stacks=big|all|off|1/N][,start=<кадр>]`
  (по умолчанию `all`, `stacks=big`, старт в `WinMain` до устройства). Трасса с CPU-событиями включает карту M3 с
  первой аллокации (`WantsCpuRaw` по сырой командной строке, как `--memtrack`).
- Буфер 64 КБ на поток (`VirtualAlloc`), владелец публикует курсор после каждого целого события, поток-писатель
  `MemTrace writer` раз в 10 мс (или по заполнению буфера) сливает опубликованное в записи-чанки и возвращает
  отработанные буферы в пул (UE `TlsBuffer`). Порядок между потоками — глобальный serial (как sync-события TraceLog);
  событие пишется **под локом, который владеет ключом** (полоса карты, мьютекс переписи) — free и следующий alloc того
  же адреса не меняются местами, а снимок на старте не теряет и не дублирует ничего.
- Старт: перепись (под своим локом) и карта (полоса за полосой, под локом полосы) включают события и выдают всех
  живых синтетическими `Alloc` (бит `0x80` в kind). Стоп (после главного цикла, до разрушения; в `--scene-stress` —
  после прогона) — то же место выключает события и считает живых: эти итоги уходят в запись `End`, и проигрыш файла
  сверяется с ними.
- Стеки: `RtlCaptureStackBackTrace(32)` + свой 64-битный хеш (хеш Rtl — 32 бит), таблица хеш→id и множество
  адресов на `VirtualAlloc` под одним SRW; определение стека — один раз, в поток встретившего первым. Модули с PDB
  GUID+age — на старте и на стопе; символы (dbghelp, отложенная загрузка PDB) — в конце файла.
- `GpuAlloc.cpp`: хук старта/стопа регистрируется статически; ключ GPU-события — запись переписи (тот же объект,
  что живёт, пока жив ресурс); имя ресурса — в самом `Alloc`.
- `tools/memtrace.py summary|verify` — разбор, проигрыш, сверка с `End` (код выхода 1 при расхождении); M5 добавит
  правила запросов.

Формат (отступления от §4.4): события — varint-поток (LEB128, знаковые — zigzag; у UE свой `VarInt`); ключ и serial
— дельтой от предыдущего события того же буфера; поток (thread) — один раз в `Reset` в начале заполнения буфера, а не
в каждом событии; отдельного `Name` нет — имя едет в GPU-`Alloc`; у `Free` нет стека (у UE с версии 2 есть).

Итоги (Release, wind_test, камера владельца, `--shot-delay=10`; замеры шли рядом с GBV-прогоном Codex на той же
машине — шум выше обычного):

| | |
|---|---|
| Трасса `all,stacks=big`, 2890 кадров | 26.9 МБ, 3.4 млн событий (1.72 млн alloc), 549 стеков, 27 буферов, потерь 0; закрытие 70–93 мс |
| `verify` | **PASS байт в байт по каждому тегу**: CPU 33.5 МБ / 28 865 аллокаций, GPU 1408.3 МБ; разбор 2.6 с |
| Поздний старт `start=600` | 29 881 живых ушли синтетическими (CPU + GPU), `verify` PASS; `gpu`-only — PASS |
| `--scene-stress=150 --memtrace` | Release: CLEAN 24.4 с, 2.8 млн событий, 38 тыс. GPU-аллокаций (смены уровней), `verify` PASS (CPU 24.2, GPU 994.2 МБ). Debug: CLEAN 48.8 с (с одним `--memtrack` в M3 — 46 с), 12.5 млн событий / 99 МБ, `verify` PASS, разбор 9.6 с |
| Цена, `Whole Cycle` avg | база 1.730 / 1.616 (два прогона — это и есть шум); `gpu` 1.718; `all,stacks=big` 1.704; `all,stacks=1/16` 1.803 (~94 захвата стека за кадр) |

`gpu` и `stacks=big` в шуме; выборочные стеки `1/16` — ~+0.1 мс главного потока. Свой раскрутчик UE не нужен.

Гейты: три конфигурации; `check_logging.py` 0, `check_gpu_alloc.py` 0; `--scene-stress=150 --memtrace` (Debug и
Release) CLEAN + `verify` PASS. Проходы, барьеры и ресурсы не менялись — GBV не нужен. `--scene-stress` теперь
понимает `--memtrace` (ключ разбирается выше его ветки, стоп — после прогона).

### M5. Анализатор `tools/memtrace.py` — [1 день]

- **Что**: проигрывание → живое множество по кучам; правила UE с их именами (`aAf`, `AaBf`, `aAfB`, `AafB`,
  `aABf`, `AaBCf`, `AaBfC`), метки A/B/C — кадр или секунды; группировки: тег, стек (сверху вниз и
  инвертированно), имя GPU-ресурса, корзины размеров; таблица тегов A/B/Diff; экспорт временных рядов тегов в
  Chrome-trace JSON (Perfetto). Символизация: таблица из файла или dbghelp через `ctypes`.
- **UE-референс**: `AllocationsProvider.h:25-52`, `SMemAllocTableTreeView.cpp:201-222`, `MemTagTable.cpp:289-497`.
- **Критерий приёмки**: на трассе с инъекцией (M6) `leaks A B C` ставит стек инъекции первым; 60 с орбиты
  wind_test — топ-10 роста и churn; время разбора на 10^7 событий замерено (если неприемлемо — C++-разборщик в
  `tools/`, а не SbTree).

**СТАТУС: СДЕЛАНО 2026-10-22, не закоммичено.** `tools/memtrace.py` (докстринг — справка):
- Проигрыш (`Model`) — строка на аллокацию, как `FAllocationItem`: serial alloc `a`, serial free `f` (∞ — не
  освобождена), размер, тег, куча, kind, стек; компактные `array`; заодно живые байты по тегу (CPU и GPU) на конце
  каждого кадра.
- `query RULE A [B [C [D]]]` — все 14 правил `AllocationsQuery.cpp:196-397`, границы включительные, как у UE.
  Метки: кадр (`600`, `f600` — конец кадра), секунды от старта трассы (`12.5s`), `start`, `end`. Псевдонимы
  `leaks` (AaBCf), `growth` (AaBf), `churn` (AafB, сортировка по числу), `decline` (aAfB), `live` (aAf).
- Группировки (`--by`): `tag`, `stack` (стек с кадрами до `file:line`), `tree` (сверху вниз по функциям) и
  `inverted` (от места аллокации к вызывающим), `name` (GPU-ресурс), `size` (корзины степеней двойки, как
  `MemAllocGroupingBySize`), `heap`; `--sort bytes|count`, `--top`, `--heap cpu|gpu|all`, `--depth`. Ведущие кадры
  `std::`/аллокатора/обёрток переписи пропускаются: группа — код, который заставил контейнер расти.
- `tags A B` — таблица тегов UE (`MemTagTable.cpp:289-497`): размер в A, в B, B − A, min/max/avg по кадрам между.
- `timeline` — живые МБ по тегам на каждый кадр как Chrome-trace счётчики (`"ph":"C"`, два трека cpu/gpu) в
  `traces/<имя трассы>.json` — открывается в Perfetto/chrome://tracing.
- `selftest` — синтетическая трасса с известными временами жизни: 16 запросов по всем правилам, ряды на кадре,
  столкновение одинакового ключа в CPU и GPU-пространствах. PASS; подмена условия `AaBCf` на лету даёт FAIL —
  тест ловит. Проверка «`leaks` ставит стек инъекции первым» переезжает в M6: инъекция строится там.

Итоги:

| | |
|---|---|
| Разбор | 12.5 млн событий (Debug-стресс): чтение 7.0 с + проигрыш 5.1 с ≈ **10 с на 10^7** — C++-разборщик не нужен |
| Орбита wind_test 60 с, `--memtrace=all,stacks=all --set=frame.maxFps:60` | 3.8 млн событий, 11 899 стеков, `verify` PASS; цена `stacks=all` ≈ +0.5 мс главного потока (Whole Cycle 2.16 мс) |
| Рост после 10 с (`growth 10s end`) | 269 МБ: пул `LocalShadowCache` 64 МБ и кольца `ShadowGpuData` (19 + 19 + 14×4 МБ) пересозданы в `ReconcileShadowMode`/`ReconcileShadowCapacity` посреди орбиты, wind-BLAS 73 шт. (28 + 19 МБ scratch), стриминг 35 МБ. По `tags 10s end` это рост ёмкости, не утечка: `shadow.gpudata` +44, `tex.stream` +35, `shadow.local` +16 МБ |
| Churn после 10 с (`churn 10s end`) | 976 тыс. короткоживущих за 50 с. **`RtSceneAs::Build` — 80 % байт CPU-churn: 140 тыс. переаллокаций векторов (строки 346/355/426), 440 МБ трафика** — кандидат на `reserve`/переиспользование; `BuildOrRefitWindSlot:269` 79 тыс.; `TextureStreamingManager::Apply_` — копии путей `wstring` в строки вкладки, 75 тыс.; `SubmitTimeline::GatherFrameLists` 109 тыс.; `Material::FSProbeAndFlagPending` 33 тыс. |

Release-имена стандартных шаблонов в стеках ненадёжны (ICF склеивает одинаковые тела: вектор указателей выглядит как
вектор `nlohmann::json*`) — поэтому группировка начинается с первого не-`std::` кадра; точные имена — в Debug.

### M6. Гейты и самопроверка — [полдня]

- `--mem-selftest=leak@<tag>[,kb=<n>]` — нарочно теряет N КБ за кадр (CPU `new` и/или GPU-буфер) под тегом;
  строка `mem self-test: injected …` в логе, как у `--gbv-selftest`.
- `--mem-readout=<с>`: орбита, после прогрева — наклон МБ/с по private, vram local и каждому тегу (МНК, как
  `MallocLeakDetection.cpp:270-290`); вердикт `mem readout: PASS|FAIL <tag> <MB/s>` в session log, код выхода =
  число превышений; порог > 1 МБ/с после 15 с прогрева (Q5).
- `--scene-stress`: при возврате на уже виденный уровень теги сверяются с первым визитом; расхождение —
  Warning со списком тегов, вердикт в существующий `scene_stress.log`.
- **Критерий приёмки**: оба гейта ловят `--mem-selftest` и молчат без него (3 прогона); раздел «Memory profiling
  — how to run it» в `AGENTS.md` по образцу раздела GBV; перенесено из M5: на трассе с инъекцией
  `memtrace.py leaks A B C` ставит стек инъекции первым.

### M7 (по желанию). Освобождённые GPU VA для page fault

- Кольцо последних N освобождённых буферов `{имя, тег, VA-диапазон, кадр}` (у текстур VA нет — только буферы);
  при DRED page fault — строка «VA принадлежал X, освобождён в кадре F». UE: `D3D12Adapter.cpp:1953-2025`.

---

## 6. Что НЕ делаем

- Не меняем аллокатор: mimalloc 2.2.5 prebuilt остаётся; пересборка с `MI_TRACK_ETW` и хуки его `VirtualAlloc`
  (как `WindowsPlatformMemory.cpp:801-906`) не нужны — `mi commit` из `mi_process_info` даёт платформенный итог.
- Не хукаем vtable `ID3D12Device` (прокси Streamline, отладочный слой) — явные обёртки + скрипт-проверка.
- Не делаем SbTree, асинхронные запросы, свой GUI-вьюер, LLM tag sets (Assets/Class), метаданные-стек UE.
- Не кладём текстуры в кучи ради GPU VA (`D3D12Adapter.cpp:1528-1533`).
- Не переводим ресурсы на placed/пулы — перепись измерит цену committed, решение — отдельным планом.
- Не пишем `logs/<name>.log` для диагностики и не логируем каждый кадр.
- Чужие DLL со своей CRT (Streamline/NGX, dxcompiler) не учитываем — они в `private.other`.

## 7. Отступления от UE

| UE | Мы | Почему |
|---|---|---|
| LLM — хуки в `FMemory` над `GMalloc` | хук в своём `operator new/delete` | у нас единственный путь в mimalloc — `new/delete` |
| карта хранит размер (17 Б/аллокацию) | карта хранит только тег, размер — `mi_usable_size` | аллокатор свой и сам знает размер |
| трекер `Platform` по страницам ОС | `mi commit` + `HeapSummary` + `QueryVideoMemoryInfo` | без исходников mimalloc страницы не хукнуть |
| GPU-ключ = GPU VA, текстуры в кучи | ключ = `ID3D12Resource*`, освобождение — sentinel | всё committed; VA у текстур нет |
| поздний старт трассы невозможен | GPU (и CPU при `--memtrack`) — синтетические Alloc из переписи | перепись знает всех живых |
| свой раскрутчик стека | `RtlCaptureStackBackTrace` + свой 64-битный хеш | проще; замер M4: `stacks=big` в шуме, `1/16` ≈ +0.1 мс |
| трасса не знает, что считает процесс | запись `End` — живые по тегам на момент стопа, `memtrace.py verify` | трасса проверяет себя сама (M4: байт в байт) |
| время — маркер раз в 4095 событий | плюс маркер на каждой границе кадра | метки A/B у нас — кадры |
| символы оффлайн по PDB | таблица символов в конце файла + оффлайн как запас | без отдельного резолвера |
| теги задач — руками | тег захватывается при постановке задачи | 1 байт на задачу, меньше untagged |

## 8. Решения владельца (2026-10-22)

- **Q1 — РЕШЕНО: `traces/`.** Трасса — разрешённый артефакт: `traces/memtrace_<stamp>_<pid>.mtrace`, рядом с
  Chrome-трассами профайлера; хранить последние 10 (ротация как у session-логов).
- **Q2 — РЕШЕНО: по ключу.** CPU-теги (карта ptr→тег) выключены по умолчанию во всех конфигурациях, включаются
  `--memtrack` и в гейтах M6. GPU-перепись и счётчики аллокаций/кадр — всегда.
- **Q3 — РЕШЕНО: удалить.** После сверки M1c (±1 %) провайдеры `sky.luts`, `cloud.textures`, `tex`, `rt.as`,
  `rt.as.bin` удаляются; кумулятивные `rt.as.retired`, `tex.ret` остаются.
- **Q4 — РЕШЕНО.** Список тегов — таблица §4.1: тег = владелец (вид и куча — колонки), цели RTM — по фичам, тени —
  пять тегов, имена короткие.
- **Q5 — РЕШЕНО: > 1 МБ/с.** Устойчивый рост больше 1 МБ/с по private, vram local или любому тегу после 15 с
  прогрева = FAIL (утечка корзины RT была 8.5 МБ/с).

## 9. Ловушки

- Аллокации до `WinMain` и в деструкторах потоков: `thread_local` — только POD без деструктора, поточный блок не
  освобождается.
- Рекурсия: учёт никогда не зовёт `new`; хранилище карты и буферы трассы — `VirtualAlloc`.
- Free в чужом потоке: карта полосатая под мьютексами, дельты вычитаются в блоке освобождающего потока — сумма верна.
- Sentinel может сработать на любом потоке и после начала остановки: перепись — статическая, переживает устройство.
- Под отладочным слоем объект может умереть позже последнего `Release` приложения — проверить в M1.
- Стеки в Release/Release_Editor склеены LTCG — для точных стеков Debug.
- `vram local` включает DLSS/Streamline и драйвер — это `gpu.untracked`, а не утечка.
