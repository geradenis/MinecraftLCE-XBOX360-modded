# MinecraftLCE-XBOX360-modded

> ## ⚠️ Если сборка не запускается
>
> Текущий `default.xex` — это TCP PoC. Если он не запускается на вашей консоли или в Xenia, используйте предыдущую QNet-сборку `default_qnet_backup_20261108.xex` из того же архива. Она предназначена для локальной игры и System Link.
>
> Релиз: https://github.com/geradenis/MinecraftLCE-XBOX360-modded/releases/tag/xbox-tcp-poc-2026-11-08
>
> **Important:** If the current `default.xex` does not start, use the previous QNet build `default_qnet_backup_20261108.xex` from the same archive. It is intended for local play and System Link.


Модифицированная сборка **Minecraft: Legacy Console Edition** для **Xbox 360** с бесконечным миром (Overworld / Nether / End), ограниченными кэшами чанков и исправлениями сохранения/загрузки.

> ## ⚠️ ВАЖНО — прочитайте перед запуском
>
> - **Проект НЕ тестировался на реальной Xbox 360 (RGH/JTAG).** Всё тестирование выполнялось только в эмуляторе **Xenia Canary**. На реальном железе возможны краши, утечки памяти и другие проблемы.
> - **Проект модифицирован и собран AI-агентом OpenCode** (модель DeepSeek V4.1 Flash) по заданию владельца репозитория. Это не официальная сборка 4J Studios / Mojang / Microsoft.
> - Сборка **не подписана**. Для запуска на консоли нужен RGH/JTAG (или Xenia).
> - Перед запуском **сделайте резервную копию сейвов**. Переполнение сейва теперь обрабатывается, но гарантий нет.
> - Используйте на свой риск.

---

## Что это

Это исходники и готовая сборка Minecraft LCE для Xbox 360, в которую добавлен «бесконечный» мир. Оригинальная игра ограничивала мир небольшим квадратом (54–320 чанков); здесь генерация, стриминг чанков, кэши и сетевые флаги переработаны так, чтобы можно было уходить далеко за старые границы.

Основные возможности:

- генерация Overworld / Nether / End за старыми границами (до ±30 млн блоков);
- ограниченный клиентский кэш чанков (LRU) вместо бесконечного роста памяти;
- ограниченный серверный кэш выгруженных чанков;
- корректная переотправка чанков при возврате в посещённую область;
- сохранение/загрузка больших миров без записи за границу зарезервированной памяти;
- split-screen без коллизий слотов рендера (4 квадранта 32×32);
- структуры (деревни, крепости, адские крепости) генерируются по всему миру, а не только у спавна.

---

## Что было модифицировано (M3)

Ниже — список правок, сделанных относительно исходного снимка M2. Формат: `файл` — суть.

### P0 — критичное

1. **Клиентский кэш чанков ограничен.**
   `Minecraft.Client/MultiPlayerChunkCache.h/.cpp` — добавлен LRU (`m_droppedLRU` / `m_droppedSet`) и лимит `m_maxCachedChunks = 2048` (на Windows64 переопределяется `MC_MAX_CLIENT_CHUNKS`). Раньше `drop()` только выгружал сущности и оставлял чанк в памяти навсегда.

2. **Защита от `emptyChunk`.**
   `Minecraft.World/LevelChunk.cpp` (`startSharingTilesAndData`, `reSyncLighting`) и `Minecraft.Client/MultiPlayerChunkCache.cpp` (`create`) проверяют `ServerChunkCache::isSentinelChunk()`. Если серверный чанк уже выгружен LRU, клиентский чанк больше не «перешаривается» с пустым — блоки и свет не пропадают.

3. **Кольцо рендера без коллизий в split-screen.**
   `Minecraft.Client/LevelRenderer.h/.cpp`, `Chunk.h/.cpp` — кольцо 64×64 на измерение разбито на 4 квадранта 32×32, по одному на локального игрока. Память не изменилась. `playerIndex` протянут через `Chunk`, `getGlobalIndexForChunk` и все flag/refcount-функции; `setDirty` помечает все квадранты; tile entities рендерятся только из квадранта текущего игрока.

4. **Сетевые флаги заменены на per-player.**
   `Minecraft.Client/ServerPlayer.h/.cpp`, `PlayerChunkMap.cpp`, `PlayerList.cpp` — вместо глобальных `SystemFlag` используется `sentChunks` на игрока. Сбрасывается при уходе чанка из вида и при смене измерения. Это чинит «невидимые чанки» при возврате и пропущенный телепорт.

5. **Переполнение сейва больше не пишет за границу.**
   `Minecraft.World/ConsoleSaveFileOriginal.h/.cpp` — `commitHeapPages()` возвращает `bool` и не урезает размер молча; `PrepareForWrite` / `MoveDataBeyond` / `writeFile` / `zeroFile` прерывают запись, не трогая данные, длину файла и заголовок. Убран дубликат `4096` в `reserveAttempts`.

### P1 — важное

6. **`ServerChunkCache::tick`** — `unload(true)` выполняется под `m_csLoadCreate`; `create()` больше не возвращает чанк, из которого уже удалены сущности.
7. **`trimUnloadedCache`** — занятые (`m_clientShareCount != 0`) чанки остаются в LRU и повторяются позже, а не теряются навсегда. Скан ограничен 256 записями за тик.
8. **Структуры** — `VillageFeature`, `VillagePieces`, `StrongholdPieces`, `NetherBridgePieces` используют `dimension->getXZSize()` вместо legacy `LevelData::m_xzSize` (320 чанков). Исправлен пропущенный `<<4` в `VillageStart::isValid` (деревни были только в ±160 блоков).
9. **`RegionFileCache`** — добавлен `CRITICAL_SECTION`, доступ к кэшу и `RegionFile` сериализован; полный `_clear()` при достижении `MAX_CACHE_SIZE` убран (он удалял объекты под работающими save-потоками).
10. **Выгрузка чанков** — `m_toDropSet` (O(1) вместо линейного `find`), `removeFromLoadedList` (swap-pop вместо `std::remove`).
11. **`MultiPlayerLevel::animateTick`** — координаты пакуются по 24 бита на ось (`set<__int64>`), а не 8; анимация больше не зацикливается через 2048 блоков и не обрезается на ±2560.

### P2 и загрузка мира

12. `Chunk::ReleaseThreadStorage` — `delete[] tileIds`.
13. Комментарии в `LevelData.h/.cpp`: `m_xzSize` — legacy, не для границ мира.
14. **Фикс прозрачных чанков после save/exit/reload.**
    `MultiPlayerChunkCache::create`: `setTilesDirty` вызывался **до** `recalcHeightmapOnly()`. Рендер-поток успевал собрать чанк пустым, и повторно dirty не ставился. Теперь dirty ставится после пересчёта heightmap. Проверено в Xenia на загруженном сейве: чанки рендерятся, 60 fps.

---

## Сборка

Требования:

1. Visual Studio 2010 Professional (или совместимый toolset).
2. Xbox 360 SDK (XDK) 2010-01.
3. MSBuild 4.0 (из .NET Framework 4.0).

Переменные окружения:

```bat
set XEDK=<путь к Xbox360XDK>
set TEMP=H:\Temp
set TMP=H:\Temp
```

Сборка (из корня репозитория):

```bat
msbuild Minecraft.World\Minecraft.World.vcxproj /p:Configuration=Release /p:Platform="Xbox 360" /m
msbuild Minecraft.Client\Minecraft.Client.vcxproj /p:Configuration=Release /p:Platform="Xbox 360" /p:SolutionDir=<путь к корню репозитория>\ /m
```

Результат:

```
Minecraft.Client\bin\Xbox 360\Release\Minecraft.Client.xex
```

`res/` в репозиторий не входит — он лежит в разделе **Releases** (или собирается из медиа-ассетов).

---

## Запуск в Xenia

1. Положите `default.xex` и папку `res` в одну директорию (например `H:\Temp\mc360\`).
2. Запустите Xenia Canary:
   ```
   xenia_canary.exe "H:\Temp\mc360\default.xex"
   ```
3. Профиль: Xenia → Profile → создать/выбрать профиль (иначе игра покажет «Gamer profile not online» — можно выбрать «Continue playing offline»).
4. Для клавиатуры в `xenia-canary.config.toml`:
   ```toml
   [HID]
   keyboard_mode = 1
   keyboard_user_index = 0
   ```
   (или `keyboard_mode = 2` для passthrough, если подключён контроллер).

---

## Xbox 360 TCP PoC

- `default.xex` — TCP-клиент к PC-серверу. В Xenia проходит PreLogin/Login, но загрузка мира пока зависает на «Connecting to host».
- Адрес сервера жёстко задан `192.168.0.209:25565`; для другой сети нужна пересборка.
- Для локальной игры и System Link используйте `default_qnet_backup_20261108.xex`.
- Релиз: https://github.com/geradenis/MinecraftLCE-XBOX360-modded/releases/tag/xbox-tcp-poc-2026-11-08

---

## Что в репозитории

- `Minecraft.Client/`, `Minecraft.World/` — исходники;
- `build/` — файлы проектов Visual Studio;
- `MinecraftConsoles.sln`, `MinecraftPC.sln`, `m2_serial_compile.targets`, `compat/`;
- `README.md`, `FIXES_M3.md`, `CHANGES.md`, `ANALYSIS.md`, `STREAMING_PLAN.md`, `XBOX360_PORT_REVIEW.md`;
- `Xbox360-Build/default.xex` — текущая TCP PoC-сборка (38 719 488 байт); `Xbox360-Build/default_qnet_backup_20261108.xex` — предыдущая QNet/System Link сборка (39 014 400 байт); `README_RU.md`, конфиг Xenia.

Не включено: `bin/`, `_sdk_install/`, медиа-ассеты (`XboxMedia`, `DurangoMedia`, `PS3Media`, `PSVitaMedia`, `OrbisMedia`, `Windows64Media`, `music`), `repo/`, `src/`, `res/`.

## Что в релизе

- `MinecraftLCE-Xbox360-TCP-PoC-2026-11-08.zip` — `default.xex` (TCP PoC), `default_qnet_backup_20261108.xex` (QNet/System Link), `res/`, `README_RU.md`, `xenia-canary.config.toml`.
- Ссылка: https://github.com/geradenis/MinecraftLCE-XBOX360-modded/releases/tag/xbox-tcp-poc-2026-11-08
- Если TCP-сборка не запускается — используйте `default_qnet_backup_20261108.xex` из архива.

---

## Известные ограничения

- **На реальной Xbox 360 не тестировалось** — только Xenia Canary.
- Остались перф-моменты: `setDirty` при обновлении блока помечает все 4 квадранта (лишняя работа для одиночной игры); `MultiPlayerChunkCache::getChunk` берёт лок на каждый вызов; `markActiveLocked` делает O(n) `list::remove`.
- Память рендера: 393 216 display lists; кольцо Нижнего мира/Энда 64 (можно уменьшить).
- `MC_MAX_UNLOADED = 256` на каждое измерение.
- `RegionFileCache::_clear()` из `McRegionLevelStorage` при закрытии мира всё ещё может удалить `RegionFile` под save-потоком (низкий риск).
- `writeFile` возвращает 0 при переполнении, но `RegionFile` результат не проверяет — чанк может записаться частично.
- `BaseHeap::Release` предупреждения в Xenia были и в старом билде; в последнем прогоне — 0.

---

## Апстрим и благодарности

Проект основан на исходниках Minecraft: Legacy Console Edition (4J Studios / Mojang). Оригинальный README и структура сохранены; этот форк добавляет бесконечный мир и перечисленные выше исправления.

Все правки M3 и этот README сделаны **AI-агентом OpenCode** (DeepSeek V4.1 Flash) в ноябре 2026 года по заданию владельца репозитория. Если что-то сломалось — это не официальная поддержка 4J/Mojang.
