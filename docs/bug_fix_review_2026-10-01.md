# Ревью исправлений Opus в test_cube

Дата: 1 октября 2026 года, Europe/Berlin.

Корень проекта: `D:\Programming\test_cube`.

База сравнения: `dcb4eaa87fc3c5f85318cd95c4590f15e02b3468`. Проверены незакоммиченные изменения рабочего дерева: 25 tracked-файлов, 384 добавленные и 52 удалённые строки. Исходный отчёт: [bug_review_2026-10-01.md](/D:/Programming/test_cube/docs/bug_review_2026-10-01.md).

## 1. Вердикт

**Подтверждённых замечаний к исправлениям не найдено.** По анализу кода причины всех пяти исходных ошибок устранены. CPU-регрессии прошли; переход ocean readback `One → None → One` независимо проверен в работающем renderer.

Это вывод о проверенном diff, а не гарантия отсутствия других багов в проекте. Для BUG-04 полный сценарий пересоздания скрытого объекта в редакторе с GPU не выполнен: проверены центральный helper и все его вызовы. Остальные ограничения перечислены ниже.

Независимый GPU-прогон проверил переключение readback, но **не завершился штатно** после последнего снимка. Главный поток находился внутри завершения NVIDIA NGX/Streamline, а не внутри OceanReadback. Оснований приписать задержку исправлениям Opus не установлено; также не установлено, что она существовала на базовом commit. Этот прогон нельзя записывать как полностью успешный тест жизненного цикла приложения.

## 2. Проверка исходных пяти ошибок

| ID | Что изменено | Результат ревью | Независимое подтверждение |
|---|---|---|---|
| BUG-01 | SetEnabledCommand сохраняет исходную видимость один раз; registry пропускает уже достигнутые состояния | Undo возвращает исходное значение; Redo не перезаписывает точку возврата; no-op environment edit больше не откатывает смешанную выборку | Новые тесты в intent_regression, общий regression PASS |
| BUG-02 | Сохраняются присутствие и точное JSON-значение ключей material и materials | Undo удаляет прежде отсутствовавший ключ; наследование asset defaults восстанавливается; явное пустое значение остаётся явным | Тесты отсутствующего и пустого material, ResolveMeshAsset после Undo |
| BUG-03 | Назначение всему объекту формирует materials для всех известных слотов | Список из asset/runtime больше не перекрывает назначенный scalar material; Undo возвращает прежние overrides точно | Три слота палатки, наследуемый и собственный список, Undo/Redo |
| BUG-04 | AddInitializedEditorObject требует явный visible; respawn-вызовы передают document.enabled | Видимость задаётся до Init и публикации объекта в scene; пропущенных вызовов helper не найдено | Анализ всех вызовов; отдельного GPU-теста скрытого respawn нет |
| BUG-05 | Poll сбрасывает поверхность при None; незавершённые копии помечаются discard; buoyancy сбрасывает историю waterFrame | None использует текущий waterLevel; старые копии не принимаются после сброса и не переиспользуются до завершения fence | Независимый GPU-переход One → None → One; дополнительно изучен прогон Opus None → One → None |

### BUG-01: видимость и история команд

В [SetEnabledCommand.cpp:37](/D:/Programming/test_cube/sources/editor/commands/SetEnabledCommand.cpp:37) исходное значение захватывается только при первом Execute. Приоритет отдаётся документу; для runtime без документа предусмотрен fallback к IsVisible. Undo применяет сохранённое oldEnabled_, а не противоположное целевому значение. Повторное выполнение после Undo не меняет сохранённое состояние.

В [EditorActionRegistry.cpp:370](/D:/Programming/test_cube/sources/editor/intent/EditorActionRegistry.cpp:370) builder не включает в composite команды, которые ничего не меняют. Это существенно для environment entities: EditEnvironmentCommand отклоняет no-op, и раньше такой отказ мог отменить всю смешанную операцию. Теперь уже включённое солнце не мешает показать скрытый mesh. Полный no-op возвращает «Already shown»/«Already hidden» без новой записи истории.

Дополнительно проверено изменение [FoldIntoOneEntry:153](/D:/Programming/test_cube/sources/editor/intent/EditorActionRegistry.cpp:153): вектор принимается через rvalue reference. Привязка ссылки сама по себе не перемещает вектор, поэтому вычисление commands.size() в другом аргументе не зависит от порядка вычисления аргументов. В просмотренных вызовах аргументы не меняют вектор; перемещение происходит уже внутри helper.

Тест [TestShowingWhatIsShownUndoesToShown:2874](/D:/Programming/test_cube/tools/intent_regression.cpp:2874) проверяет показ показанного, скрытие скрытого, реальное изменение с Undo/Redo, смешанную выборку с environment light и отсутствие записи истории для полного no-op.

### BUG-02 и BUG-03: материал и наследование

В [SetMaterialCommand.cpp:24](/D:/Programming/test_cube/sources/editor/commands/SetMaterialCommand.cpp:24) Apply отдельно восстанавливает оба ключа. Структура Key различает отсутствие свойства и присутствующее значение, включая пустую строку. Снимок исходных свойств берётся один раз и не пересоздаётся на Redo.

В [SetMaterialCommand.cpp:91](/D:/Programming/test_cube/sources/editor/commands/SetMaterialCommand.cpp:91) число слотов берётся из разрешённого materials и уточняется по live GBufferRenderable::SlotCount. Это покрывает многослотовый live mesh без явно заданного списка. При наличии списка или нескольких слотов формируется массив выбранного материала; для обычного одного слота без списка достаточно scalar material.

Проверено сохранение исходного собственного массива materials, отсутствие закрепления унаследованного массива после Undo и восстановление отсутствующего material. Для скрытого live mesh пересоздание также передаёт obj->enabled центральному helper.

Тест [TestWholeObjectMaterial:2929](/D:/Programming/test_cube/tools/intent_regression.cpp:2929) проверяет box с наследуемым bronze, box с явным пустым material, трёхслотовую палатку с наследуемыми материалами, палатку с собственным списком, Undo и Redo. Фабрика проверяется до GPU Init; визуальная корректность textures/shading этим CPU-тестом не доказывается.

### BUG-04: видимость пересозданного runtime

В [Scene.cpp:1268](/D:/Programming/test_cube/sources/app/scene/Scene.cpp:1268) visible — обязательный параметр AddInitializedEditorObject, без default. SetVisible вызывается до Init, назначения editor id и SyncSceneState. Просмотренный Init и SyncSceneState не сбрасывают эту видимость.

Все обнаруженные вызовы обновлены: Delete Undo, Duplicate, Paste, SpawnMesh, SetMeshAsset, SetMaterial, SetMaterialSlot, SetParticlePreset, InspectorMultiEdit, editor respawn при обновлении asset, MaterialEditorPanel и MeshEditorPanel. Они передают enabled соответствующего document object. Drag ghost в ViewportGizmo и объект scene stress намеренно передают true.

Такой обязательный параметр предотвращает повторное забывание видимости новым вызывающим кодом. При анализе diff не найдено пути через этот helper, который пересоздаёт скрытый документный объект с true.

### BUG-05: сброс readback и возврат волн

В [OceanReadback.cpp:262](/D:/Programming/test_cube/sources/ocean/OceanReadback.cpp:262) DropSurface обнуляет adoptedFrame и latency, сбрасывает состояние shore map и помечает pending slots как discard. Pending slot остаётся занятым: его frame не сбрасывается до подтверждения fence в Poll. Это сохраняет запрет на перезапись ресурса, в который GPU ещё пишет.

В [OceanReadback.cpp:281](/D:/Programming/test_cube/sources/ocean/OceanReadback.cpp:281) Poll проверяет текущий режим до принятия готовых копий. Отброшенные копии освобождаются без чтения; новая поверхность выбирается только среди допустимых готовых slots. Общий CascadesWanted используется и Poll, и BuildPass, поэтому условия отсутствия копируемых каскадов согласованы.

В [OceanBuoyancy.cpp:204](/D:/Programming/test_cube/sources/ocean/OceanBuoyancy.cpp:204) отсутствие поверхности сбрасывает waterFrame. Высоты в этом состоянии берутся из текущего ocean.GetWaterLevel. При возвращении readback первая новая копия начинает с нулевой скорости воды, без разности относительно старой волны до паузы.

## 3. Выполненные проверки

| Проверка | Результат | Что она доказывает |
|---|---|---|
| x64/Debug/intent_regression.exe | PASS, exit 0 | Intent/preview/команды/Undo и новые случаи видимости и материалов |
| x64/Debug/inspector_regression.exe | PASS, exit 0 | Существующие регрессии inspector edits, Undo/Redo и ImGui Draw paths |
| python tools/check_logging.py | 0 finding(s), exit 0 | Нет обнаруженных нарушений проверяемых правил логирования |
| git diff --check | exit 0 | Нет обнаруженных whitespace errors в tracked diff |
| Проверка line endings 25 изменённых файлов | Нарушений нет, CRLF | Нет смешанных LF/CRLF в проверенном diff |
| Сопоставление изменённых engine .cpp с Debug .obj | Нет отсутствующих/более старых соответствующих .obj | Для просмотренных реализаций отсутствует очевидная устарелость по timestamp |
| Release renderer, readback 1,0,1 | Переход подтверждён журналом; штатное завершение не пройдено | Readback сбрасывается и снова принимает новые волны; shutdown требует отдельной проверки |

Использованные regression EXE построены после изменений Opus: intent_regression — 01.10.2026 03:56:50, inspector_regression — 03:54:07. Время изменённого tools/intent_regression.cpp — 03:53:45. Это устраняет прежнее ограничение исходного отчёта со старыми regression binaries. Независимая чистая пересборка всех конфигураций в рамках этого ревью не выполнялась; timestamp-проверка не заменяет её.

Новые проверки видимости и материалов входят в обычный запуск intent_regression, отдельного ключа для них не требуется. Старый x64/BugReview/bug_review.exe из первоначального поиска не использован как acceptance test: его программа ожидает наличие старых ошибок, а бинарник может содержать прежние реализации и ABI.

### Независимый GPU-прогон

Команда из корня репозитория:

```powershell
& .\x64\Release\test_cube.exe `
  --level=data/levels/wind_test.json `
  --window=1280x720 `
  --shot=x64/BugReview/fix_review_readback.png `
  --sweep=ocean.readbackCascades:1,0,1 `
  --shot-delay=5 --shot-interval=3 --log-level=debug
```

В этом harness shot-delay и shot-interval измеряются в секундах, согласно App.h/main.cpp. Wind freeze не применялся: проверялось возвращение анимированной поверхности, а не pixel diff.

Журнал: [session_20261001_040807_34764_release.log](/D:/Programming/test_cube/logs/session_20261001_040807_34764_release.log). Session logs ротируются, поэтому файл не является постоянным приложением к отчёту. Существенные наблюдения сохранены здесь:

| Время / frame | Наблюдение |
|---|---|
| 04:08:12.119 / 0 | Sweep применил One; после прогрева buoyancy использует 1 cascade |
| 04:08:15.662 / 1200 | 5 floating, 1 cascade, heave 0.117 m, velocity -0.132 |
| 04:08:17.222 / 1684 | Sweep переключил None |
| 04:08:17.222 / 1685 | Surface dropped; переход выполнен на следующем tick |
| 04:08:19.669 / 2468 | 5 floating, 0 cascades, heave 0.015 m, velocity -0.011 — затухание к flat equilibrium |
| 04:08:20.310 / 2673 | Sweep вернул One |
| 04:08:20.342 / 2676 | Surface adopted: 1 cascade, 3 iterations, 2 frames old |
| 04:08:21.669 / 3088 | 5 floating, 1 cascade, heave 0.217 m, velocity -0.096 — плавучесть снова использует волны |
| 04:08:23.414 / 3647 | Последний, третий снимок успешно записан; новых записей после него не было |

Стартовая копия до стабилизации One могла нести два каскада от начального preset; None сбрасывает её вместе с остальным прежним состоянием. После возврата One журнал явно подтверждает новую принятую поверхность с одним каскадом.

Снимки находятся в игнорируемом каталоге x64/BugReview: fix_review_readback_00.png, _01.png, _02.png. Они не сравнивались попиксельно; доказательство режима readback и движения buoyancy — журнал и анализ кода.

Дополнительно просмотрен уже существовавший прогон Opus: [session_20261001_035517_17240_release.log](/D:/Programming/test_cube/logs/session_20261001_035517_17240_release.log), переход 0,1,0. Он содержит принятие поверхности на frame 4003, её сброс на frame 7240 и session end: clean shutdown на frame 10473. Это дополнительное свидетельство, а не наш независимый запуск.

### Ограничение завершения GPU-прогона

После последнего снимка собственный процесс PID 34764 оставался жив несколько минут. Снят стек главного потока TID 64720 через GetThreadContext/StackWalk64; поток после чтения немедленно возобновлён. Существенная цепочка:

```text
NtWaitForSingleObject
WaitForSingleObjectEx
[NVIDIA telemetry internals]
UninitializeTelemetry
[NGX internals]
NVSDK_NGX_D3D12_Shutdown
[Streamline plugin internals]
slShutdown
Renderer::Shutdown
App::Run
WinMain
```

Другой активный поток находился в цепочке NvTelemetrySendEvent → WaitNamedPipeW/NtOpenFile. Это локализует задержку в завершении NVIDIA telemetry/NGX/Streamline. Код Renderer::Shutdown, TaskSystem и SDK shutdown в diff Opus не менялся. Однако сравнения того же запуска на базовом commit нет, поэтому причинность окончательно не установлена.

Только собственный процесс остановлен принудительно; перед остановкой проверены executable path и StartTime, после остановки проверено отсутствие PID. В журнале нет footer штатного завершения. Exit 0 PowerShell-обёртки после Stop-Process не используется как доказательство успешного завершения renderer.

## 4. Границы покрытия

Следующие случаи не получили отдельного runtime-подтверждения и не объявляются найденными багами:

- Скрытый mesh/particle emitter: полный GPU respawn через каждую команду, Undo и Redo. Покрытие BUG-04 основано на обязательном параметре helper и проверке всех call sites.
- Многослотовый glTF без materials в документе и manifest: подсчёт через live SlotCount просмотрен в коде, но отдельный GPU asset fixture не запускался.
- None → One с копиями, которые намеренно остаются в полёте при большом GPU lag; защита discard/fence проверена по реализации. Обычный sweep не гарантирует искусственное создание такого lag.
- Переключение readback во время отсутствия видимых плавучих объектов либо скрытого океана. Tick рано возвращается, когда ничего не плавает; отдельного теста такой паузы не было.
- GPU-based validation, scene stress и визуальная проверка всех editor panels после изменения API не запускались.
- Завершение собственного GPU-прогона не подтверждено; обнаруженная задержка SDK описана отдельно, а не скрыта за общей отметкой PASS.

Исходники движка и новые тесты Opus при ревью не редактировались. Создан только этот отчёт; первоначальный отчёт сохранён. Commit, запуск llama-server и сохранение уровней из редактора не выполнялись.
