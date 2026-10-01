# Отчёт о поиске багов в test_cube

Дата: 1 октября 2026 года, Europe/Berlin.

Проверенный commit: `dcb4eaa87fc3c5f85318cd95c4590f15e02b3468`.

Корень проекта: `D:\Programming\test_cube`.

Назначение: независимая перепроверка находок другим агентом, включая Claude Opus. Исправления не вносились. Ни одна находка не должна считаться визуально подтверждённой только потому, что она включена в этот отчёт.

## 1. Результат и степень доказанности

Найдено пять отдельных проблем. Все имеют предварительный приоритет P2: функциональные ошибки, которые следует исправить, но для которых в рамках проверки не установлен аварийный отказ приложения. Уровень уверенности в механизме ошибки и полнота воспроизведения указаны отдельно.

| ID | Проблема | Что проверено | Что ещё проверить |
|---|---|---|---|
| BUG-01 | Undo видимости применяет противоположное значение вместо исходного | Реальный `SetEnabledCommand`, Execute и Undo, состояние документа на CPU | Проход через action registry/MCP и смешанную выборку в редакторе |
| BUG-02 | Undo назначения материала превращает отсутствие ключа в пустую строку | Реальная команда и реальный `ResolveMeshAsset`; наследуемый `bronze` становится `""` | Внешний вид объекта после Undo на GPU |
| BUG-03 | Назначение материала всему объекту игнорируется при наличии списка `materials` | Реальная команда, фабрика меша и имена трёх runtime-слотов палатки | Изображение после GPU Init; дополнительные варианты мешей |
| BUG-04 | Пересоздание скрытого объекта не восстанавливает его видимость | Фабрика возвращает visible=true для JSON с enabled=false; проанализирована цепочка respawn | Полный Execute команды с GPU и live runtime; аналогичный случай с частицами |
| BUG-05 | Переключение readback One/Two → None оставляет плавучесть на старой поверхности | Анализ BuildPass, Poll, HasSurface, OceanBuoyancy и смены настроек | Runtime-переход с уже принятым readback, включая незавершённые GPU-копии |

Важное уточнение к выводу тестовой программы `Confirmed 4/4 review cases`: это четыре локальных проверки, а не четыре полностью воспроизведённых GUI/GPU-сценария. Четвёртая проверка вызывает только фабрику, без Execute команды и GPU Init. BUG-05 вообще не входит в эту программу.

## 2. Область проверки и ограничения

Проверка была выборочной, а не исчерпывающим аудитом всего движка. Основное внимание: команды редактора, наследование параметров mesh assets, пересоздание runtime-объектов, ocean readback и плавучесть. Также просмотрены некоторые недавние изменения bloom/sun probe и ocean shaders; отдельные подтверждённые баги в них в этот список не добавлены.

В ходе поиска:

- Запущены существующие `intent_regression.exe` и `inspector_regression.exe`.
- Запущены `tools/check_logging.py` и `tools/check_shaders.py`.
- Собрана и запущена небольшая CPU-программа с реальными объектными файлами движка.
- Не запускались редактор, renderer с окном, GPU scene stress или GPU-based validation.
- Не менялись уровни, mesh manifests, настройки редактора и исходники движка.
- Не запускался llama-server и не выполнялись запросы к локальной модели.

Готовые regression EXE не пересобирались специально для аудита: `intent_regression.exe` имеет время сборки 24 сентября 2026 года, `inspector_regression.exe` — 23 сентября. Их успешный запуск нельзя трактовать как свежую полную сборку текущего HEAD.

Новая программа использует Debug-объекты из engine linker manifest. Для проверенных реализаций проверена свежесть по времени файлов: `SetEnabledCommand.obj` и `SetMaterialCommand.obj` собраны 28 сентября, после соответствующих исходников; `SceneObjectFactory.obj` собран 24 сентября после исходника фабрики. Это не заменяет полную независимую пересборку для окончательной верификации.

## 3. BUG-01 — Undo видимости не восстанавливает исходное состояние

**Приоритет:** P2. **Уверенность:** высокая. **Доказательство:** прямое CPU-воспроизведение.

### Код и достижимость

- [SetEnabledCommand.cpp:37](/D:/Programming/test_cube/sources/editor/commands/SetEnabledCommand.cpp:37): Execute вызывает `Apply(ctx, enabled_)` и возвращает true.
- [SetEnabledCommand.cpp:43](/D:/Programming/test_cube/sources/editor/commands/SetEnabledCommand.cpp:43): Undo вызывает `Apply(ctx, !enabled_)`.
- [SetEnabledCommand.h:25](/D:/Programming/test_cube/sources/editor/commands/SetEnabledCommand.h:25): команда хранит только целевое значение; исходное значение не захватывает.
- [EditorActionRegistry.cpp:354](/D:/Programming/test_cube/sources/editor/intent/EditorActionRegistry.cpp:354): `BuildSetEnabled` создаёт команды для всех targets без пропуска объектов, уже имеющих нужное значение.
- [EditorCommandStack.cpp:44](/D:/Programming/test_cube/sources/editor/commands/EditorCommandStack.cpp:44): успешная команда попадает в историю.

### Минимальное воспроизведение

1. Создать документ с объектом `enabled=true`.
2. Выполнить `SetEnabledCommand(id, true)`.
3. Выполнить Undo этой команды.
4. Прочитать `document.Find(id)->enabled`.

Ожидается `true`: до команды объект был видимым, команда ничего не изменила.

Получено `false`.

Фактический вывод программы:

```text
enable already-visible then undo: expected=true actual=false
```

### Пользовательский эффект

Команда «показать» уже показанный объект, а затем Ctrl+Z скрывает его. При массовом показе смешанной выборки Undo скрывает также те объекты, которые были видимыми до операции. Аналогично для повторного скрытия: Undo показывает ранее скрытый объект.

`BuildIsolate` отдельно пропускает объекты, уже находящиеся в нужном состоянии ([EditorActionRegistry.cpp:1544](/D:/Programming/test_cube/sources/editor/intent/EditorActionRegistry.cpp:1544)). Поэтому этот отчёт не утверждает, что обычный isolate обязательно воспроизводит BUG-01. Доказанная достижимость — общий `setEnabled` и прямой вызов команды.

### Причина и исправление

Инверсия целевого значения совпадает с исходным только если Execute действительно переключил состояние. Для команды присваивания это предположение неверно.

Предпочтительно захватывать исходный `enabled` при первом Execute, хранить его и восстанавливать при Undo. Захват нужен однократный: повторный Execute при Redo не должен менять сохранённую исходную точку.

Возможен пропуск неизменяющих состояние объектов в builder, но это не делает саму команду корректной для других callers. Не следует просто возвращать false для no-op внутри команды: `CompositeCommand::Execute` трактует false как отказ и откатывает ранее выполненные дочерние команды.

### Проверки исправления

- true → set true → Undo → true.
- false → set false → Undo → false.
- true → set false → Undo → true → Redo → false.
- Смешанная выборка [true, false] → set true → Undo → [true, false].
- Повторный цикл Undo/Redo не изменяет сохранённое исходное состояние.

## 4. BUG-02 — Undo назначения материала ломает наследование из mesh asset

**Приоритет:** P2. **Уверенность:** высокая. **Доказательство:** прямое CPU-воспроизведение через команду и resolver.

### Код и данные

- [SetMaterialCommand.cpp:60](/D:/Programming/test_cube/sources/editor/commands/SetMaterialCommand.cpp:60): отсутствующий ключ читается как пустая строка через `properties.value("material", std::string())`.
- [SetMaterialCommand.cpp:28](/D:/Programming/test_cube/sources/editor/commands/SetMaterialCommand.cpp:28): Apply всегда записывает ключ `material`.
- [SetMaterialCommand.cpp:66](/D:/Programming/test_cube/sources/editor/commands/SetMaterialCommand.cpp:66): Undo вызывает тот же Apply с сохранённой строкой.
- [SceneObjectFactory.cpp:106](/D:/Programming/test_cube/sources/app/scene/SceneObjectFactory.cpp:106): `ResolveMeshAsset` подставляет значения asset только для отсутствующих ключей объекта.
- [models/box.mesh.json:4](/D:/Programming/test_cube/models/box.mesh.json:4): asset задаёт `"material": "bronze"`.

### Минимальное воспроизведение

Исходный объект:

```json
{
  "type": "staticMesh",
  "mesh": "models/box.mesh.json"
}
```

1. Проверить effective material через `ResolveMeshAsset`: он равен `bronze`.
2. Выполнить `SetMaterialCommand(id, "tent_0")`.
3. Выполнить Undo.
4. Снова разрешить mesh defaults.

Ожидается отсутствие собственного ключа `material` и effective material `bronze`.

Получено собственное переопределение `"material": ""`; effective material равен пустой строке.

```text
assign material then undo inherited box: expected='bronze' actual=''
```

### Пользовательский эффект и граница доказательства

Ctrl+Z не возвращает исходную привязку материала. Если сохранить документ после Undo, пустое переопределение попадает в уровень и продолжает блокировать defaults mesh asset.

Тест доказал потерю ссылки на `bronze`, но не измерял результирующие пиксели. Конкретный внешний вид fallback зависит от пути загрузки и material resolver; этот отчёт не утверждает, что все такие объекты обязательно становятся одного определённого цвета.

### Причина и исправление

Отсутствие ключа и существующий ключ с пустой строкой имеют разную семантику. Команда сохраняет только строку и теряет этот бит состояния.

Нужно сохранять наличие ключа и его точное значение. При Undo исходно отсутствовавший ключ удалять, а исходно существовавший — возвращать. Не заменять отсутствие копией effective material: это превратит наследование в постоянный override и изменит поведение при будущих правках asset.

Пример уже существующего корректного подхода для массива: [SetMaterialSlotCommand.cpp:139](/D:/Programming/test_cube/sources/editor/commands/SetMaterialSlotCommand.cpp:139) сохраняет `oldHadMaterials_`, а Undo восстанавливает либо удаляет ключ.

### Проверки исправления

- Mesh с унаследованным scalar material, без собственного ключа: Execute/Undo возвращают отсутствие ключа и прежний effective material.
- Объект с явным scalar material: Undo возвращает именно его.
- Объект с явно пустой строкой: Undo сохраняет пустую строку, а не удаляет ключ.
- После Undo изменение material в asset должно оставаться видимым для объекта без собственного override.
- Проверить вместе с BUG-03, поскольку исправление whole-object assignment может начать менять и `materials`.

## 5. BUG-03 — Назначение материала всему multi-slot объекту может не менять ни одного слота

**Приоритет:** P2. **Уверенность:** высокая. **Доказательство:** реальная команда и фабрика runtime-меша на CPU.

### Контракт

[EditorActionRegistry.cpp:3261](/D:/Programming/test_cube/sources/editor/intent/EditorActionRegistry.cpp:3261) явно описывает `setMaterial`: если `slot` не указан, меняется весь объект. Это не предположение об интерфейсе, а контракт существующего action registry.

[EditorActionRegistry.cpp:1493](/D:/Programming/test_cube/sources/editor/intent/EditorActionRegistry.cpp:1493) направляет вызов без slot в `SetMaterialCommand`, а с slot — в `SetMaterialSlotCommand`. Назначение материала через Content Browser также использует первую команду ([EditorController.cpp:3104](/D:/Programming/test_cube/sources/editor/EditorController.cpp:3104)); drag-and-drop на Inspector — [InspectorPanel.cpp:379](/D:/Programming/test_cube/sources/editor/ui/InspectorPanel.cpp:379).

### Механизм

1. `SetMaterialCommand::Apply` меняет только `properties["material"]`.
2. `ResolveMeshAsset` независимо подставляет отсутствующий ключ `materials` из asset.
3. Фабрика сначала создаёт StaticMesh со scalar material, затем вызывает `SetSlotPresets` с effective массивом `materials` ([SceneObjectFactory.cpp:414](/D:/Programming/test_cube/sources/app/scene/SceneObjectFactory.cpp:414)).
4. Непустой массив заменяет `slotPresets_` полностью ([GBufferRenderable.h:132](/D:/Programming/test_cube/sources/rendering/renderables/GBufferRenderable.h:132)).
5. Runtime material resolver читает именно `slotPresets_[i]` ([GBufferRenderable.cpp:383](/D:/Programming/test_cube/sources/rendering/renderables/GBufferRenderable.cpp:383)).

### Минимальное воспроизведение

Объект с `mesh: "models/tent.mesh.json"`, без собственных `material` и `materials`.

Asset [tent.mesh.json:4](/D:/Programming/test_cube/models/tent.mesh.json:4) содержит:

```json
"materials": ["tent_0", "tent_1", "tent_2"]
```

1. Выполнить `SetMaterialCommand(id, "bronze")`.
2. Передать JSON объекта в `CreateStaticMeshFromJson`.
3. Прочитать `AsGBufferRenderable()->SlotPreset(0..2)`.

Ожидается `bronze` во всех трёх слотах по контракту whole-object assignment.

Получено:

```text
assign whole tent material: expected='bronze' x3 actual='tent_0','tent_1','tent_2'
```

GPU Init не выполнялся. Доказана конфигурация имён материалов, передаваемая runtime resolver; изображение не снималось.

### Исправление и ловушки

Whole-object assignment должен формировать effective переопределение для всех material slots. Нужно согласованно обрабатывать scalar `material` и массив `materials`, а Undo должен точно вернуть наличие и значения затронутых ключей.

Просто удалить собственный `materials` недостаточно: `ResolveMeshAsset` снова подставит его из asset. Простая запись scalar material также не покрывает все слоты glTF-меша с несколькими submeshes и auto-materials.

Не следует менять семантику `SetMaterialSlotCommand`: назначение конкретному слоту должно оставлять остальные слоты без изменений.

### Проверки исправления

- Унаследованный список материалов палатки: все слоты меняются на bronze.
- Собственный список `materials`: все слоты меняются, Undo возвращает точный массив.
- Scalar-only single-slot mesh сохраняет рабочее поведение.
- glTF с несколькими auto-material slots: whole-object assignment меняет все фактические слоты.
- Вызов с явным slot меняет только этот slot.
- Undo/Redo сохраняют различие между отсутствующим ключом и собственным override.

## 6. BUG-04 — Пересоздание runtime-объекта теряет disabled/hidden состояние

**Приоритет:** P2. **Уверенность в цепочке:** высокая. **Полнота воспроизведения:** фабрика на CPU плюс анализ callers; полного GPU Execute нет.

### Предусловие

Нужен объект, присутствующий в runtime сцены, но скрытый через `SetVisible(false)` и `document.enabled=false`. Например, скрыть уже существующий меш в редакторе: `SetEnabledCommand` меняет видимость, не удаляя runtime.

Не следует ограничиваться проверкой document-only объекта без runtime: команды respawn пропускают пересоздание, если `FindEditorObject` возвращает nullptr.

### Цепочка исполнения

- [RenderableObjectBase.h:319](/D:/Programming/test_cube/sources/rendering/renderables/RenderableObjectBase.h:319): новый объект по умолчанию имеет `visible_=true`.
- [SceneObjectFactory.cpp:390](/D:/Programming/test_cube/sources/app/scene/SceneObjectFactory.cpp:390): фабрика StaticMesh не применяет JSON-поле enabled; `ApplyStaticMeshJsonProperties` тоже этого не делает.
- [SetMeshAssetCommand.cpp:56](/D:/Programming/test_cube/sources/editor/commands/SetMeshAssetCommand.cpp:56): создаётся новый runtime.
- [SetMeshAssetCommand.cpp:68](/D:/Programming/test_cube/sources/editor/commands/SetMeshAssetCommand.cpp:68): старый удаляется, новый добавляется без `runtime->SetVisible(obj->enabled)`.
- [Scene.cpp:1268](/D:/Programming/test_cube/sources/app/scene/Scene.cpp:1268): `AddInitializedEditorObject` вызывает Init, задаёт id, SyncSceneState и добавляет runtime; enabled из документа не принимает и не восстанавливает.
- [RenderableObject.cpp:479](/D:/Programming/test_cube/sources/rendering/renderables/RenderableObject.cpp:479): SyncSceneState занимается transform/history/bounds, видимость не восстанавливает.

Тот же пропуск в:

| Команда | Место добавления нового runtime |
|---|---|
| Назначение whole-object material | [SetMaterialCommand.cpp:43](/D:/Programming/test_cube/sources/editor/commands/SetMaterialCommand.cpp:43) |
| Назначение material slot | [SetMaterialSlotCommand.cpp:46](/D:/Programming/test_cube/sources/editor/commands/SetMaterialSlotCommand.cpp:46) |
| Замена particle preset | [SetParticlePresetCommand.cpp:68](/D:/Programming/test_cube/sources/editor/commands/SetParticlePresetCommand.cpp:68) |

Для частиц фабрика [SceneObjectFactory.cpp:443](/D:/Programming/test_cube/sources/app/scene/SceneObjectFactory.cpp:443) также не применяет enabled. Этот вариант отдельно не исполнялся.

### Что фактически проверено

В CPU-проверке вызвана фабрика для:

```json
{
  "type": "staticMesh",
  "mesh": "models/box.mesh.json",
  "enabled": false
}
```

Получено `runtime->IsVisible() == true`:

```text
factory used by material/mesh respawn on disabled object: expected=false actual=true
```

Название expected в этой строке отражает требование к конечному respawn-пути. Сам по себе результат фабрики не является доказательством нарушения её контракта: callers могут быть ответственны за enabled. Ошибка локализуется в связке фабрика → respawn caller → AddInitializedEditorObject, где ни один шаг не восстанавливает видимость.

### Сценарий для независимого GPU-воспроизведения

1. Открыть тестовый уровень с обычным staticMesh в редакторе.
2. Скрыть объект, убедиться, что runtime остался в сцене и IsVisible=false.
3. Выделить его через Outliner.
4. Сменить mesh asset или назначить материал через доступный путь редактора.
5. Проверить одновременно `document.enabled`, `runtime.IsVisible()` и видимость в кадре.

Ожидается false/false. По цепочке кода ожидается false/true: документ и экран расходятся. Последующее сохранение и повторная загрузка могут вернуть объект в скрытое состояние, что дополнительно маскирует причину.

### Исправление и тесты

Применить документный enabled к новому runtime перед добавлением в сцену во всех затронутых respawn-путях либо вынести общий корректный helper. Изменять всю фабрику без аудита её остальных callers не обязательно и может расширить scope.

Проверить hidden и visible объекты, Execute/Undo/Redo каждого respawn-пути; отдельно проверить материал слота и particle preset. Проверять не только сериализуемый документ, но и реальную runtime-видимость.

## 7. BUG-05 — Readback None прекращает копии, но сохраняет старую поверхность для плавучести

**Приоритет:** P2. **Уверенность в механизме:** высокая. **Полнота воспроизведения:** статический анализ; runtime/GPU-переход не проверен.

### Контракт и предусловия

Repository instructions явно определяют: `readbackCascades=None` означает плоскую поверхность для плавучести. Это также написано в предупреждении [OceanBuoyancy.cpp:330](/D:/Programming/test_cube/sources/ocean/OceanBuoyancy.cpp:330).

Нужны видимый океан, buoyant объект и хотя бы один успешно принятый readback в режиме One/Two. Холодный старт сразу в None не демонстрирует эту проблему: тогда старой поверхности ещё нет.

### Цепочка состояния

1. `Poll` принимает завершённую копию и записывает ненулевой `adoptedFrame_` ([OceanReadback.cpp:307](/D:/Programming/test_cube/sources/ocean/OceanReadback.cpp:307)).
2. `HasSurface()` проверяет только `adoptedFrame_ != 0`, без проверки текущего режима ([OceanReadback.h:79](/D:/Programming/test_cube/sources/ocean/OceanReadback.h:79)).
3. При None количество cascades равно нулю. `BuildPass` возвращается до инвалидирования принятой поверхности ([OceanReadback.cpp:139](/D:/Programming/test_cube/sources/ocean/OceanReadback.cpp:139)).
4. `OceanBuoyancy::Tick` всё равно вызывает Request/Poll и затем Step. Проверка None только пишет warning, а не отключает ветку чтения старой поверхности.
5. Step выбирает старые `body.waterHeight` и `body.waterRate`, пока `HasSurface()` остаётся true ([OceanBuoyancy.cpp:223](/D:/Programming/test_cube/sources/ocean/OceanBuoyancy.cpp:223)).
6. В отличие от этого, плоский fallback `ocean.GetWaterLevel()` выбирается только при false.

Смена simulation settings вызывает ResetGpuResources ([OceanSimulation.cpp:75](/D:/Programming/test_cube/sources/ocean/OceanSimulation.cpp:75)), но сам OceanReadback принадлежит OceanRenderable. В просмотренном пути смены настроек его adopted state не сбрасывается. Единственное явное присваивание `adoptedFrame_=0` в OceanReadback.cpp находится в EnsureBuffer; при нуле cascades вызов не доходит до EnsureBuffer.

### Что происходит с высотой

После переключения плавучесть продолжает использовать последний принятый рельеф волн. Экстраполяция возраста ограничена `kMaxCarrySeconds=0.25`, поэтому отчёт не утверждает, что прогноз высоты бесконечно растёт. После насыщения age остаётся фиксированная старая высота с фиксированной поправкой скорости, а тело может продолжать интегрироваться и сходиться к этой неверной поверхности.

В результате объект может остановиться с неверным heave/pitch/roll вместо перехода на плоский текущий waterLevel. Изменение waterLevel при None также следует проверить: старые значения могут продолжать определять равновесие.

### Независимое воспроизведение

1. Запустить редактор с видимым океаном и buoyant mesh.
2. В Ocean controls установить Readback cascades=One или Two.
3. Дождаться принятой поверхности: HasSurface=true; session log содержит событие `ocean readback: surface adopted`.
4. Дождаться неплоского положения объекта или выбрать момент с заметной волной.
5. Переключить Readback cascades=None.
6. Подождать больше 0.25 s и время затухания движения тела.
7. Сравнить высоту/наклон с новым экземпляром того же объекта или холодным запуском в None; проверить HasSurface и внутреннюю выборку water.

Отдельно проверить быстрое One → None → One с копиями в полёте. Поступившая после выключения старая копия не должна снова активировать устаревшую поверхность.

Если нужны сравнения кадров, использовать repository-механизм `--wind-freeze[=<seconds>]` и одну камеру. Здесь важно проверить сам runtime-переход, а не только два холодных запуска с разными presets.

### Исправление и риски реализации

Нужно сделать режим None авторитетным для consumers: инвалидировать/отключать adopted surface при смене режима и сбрасывать относящуюся к предыдущим сэмплам историю плавучести по необходимости. Одновременно учесть pending slots: Poll не должен принять копию от старого режима после выключения.

Не освобождать буферы и не считать pending slots свободными до завершения соответствующего GPU fence. Смена режима, возраст принятого снимка и GPU lifetime — разные состояния; исправление семантики None не должно создать гонку повторного использования ring slots.

### Проверки исправления

- Холодный None: плоский текущий waterLevel.
- One/Two → None после принятой копии: никакого старого wave displacement.
- Pending copy → None → завершение copy: consumer остаётся на плоской поверхности.
- None → One/Two: новые копии снова принимаются и движение возобновляется.
- Быстрый One → None → One при GPU lag.
- Изменение waterLevel в None.
- Предусмотренные project checks для изменения readback/resource lifecycle, включая Debug GPU validation.

## 8. Выполненные проверки и реальные результаты

Команды выполнялись из `D:\Programming\test_cube`.

| Проверка | Результат | Ограничение |
|---|---|---|
| `x64\Debug\intent_regression.exe` | exit 0, финальный PASS | Готовый бинарник от 24 сентября; не полная fresh build |
| `x64\Debug\inspector_regression.exe` | exit 0, PASS по Inspector/multi-edit/undo/redo | Готовый бинарник от 23 сентября; не GPU проверка |
| `python tools/check_logging.py` | exit 0, `0 finding(s)` | Это lint прямого вывода в лог, а не проверка найденных функциональных ошибок |
| `python tools/check_shaders.py` | exit 0, `112/112 compiled` | Offline compile перечисленных entry points/permutations; не runtime/GPU validation |
| `x64\Debug\bug_review.exe` | exit 0, четыре ожидаемых отклонения воспроизведены | BUG-01..03 на CPU; для BUG-04 только фабрика; BUG-05 отсутствует |

Финальные строки штатных программ:

```text
PASS: intent layer parses, previews, places, undoes, and the model contract holds
PASS: actual Post Process draw, no GPU or editor state writes
0 finding(s)
112/112 compiled
```

Полный вывод отдельного reproducer:

```text
enable already-visible then undo: expected=true actual=false
assign material then undo inherited box: expected='bronze' actual=''
assign whole tent material: expected='bronze' x3 actual='tent_0','tent_1','tent_2'
factory used by material/mesh respawn on disabled object: expected=false actual=true
Confirmed 4/4 review cases
```

Успешные штатные проверки не опровергают эти находки. В рамках этого аудита не доказано, что существующие regression suites покрывают именно no-op enabled Undo, отсутствие scalar material при наследовании, whole-object assignment с inherited materials, hidden respawn и смену readback режима после принятой копии.

## 9. Как собрать и запустить reproducer

Файлы, созданные во время проверки:

- [bug_review.cpp](/D:/Programming/test_cube/x64/BugReview/bug_review.cpp).
- [bug_review.vcxproj](/D:/Programming/test_cube/x64/BugReview/bug_review.vcxproj).
- [bug_review.exe](/D:/Programming/test_cube/x64/Debug/bug_review.exe).

`x64/` игнорируется Git. Исходник и проект вложены ниже, чтобы отчёт оставался пригодным для перепроверки даже без этих локальных файлов.

Проект построен на `tools/intent_regression.vcxproj` и использует engine objects из `test_cube/x64/Debug/test_cube.tlog/link.command.1.tlog`, а не произвольный glob всех obj. Нужна существующая успешная Debug|x64 сборка движка и соответствующие third-party libs/DLL.

Проект содержит абсолютный Repo для этой машины. При переносе нужно заменить его на актуальный корень. Обновлять test или движок для обычного чтения отчёта не требуется.

```powershell
Set-Location -LiteralPath 'D:\Programming\test_cube'
& 'C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/MSBuild.exe' `
  'x64/BugReview/bug_review.vcxproj' `
  /p:Configuration=Debug /p:Platform=x64 /m:2 /nologo /v:minimal
& '.\x64\Debug\bug_review.exe'
```

На этой сессии первый прямой запуск MSBuild отказал из-за дублирования environment keys `Path`/`PATH` в окружении процесса. Это проблема build environment, а не отдельный баг проекта. После нормализации имён environment variables через Python subprocess reproducer собрался успешно. Точная применённая обходная команда:

```powershell
python -c "import os, subprocess; fixed = {}; seen = set(); [(fixed.__setitem__(k, v), seen.add(k.upper())) for k, v in os.environ.items() if k.upper() not in seen]; raise SystemExit(subprocess.call([r'C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe', r'D:\Programming\test_cube\x64\BugReview\bug_review.vcxproj', '/p:Configuration=Debug', '/p:Platform=x64', '/m:2', '/nologo', '/v:minimal'], env=fixed))"
```

Важно: `bug_review.exe` является reproducer известных отклонений, а не готовым acceptance test исправлений. Exit 0 означает, что все четыре ожидаемых отклонения наблюдались. После исправлений нужно изменить проверки на assertions корректного поведения; старый reproducer закономерно перестанет возвращать `Confirmed 4/4`.

## 10. Задание для независимого ревью

Для каждого BUG-01..05 желательно дать отдельный вердикт: **подтверждён**, **опровергнут**, либо **нужно runtime/GPU-воспроизведение**. Для опровержения указать конкретную ветку/контракт, из-за которой описанный эффект не достигается.

Проверить особенно:

1. Проходит ли setEnabled no-op через реальный action builder и историю; не обобщать исключение BuildIsolate на весь setEnabled.
2. Сохраняется ли различие «ключ отсутствует»/«пустая строка» в Undo материала и сериализации.
3. Какой ключ реально побеждает после ResolveMeshAsset и SetSlotPresets; сопоставить это с явным whole-object контрактом registry.
4. Кто ответственен за enabled при respawn; проверить все перечисленные callers и реальный live hidden runtime, а не только JSON или фабрику.
5. Нет ли непросмотренного reset/readback path, который инвалидирует adopted state при смене readback режима; проверить также Poll после выключения и буферы в полёте.

Возможные исправления выше являются предложениями и не были реализованы или протестированы. Не нужно расширять их до большого рефакторинга без необходимости.

Соблюдать AGENTS.md: CRLF у C++/Windows project files; диагностические события писать в session log, не создавать новые `logs/<name>.log`; при GPU-воспроизведении учитывать безопасную работу с уровнем и не оставлять запущенные вспомогательные процессы без наблюдения. Для проверки этих пяти находок локальная LLM не нужна.

## Приложение A. Точный исходник CPU-reproducer

Ниже вложена копия `x64/BugReview/bug_review.cpp`, использованная в проверке. Первые три блока вызывают реальные команды; четвёртый — только фабрику StaticMesh.

```cpp
#include <cstdio>
#include <memory>
#include "app/levels/LevelManager.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneObjectFactory.h"
#include "editor/EditorContext.h"
#include "editor/commands/SetEnabledCommand.h"
#include "editor/commands/SetMaterialCommand.h"
#include "editor/scene/EditorSceneDocument.h"
#include "rendering/core/Renderer.h"
#include "rendering/renderables/GBufferRenderable.h"

using Json = nlohmann::json;

int main()
{
    auto renderer = std::make_unique<Renderer>();
    auto scene = std::make_unique<Scene>();
    LevelManager levels;
    EditorSelection selection;
    int confirmed = 0;
    {
        EditorSceneDocument document;
        document.Add(EditorSceneDocument::ObjectFromJson({1},
            Json{{"type", "staticMesh"}, {"enabled", true}, {"mesh", "models/box.mesh.json"}}));
        EditorContext ctx{*renderer, *scene, levels, document, selection};
        SetEnabledCommand command({1}, true);
        command.Execute(ctx);
        command.Undo(ctx);
        const bool value = document.Find({1})->enabled;
        std::printf("enable already-visible then undo: expected=true actual=%s\n", value ? "true" : "false");
        confirmed += !value;
    }
    {
        EditorSceneDocument document;
        document.Add(EditorSceneDocument::ObjectFromJson({1},
            Json{{"type", "staticMesh"}, {"mesh", "models/box.mesh.json"}}));
        EditorContext ctx{*renderer, *scene, levels, document, selection};
        const Json original = EditorSceneDocument::ObjectToJson(*document.Find({1}));
        const Json before = SceneObjectFactory::ResolveMeshAsset(original);
        SetMaterialCommand command({1}, "tent_0");
        command.Execute(ctx);
        command.Undo(ctx);
        const Json after = SceneObjectFactory::ResolveMeshAsset(
            EditorSceneDocument::ObjectToJson(*document.Find({1})));
        const std::string initial = before.value("material", std::string{});
        const std::string final = after.value("material", std::string{});
        std::printf("assign material then undo inherited box: expected='%s' actual='%s'\n",
            initial.c_str(), final.c_str());
        confirmed += initial != final;
    }
    {
        EditorSceneDocument document;
        document.Add(EditorSceneDocument::ObjectFromJson({1},
            Json{{"type", "staticMesh"}, {"mesh", "models/tent.mesh.json"}}));
        EditorContext ctx{*renderer, *scene, levels, document, selection};
        SetMaterialCommand command({1}, "bronze");
        command.Execute(ctx);
        auto runtime = SceneObjectFactory::CreateStaticMeshFromJson(
            EditorSceneDocument::ObjectToJson(*document.Find({1})));
        auto gb = runtime->AsGBufferRenderable();
        std::printf("assign whole tent material: expected='bronze' x3 actual='%s','%s','%s'\n",
            gb->SlotPreset(0).c_str(), gb->SlotPreset(1).c_str(), gb->SlotPreset(2).c_str());
        confirmed += gb->SlotPreset(0) != "bronze";
    }
    {
        const Json object{{"type", "staticMesh"}, {"mesh", "models/box.mesh.json"}, {"enabled", false}};
        auto runtime = SceneObjectFactory::CreateStaticMeshFromJson(object);
        std::printf("factory used by material/mesh respawn on disabled object: expected=false actual=%s\n",
            runtime->IsVisible() ? "true" : "false");
        confirmed += runtime->IsVisible();
    }
    std::printf("Confirmed %d/4 review cases\n", confirmed);
    return confirmed == 4 ? 0 : 1;
}
```

## Приложение B. Точный проект reproducer

Ниже — локальный .vcxproj, производный от intent_regression.vcxproj. Комментарии о прежней regression suite унаследованы от шаблона; фактическое содержимое собираемого файла приведено в приложении A.

```xml
<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <!-- Build test_cube.vcxproj Debug|x64 first. This CPU-only executable reuses its
       objects so the tests exercise the real action registry, resolver and undo stack.
       It is deliberately NOT a dependency of the engine/solution. -->
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals"><WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion></PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Label="Configuration">
    <ConfigurationType>Application</ConfigurationType><PlatformToolset>v143</PlatformToolset>
    <UseDebugLibraries>true</UseDebugLibraries>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.props" />
  <PropertyGroup>
    <Repo>D:\Programming\test_cube\</Repo>
    <OutDir>$(Repo)x64\Debug\</OutDir>
    <IntDir>$(Repo)x64\BugReview\obj\</IntDir>
    <TargetName>bug_review</TargetName>
  </PropertyGroup>
  <ItemDefinitionGroup>
    <ClCompile>
      <!-- Same reason as the engine's: the test phrases here are Russian, and without this
           the compiler encodes them to the system codepage, which has no Cyrillic. It only
           worked before because this file has no BOM and the compiler was reading its UTF-8
           bytes as codepage 1252 and writing them straight back. -->
      <AdditionalOptions>/utf-8 %(AdditionalOptions)</AdditionalOptions>
      <LanguageStandard>stdcpp20</LanguageStandard><Optimization>Disabled</Optimization>
      <RuntimeLibrary>MultiThreadedDebugDLL</RuntimeLibrary><ExceptionHandling>Sync</ExceptionHandling>
      <PreprocessorDefinitions>_DEBUG;WITH_EDITOR=1;NOMINMAX;WIN32_LEAN_AND_MEAN;%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <AdditionalIncludeDirectories>$(Repo);$(Repo)sources;$(Repo)third_party;$(Repo)third_party\imgui;$(Repo)third_party\imgui\backends;$(Repo)third_party\tbb\include;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
    </ClCompile>
    <Link>
      <SubSystem>Console</SubSystem><EntryPointSymbol>mainCRTStartup</EntryPointSymbol>
      <AdditionalLibraryDirectories>$(OutDir);$(Repo)third_party\tbb\lib;$(Repo)third_party\mimalloc\lib\Debug;$(Repo)third_party\streamline\lib\x64;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>
      <AdditionalDependencies>third_party_ui.lib;third_party_tasks.lib;third_party_assets.lib;mimalloc.lib;tbb12_debug.lib;sl.interposer.lib;gdi32.lib;%(AdditionalDependencies)</AdditionalDependencies>
    </Link>
  </ItemDefinitionGroup>
  <ItemGroup>
    <ClCompile Include="bug_review.cpp" />
  </ItemGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />
  <Target Name="UseEngineLinkInputs" BeforeTargets="ComputeLinkInputsFromProject">
    <!-- The linker manifest excludes stale objects from past builds (notably old
         ImGui objects that now live in third_party_ui.lib). Never glob *.obj. -->
    <ReadLinesFromFile File="$(Repo)test_cube\x64\Debug\test_cube.tlog\link.command.1.tlog">
      <Output TaskParameter="Lines" ItemName="EngineLinkLog" />
    </ReadLinesFromFile>
    <ItemGroup>
      <Object Include="$([System.String]::Copy('%(EngineLinkLog.Identity)').Substring(1).Split('|'))"
              Condition="$([System.String]::Copy('%(EngineLinkLog.Identity)').StartsWith('^'))" />
    </ItemGroup>
  </Target>
</Project>
```

## 11. Перепроверка и исправления (Claude Opus 5.5, 1 октября 2026) — на ревью

Изменения в рабочем дереве, не закоммичены. База — `dcb4eaa`. Посмотреть: `git diff` (25 файлов).

### Вердикты по разделу 10

Все пять находок **подтверждены**. Опровергнутых нет. Дополнения к отчёту:

- **BUG-01 достижим обычным кликом.** Глаз в Outliner на объекте внутри выделения вызывает `SetSelectionEnabled`, то есть `setEnabled` на ВСЁ выделение ([EditorController.cpp:3325](/D:/Programming/test_cube/sources/editor/EditorController.cpp:3325)). Есть и составной случай. Если в выделении есть свет, уже стоящий в нужном состоянии, то no-op `EditEnvironmentCommand::Execute` возвращает false. `CompositeCommand` откатывает уже выполненные команды, и инвертированный Undo при откате ПРЯЧЕТ видимый объект. В итоге «показать» скрывает объект и не оставляет записи в истории.
- **BUG-04: восемь путей respawn, а не четыре.** Пропущенные в отчёте: `RespawnPlacedMeshes` (EditorController), live-apply в Material Editor, сохранение в Mesh Editor, `InspectorMultiEdit`. В редакторной сборке отключённый объект загружается В runtime скрытым (`JsonLevel.cpp`, `AddLoadedObjects` → `SetVisible(enabled)`). Поэтому комментарии «disabled → no runtime» в четырёх местах были неверны. Сохранение материала в Material Editor показывало все скрытые копии.
- **Побочное.** `FoldIntoOneEntry(std::move(commands), "..." + std::to_string(commands.size()))` встречается в 9 местах. Параметр принимался по значению, а порядок инициализации параметров не задан стандартом. MSVC случайно давал правильную подпись, на другом компиляторе было бы «0 Objects».

### Что изменено

| Баг | Изменение |
|---|---|
| BUG-01 | `SetEnabledCommand` захватывает исходный `enabled` при первом Execute (из документа, иначе из runtime) и в Undo возвращает его. `BuildSetEnabled` пропускает цели, уже стоящие в нужном состоянии, как это делают Inspector и isolate. Если менять нечего — `Already shown`/`Already hidden`, nullptr, история не пишется. |
| BUG-02 | `SetMaterialCommand` хранит наличие и значение ключей `material` и `materials`. Undo возвращает или удаляет каждый ключ. Явная пустая строка остаётся значением. |
| BUG-03 | «Весь объект» теперь значит каждый слот. Если у effective-объекта есть список `materials` (свой или из asset) или у живого runtime больше одного слота, пишется `materials` = [M]×N, где N = max(длина effective-списка, `SlotCount()` runtime). Scalar `material` тоже пишется. `SetMaterialSlotCommand` не тронут. Семантику выбрал Claude по делегированию владельца. |
| BUG-04 | `Scene::AddInitializedEditorObject(..., bool visible)` — параметр ОБЯЗАТЕЛЬНЫЙ, все 13 вызовов передают видимость. Отдельные `SetVisible` в Duplicate/Delete/Paste удалены, так как теперь это делает Scene. Комментарии «disabled → no runtime» исправлены. |
| BUG-05 | `OceanReadback::Poll(renderer, ocean)`: при None вызывается `DropSurface()`. Он обнуляет `adoptedFrame_` и shore (`hasShore_`, `shoreQueued_`). Копии в полёте помечаются `discard`: слот освобождается по своему фенсу, содержимое не принимается. `BuildPass` и `EnsureBuffer` сбрасывают `discard` у слота, который занимают. `OceanBuoyancy::Step` при `!HasSurface()` обнуляет `waterFrame`, чтобы скорость вернувшейся поверхности стартовала с нуля. Debug-строка плавучести при плоской воде пишет `0 cascade(s)`. |
| тест-ключ | `--set`/`--sweep` `ocean.readbackCascades` (0/1/2) через тот же `OceanSimulation::SetSettings`, что и комбо в Ocean controls. |
| побочное | `FoldIntoOneEntry` принимает `std::vector<...>&&`: перемещение происходит в теле, после вычисления всех аргументов. |

Четыре C++-файла (`EditorController.cpp`, `EditorActionRegistry.cpp`, `InspectorMultiEdit.cpp`, `tools/intent_regression.cpp`) были в рабочем дереве в LF. По AGENTS.md и `.gitattributes` (`*.cpp eol=crlf`) они нормализованы в CRLF. В `git diff` это не видно.

### Проверки

| Проверка | Результат |
|---|---|
| Debug, Release, Release_Editor | все три собраны, exit 0 (Debug — `/WX`) |
| `intent_regression` | PASS. Новые: `TestShowingWhatIsShownUndoesToShown` (команда, Redo, Outliner-выделение «видимый + солнце + скрытый», «Already shown») и `TestWholeObjectMaterial` (box: наследование bronze; явная `""`; tent: все три слота через `CreateStaticMeshFromJson`, Undo без обоих ключей; свой список: точный возврат, Redo) |
| контроль для теста | Undo временно возвращён в `!enabled_`: тест падает на `show on a shown object, then undo: still shown`. С фиксом снова PASS |
| `inspector_regression` | PASS |
| `tools/check_logging.py` | 0 findings |
| BUG-05 вживую | Release, `wind_test`, `--sweep=ocean.readbackCascades:0,1,0 --shot-delay=15 --shot-interval=12 --log-level=debug`, один запуск. Холодный None: heave 0.018 м, tilt z −1.50°. One: `surface adopted`, качка до ±3.9°. Снова None: `surface dropped`, лодка возвращается к heave 0.018 / tilt z −1.50, то есть к эталону плоской воды. Clean shutdown. Лог: `logs/session_20261001_035517_17240_release.log` |
| CRLF | все 25 изменённых файлов: 0 lone LF / 0 lone CR (подсчёт байтов) |

### Не проверено

- BUG-04 на GPU. CPU-тест невозможен: `UploadBatch::Begin` без устройства не открывается. Гарантия здесь — сигнатура: путь, который забудет видимость, не скомпилируется.
- Внешний вид материалов на GPU (палатка, пальмы, лодка после назначения целиком).
- Debug GPU validation и scene-stress не запускались. Правка readback не трогает пассы, барьеры и ресурсы, только CPU-учёт слотов.
- Объект без живого runtime с glTF из нескольких submesh и без списка: число слотов неизвестно, поэтому пишется только scalar (слот 0), как раньше.

### На что смотреть ревьюеру

1. `SetMaterialCommand::Execute`: правильно ли выбирается N и не ломает ли запись `materials` чего-то у одно-слотовых мешей. Для box ключ не пишется; это проверено тестом.
2. Изменение поведения: `setEnabled`, которому нечего менять, теперь возвращает «отказ» с `Already shown/hidden`. MCP `run_action` вернёт его как ошибку — так же, как уже делает `isolate` (`Already isolated`).
3. `OceanReadback`: `DropSurface` вызывается каждый `Poll` в режиме None. Переход логируется один раз, потому что `adoptedFrame_` уже 0. Проверить, что нет пути, где помеченный `discard` слот будет переиспользован до своего фенса.
4. Нет ли ещё вызовов, создающих runtime редакторного объекта в обход `AddInitializedEditorObject` (кроме загрузки уровня, которая уже ставит `SetVisible`).
