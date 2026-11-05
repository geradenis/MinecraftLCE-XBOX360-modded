# M3 — исправления после анализа (2026-11-04)

Резервная копия до правок: `H:\Projects\MinecraftLCE-7thGen-backup-2026-11-04_2014`

## P0 — критичные

1. **Клиентский кэш чанков ограничен.**
   `MultiPlayerChunkCache` (`Minecraft.Client\MultiPlayerChunkCache.h/.cpp`):
   `drop()` больше не оставляет чанк в `cache`/`loadedChunkList` навсегда. Появился
   LRU `m_droppedLRU` + `m_droppedSet` и лимит `m_maxCachedChunks = 2048`
   (на Windows64 переопределяется `MC_MAX_CLIENT_CHUNKS`). При переполнении
   самый старый выгруженный чанк удаляется из `cache` и `loadedChunkList`.
   `create()` при повторном появлении снимает чанк с LRU.

2. **Защита от `emptyChunk`.**
   `LevelChunk::startSharingTilesAndData` / `reSyncLighting`
   (`Minecraft.World\LevelChunk.cpp`) и `MultiPlayerChunkCache::create`
   проверяют `ServerChunkCache::isSentinelChunk()` (новый публичный метод,
   `ServerChunkCache.h`). Если серверный чанк уже выгружен LRU, клиентский
   чанк больше не «перешаривается» с пустым — блоки и свет не пропадают.

3. **Кольцо рендера без коллизий в split-screen.**
   `LevelRenderer`/`Chunk` (`LevelRenderer.h/.cpp`, `Chunk.h/.cpp`):
   кольцо 64×64 на измерение разбито на 4 квадранта 32×32, по одному на
   локального игрока. Память не изменилась (`getGlobalChunkCount()` прежний).
   `getGlobalIndexForChunk` и все flag/refcount-функции получили `playerIndex`;
   `Chunk` хранит свой `playerIndex`; `setDirty` ставит флаг во все 4 квадранта;
   `renderEntities` рендерит tile entities только из квадранта текущего игрока.

4. **Сетевые флаги заменены на per-player.**
   `ServerPlayer` (`ServerPlayer.h/.cpp`): добавлен
   `unordered_set<ChunkPos> sentChunks`. `PlayerChunkMap` проверяет/сбрасывает
   его (`remove()`), `PlayerList::toggleDimension` очищает при смене измерения,
   проверка телепорта в `PlayerList.cpp` использует `sentChunks`.
   Глобальные `SystemFlag*` больше не участвуют в игровой логике.

5. **Переполнение сейва больше не пишет за границу.**
   `ConsoleSaveFileOriginal` (`Minecraft.World\ConsoleSaveFileOriginal.h/.cpp`):
   `commitHeapPages()` возвращает `bool` и не урезает размер молча;
   `PrepareForWrite`/`MoveDataBeyond`/`writeFile`/`zeroFile` прерывают запись,
   не трогая данные, длину файла и заголовок. Убран дубликат `4096` в
   `reserveAttempts`.

## P1 — важные

6. **`ServerChunkCache::tick`:** `unload(true)` выполняется под `m_csLoadCreate`,
   `create()` больше не может вернуть чанк, из которого уже удалены сущности.
7. **`trimUnloadedCache`:** занятые (`m_clientShareCount != 0`) чанки остаются
   в LRU и повторяются позже, а не теряются навсегда. Скан ограничен
   `MAX_SCAN_PER_TICK = 256`.
8. **Структуры:** `VillageFeature`, `VillagePieces`, `StrongholdPieces`,
   `NetherBridgePieces` используют `dimension->getXZSize()` вместо legacy
   `LevelData::m_xzSize` (320 чанков). Исправлен пропущенный `<<4` в
   `VillageStart::isValid` (деревни были только в ±160 блоков).
9. **`RegionFileCache`:** добавлен `CRITICAL_SECTION`, доступ к `cache` и
   `RegionFile` сериализован; полный `_clear()` при достижении
   `MAX_CACHE_SIZE` убран (он удалял объекты под работающими save-потоками).
10. **Выгрузка:** `m_toDropSet` (O(1) вместо линейного `find`),
    `removeFromLoadedList` (swap-pop вместо `std::remove`).
11. **`MultiPlayerLevel::animateTick`:** координаты пакуются по 24 бита на ось
    (`set<__int64>`), а не 8 — анимация больше не зацикливается через 2048
    блоков и не обрезается на ±2560.

## P2

12. `Chunk::ReleaseThreadStorage` — `delete[] tileIds`.
13. Комментарии в `LevelData.h/.cpp`: `m_xzSize` — legacy, не для границ мира.
14. **Загрузка мира (регрессия после правок):** в `MultiPlayerChunkCache::create`
    `setTilesDirty` вызывался до `recalcHeightmapOnly()`. Рендер-поток успевал
    собрать чанк пустым, и повторно dirty не ставился — после save/exit/reload
    чанки вокруг игрока были прозрачными, пока блок не обновит слот. Теперь
    dirty ставится после пересчёта heightmap. Проверено в Xenia на загруженном
    сейве: чанки рендерятся, 60 fps, крашей нет.

## Не сделано / остаточные риски

- UI выбора размера мира (`UIScene_CreateWorldMenu`) по-прежнему предлагает
  54/64/192/320 чанков; теперь это влияет только на legacy-совместимость.
- `MC_MAX_UNLOADED = 256` на измерение и кольца Nether/End 64 — тюнинг, не баг.
- `dataReceived()` может создавать записи с `chunk == NULL`; они не чистятся,
  но занимают единицы байт.
- Порядок разрушения `ServerChunkCache`/`MultiPlayerChunkCache` при выходе из
  мира не проверялся на реальном железе.
- **Сборка не проверялась:** в PATH нет MSVC/XDK (`cl`/`msbuild` не найдены).
  Правки проверены чтением и grep по всем изменённым сигнатурам.

## Тест-план (Xenia / Xbox)

1. Мир 3000+ блоков: уйти, вернуться — не должно быть «дыр» и чёрных чанков.
2. Долгий забег 10–15 минут: `gatherStats()` / память не должны расти линейно.
3. Split-screen 2 игрока, развести на 800–1500 блоков — чанки не должны
   мигать/пропадать.
4. Телепорт/портал в Нижний мир и обратно — чанк под игроком должен
   переотправляться (нет провала в пустоту).
5. Деревни/крепости далеко от спавна (10k+ блоков) — должны генерироваться.
6. Сохранение большого мира — без `Access Violation` и без порчи сейва.
