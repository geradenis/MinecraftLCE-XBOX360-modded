# Журнал изменений

Репозиторий: `repo/MinecraftLCE-7thGen-main`
Рабочая копия: H:\Projects\MinecraftLCE-7thGen\M2-working-copy
Сборка: Debug | x64, MSVC v143 (VS2022 17.14, MSVC 14.44.35207), Windows SDK 10.0.26100.0.

## M0 — сборка PC x64

### Правки совместимости с современным MSVC

Все правки — минимальные, поведение игры не меняется.

1. `Minecraft.World/Language.h`, `Minecraft.World/Language.cpp`
   `getElement(const wstring& elementId, ...)` → `getElement(wstring elementId, ...)`.
   Причина: современный MSVC (static_assert в `vadefs.h`) запрещает `va_start` от
   параметра-ссылки. Аргументы в этой функции всё равно не используются.

2. `Minecraft.World/I18n.h`, `Minecraft.World/I18n.cpp`
   `get(const wstring& id, ...)` → `get(wstring id, ...)`.
   Причина та же. Функция просто пробрасывает управление в `Language::getElement`,
   который игнорирует varargs.

3. `Minecraft.World/Hasher.cpp` (строка 22)
   `hash_value(s)` → `hash<wstring>()(s)`.
   Причина: `std::hash_value` — нестандартное расширение старых STL (VC10/XDK),
   в STL VS2022 отсутствует. Результат и раньше не был MD5 (см. TODO 4J Stu),
   это тот же хэш, только другим способом вызова.

4. `Minecraft.World/Player.cpp` (строка 2564, ветка не-PS3)
   `std::hash_value(k->name)` → `std::hash<std::wstring>()(k->name)`.
   Причина та же. Хэш используется только как ключ в runtime-контейнере игроков,
   в сохранения не попадает.

### Результат

- `Minecraft.World` → `Minecraft.World/bin/x64/Debug/Minecraft.World.lib` собирается.

### Линк с предсобранными библиотеками 4J (PC testbed)

Компиляция клиента проходит полностью, но линк упирается в предсобранные
`4J_Input_d.lib`, `4J_Render_PC_d.lib`, `4J_Storage_d.lib`, `4J_Profile_d.lib`,
которые лежат в репозитории только в бинарном виде и собраны MSVC v110 (VS2012).

Их директивы (проверено `dumpbin /directives`):

```
/FAILIFMISMATCH:_ITERATOR_DEBUG_LEVEL=2
/FAILIFMISMATCH:_MSC_VER=1700
/FAILIFMISMATCH:RuntimeLibrary=MTd_StaticDebug
```

Наши объекты в Debug|x64 имеют те же `_ITERATOR_DEBUG_LEVEL=2` и `MTd_StaticDebug`,
отличается только `_MSC_VER` (VS2022 ставит 1900 — общий ABI-идентификатор VS2015+),
из-за чего линкер выдаёт LNK2038.

Обход для PC-стенда (console-конфиги не затрагиваются):

- `compat/MscVerCompat.h` — `#pragma detect_mismatch("_MSC_VER", "1700")`;
- в `ClCompile` для `Debug|x64` обоих проектов добавлено
  `/D_ALLOW_MSC_VER_MISMATCH /FI"$(SolutionDir)compat\MscVerCompat.h"`.
  Первый флаг убирает автоматическую запись `_MSC_VER=1900`, второй добавляет 1700.

Важно: это именно обход проверки, а не настоящая совместимость. Layout `std::string`/
`std::wstring` в VS2012 и VS2015+ различается, поэтому функции 4J, принимающие или
возвращающие STL-строки (`C_4JProfile::GetDisplayName`, `C4JStorage::GetMountedPath`
и т.п.), при вызове могут падать. Если линк пройдёт, эти места придётся либо
переписать на безопасные обёртки, либо заменить вызовы.

Альтернатива — собрать всё ровно тем тулсетом, что задумано в README: VS2012 (v110).

## Окружение

- Visual Studio 2022: `H:\Tools\VS2022`
- MSBuild: `H:\Tools\VS2022\MSBuild\Current\Bin\MSBuild.exe`
- Команда сборки:
  `MSBuild.exe MinecraftPC.sln /p:Configuration=Debug /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0.26100.0 /m`
- Переопределение toolset делается флагами командной строки, файлы проектов пока не тронуты.

## M1 — ограниченный кэш выгруженных чанков (PC x64)

### Что сделано

1. `Minecraft.Client/ServerChunkCache.{h,cpp}` — к `m_unloadedCache` добавлен LRU:
   - `m_unloadedLRU` (список `LevelChunk*` + тик выгрузки) и `m_unloadedLRUIndex`
     (хэш-индекс для O(1) удаления при повторной загрузке);
   - при выгрузке чанк попадает в LRU (`onChunkUnloaded`), при загрузке — убирается
     (`onChunkReloaded`);
   - `trimUnloadedCache()` раз в тик удаляет самые старые чанки, если размер превышает
     `MC_MAX_UNLOADED` (по умолчанию 256; `0` — прежнее неограниченное поведение,
     используется как baseline M0);
   - удаление только после «льготных» 200 тиков и только при `getClientShareCount()==0`;
   - исправлено `delete` → `delete[]` для массивов `cache` и `m_unloadedCache`.

2. `Minecraft.World/LevelChunk.{h,cpp}` — учёт совместного использования хранилища:
   - `m_clientShareCount` на серверном чанке и `m_sharedServerChunk` на клиентской копии;
   - инкремент при создании/старте шаринга, декремент при остановке/удалении;
   - `stopSharingTilesAndData(bool force)` — принудительный отказ от шаринга без ожидания
     post-processing (нужен при выгрузке чанка из вида).

3. `Minecraft.Client/MultiPlayerChunkCache.cpp` — `drop()` принудительно расшаривает
   чанк у сервера (host), чтобы LRU мог его удалить.

4. Диагностика: `MC_STATS_FILE` — раз в 100 тиков строка
   `tick=… dim=… max=… loaded=… unloaded=… evicted=… px=… pz=… ws=…MB priv=…MB`.
   Файл открывается/закрывается на каждую запись, чтобы его можно было читать на ходу.

5. Бенчмарк-тур `MinecraftServer::RunChunkTour()` (только Windows64):
   `MC_BENCH_TOUR=<прыжков>`, `MC_BENCH_STEP=<блоков>`, `MC_BENCH_PERIOD=<тиков>` —
   телепортирует host-игрока «змейкой» и вызывает `PlayerList::move()`, заставляя сервер
   грузить/выгружать чанки. Без env-переменной не активен.

6. `UIScene_MainMenu.cpp` — на Windows64 кнопка Exit теперь вызывает `PostQuitMessage(0)`,
   неизвестные контролы логируются вместо `__debugbreak()`.

### Замеры

Один и тот же exe, один и тот же сейв, тур 60 прыжков × 64 блока / 40 тиков:

| режим | MC_MAX_UNLOADED | unloaded (Overworld) | evicted (сумма) | WS, МБ | Private, МБ |
|---|---|---|---|---|---|
| M0 (baseline) | 0 (без лимита) | 3372 | 0 | 513.1 | 655.5 |
| M1 | 256 | 256 | ~2986 | 432.6 | 570.1 |

Вывод: LRU ограничивает кэш выгруженных чанков и экономит ~80 МБ WS / ~85 МБ private
на этом прогоне; вытесненные чанки корректно перезагружаются с диска (мир продолжал
работать, в т.ч. после возврата из Нижнего мира).

Примечание: в M1-прогоне игрок по пути попал в портал Нижнего мира и вернулся обратно;
на итоговые цифры это не влияет (оба замера включают все три измерения), но для чистых
замеров в будущем тур стоит ограничить Обычным миром.

### Дальше

M2 — заменить плоские массивы `LevelChunk*[XZSIZE*XZSIZE]` на хэш-карту по координатам,
убрать зависимость от `XZOFFSET`/`XZSIZE` и перейти к бесконечному миру (стриминг чанков
вокруг игрока без фиксированной границы).

## M2 — бесконечные измерения, стабильный FPS и полноэкранный режим

### Бесконечные Overworld / Nether / End

1. `Minecraft.World/Dimension.cpp`, `HellDimension.cpp`
   - `Level::MAX_LEVEL_SIZE = 30000000` (полуразмер мира в блоках);
   - `Dimension::getXZSize()` теперь возвращает `(MAX_LEVEL_SIZE * 2) / 16 = 3 750 000`
     чанков вместо прежнего фиксированного квадрата;
   - проверки границ в `Level::getCubes()`, `ServerLevel`, `ServerPlayer`,
     `PortalForcer` и генераторах структур переведены на новый предел.

2. `Minecraft.Client/ServerChunkCache.{h,cpp}`,
   `Minecraft.Client/MultiPlayerChunkCache.{h,cpp}`
   - плоские массивы `LevelChunk*[XZSIZE*XZSIZE]` заменены на `unordered_map`;
   - ключ — 64-битный `(x + 0x7fffffffLL) | ((z + 0x7fffffffLL) << 32)`,
     хэш/сравнение — `LongKeyHash` / `LongKeyEq`;
   - `XZSIZE`/`XZOFFSET` больше не ограничивают хранение и стриминг чанков;
   - LRU выгруженных чанков из M1 сохранён.

3. `Minecraft.World/HellDimension.cpp`
   - убрана стена на границе Нижнего мира;
   - генерация крепостей Нижнего мира переведена на PC-подобное размещение
     (`NetherBridgeFeature`), а не только на одну заранее заданную крепость.

4. `Minecraft.World/TheEndLevelRandomLevelSource.cpp`
   - добавлены внешние острова Энда за пределами центрального острова;
   - дальние границы Энда больше не обрезают генерацию.

5. `Minecraft.Client/ServerChunkCache.cpp`
   - `create()` больше не держит `m_csLoadCreate` во время `load()`,
     `source->getChunk()` и `chunk->load()`; генерация сериализуется отдельным
     `m_csCreate`, а `m_csLoadCreate` защищает только карты и списки;
   - `save()` и `saveAllEntities()` копируют список чанков под lock, а дисковый I/O
     выполняют вне lock, чтобы не блокировать генерацию и клиент.

6. `Minecraft.Client/ServerLevel.cpp`
   - для `_LARGE_WORLDS` `saveInterval = 20 * 60` (раз в 60 секунд) вместо
     агрессивного сейва каждые 2–3 тика. Это снижает фоновую нагрузку, но главной
     причиной провалов FPS был не сейв (см. ниже).

7. `Minecraft.World/LevelData.cpp`
   - `LevelData::getXZSize()` оставлен равным 320: это ограничение протокола
     (`RespawnPacket` передаёт размер через `short`), а не граница фактического
     хранения чанков. Часть проверок структур (деревни, крепости) всё ещё
     использует это значение.

### Причина провалов FPS и исправление

После перехода на бесконечные измерения `Dimension::getXZSize()` стало равно
3 750 000. В `Minecraft.Client/MultiPlayerLevel.cpp` остался старый цикл обслуживания
чанков, который каждый тик перебирал до `ls` позиций по всему квадрату мира:

- `startSharingTilesAndData()` — раз в тик;
- `compressLighting()` / `compressBlocks()` / `compressData()` — до `ls` проверок
  `Level::reallyHasChunk()` за тик.

При `ls = 3 750 000` и 20 TPS это до ~75 млн вызовов `reallyHasChunk()` в секунду,
каждый — с входом в critical section и поиском в хэш-карте. Клиент попеременно
показывал ~35 FPS и провал в 0 FPS на секунду; сервер при этом держал 20 TPS.

Исправление:

- в `MultiPlayerLevel::tick()` введено окно `MAINTENANCE_RADIUS = 32` (64×64 чанков),
  центр — чанк локального игрока (`Mth::floor(player->x) >> 4`);
- оба цикла (unshare и compress) проверяют не весь мир, а только это окно;
- за тик выполняется не более 64 проверок вместо 3,75 млн;
- если игрок ещё не создан, окно центрируется на (0,0).

Файл: `Minecraft.Client/MultiPlayerLevel.cpp`.

Результаты замеров (Debug x64, `MC_MAX_UNLOADED=256`, FPS-лог раз в секунду):

| измерение | FPS | нулевые кадры |
|---|---|---|
| Overworld | 112–157 | нет |
| Nether | 82–156 | нет |
| End | 55–93 | нет |

Сервер: 20 TPS, `loaded`/`unloaded`/`evicted` меняются штатно, LRU продолжает
вытеснять старые чанки. Прежний паттерн `0 / 35 / 0 / 35` исчез.

### FPS-счётчик в окне игры

- `Minecraft.Client/Options.h`, `Options.cpp` — `showFps`, по умолчанию включён;
  `MC_SHOW_FPS=0` отключает.
- `Minecraft.Client/Gui.cpp` — строка `N fps, X chunk updates` рисуется жёлтым
  (`0xffff00`) в левом верхнем углу (`iSafezoneXHalf + 2, 8`) только в игре, не в меню.
- `MC_FPS_FILE` — раз в секунду пишет одно целое число (удобно для замеров).

### Полноэкранный режим (текущее состояние исходника)

Minecraft.Client/Windows64/Windows64_Minecraft.cpp:

- F11 и Alt+Enter переключают borderless-fullscreen;
- WndProc не перехватывает SC_MAXIMIZE, SC_RESTORE или WM_GETMINMAXINFO: развертывание и восстановление окна остаются поведением Windows;
- окно снимается с topmost при старте и при выходе из полноэкранного режима;
- перед переключением сохраняются стиль и WINDOWPLACEMENT, при выходе они восстанавливаются.

Замеры 2048×1152 и FPS 151–157 из предыдущей записи относятся к более раннему варианту обработки окна и не считаются проверкой этого исходника. В проверке рабочей копии 2026-10-24 exe открыл окно Minecraft и оставался запущен не менее 15 секунд; визуально проверить F11, Alt+Enter, изменение размера и разворачивание в этой проверке не удалось.

### Ограничения и известные проблемы

- Сборка Xbox 360 невозможна без XDK; проверялся только PC x64 Debug.
- Периодический краш `Minecraft.Client.exe` в `mss64.dll`
  (`0xC0000005`, offset `0x184a1`/`0x1d17c`) остаётся; перезапуск обычно помогает.
  Один из тестов F11 завершился этим крашем после сворачивания окна.
- `LevelData::getXZSize() = 320` — ограничение протокола `RespawnPacket`, а не
  граница мира. Некоторые проверки структур по-прежнему используют 320.
- Обслуживание (unshare/compress) теперь идёт только в окне 64×64 чанков вокруг
  игрока. Далёкие чанки не сжимаются, пока игрок не вернётся; это сознательный
  компромисс ради стабильного FPS, память далёких чанков ограничивает LRU.
- Замеры FPS: `H:\Temp\fps_log4.txt`, `H:\Temp\fps_log5.txt`,
  `H:\Temp\fps_nether.txt`, `H:\Temp\fps_end.txt`;
  статистика: `H:\Temp\m2_stats_fps5.txt`, `H:\Temp\stats_nether.txt`,
  `H:\Temp\stats_end.txt`.


## Проверка рабочей копии — 2026-10-24

- Рабочая копия находится в H:\Projects\MinecraftLCE-7thGen\M2-working-copy; исходный каталог не изменялся.
- PC Debug|x64 собран успешно в Minecraft.Client/bin/x64/Debug/Minecraft.Client.exe.
- Для этого набора MSVC штатный /MP давал C1902, поэтому сборочный helper m2_serial_compile.targets выключает параллельную компиляцию только на время сборки. Явные пути CLToolPath/LinkToolPath/LibToolPath устраняют ошибку линковки LNK1101. Постоянные настройки проектов ради этого не менялись.
- Команда повторной сборки: powershell -ExecutionPolicy Bypass -File .\build_pc_x64_debug.ps1 из корня рабочей копии. Скрипт пишет журнал в build_pc_x64_debug.log и копирует runtime DLL из Minecraft.Client/Windows64/Runtime в выходную папку.
- В Windows64/Runtime сохранены mss64.dll и совместимый iggy_w64.dll; такие же файлы находятся рядом с exe. iggy_w64.dll из Iggy/lib/redist64 отличается от DLL прежней рабочей PC-сборки и на первом запуске приводила к аварийному завершению.
- В рабочую копию выходной папки перенесены три saveData.ms (около 33 МБ суммарно); SHA-256 всех трёх совпадает с исходными файлами. Оригиналы не менялись.
- После установки runtime DLL exe остаётся запущен с окном Minecraft. При запуске проверена только устойчивость старта; мир, генерацию Энда, поведение дракона, F11 и resize визуально не проверял.
- UI-среда node_repl не инициализировалась (failed to write kernel assets), поэтому управление меню в этой проверке недоступно. Диагностические записи атак дракона в исходниках пока оставлены для следующего игрового прогона; H:\Temp\m2_dragon.log не обновлялся и содержит старые данные.
- Xbox 360 сборка не проверялась: в окружении нет XDK.

## Работа с обновлённым проектом — 2026-10-26

- В рабочую копию перенесены 33 исходных файла из обновлённого каталога; SHA-256 проверены. Оригинальный каталог не изменялся.
- Xbox-скрипты `SubmissionAutobuild.cmd` и `ReleaseAutobuild.cmd` исправлены: вместо отсутствующего `Minecraft360.sln` они используют `MinecraftConsoles.sln` и явно выбирают платформу `Xbox 360`.
- В LRU-коде `ServerChunkCache.cpp` `std::prev` заменён на декремент итератора для старого toolset.
- `_LARGE_WORLDS` включён во всех семи Xbox 360 конфигурациях в `Minecraft.Client.vcxproj` и `Minecraft.World.vcxproj`; XML обоих проектов проверен.
- Xbox 360 сборка пока не выполнена: нужен XDK/VS2010 toolset `2010-01`. При включённом макросе сохранение резервирует буфер до 2 GiB, поэтому сборку и работу сохранений нужно проверить до запуска на консоли. Анализ и дальнейшие шаги: `XBOX360_PORT_REVIEW.md`.

