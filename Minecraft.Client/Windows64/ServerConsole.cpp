#include "stdafx.h"
#include "ServerConsole.h"

#include "..\MinecraftServer.h"
#include "..\Settings.h"
#include "..\PlayerList.h"
#include "..\ServerPlayer.h"
#include "..\ServerLevel.h"
#include "..\PlayerConnection.h"
#include "..\..\Minecraft.World\Level.h"
#include "..\..\Minecraft.World\ChatPacket.h"
#include "..\..\Minecraft.World\SetTimePacket.h"
#include "..\..\Minecraft.World\ServerSettingsChangedPacket.h"
#include "..\..\Minecraft.World\ItemInstance.h"
#include "..\..\Minecraft.World\Item.h"
#include "..\..\Minecraft.World\LevelSettings.h"
#include "..\..\Minecraft.World\DisconnectPacket.h"
#include "..\..\Minecraft.World\StringHelpers.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <sstream>
#include <algorithm>
#include <share.h>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")

bool ServerConsole::s_active = false;
volatile bool ServerConsole::s_stopRequested = false;
CRITICAL_SECTION ServerConsole::s_cs;
std::deque<std::wstring> ServerConsole::s_commands;
int ServerConsole::s_lastProgress = -1;

extern bool g_dedicatedServer;

static std::wstring TrimW(const std::wstring& value)
{
	size_t start = value.find_first_not_of(L" \t\r\n");
	if (start == std::wstring::npos) return L"";
	size_t end = value.find_last_not_of(L" \t\r\n");
	return value.substr(start, end - start + 1);
}

static void ToLowerW(std::wstring& value)
{
	std::transform(value.begin(), value.end(), value.begin(),
		[](wchar_t c) { return (wchar_t)::towlower(c); });
}

void ServerConsole::EnsureCs()
{
	static bool initialised = false;
	if (!initialised)
	{
		InitializeCriticalSection(&s_cs);
		initialised = true;
	}
}

void ServerConsole::Init()
{
	if (s_active) return;

	// Create a real console window. AttachConsole can silently attach to a
	// parent pseudoconsole with no visible window, which is not what a server
	// operator wants.
	if (GetConsoleWindow() == NULL)
	{
		if (!AllocConsole()) return;
	}

	SetConsoleTitleW(L"MinecraftLCE Server");
	SetConsoleOutputCP(CP_UTF8);

	// The command file is a headless test hook. A real console server must
	// not execute stale commands left over from an automated test (a old
	// "stop" line made run_server.bat exit immediately after loading).
	DeleteFileW(L"H:\\Temp\\minecraft_server_cmd.txt");

	FILE *f = NULL;
	freopen_s(&f, "CONOUT$", "w", stdout);
	freopen_s(&f, "CONOUT$", "w", stderr);

	EnsureCs();
	s_active = true;
	s_lastProgress = -1;

	printf("==============================================\n");
	printf(" MinecraftLCE dedicated server (Windows64)\n");
	printf(" Type 'help' for commands.\n");
	printf("==============================================\n");
	printf("> ");
	fflush(stdout);

	CreateThread(NULL, 0, InputThread, NULL, 0, NULL);
}

void ServerConsole::PrintA(const char* fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	// Always mirror dedicated-server output to a file: headless runs
	// (--noconsole) have no console, and automated tests need to read command
	// results. Clients also call PrintA through app.DebugPrintf, so only the
	// dedicated server writes this log.
	static FILE *s_consoleLog = NULL;
	static bool s_consoleLogTried = false;
	if (g_dedicatedServer)
	{
		EnsureCs();
		EnterCriticalSection(&s_cs);
		if (!s_consoleLogTried)
		{
			s_consoleLogTried = true;
			s_consoleLog = _fsopen("H:\\Temp\\server_console.log", "w", _SH_DENYNO);
		}
		if (s_consoleLog != NULL)
		{
			fprintf(s_consoleLog, "%s\n", buf);
			fflush(s_consoleLog);
		}
		LeaveCriticalSection(&s_cs);
	}

	if (!s_active) return;

	EnterCriticalSection(&s_cs);
	fputs(buf, stdout);
	size_t len = strlen(buf);
	if (len == 0 || buf[len - 1] != '\n') fputc('\n', stdout);
	fflush(stdout);
	LeaveCriticalSection(&s_cs);
}

void ServerConsole::Progress(const char* stage, int percent)
{
	if (!s_active) return;
	if (percent < 0) percent = 0;
	if (percent > 100) percent = 100;

	static std::string lastStage;
	if (lastStage != stage)
	{
		lastStage = stage;
		s_lastProgress = -1;
	}
	if (percent == s_lastProgress) return;
	s_lastProgress = percent;

	EnterCriticalSection(&s_cs);
	printf("\r[%s] %d%%   ", stage, percent);
	fflush(stdout);
	if (percent >= 100) printf("\n");
	LeaveCriticalSection(&s_cs);
}

std::string ServerConsole::WstringToUtf8(const std::wstring& value)
{
	if (value.empty()) return std::string();
	int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(), NULL, 0, NULL, NULL);
	if (len <= 0) return std::string();
	std::string out(len, 0);
	WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(), &out[0], len, NULL, NULL);
	return out;
}

DWORD WINAPI ServerConsole::InputThread(LPVOID param)
{
	HANDLE input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (input == INVALID_HANDLE_VALUE) return 0;

	wchar_t line[512];
	DWORD read = 0;
	while (ReadConsoleW(input, line, 511, &read, NULL))
	{
		if (read == 0) continue;
		line[read] = 0;
		std::wstring value = line;
		while (!value.empty() && (value[value.size() - 1] == L'\r' || value[value.size() - 1] == L'\n'))
		{
			value.erase(value.size() - 1);
		}
		if (value.empty()) continue;
		QueueCommandW(value);
		printf("> ");
		fflush(stdout);
	}
	CloseHandle(input);
	return 0;
}

void ServerConsole::QueueCommand(const std::string& line)
{
	QueueCommandW(convStringToWstring(line));
}

void ServerConsole::QueueCommandW(const std::wstring& line)
{
	EnsureCs();
	EnterCriticalSection(&s_cs);
	s_commands.push_back(line);
	LeaveCriticalSection(&s_cs);
}

// Test/automation hook: commands appended to H:\Temp\minecraft_server_cmd.txt
// are executed by the server thread. This makes it possible to drive the
// console without a console window (--noconsole) and without focus.
void ServerConsole::PollCommandFile()
{
#ifdef _WINDOWS64
	static ULONGLONG lastPoll = 0;
	static long long offset = 0;
	static bool initialised = false;

	ULONGLONG now = GetTickCount64();
	if (now - lastPoll < 500) return;
	lastPoll = now;

	const wchar_t *path = L"H:\\Temp\\minecraft_server_cmd.txt";
	HANDLE h = CreateFileW(path, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
	{
		initialised = false;
		offset = 0;
		return;
	}

	LARGE_INTEGER size;
	if (!GetFileSizeEx(h, &size) || size.QuadPart <= offset)
	{
		if (size.QuadPart < offset) { offset = 0; initialised = true; }
		CloseHandle(h);
		return;
	}

	LARGE_INTEGER pos;
	pos.QuadPart = offset;
	if (!SetFilePointerEx(h, pos, NULL, FILE_BEGIN))
	{
		CloseHandle(h);
		return;
	}

	std::string data;
	data.resize((size_t)(size.QuadPart - offset));
	DWORD read = 0;
	BOOL ok = ReadFile(h, &data[0], (DWORD)data.size(), &read, NULL);
	CloseHandle(h);
	if (!ok || read == 0) return;
	data.resize(read);

	size_t lastNewline = data.find_last_of('\n');
	if (lastNewline == std::string::npos) return; // wait for a complete line
	offset += (long long)lastNewline + 1;
	initialised = true;

	size_t start = 0;
	while (start <= lastNewline)
	{
		size_t end = data.find('\n', start);
		std::string line = data.substr(start, end - start);
		start = end + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
		if (line.empty() || line[0] == '#') continue;
		PrintA("[cmd] %s", line.c_str());
		QueueCommandW(convStringToWstring(line));
	}
#endif
}

void ServerConsole::ProcessPendingCommands()
{
	if (MinecraftServer::getInstance() == NULL) return;
	EnsureCs();
	// Only the headless/test mode consumes the command file. With a real
	// console, commands come from the operator's console input thread.
	if (!s_active) PollCommandFile();

	while (true)
	{
		std::wstring line;
		EnterCriticalSection(&s_cs);
		if (s_commands.empty())
		{
			LeaveCriticalSection(&s_cs);
			break;
		}
		line = s_commands.front();
		s_commands.pop_front();
		LeaveCriticalSection(&s_cs);

		ProcessCommand(line);
	}
}

void ServerConsole::PrintHelp()
{
	PrintA("Commands:");
	PrintA("  help                              - this list");
	PrintA("  list                              - players in the session");
	PrintA("  say <message>                     - broadcast a chat message");
	PrintA("  time set day|noon|night|midnight|<ticks>");
	PrintA("  weather clear|rain");
	PrintA("  difficulty <0-3>                  - 0 peaceful .. 3 hard");
	PrintA("  gamemode <0|1> [player]           - 0 survival, 1 creative");
	PrintA("  tp <player> <x> <y> <z>           - teleport a player");
	PrintA("  dim <player> <0|-1|1>             - 0 overworld, -1 nether, 1 end");
	PrintA("  give <player> <itemId> [count]    - give an item");
	PrintA("  kick <player>                     - disconnect a player");
	PrintA("  save                              - save chunks and players");
	PrintA("  seed                              - print the world seed");
	PrintA("  ip [lan|all]                      - show server addresses/port");
	PrintA("  stop                              - save and stop the server");
}

void ServerConsole::PrintServerAddresses(bool includeVirtual)
{
	int port = 25565;
	MinecraftServer *server = MinecraftServer::getInstance();
	if (server != NULL && server->settings != NULL)
	{
		port = server->settings->getInt(L"server-port", 25565);
	}

	PrintA("Server addresses (port %d):", port);
	PrintA("  127.0.0.1:%d  (this PC)", port);

	std::vector<std::string> lanAddresses;
	std::vector<std::string> virtualAddresses;

	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	ULONG size = 16 * 1024;
	std::vector<BYTE> buffer(size);
	PIP_ADAPTER_ADDRESSES addrs = (PIP_ADAPTER_ADDRESSES)&buffer[0];
	ULONG result = GetAdaptersAddresses(AF_INET, flags, NULL, addrs, &size);
	while (result == ERROR_BUFFER_OVERFLOW)
	{
		buffer.resize(size);
		addrs = (PIP_ADAPTER_ADDRESSES)&buffer[0];
		result = GetAdaptersAddresses(AF_INET, flags, NULL, addrs, &size);
	}

	if (result == NO_ERROR)
	{
		for (PIP_ADAPTER_ADDRESSES adapter = addrs; adapter != NULL; adapter = adapter->Next)
		{
			if (adapter->OperStatus != IfOperStatusUp) continue;

			std::wstring adapterName = adapter->FriendlyName ? adapter->FriendlyName : L"";
			std::wstring lowerName = adapterName;
			std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
				[](wchar_t c) { return (wchar_t)::towlower(c); });
			bool virtualAdapter =
				lowerName.find(L"radmin") != std::wstring::npos ||
				lowerName.find(L"vpn") != std::wstring::npos ||
				lowerName.find(L"hyper-v") != std::wstring::npos ||
				lowerName.find(L"vethernet") != std::wstring::npos ||
				lowerName.find(L"virtual") != std::wstring::npos ||
				lowerName.find(L"vmware") != std::wstring::npos ||
				lowerName.find(L"virtualbox") != std::wstring::npos ||
				lowerName.find(L"tailscale") != std::wstring::npos ||
				lowerName.find(L"zerotier") != std::wstring::npos ||
				lowerName.find(L"wireguard") != std::wstring::npos ||
				lowerName.find(L"tap-") != std::wstring::npos;

			bool physical = (adapter->IfType == IF_TYPE_ETHERNET_CSMACD ||
				adapter->IfType == IF_TYPE_IEEE80211) && !virtualAdapter;
			if (!includeVirtual && !physical) continue;

			for (PIP_ADAPTER_UNICAST_ADDRESS unicast = adapter->FirstUnicastAddress;
				unicast != NULL; unicast = unicast->Next)
			{
				sockaddr *sa = unicast->Address.lpSockaddr;
				if (sa == NULL || sa->sa_family != AF_INET) continue;
				sockaddr_in *sin = (sockaddr_in *)sa;
				const unsigned char *b = (const unsigned char *)&sin->sin_addr.S_un.S_addr;
				char ip[32];
				snprintf(ip, sizeof(ip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
				if (strcmp(ip, "127.0.0.1") == 0 || strncmp(ip, "169.254.", 8) == 0) continue;

				char entry[512];
				if (physical)
				{
					// Avoid locale-dependent adapter names in the console;
					// the IP itself is what the client needs.
					snprintf(entry, sizeof(entry), "%s:%d  (LAN/Wi-Fi)", ip, port);
					lanAddresses.push_back(entry);
				}
				else
				{
					snprintf(entry, sizeof(entry), "%s:%d  (%s)", ip, port,
						WstringToUtf8(adapterName).c_str());
					virtualAddresses.push_back(entry);
				}
			}
		}
	}

	if (!lanAddresses.empty())
	{
		PrintA("LAN/Wi-Fi:");
		for (size_t i = 0; i < lanAddresses.size(); ++i) PrintA("  %s", lanAddresses[i].c_str());
	}
	if (includeVirtual && !virtualAddresses.empty())
	{
		PrintA("Virtual/VPN (optional):");
		for (size_t i = 0; i < virtualAddresses.size(); ++i) PrintA("  %s", virtualAddresses[i].c_str());
	}
	if (lanAddresses.empty() && (!includeVirtual || virtualAddresses.empty()))
	{
		PrintA("  (no active IPv4 addresses)");
	}

	PrintA("Use a LAN/Wi-Fi address for a local client: --join <ip>:%d", port);
	PrintA("Use a Virtual/VPN address only when both PCs are in that VPN.");
}

void ServerConsole::PrintPlayerList()
{
	MinecraftServer *server = MinecraftServer::getInstance();
	if (server == NULL) return;
	PlayerList *players = server->getPlayers();
	if (players == NULL) return;

	PrintA("Players: %d", (int)players->players.size());
	for (AUTO_VAR(it, players->players.begin()); it != players->players.end(); ++it)
	{
		shared_ptr<ServerPlayer> player = *it;
		if (player == NULL) continue;
		unsigned char smallId = 0;
		if (player->connection != NULL && player->connection->getNetworkPlayer() != NULL)
		{
			smallId = player->connection->getNetworkPlayer()->GetSmallId();
		}
		PrintA("  %s  smallId=%u  entityId=%d  dim=%d  pos=%.1f %.1f %.1f",
			WstringToUtf8(player->name).c_str(), (unsigned int)smallId, player->entityId,
			player->dimension, player->x, player->y, player->z);
	}
}

void ServerConsole::ProcessCommand(const std::wstring& line)
{
	MinecraftServer *server = MinecraftServer::getInstance();
	if (server == NULL)
	{
		PrintA("Server is not ready yet.");
		return;
	}
	PlayerList *players = server->getPlayers();

	std::wistringstream iss(line);
	std::wstring cmd;
	iss >> cmd;
	ToLowerW(cmd);

	if (cmd == L"help" || cmd == L"?")
	{
		PrintHelp();
	}
	else if (cmd == L"list")
	{
		PrintPlayerList();
	}
	else if (cmd == L"ip" || cmd == L"address" || cmd == L"where")
	{
		std::wstring mode;
		iss >> mode;
		ToLowerW(mode);
		PrintServerAddresses(mode != L"lan");
	}
	else if (cmd == L"say")
	{
		std::wstring message;
		std::getline(iss, message);
		message = TrimW(message);
		if (message.empty())
		{
			PrintA("Usage: say <message>");
			return;
		}
		if (players != NULL)
		{
			players->broadcastAll(shared_ptr<ChatPacket>(new ChatPacket(L"[Server] " + message)));
		}
		PrintA("[Server] %s", WstringToUtf8(message).c_str());
	}
	else if (cmd == L"time")
	{
		std::wstring sub;
		iss >> sub;
		ToLowerW(sub);
		if (sub != L"set")
		{
			PrintA("Usage: time set day|noon|night|midnight|<ticks>");
			return;
		}
		std::wstring value;
		iss >> value;
		ToLowerW(value);
		__int64 ticks = -1;
		if (value == L"day") ticks = 1000;
		else if (value == L"noon") ticks = 6000;
		else if (value == L"sunset") ticks = 12000;
		else if (value == L"night") ticks = 13000;
		else if (value == L"midnight") ticks = 18000;
		else if (value == L"sunrise") ticks = 23000;
		else if (!value.empty()) ticks = _wtoi64(value.c_str());

		ServerLevel *level = server->getLevel(0);
		if (level == NULL || ticks < 0)
		{
			PrintA("Usage: time set day|noon|night|midnight|<ticks>");
			return;
		}
		level->setTime(ticks);
		if (players != NULL)
		{
			players->broadcastAll(shared_ptr<SetTimePacket>(new SetTimePacket(level->getTime())));
		}
		PrintA("Time set to %lld", (long long)ticks);
	}
	else if (cmd == L"weather")
	{
		std::wstring value;
		iss >> value;
		ToLowerW(value);
		ServerLevel *level = server->getLevel(0);
		if (level == NULL)
		{
			PrintA("No overworld level.");
			return;
		}
		if (value == L"rain")
		{
			level->setRainLevel(1.0f);
			PrintA("Weather: rain");
		}
		else if (value == L"clear" || value == L"sun")
		{
			level->setRainLevel(0.0f);
			PrintA("Weather: clear");
		}
		else
		{
			PrintA("Usage: weather clear|rain");
		}
	}
	else if (cmd == L"difficulty")
	{
		std::wstring value;
		iss >> value;
		int difficulty = _wtoi(value.c_str());
		if (difficulty < 0) difficulty = 0;
		if (difficulty > 3) difficulty = 3;
		ServerLevel *level = server->getLevel(0);
		if (level == NULL)
		{
			PrintA("No overworld level.");
			return;
		}
		level->difficulty = difficulty;
		if (players != NULL)
		{
			players->broadcastAll(shared_ptr<ServerSettingsChangedPacket>(
				new ServerSettingsChangedPacket(ServerSettingsChangedPacket::HOST_DIFFICULTY, difficulty)));
		}
		PrintA("Difficulty set to %d", difficulty);
	}
	else if (cmd == L"gamemode")
	{
		std::wstring modeValue, playerName;
		iss >> modeValue >> playerName;
		int mode = _wtoi(modeValue.c_str());
		if (mode != 0 && mode != 1)
		{
			PrintA("Usage: gamemode <0|1> [player]");
			return;
		}
		GameType *gameType = GameType::byId(mode);
		if (players == NULL || gameType == NULL)
		{
			PrintA("No player list.");
			return;
		}
		if (playerName.empty())
		{
			for (AUTO_VAR(it, players->players.begin()); it != players->players.end(); ++it)
			{
				shared_ptr<ServerPlayer> player = *it;
				player->setGameMode(gameType);
				player->setPlayerGamePrivilege(Player::ePlayerGamePrivilege_CreativeMode, mode);
			}
			PrintA("Gamemode set to %d for all players", mode);
		}
		else
		{
			shared_ptr<ServerPlayer> player = players->getPlayer(playerName);
			if (player == NULL)
			{
				PrintA("Player not found: %s", WstringToUtf8(playerName).c_str());
				return;
			}
			player->setGameMode(gameType);
			player->setPlayerGamePrivilege(Player::ePlayerGamePrivilege_CreativeMode, mode);
			PrintA("Gamemode set to %d for %s", mode, WstringToUtf8(playerName).c_str());
		}
	}
	else if (cmd == L"tp")
	{
		std::wstring playerName;
		double x = 0, y = 0, z = 0;
		iss >> playerName >> x >> y >> z;
		if (playerName.empty() || iss.fail())
		{
			PrintA("Usage: tp <player> <x> <y> <z>");
			return;
		}
		shared_ptr<ServerPlayer> player = (players != NULL) ? players->getPlayer(playerName) : nullptr;
		if (player == NULL)
		{
			PrintA("Player not found: %s", WstringToUtf8(playerName).c_str());
			return;
		}
		player->teleportTo(x, y, z);
		if (player->connection != NULL)
		{
			player->connection->teleport(x, y, z, player->yRot, player->xRot);
		}
		PrintA("Teleported %s to %.1f %.1f %.1f", WstringToUtf8(playerName).c_str(), x, y, z);
	}
	else if (cmd == L"dim")
	{
		std::wstring playerName;
		int dimension = 0;
		iss >> playerName >> dimension;
		if (playerName.empty() || iss.fail())
		{
			PrintA("Usage: dim <player> <0|-1|1>");
			return;
		}
		if (dimension != 0 && dimension != -1 && dimension != 1)
		{
			PrintA("Invalid dimension %d (use 0, -1 or 1)", dimension);
			return;
		}
		shared_ptr<ServerPlayer> player = (players != NULL) ? players->getPlayer(playerName) : nullptr;
		if (player == NULL)
		{
			PrintA("Player not found: %s", WstringToUtf8(playerName).c_str());
			return;
		}
		if (player->dimension == dimension)
		{
			PrintA("%s is already in dimension %d", WstringToUtf8(playerName).c_str(), dimension);
			return;
		}
		players->toggleDimension(player, dimension);
		PrintA("Moved %s to dimension %d", WstringToUtf8(playerName).c_str(), dimension);
	}
	else if (cmd == L"give")
	{
		std::wstring playerName, itemValue;
		int count = 1;
		iss >> playerName >> itemValue >> count;
		int itemId = _wtoi(itemValue.c_str());
		if (playerName.empty() || itemId <= 0)
		{
			PrintA("Usage: give <player> <itemId> [count]");
			return;
		}
		shared_ptr<ServerPlayer> player = (players != NULL) ? players->getPlayer(playerName) : nullptr;
		if (player == NULL)
		{
			PrintA("Player not found: %s", WstringToUtf8(playerName).c_str());
			return;
		}
		if (count < 1) count = 1;
		if (count > 64) count = 64;
		shared_ptr<ItemInstance> item = shared_ptr<ItemInstance>(new ItemInstance(itemId, count, 0));
		if (!player->inventory->add(item))
		{
			PrintA("Inventory full for %s", WstringToUtf8(playerName).c_str());
			return;
		}
		player->refreshContainer(player->inventoryMenu);
		PrintA("Gave %d x item %d to %s", count, itemId, WstringToUtf8(playerName).c_str());
	}
	else if (cmd == L"kick")
	{
		std::wstring playerName;
		iss >> playerName;
		shared_ptr<ServerPlayer> player = (players != NULL) ? players->getPlayer(playerName) : nullptr;
		if (player == NULL)
		{
			PrintA("Player not found: %s", WstringToUtf8(playerName).c_str());
			return;
		}
		if (player->connection != NULL)
		{
			player->connection->disconnect(DisconnectPacket::eDisconnect_Kicked);
		}
		PrintA("Kicked %s", WstringToUtf8(playerName).c_str());
	}
	else if (cmd == L"save")
	{
		server->saveAllChunks();
		server->saveGameRules();
		if (players != NULL) players->saveAll(NULL, false);
		PrintA("Saved.");
	}
	else if (cmd == L"seed")
	{
		ServerLevel *level = server->getLevel(0);
		if (level != NULL) PrintA("Seed: %lld", (long long)level->getSeed());
	}
	else if (cmd == L"stop")
	{
		PrintA("Stopping server...");
		server->saveAllChunks();
		server->saveGameRules();
		if (players != NULL) players->saveAll(NULL, false);
		s_stopRequested = true;
		MinecraftServer::HaltServer();
	}
	else if (cmd.empty())
	{
		// ignore
	}
	else
	{
		PrintA("Unknown command: %s (type 'help')", WstringToUtf8(cmd).c_str());
	}
}
