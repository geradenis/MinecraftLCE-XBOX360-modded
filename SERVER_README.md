# MinecraftLCE PC: dedicated-server prototype (Windows64)

This build turns the Windows64 client into a small dedicated-server + TCP
client pair. The same `Minecraft.Client.exe` is used for both roles; the
command line selects the mode.

## Quick start

Build first (from `M2-working-copy`):

```powershell
powershell -ExecutionPolicy Bypass -File .\build_pc_x64_debug.ps1
```

Output: `Minecraft.Client\bin\x64\Debug\Minecraft.Client.exe`.

### Server

```bat
cd Minecraft.Client\bin\x64\Debug
run_server.bat
```

`run_server.bat` starts:

```
Minecraft.Client.exe --server --headless
```

The server creates/hosts the world configured by `server.properties`
(`level-name`, default `TestWorld`), listens on `server-port` (default
`25565`) and accepts TCP clients from any interface (`0.0.0.0`).

On a separate server machine allow inbound TCP `25565` in Windows Firewall,
and use that machine's LAN IP in the client (`run_client.bat 192.168.1.10:25565`).

`--headless` only hides the window; the renderer still runs, so the server
machine needs a working Direct3D 11 device.

Extra launchers in the same folder:

- `run_server_flat.bat` - flat test world, visible window.
- `run_server_windowed.bat` - normal world, visible window (debugging).

To host an existing save, start the client normally (no `--server`), use
Play Game to load the world, and leave it running. The TCP listener starts
with the world; clients connect the same way.

`--server` disables the Miles sound engine by default (servers often have no
audio device and `mss64.dll` can crash on startup). Use `--sound` to keep
audio, or `--nosound` on a client for testing multiple instances on one PC.

### Client

```bat
cd Minecraft.Client\bin\x64\Debug
run_client.bat 192.168.1.10:25565
```

or simply double-click `run_client.bat` and type the address. With no
argument the client reads `server.txt` (`ip` or `ip:port`).

`run_client.bat` also asks for a nickname (or accepts it as the second
argument). On Windows64 the nickname can also be set with:

```
Minecraft.Client.exe --join 192.168.1.10:25565 --name "Steve"
Minecraft.Client.exe --name "Steve"          (normal host / menu)
```

`--nick` is an alias. If `--name` is omitted, the client uses the first
line of `client_name.txt`; if that is missing it falls back to
`Player<pid>`. Names are sanitized, limited to 20 characters, and the
server appends `_2`, `_3`, ... if two players choose the same nickname.
On Xbox 360 the name is the profile gamertag, not a free-form field.

The old manual path still works: Play Game -> Join Game -> the entry
`Direct TCP Server (ip:port)` -> Join Game.

## Server console

`--server` creates a real console window (the LCE equivalent of Java's
`nogui`). It prints the startup log and world-generation progress, and accepts
commands:

```
help
list
ip
say <message>
time set day|noon|night|midnight|<ticks>
weather clear|rain
difficulty <0-3>
gamemode <0|1> [player]
tp <player> <x> <y> <z>
dim <player> <0|-1|1>
give <player> <itemId> [count]
kick <player>
save
seed
stop
```

`ip` (aliases `address`, `where`) prints the loopback address, every
non-loopback IPv4 address of the server machine and the configured port.
The same list is printed once at startup, so the operator can tell clients
which `--join <ip>:<port>` to use. For a real remote client, allow inbound
TCP `server-port` in Windows Firewall and use the machine's LAN/public IP
(port-forward the port on the router if the client is outside the LAN).

`dim` moves a player between the Overworld (0), Nether (-1) and End (1).
The same command file hook used by the automated tests works with
`--noconsole`: lines appended to `H:\Temp\minecraft_server_cmd.txt` are
executed by the server thread.

The server package uses a separate `Minecraft.Server.exe`; `run_server.bat`
starts it with a hidden game window but a visible console.

## Configuration files

- `server.properties` (server): `max-players` (up to 255), `server-port`,
  `level-name`, `gamemode`, spawn toggles, `chunks-per-tick` (default 9,
  matching Java Edition's `START_CHUNKS_PER_TICK`; large sessions fall back
  to 1 automatically), `view-distance` (3..12, the client renderer's fixed
  distance is 12) and `simulation-distance` (entity tick range,
  2..view-distance). The server also enforces a global per-tick chunk budget
  so a large session cannot stall the tick.
- `server.txt` (client): first line `ip` or `ip:port`.
- `client_name.txt` (client, optional): first line is the display name.
  Without it the client uses `Player<PID>`, which is unique per process.
  `--name`/`--nick` overrides this file. On Xbox the profile gamertag is
  used instead.

## What is implemented

- TCP transport inside `Socket` (Windows64 only; Xbox QNet code untouched).
- TCP framing for Windows64: every packet is sent as
  `magic "LCEF" + version + payload length + payload`. QNet preserved
  message boundaries; raw TCP does not, so without this a single damaged
  packet shifted the whole stream (the old `readUtf`/`packet stream desync`
  failure). The frame reader rejects a bad frame and closes only that
  connection. Client, server and bots must be built from the same version.
- Server listener on `server-port`, accepts many simultaneous clients.
- Per-client small id assigned by the server; the client adopts the id from
  `LoginPacket`.
- Unique client names and a session roster (`PlayerInfoPacket` carries the
  display name), so all clients see all players in the player list even at
  distance.
- `PlayerInfoPacket`/`AddPlayerPacket` are delivered to every client
  (`canReceiveAllPackets` fix): all players see each other's entities.
- Small ids are freed when a player leaves.
- `--server` is a true dedicated host: **no local host player is added**
  to the world. The console owns the world and TCP clients are the only
  players. Normal client hosting (Play Game) still creates the host player.
- `--server` does not poll the gamepad or the game window's keyboard. When
  the server and an interactive client run on the same PC, controller
  input only reaches the client. Console commands still work through the
  separate `ServerConsole` input thread.
- `MINECRAFT_NET_MAX_PLAYERS = 255`. In `--server` mode up to 255 TCP
  clients (no local host); normal hosting keeps the host player plus TCP
  clients.
- Login throttling: at most two accepted logins are processed per server
  tick. Accepting a player sends the roster and every nearby entity to the
  new client and to everyone already online; doing 255 at once used to make
  one server tick take minutes.
- Entity tracking has fast paths for TCP: the splitscreen visibility scan
  and the `seenBy x sentTo` dedup loop are skipped when no player has a
  same-system sibling. Movement packets are coalesced per tick (first and
  newest position/rotation are kept), so a client that sends faster than
  20 TPS cannot flood the server thread.
- Per-connection `Packet::s_readError` is thread-local, so an error on one
  connection cannot reject a valid packet on another connection.
- PC nickname selection: `--name`/`--nick`, `client_name.txt`, or the
  prompt in `run_client.bat`. Names are sanitized, limited to 20
  characters, and made unique on the server (`Name_2`, `Name_3`, ...).
  Xbox names remain the profile gamertag.
- Disconnects are throttled to one connection teardown per tick, so a mass
  logout does not freeze the server in a single 25-second tick.
- Network entity ids expanded from 2048 to 16384, so 255 players plus world
  entities fit.
- `--server`, `--join ip:port`, `--headless`, `--flat`, `--nosound`
  command-line modes. Dedicated servers default to no sound.
- `--bots N --bot-prefix Name --join ip:port` runs N lightweight protocol
  bots in one process (no window, no D3D, no world) for load testing up to
  the 255-player cap. Bots log to `H:\Temp\protocol_bots.log`. Bot mode
  calls `Packet::staticCtor()` explicitly because it runs before the normal
  game init (without it the packet factory map is empty and the bot cannot
  parse the server's PreLogin/Login responses).

## Tested

- 3 TCP clients: server-assigned small ids 1-3; every client received
  `AddPlayerPacket` for the host and both other clients; nametags and player
  models were visible in-game.
- 5 TCP clients: small ids 1-5; every client's roster contained the host and
  all other clients; no crashes with `--nosound`.
- 10 TCP clients: all joined (small ids 1-10), ran for 100 s with a server
  tick of ~31 ms and ~309 MB working set; killing 5 processes left the other
  5 players and the server removed the ghosts immediately.
- Crash diagnostics: `--crash-test` produces `H:\Temp\crashes\*.dmp` and a
  symbolized stack with file/line in `H:\Temp\server_crash.log`.
- Final regression on the restored save: client join, 600 chunks, Nether
  round trip, kick returns to the main menu, kick removes the player, a
  second client joins, `stop` with a connected client exits cleanly with no
  new crash dump. A malformed/partial packet ID no longer triggers the
  debug `assert` that used to crash the server during shutdown.
- 255 protocol bots (view-distance 3, simulation-distance 2): all 255
  joined, every bot received chunks, server stayed alive for the full
  stability window, and the steady-state tick was **31-32 ms** (about
  20 TPS) with roughly 700-740 MB working set and 540 threads. Before the
  tracker/login/movement fixes the same test showed a single 373-second
  tick and could not get all 255 players in.
- 50 protocol bots: all joined, received chunks, and were removed from the
  roster (`list=0`) after the bot process stopped. The ghost-player bug was
  a queue-drain deadlock: after EOF, `Connection::tick` refused to drain the
  remaining incoming packets and therefore never reached `onDisconnect`.
- Nickname test: two PC clients started with `--name TestNick` produced
  `TestNick` and `TestNick_2` in the server roster. A normal client without
  `--name` still joins as `Player<pid>`.
- LAN address test: the `ip` command listed `192.168.0.209:25565` (plus
  loopback/VPN/Hyper-V addresses), and a client launched with
  `--join 192.168.0.209:25565 --name LanTest` joined successfully. This
  confirms the server listener is reachable through a non-loopback IP.
- Real TCP client after the framing change: join, chunk stream, kick to
  menu, second join and clean `stop` all passed with no new dumps.
- Packaged `dist` server/client folders: server listened on 25565 and a
  client from the packaged folder joined successfully.

## Logs

Development builds write to `H:\Temp`:

- `server_crash.log` - unhandled exceptions and CRT asserts, with a
  symbolized stack (file/line) and `H:\Temp\crashes\*.dmp` minidumps.
- `server_debug.log` - the last `app.DebugPrintf` events before a crash.
- `client_<pid>_crash.log`, `client_<pid>_debug.log` - per-client versions.
- `server_console.log` - headless console mirror (command output).
- `tcp_join.log` - client join attempt and TCP connect result.
- `tcp_players.log` - TCP roster add/remove events.
- `protocol_bots.log` - bot load-test progress.

If `H:\Temp` does not exist the writes fail silently and gameplay is not
affected.

## Known limitations

- The server still renders a window and runs the normal game loop. It is not
  a fully headless process yet.
- The Windows64 save heap is no longer limited to the console 2 GB value:
  it reserves up to 64 GB of virtual address space and commits pages only
  as the save grows. The 64-bit process otherwise uses OS-managed RAM; the
  save file format itself still uses 32-bit offsets, so one save file is
  practically limited to 4 GB.
- Chunk generation is synchronous on the server thread; large worlds can
  still cause tick spikes. Chunk sending is bounded by a global per-tick
  budget and per-player queues. There is still no client chunk-batch
  acknowledgement, so the budget is global rather than per player.
- Login is throttled to two players per tick. A very large simultaneous
  connect (255 at once) therefore takes several seconds to drain, but the
  server keeps ticking instead of freezing for minutes.
- Movement packets are coalesced on the server. This is safe for normal
  clients (20 packets/s) and prevents test bots from starving the tick;
  intermediate positions are intentionally not replayed.
- Disconnecting many players at the same instant is still serialized, but
  now at most one connection is torn down per tick. The server keeps
  ticking (roughly 100 ms ticks while the queue drains) instead of freezing
  in one 25-second tick. Normal disconnects and the 50-bot test are fast.
- Debug builds now log `__debugbreak`/CRT asserts with a symbolized stack
  and minidump instead of closing silently. This is intended to diagnose
  host-mode failures on existing saves; the log is
  `H:\Temp\client_<pid>_crash.log`.
- The Debug client's chunk rebuild is very slow (the 4J renderer libraries
  are debug builds). On a fresh world the client can spend a long time
  rebuilding the first ~400 chunks. Use the existing `world` save and keep
  `view-distance=12` (the client renderer's fixed view distance) for normal
  play; the bot load tests use a smaller view distance because bots do not
  render.
- Xbox 360 clients now use the TCP transport through
  `CPlatformNetworkManagerStub`. The client discovers `Direct TCP Server`
  in `Join Game`, completes PreLogin/Login and is added to the world. The
  world load still hangs in Xenia at "Connecting to host"; see
  `Xbox360-Build/README_RU.md` for the PoC status and next steps.
