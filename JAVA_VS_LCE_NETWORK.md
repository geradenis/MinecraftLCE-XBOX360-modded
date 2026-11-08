# Java Minecraft vs Minecraft LCE: устройство сети и принципиальные различия

Дата: 2026-11-08

## Что сравнивалось

Java-сторона:

- `H:\MultiMC\libraries\net\minecraft\client\1.21.8\client-1.21.8-official.jar` — основной источник, официальные (читаемые) имена классов.
- `H:\MultiMC\libraries\net\minecraft\client\1.20.1-20230612.114412\client-1.20.1-20230612.114412-srg.jar` — для проверки эволюции протокола (1.20.1 → 1.21.8).
- `H:\загрузки\server.jar` (26.3) — серверная версия; имена обфусцированы, но архитектура та же.

LCE-сторона:

- `H:\Projects\MinecraftLCE-7thGen\M2-working-copy\Minecraft.World\Connection.cpp/.h`
- `...\Minecraft.World\Socket.cpp`
- `...\Minecraft.World\Packet.cpp/.h`
- `...\Minecraft.World\BlockRegionUpdatePacket.cpp`
- `...\Minecraft.Client\ServerConnection.cpp`
- `...\Minecraft.Client\PlayerList.cpp`
- `...\Minecraft.Client\PlayerChunkMap.cpp`
- `...\Minecraft.Client\EntityTracker.cpp`, `TrackedEntity.cpp`
- `...\Minecraft.Client\MinecraftServer.cpp`, `ServerLevel.cpp`
- `...\Minecraft.Client\Windows64\ProtocolBot.cpp`, `ServerConsole.cpp`
- `...\Minecraft.World\ConsoleSaveFileOriginal.*`, `McRegionLevelStorage.*`, `McRegionChunkStorage.*`

Важно: LCE — это не «Java-сервер, переписанный на C++». Это консольный движок 4J Studios, выросший из Java-кода 1.8-эпохи, у которого сетевой слой изначально был завязан на QNet (Xbox 360/PS3), а не на TCP. Ветка `_WINDOWS64` с `Socket` — уже поздняя адаптация под PC. Поэтому многие различия объясняются не «хуже/лучше», а происхождением: QNet давал границы сообщений и надёжность, TCP — нет.

---

## 1. Кратко: десять принципиальных отличий

| Принцип | Java Edition (1.21.8) | Minecraft LCE (TCP-ветка) |
|---|---|---|
| Транспорт | Netty: NIO/epoll event loops, неблокирующие каналы | Блокирующие `recv`/`send`, по 2 потока на каждое соединение |
| Границы пакетов | Явный frame: VarInt-длина + (опционально) сжатие + VarInt ID | Нет frame. Поток байтов: 1 байт ID + payload, парсер сам должен съесть ровно столько байт, сколько записал отправитель |
| Сжатие | zlib (`Deflater/Inflater`), порог, проверка размера | Только внутри chunk-пакета: LZXRLE; транспортного сжатия нет |
| Шифрование | AES/CFB8 + RSA-обмен ключами | В пакетном слое нет; на Xbox защита обеспечивалась платформой, на PC TCP — открытый текст |
| Состояния протокола | `HANDSHAKING → STATUS/LOGIN → CONFIGURATION → PLAY`, отдельные реестры пакетов на состояние | `PreLogin → Login → Play`; один плоский реестр ID 0..255, направление фильтруется множествами |
| Версионирование | protocol version в handshake, `ProtocolInfo`, поддержка нескольких версий | `MINECRAFT_NET_VERSION` (495) + `clientVersion` в LoginPacket; одна версия |
| Потоки | Несколько Netty-потоков на все соединения + серверный тик-поток + worldgen/light/IO пулы | 2 потока на соединение + серверный тик-поток + post-process + save-потоки |
| Чанки | `ChunkMap` + `DistanceManager` + `ChunkLevel` (33/32/31) + `PlayerChunkSender` с batch-ack | `PlayerChunkMap` с радиусом, `PlayerChunk`, `chunksToSend`, глобальный бюджет чанков на тик, без ack |
| Сущности | `ChunkMap.TrackedEntity` + `ServerEntity`, `seenBy`, чанк-ориентированный отбор игроков | `EntityTracker` + `TrackedEntity`, глобальный перебор «все сущности × все игроки» |
| Сохранение | Регионные файлы `.mca` по измерениям, `level.dat`, `playerdata` | Единый console-save (`ConsoleSaveFileOriginal/Split`) с region-хранилищем внутри, слоты `GameHDD` |

---

## 2. Транспорт и кадрирование (самое важное различие)

### 2.1 Java: конвейер Netty

В 1.21.8 `net.minecraft.network.Connection` — это `SimpleChannelInboundHandler<Packet<?>>`, работающий поверх Netty. В `Connection` есть:

- `NETWORK_WORKER_GROUP` (`NioEventLoopGroup`), `NETWORK_EPOLL_WORKER_GROUP` (`EpollEventLoopGroup`), `LOCAL_WORKER_GROUP` (`DefaultEventLoopGroup`);
- `Channel channel`, `SocketAddress address`;
- `Queue<Consumer<Connection>> pendingActions`;
- `setupInboundProtocol`, `setupOutboundProtocol`, `setupCompression`, `setEncryptionKey`;
- `PacketBundlePacker`/`PacketBundleUnpacker`, `BandwidthDebugMonitor`, `HiddenByteBuf`.

Конвейер (упрощённо):

```
TCP
  → Varint21FrameDecoder      (читает VarInt-длину кадра, максимум 21 бит)
  → CompressionDecoder        (zlib, если включён порог)
  → CipherDecoder             (AES/CFB8, если включено шифрование)
  → PacketDecoder             (VarInt packet id + payload → Packet)
  → Connection.channelRead0   (передаёт PacketListener)
```

Исходящий поток — зеркально:

```
Packet
  → PacketEncoder              (VarInt id + payload)
  → CompressionEncoder         (zlib при размере >= threshold)
  → CipherEncoder              (AES/CFB8)
  → Varint21LengthFieldPrepender (VarInt-длина кадра)
  → TCP
```

Ключевой момент: **у каждого пакета есть явная длина**. Даже если payload повреждён, decoder знает, где заканчивается кадр, и может отбросить соединение, не «съедая» следующие пакеты. `CompressionDecoder` дополнительно проверяет `MAXIMUM_COMPRESSED_LENGTH`/`MAXIMUM_UNCOMPRESSED_LENGTH`, то есть защищает от decompression bomb. `Varint21FrameDecoder` ограничивает кадр 2 МБ (21 бит).

В 1.20.1 архитектура та же: `Connection`, `PacketEncoder`, `PacketDecoder`, `CompressionEncoder/Decoder`, `ConnectionProtocol` с `HANDSHAKING/PLAY/STATUS/LOGIN`. В 1.21.8 добавился `CONFIGURATION`, `ProtocolInfo`, bundle-пакеты, `HiddenByteBuf` и т.п. То есть Java-сеть развивалась слоями, не ломая базовый принцип «frame + state machine».

### 2.2 LCE: поток байтов без frame

В LCE `Packet::writePacket` делает буквально:

```cpp
dos->write(packet->getId());   // 1 байт ID
packet->write(dos);            // payload
```

`Packet::readPacket` читает один байт ID, берёт фабрику из `idToCreateMap`, вызывает `packet->read(dis)`. **Длины пакета нет.** `DataInputStream` — это обёртка над `SocketInputStreamTcp`, который делает блокирующий `recv` ровно на столько байт, сколько просит парсер. Если парсер ошибся на один байт (или клиент/сервер собраны из разных версий), следующий `readShort()` прочитает мусор — это и есть `readUtf: negative string length`, `packet stream desync` и т.п.

Почему так? В оригинальной консольной ветке `Connection` писал не в TCP-поток, а в QNet:

```cpp
sos->writeWithFlags(baos->buf, 0, baos->size(), flags);
```

QNet — это message-oriented транспорт: он сохраняет границы сообщений и доставку. Поэтому LCE-коду не нужен был length-prefix: один вызов `writeWithFlags` = одно сообщение, получатель знает его размер. При переносе на TCP (`_WINDOWS64`) нормальные пакеты стали писаться в `bufferedDos` (обычный поток), а `writeWithFlags` на TCP просто игнорирует флаги и пишет байты. Границы сообщений исчезли, а протокол остался прежним.

Это главная архитектурная уязвимость TCP-порта LCE:

- нет frame decoder, который мог бы изолировать повреждённый пакет;
- нет length-prefix, по которому можно проверить «а сколько байт должно было быть»;
- ошибка в одном `read()` навсегда сдвигает поток;
- `readUtf` с отрицательной длиной — лишь самый заметный симптом, а не причина.

В новой сборке `readUtf` больше не вызывает модальный `assert`, а ставит `Packet::s_readError`, после чего `readPacket` возвращает `nullptr` и соединение закрывается. Это правильная защита от падения, но **не устранение причины**: причина — отсутствие кадрирования и несовпадение форматов.

### 2.3 Сравнение форматов

| Элемент | Java 1.21.8 | LCE |
|---|---|---|
| Длина кадра | VarInt (до 21 бита, 2 МБ) | Нет |
| ID пакета | VarInt, зависит от состояния | 1 байт, глобальная таблица |
| Строки | VarInt длина + UTF-8 (до 32767 символов) | `short` длина + UTF-16 code units |
| Сжатие | zlib, порог, проверка распакованного размера | LZXRLE только для `BlockRegionUpdatePacket` |
| Шифрование | AES/CFB8 | Нет в пакетном слое |
| Backpressure | Netty writability + очередь `Connection` | `estimatedRemaining > 1 МБ → disconnect(Overflow)`, `outgoing_slow` |
| Таймаут чтения | Netty timeout/keepalive | `SO_RCVTIMEO = 30 с`, keepalive каждые 20 тиков (1 с) |
| Ошибка парсинга | исключение → disconnect, кадр уже ограничен | `nullptr` → disconnect, но поток уже мог уехать |

---

## 3. Состояния протокола, логин, идентичность

### 3.1 Java

1. **Handshake** (первый пакет): protocol version, адрес сервера, порт, intent (`STATUS` или `LOGIN`). По intent выбирается следующее состояние.
2. **Status**: ping/pong, список игроков, favicon.
3. **Login**: `Login Start` (имя, UUID), при online-mode — `Encryption Request/Response` (RSA + AES), затем `Login Success`; при необходимости — `Set Compression` (порог zlib).
4. **Configuration** (с 1.20.2): реестры, теги, resource pack, feature flags; клиент подтверждает переход.
5. **Play**: обычные игровые пакеты.

Состояния — это `ConnectionProtocol` (`HANDSHAKING`, `STATUS`, `LOGIN`, `CONFIGURATION`, `PLAY`). Для каждого состояния свой набор packet id → class. Один и тот же числовой ID в разных состояниях может означать разные пакеты. `ProtocolInfo`/`GameProtocols`/`ConfigurationProtocols` описывают эти наборы.

### 3.2 LCE

1. **PreLoginPacket**: имя, `m_netcodeVersion` (495). Сервер сравнивает с `MINECRAFT_NET_VERSION`; при несовпадении — disconnect. Это аналог handshake, но без address/port/intent и без выбора состояния.
2. **LoginPacket**: clientVersion, имя, тип мира (`LevelType`), seed, gameType, dimension, mapHeight, maxPlayers, offline/online XUID, friendsOnlyUGC, UGC version, difficulty, multiplayerInstanceId, playerIndex, skinId, capeId, isGuest, newSeaLevel, uiGamePrivileges, при `_LARGE_WORLDS` — xzSize/hellScale.
3. **Play**: сервер отправляет `PlayerInfoPacket` (ростер), `ChunkVisibilityAreaPacket`, `BlockRegionUpdatePacket` и т.д.

Состояний всего два с половиной: prelogin, login, play. Нет configuration-фазы, нет реестров, нет resource pack negotiation. Нет шифрования и онлайн-аутентификации в пакетном слое: на консоли идентичность давал Xbox Live/XUID, на PC TCP — это offline-режим, где любой может назваться любым именем.

### 3.3 Packet ID и таблица

LCE: `Packet::staticCtor()` заполняет `unordered_map<int, packetCreateFn>`; в коде 97 `map(...)`-записей с ID от 0 до 255. Направление фильтруется `serverReceivedPackets`/`clientReceivedPackets`; `canReceiveAllPackets` — отдельный флаг. Это плоская таблица, общая для всех состояний.

Java: packet id — VarInt, и он интерпретируется через `ProtocolInfo` текущего состояния. Пакеты описаны классами с `StreamCodec`; ID выделяются регистрацией в протоколе. Это позволяет:

- иметь разные ID в разных состояниях;
- добавлять пакеты, не трогая глобальную таблицу;
- поддерживать несколько версий через отдельные `ProtocolInfo`;
- генерировать кодек и проверять типы на этапе компиляции.

LCE-подход проще и быстрее (один байт вместо VarInt, одна таблица), но хрупче: любое изменение формата требует одновременной пересборки клиента и сервера, а старый процесс с новой сборкой легко даёт desync.

---

## 4. Потоки и тик

### 4.1 Java

- Netty event loop group обслуживает все каналы (несколько потоков, обычно 2×CPU, но не по потоку на клиента).
- Серверный тик — отдельный поток `MinecraftServer` (наследник `ReentrantBlockableEventLoop<TickTask>`), 20 TPS.
- Worldgen/light — `ChunkTaskDispatcher`/`ChunkMap` dispatchers, асинхронные задачи.
- IO/сохранение — `Util.ioPool()`/`ChunkStorage`/`ChunkMap.activeChunkWrites` (ограничение одновременных записей).
- Пакеты приходят в Netty-потоке, но игровая логика, трогающая мир, выполняется на серверном потоке (через `server.execute`/main-thread executor). Это важно: Netty-поток не лезет в `Level` напрямую.

Итог: **число потоков слабо зависит от числа игроков**. 255 игроков — это всё те же несколько event loop потоков + серверный тик + пулы.

### 4.2 LCE

- Серверный поток `DedicatedServer` (`C4JThread`) крутит `MinecraftServer::run` → `tick()`.
- На каждое TCP-соединение — **два потока**: `read` и `write` (`Connection` ctor). При 255 игроках это ~510 потоков только на соединения (в тесте видели 540+).
- `Connection::readTick` читает пакеты и кладёт в `incoming`; `Connection::tick` на серверном потоке разбирает очередь (до 1000 пакетов за тик) и вызывает `handle`.
- Есть отдельный post-process поток для генерации/постобработки чанков и save-потоки `McRegionChunkStorage` (до 3).
- На Windows сервер всё равно поднимает D3D/рендер (окно скрывается), то есть это не полностью headless.

Тик-цикл LCE:

```cpp
passedTime = now - lastTime;
if (passedTime > MS_PER_TICK * 40) passedTime = MS_PER_TICK * 40;
unprocessedTime += passedTime;
while (unprocessedTime > MS_PER_TICK) {
    unprocessedTime -= MS_PER_TICK;
    tick();
}
```

То есть после сильного лага сервер пытается «догнать» до 40 тиков подряд. При 152 игроках и тике 74 секунды это может усугублять ситуацию: сервер сначала долго тикает, потом ещё выполняет очередь догоняющих тиков. Java в современном виде так не разгоняет catch-up (один тик за итерацию с ограничением времени), поэтому не входит в такой штопор.

Порядок тика LCE (`MinecraftServer::tick`):

1. таймеры;
2. для каждого измерения: `Level::tick`, lights, `tickEntities`, `EntityTracker::tick`;
3. `connection->tick()` (pending + players + чанк-бюджет);
4. `players->tick()`;
5. консольные команды.

Порядок важен: `EntityTracker::tick` идёт **до** `connection->tick`, а `ServerConnection::tick` делает и чанк-отправку, и `PlayerConnection::tick`.

---

## 5. Чанки: главный источник нагрузки

### 5.1 Java: тикеты, уровни, batch-ack

В 1.21.8 `ChunkMap`:

- `updatingChunkMap`, `visibleChunkMap`, `pendingUnloads`;
- `TicketStorage ticketStorage`;
- `ChunkMap$DistanceManager distanceManager`;
- `worldgenTaskDispatcher`, `lightTaskDispatcher`;
- `entityMap` (`Int2ObjectMap<ChunkMap$TrackedEntity>`);
- `playerMap` (`PlayerMap`);
- `activeChunkWrites`, `nextChunkSaveTime`, `chunksToEagerlySave`.

`ChunkLevel` задаёт уровни:

- 33 — full chunk (полностью загружен);
- 32 — block ticking;
- 31 — entity ticking.

`DistanceManager` считает уровни по тикетам: player ticket, forced, portal, unknown и т.д. Один и тот же чанк может быть загружен для игрока, но не тикать сущностями, если он дальше. Это разделение «загружен / тикает блоки / тикает сущности» — ключ к масштабированию.

`PlayerChunkSender` (1.21.8):

- `START_CHUNKS_PER_TICK = 9`;
- `MIN_CHUNKS_PER_TICK = 0.01`, `MAX_CHUNKS_PER_TICK = 64`;
- `MAX_UNACKNOWLEDGED_BATCHES = 10`;
- `pendingChunks` (LongSet), `desiredChunksPerTick`, `batchQuota`, `unacknowledgedBatches`;
- `sendNextChunks(ServerPlayer)`, `onChunkBatchReceivedByClient(float)`.

Клиент подтверждает получение батчей пакетами `ServerboundChunkBatchReceivedPacket`; сервер шлёт `ClientboundChunkBatchStartPacket`/`ClientboundChunkBatchFinishedPacket`. Это даёт **обратное давление**: сервер не заваливает клиента чанками быстрее, чем тот успевает их принимать и строить. `ChunkMap.isChunkTracked`/`isChunkOnTrackedBorder` решают, какие чанки игроку вообще нужны.

### 5.2 LCE: радиус и глобальный бюджет

`PlayerChunkMap`:

- `radius` (в тесте 12) — квадрат `(2r+1)×(2r+1)`, при r=12 это 625 чанков на игрока;
- `add(player)` идёт спиралью (CraftBukkit-подход), добавляет игрока в `PlayerChunk` и отправляет `ChunkVisibilityAreaPacket(minX,maxX,minZ,maxZ)`;
- `PlayerChunk::add` отправляет `ChunkVisibilityPacket(x,z,true)` и кладёт позицию в `player->chunksToSend`;
- `ServerPlayer::doChunkSendingTick` отправляет `BlockRegionUpdatePacket`, когда чанк готов;
- `move(player)` — квадратное окно, `chunkInRange` — квадратная проверка;
- `getMaxRange() = radius * 16 - 16` (при 12 → 176 блоков).

`ServerConnection::tick` задаёт **глобальный** бюджет чанков на тик:

```cpp
if (players.size() <= 8) chunkBudget = 1000;
else if (players.size() <= 64) chunkBudget = 64;
else if (players.size() <= 128) chunkBudget = 32;
else chunkBudget = 24;
```

Плюс `chunksPerTick` из настроек (9), но при >8 игроках принудительно 1. Игроки с пустой очередью бюджет не тратят, чтобы не голодали последние.

Чего нет:

- нет подтверждений получения чанков (ack);
- нет per-player квоты с обратной связью;
- нет уровней «загружен / block ticking / entity ticking»;
- нет отдельного учёта «чанк нужен для рендера» и «чанк нужен для симуляции»;
- бюджет глобальный, а не на игрока, поэтому при 255 игроках одни могут ждать, пока другие получают чанки.

При 255 игроках и r=12 это до 255×625 = 159 375 подписок «игрок-чанк» (уникальных чанков меньше, если игроки рядом, но на спавне/разбросанные — много). Даже если чанки общие, `PlayerChunk::players` и рассылки создают большую нагрузку.

`BlockRegionUpdatePacket`:

- для полного чанка (`xs=16, ys=maxBuildHeight, zs=16`) блоки переупорядочиваются для лучшего сжатия;
- данные сжимаются `CompressLZXRLE` (не zlib), размер пишется в `int`, старшие 2 бита — `levelIdx`;
- на клиенте `DecompressLZXRLE` проверяет ёмкость и возвращает ошибку; битый чанк отбрасывается (`valid=false`), а не роняет игру.

Это хорошая локальная оптимизация (LZXRLE хорошо жмёт блоки), но она не заменяет транспортное сжатие и frame-проверки.

---

## 6. Отслеживание сущностей: почему 152 игрока дают тик 74 секунды

### 6.1 Java

`ChunkMap.TrackedEntity`:

- `ServerEntity serverEntity`;
- `Entity entity`;
- `int range` — дальность трекинга;
- `SectionPos lastSectionPos`;
- `Set<ServerPlayerConnection> seenBy` — кто уже видит сущность.

`updatePlayers(List<ServerPlayer>)` вызывается для списка игроков; `updatePlayer(ServerPlayer)` проверяет, видит ли игрок сущность (через чанк/секцию и `range`), и добавляет/удаляет из `seenBy`. `broadcast` отправляет пакет только `seenBy`. `ServerEntity` отвечает за периодичность и дельты (позиция/поворот/скорость/данные/экипировка).

Ключевое: **кандидаты на видимость ограничены чанками/секциями**, а не «все игроки мира». Даже если `updatePlayers` получает список игроков, проверка идёт по `seenBy` и позиции, а не по полному перебору всех сущностей для каждого игрока. Плюс `EntityTicking`-чанки тикают сущностями только вокруг игроков.

### 6.2 LCE

`EntityTracker`:

- `entities` — набор `TrackedEntity`;
- `entityMap` — entityId → `TrackedEntity`;
- `tick()`:
  1. для каждой сущности `te->tick(...)`; если сущность — игрок и двигалась, добавляет в `movedPlayers`;
  2. для каждого сдвинувшегося игрока — **цикл по всем `entities`** и `te->updatePlayer(this, player)`;
  3. отдельно обрабатывает «все игроки на той же системе» (splitscreen) и мёртвых игроков.

`TrackedEntity::updatePlayers` также проходит по всем игрокам. Итоговая сложность в худшем случае — **O(E × P)**: каждая сущность проверяется для каждого сдвинувшегося игрока. При 152 игроках и большом числе сущностей это и даёт десятки секунд на тик. В Debug-сборке добавляются накладные расходы, но алгоритмическая причина — именно глобальный перебор.

В LCE нет `seenBy`-множества, привязанного к чанку/секции; `maxRange` — глобальный (от view distance), а не per-entity type. Поэтому масштабирование до 255 игроков упирается в трекер раньше, чем в сеть.

---

## 7. Измерения

### Java

- `MinecraftServer.levels` — `Map<ResourceKey<Level>, ServerLevel>`.
- Каждое измерение — отдельный `ServerLevel` со своим `ChunkMap`, `DistanceManager`, `TicketStorage`, entity storage, save storage.
- Измерения data-driven: overworld, nether, end, datapack-измерения.
- Переход игрока — `PlayerList.changeDimension`/`ServerPlayer.changeDimension`, с порталами, телепортами, загрузкой чанков и отправкой клиенту новых данных.
- Отдельные папки `DIM-1`, `DIM1` (и datapack-измерения) в сохранении.

### LCE

- `MinecraftServer.levels` — фиксированный массив из 3 `ServerLevel`: 0 (overworld), -1 (nether), 1 (end). В коде `levels[0..2]`, `dimension = 0`, `-1`, `1`.
- `DerivedServerLevel` для производных измерений (nether/end) от основного.
- `PlayerList::changeDimension`/`toggleDimension`, порталы, respawn, проверка кровати, выход из End.
- `McRegionLevelStorage` использует префиксы `NETHER_FOLDER`/`ENDER_FOLDER` внутри console-save.
- `_LARGE_WORLDS` добавляет `xzSize`/`hellScale` в LoginPacket и ограничения мира.

Разница: Java-измерения — это расширяемая система с реестрами и datapack; LCE — жёстко зашитые три мира с общим console-save. Для 255 игроков это не главный блокер, но добавление новых измерений в LCE потребует правок в нескольких местах.

---

## 8. Сохранение мира

### Java

- `LevelStorageSource.LevelStorageAccess` — каталог мира.
- `level.dat` — NBT с настройками мира.
- `region/r.x.z.mca` — регионные файлы 32×32 чанка, zlib-сжатые NBT-чанки.
- `entities/r.x.z.mca` — сущности по чанкам.
- `playerdata/<uuid>.dat` — игроки.
- `advancements/`, `stats/`, `data/`, `datapacks/`.
- `DIM-1/`, `DIM1/` — отдельные измерения.
- `ChunkMap` сохраняет чанки порциями (`CHUNK_SAVED_PER_TICK`, `CHUNK_SAVED_EAGERLY_PER_TICK`, `MAX_ACTIVE_CHUNK_WRITES`), асинхронно, с ограничением одновременных записей.

### LCE

- `ConsoleSaveFileOriginal` — единый console-save в памяти: резервирует виртуальное адресное пространство (на PC — до 2 ГБ, страницы по 64 КБ), коммитит страницы по мере роста; на Xbox 360/эмуляторах — меньший резерв (256 МБ) из-за ограничений.
- `ConsoleSaveFileSplit` — разбиение на подфайлы (используется в некоторых путях).
- `McRegionLevelStorage`/`McRegionChunkStorage` — region-подобное хранилище **внутри** console-save; `RegionFileCache`, `RegionFile`.
- `McRegionChunkStorage` имеет до 3 save-потоков (`s_saveThreads[3]`), очередь `s_chunkDataQueue`.
- Слоты сохранения `GameHDD/<timestamp>`; сервер загружает последний слот. В тестах восстановление из `GameHDD_backup_150715`.
- Загрузка большого save (2 ГБ) в память — одна из причин долгой загрузки мира в Debug.

Разница: Java сохраняет инкрементально по регионам и измерениям; LCE держит монолитный save в памяти и пишет его целиком/крупными частями. Для сервера на 255 игроков это означает большие паузы на автосейве и риск долгой загрузки.

---

## 9. Ростер, идентичность, splitscreen

Java:

- `PlayerList` хранит `ServerPlayer`, у каждого `GameProfile`/UUID, `ServerGamePacketListenerImpl`, `Connection`.
- Ростер синхронизируется `ClientboundPlayerInfoUpdatePacket` с действиями (ADD_PLAYER, UPDATE_LISTED, UPDATE_LATENCY, UPDATE_GAME_MODE, UPDATE_DISPLAY_NAME и т.д.).
- Один игрок = одно соединение (без консольного splitscreen в Java Edition).

LCE:

- `PlayerList` хранит `ServerPlayer`, у каждого `smallId` (1..255), `entityId`, `playerIndex` (0..3 для splitscreen), `connection`.
- `PlayerInfoPacket` — кастомный 4J-пакет; в него добавлено имя игрока (обе стороны должны быть пересобраны).
- `AddPlayerPacket` — отдельный пакет для игроков (в Java новые игроки идут через общий `ClientboundAddEntityPacket`).
- `IsSameSystem` используется для splitscreen: если один игрок на консоли сдвинулся, трекер обновляет и остальных.
- `MINECRAFT_NET_MAX_PLAYERS = 255`, small ID 1..255; 256-й получает `ServerFull`.

Для TCP-порта это означает: 255 — это не «255 консолей», а 255 **игроков** (small ID), включая splitscreen-аватары. На PC каждый бот/клиент занимает один small ID.

---

## 10. Ошибки, безопасность, версионирование

| Аспект | Java | LCE |
|---|---|---|
| Ошибка парсинга | Исключение в decoder → disconnect; кадр ограничен | `nullptr`/`s_readError` → disconnect; поток мог уехать |
| Неизвестный ID | Отбрасывание/исключение по протоколу | `bad packet id`, `nullptr` |
| Слишком длинная строка | Исключение, frame уже проверен | `readUtf` → `s_readError` (раньше `assert`) |
| Шифрование | AES/CFB8 + RSA | Нет в пакетах; на PC TCP — открытый текст |
| Аутентификация | Mojang session (online-mode) | XUID на консоли; на PC TCP — offline, имя произвольное |
| Версии | protocol version + `ProtocolInfo` | `MINECRAFT_NET_VERSION=495` + `clientVersion` |
| DoS-устойчивость | лимиты frame/compression, Netty | лимит `estimatedRemaining` 1 МБ, но нет frame-лимита |

Практический вывод: **LCE TCP-сервер нельзя выставлять в интернет** без внешнего шифрования/аутентификации (VPN, туннель, прокси). Java-сервер в online-mode имеет для этого штатные механизмы.

---

## 11. Что из Java стоит перенять LCE (по приоритету)

1. **Frame-слой для TCP.** Даже простой `uint32 length` перед пакетом (или VarInt) резко уменьшит последствия desync: decoder читает кадр целиком, при ошибке рвёт соединение, не портя следующие пакеты. Это самое дешёвое и самое важное улучшение. Можно добавить только для `_WINDOWS64`, не трогая QNet-ветку.
2. **Чанк-ориентированный entity tracking.** Вместо `for each moved player → for each entity` сделать индекс `chunk/section → entities` и `seenBy` на `TrackedEntity`. Тогда сложность станет ближе к `O(entities in range)`, а не `O(E×P)`. Это главный блокер 255 игроков.
3. **Per-player chunk queue + ack.** Аналог `PlayerChunkSender`: клиент подтверждает батчи, сервер не шлёт больше `MAX_UNACKNOWLEDGED_BATCHES`. Даже без новых пакетов можно ограничить очередь на игрока и не тратить глобальный бюджет на тех, кто не успевает.
4. **Разделить «загружен / тикает блоки / тикает сущности».** Хотя бы для симуляции: `simulation-distance` уже есть, но трекер всё равно глобальный. Чанки за пределами simulation distance не должны тикать сущностями.
5. **Убрать 2 потока на соединение.** Для 255 игроков лучше IOCP/thread pool: один-два IO-потока + очередь на серверный тик. Это уменьшит 510+ потоков и переключения контекста. На Windows это `WSAPoll`/IOCP или хотя бы пул с неблокирующими сокетами.
6. **Ограничить catch-up.** Не выполнять до 40 тиков после 74-секундного тика; лучше пропускать лишние тики и логировать «Can't keep up», как Java.
7. **Release-сборка.** Debug-сборка сильно увеличивает стоимость трекера и рендера; без Release 255 в реальном времени маловероятны даже после алгоритмических правок.
8. **Headless-сервер без D3D.** Сейчас сервер скрывает окно, но рендер идёт; для 255 игроков это лишняя нагрузка и риск. Долгосрочно — отдельный путь без `Minecraft::run`/рендера.
9. **Сохранение.** Инкрементальные регионные записи и/или выгрузка частей console-save из памяти; иначе автосейв 2 ГБ будет давать паузы.
10. **Version negotiation.** Как минимум логировать `clientVersion` и `netcodeVersion` и отказывать до создания `ServerPlayer`; сейчас `MINECRAFT_NET_VERSION` — единственный барьер, а `clientVersion` почти не проверяется.

---

## 12. Карта соответствий классов

| Java 1.21.8 | LCE |
|---|---|
| `net.minecraft.network.Connection` | `Minecraft.World/Connection.cpp` |
| `PacketEncoder`/`PacketDecoder` | `Packet::writePacket`/`Packet::readPacket` |
| `Varint21FrameDecoder` | нет (QNet давал границы; TCP — нет) |
| `CompressionEncoder/Decoder` | `BlockRegionUpdatePacket` + `CompressLZXRLE` (только чанки) |
| `CipherEncoder/Decoder` | нет |
| `ConnectionProtocol`/`ProtocolInfo` | `Packet::staticCtor` + `serverReceivedPackets`/`clientReceivedPackets` |
| `ServerConnectionListener` | `Socket::TcpListenerThread` + `ServerConnection::NewIncomingSocket` |
| `ServerGamePacketListenerImpl` | `ServerPlayer`/`PlayerConnection`/`PacketListener` |
| `PlayerChunkSender` | `ServerPlayer::doChunkSendingTick` + `PlayerChunkMap` |
| `ChunkMap`/`DistanceManager`/`ChunkLevel` | `PlayerChunkMap`/`PlayerChunk`/`ServerChunkCache` |
| `ChunkMap$TrackedEntity`/`ServerEntity` | `EntityTracker`/`TrackedEntity` |
| `ServerLevel`/`MinecraftServer.levels` | `ServerLevel`/`MinecraftServer.levels[3]` |
| `LevelStorageSource`/region `.mca` | `ConsoleSaveFileOriginal`/`McRegionLevelStorage`/`RegionFile` |
| `PlayerList`/`ClientboundPlayerInfoUpdatePacket` | `PlayerList`/`PlayerInfoPacket` |
| `ServerboundKeepAlivePacket` (15 с) | `KeepAlivePacket` каждые 20 тиков (1 с) |

---

## 13. Измеренные факты из этого проекта

- 50 ботов: join=50, chunks=11, сервер жив, ~465 МБ, тик в норме.
- 255 ботов: все подключаются и получают чанки (chunks=7..9), сервер жив, но при 152 игроках `[stats] tick=74219 ms`.
- 255 ботов: ~540 потоков, память ~2.4 ГБ, рост CPU/памяти линейный.
- Реальный клиент: join, 600 чанков, Нижний мир туда/обратно, kick → меню, второй клиент, `stop` — PASS, без новых дампов.
- `readUtf`/`packet stream desync` — следствие рассинхронизации потока, а не самостоятельный баг; новая сборка логирует ID пакета и закрывает соединение.

---

## 14. Итог

Java-сеть — это **слоистый асинхронный конвейер**, рассчитанный на много соединений: frame → compression → encryption → packet codec → protocol state → main-thread game logic. LCE-сеть — это **синхронный консольный стек**, рассчитанный на несколько игроков и QNet: блокирующие сокеты, 2 потока на соединение, плоская таблица пакетов, радиусные чанки и глобальный трекер сущностей. TCP-порт LCE сохранил структуру QNet-протокола, но потерял границы сообщений; отсюда хрупкость к desync и сложность масштабирования.

Для 255 игроков LCE нужно не «переписать всё как в Java», а добавить три вещи, которые дают наибольший эффект при минимальных изменениях:

1. frame/длину пакета в TCP-ветке;
2. чанк-ориентированный entity tracking с `seenBy`;
3. per-player chunk queue с ack/backpressure.

И параллельно — Release-сборку и headless-путь без D3D. Тогда текущая архитектура сможет держать сотни игроков, не превращаясь в Java-сервер целиком.
