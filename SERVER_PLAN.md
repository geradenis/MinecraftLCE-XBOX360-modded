# Выделенный сервер для MinecraftLCE (Xbox 360) — план

## Цель

PC-сервер, к которому подключаются Xbox 360-клиенты. Долгосрочная цель — масштабирование до 1000 игроков. Это **не** правка константы `MINECRAFT_NET_MAX_PLAYERS`: нужен отдельный серверный процесс и замена транспорта.

## Что уже есть в коде

- Игровой протокол: `Connection` (framing), `PendingConnection` (PreLogin/Login), `PlayerConnection`, пакеты `Packet`.
- Серверная логика: `MinecraftServer`, `ServerConnection`, `PlayerList`, `ServerLevel`, `PlayerChunkMap`, `EntityTracker`.
- `Socket` — абстракция транспорта: локальные очереди + QNet-очереди (`m_queueNetwork`, `networkPlayerSmallId`).
- Windows64-сборка (`MinecraftPC.sln`, `build_pc_x64_debug.ps1`) — клиент, но сетевой менеджер `PlatformNetworkManagerStub` локальный (loopback).
- Xbox 360 — QNet / Xbox Live, peer-to-peer; хост является игроком.

## Архитектура

```
Xbox 360 client (modded) ── TCP ──┐
                                  ├── MinecraftServer (headless, PC)
PC client (debug) ─────── TCP ────┘
```

- **Транспорт:** TCP, те же framed-пакеты, что и в `Connection` (длина + id). Новый `TcpSocket`/`TcpServer` вместо QNet.
- **Сервер:** headless-процесс, `MinecraftServer` + `Minecraft.World`, без рендера/UI. `Minecraft::GetInstance()`-зависимости (progressRenderer, gameRenderer, options) заменяются заглушками.
- **Клиент Xbox:** модифицированная сборка с TCP-`Socket` и прямым подключением по IP (RGH/JTAG), либо мост QNet↔TCP для Xenia.
- **Масштабирование:** interest management — каждому игроку отправляются только чанки/сущности в радиусе; spatial hash; бюджет пакетов на тик.

## Этапы

### 0. Аудит и транспортный прототип
- `TcpSocket` (Winsock) с `InputStream`/`OutputStream`, совместимый с `Connection`.
- `TcpServer`: accept-loop, `NewIncomingSocket`.
- Тест: два процесса обмениваются `KeepAlive`/`PreLogin` пакетами.
- **Результат:** протокол работает поверх TCP.

### 1. Headless-сервер + PC-клиент
- `MinecraftServer` в отдельном режиме (`--server`), без окна.
- Заглушки `Minecraft`/`ProgressRenderer`/`GameRenderer`/`Options`.
- PC-клиент подключается по `127.0.0.1:25565`, видит мир и других игроков.
- **Результат:** 2–8 PC-клиентов в одном мире.

### 2. Xbox-клиент
- Вариант A (RGH/JTAG): TCP-`Socket` в Xbox-сборке + экран Direct Connect.
- Вариант B (Xenia): мост QNet↔TCP или патч Xenia netplay.
- **Результат:** Xbox-клиент заходит на PC-сервер.

### 3. Поднятие лимита игроков
- `MINECRAFT_NET_MAX_PLAYERS`, `SessionInfo` (QoS 1020 байт), `PlayerList` (индексы), `PlayerRenderer` (цвета), `PendingConnection` (UGC-биты), UI.
- Small ID: 8 бит (255) — для 1000 нужен новый ID-механизм.
- **Результат:** 16–32 игрока.

### 4. Масштабирование
- Interest management: чанки/сущности по радиусу, приоритеты, бюджеты.
- Асинхронная отправка, сжатие, батчинг.
- **Результат:** 100+ PC-клиентов на сервере.

### 5. 1000 игроков
- Пространственная БД, шардирование, прокси, клиентский culling.
- Xbox 360-клиент в 1000 игроков — отдельная проблема (UI/render/network).

## Жёсткие ограничения

- **QoS-буфер 1020 байт** (`SessionInfo.h`): список игроков в сессии — максимум ~40.
- **Small ID 8 бит** (`GetPlayerBySmallId(unsigned char)`): максимум 255.
- **Xbox Live/QNet**: лимиты слотов сессии/party (8/16/32).
- **Xbox 360 железо**: один хост не выдержит 1000 клиентов.
- **Клиент**: player list, nametag, entity tracker, UI рассчитаны на 8.

## Что нужно решить

1. Приоритет клиентов: **PC (отладка) → Xenia → RGH** или сразу RGH?
2. Есть ли RGH/JTAG консоль для тестов?
3. Можно ли запускать Windows64-сборку на этом PC для отладки сервера?

## Текущий статус

- [x] Аудит транспорта и серверной логики.
- [x] TcpSocket/TcpServer (Windows64, порт из `server.properties`).
- [x] Серверный режим `--server` (окно скрывается через `--headless`).
- [x] PC-клиент `--join ip:port` / `server.txt`, ручной Join Game.
- [x] Мультиклиентность: серверные small id, уникальные имена, общий roster.
- [x] Все клиенты получают `AddPlayerPacket` друг о друге (фикс `IsSameSystem`).
- [x] Лимит `MINECRAFT_NET_MAX_PLAYERS = 255` (хост + 254 клиента).
- [x] Сетевые entity id расширены с 2048 до 16384.
- [x] `--nosound` и тихий режим сервера (Miles не открывает аудиоустройство).
- [ ] Xbox-мост.
- [ ] Полностью headless-сервер без рендера.
- [ ] Загрузка существующего мира на сервере.
- [ ] Асинхронная отправка / 100+ / 1000.

