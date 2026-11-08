#include "stdafx.h"
#include "ProtocolBot.h"

#include "..\\..\\Minecraft.World\\Socket.h"
#include "..\\..\\Minecraft.World\\InputOutputStream.h"
#include "..\\..\\Minecraft.World\\Packet.h"
#include "..\\..\\Minecraft.World\\PreLoginPacket.h"
#include "..\\..\\Minecraft.World\\LoginPacket.h"
#include "..\\..\\Minecraft.World\\LevelType.h"
#include "..\\..\\Minecraft.World\\KeepAlivePacket.h"
#include "..\\..\\Minecraft.World\\MovePlayerPacket.h"
#include "..\\..\\Minecraft.World\\DisconnectPacket.h"
#include "..\\..\\Minecraft.World\\SetSpawnPositionPacket.h"
#include "..\\..\\Minecraft.World\\TeleportEntityPacket.h"
#include "..\\..\\Minecraft.World\\BlockRegionUpdatePacket.h"
#include "..\\..\\Minecraft.World\\ChatPacket.h"
#include "..\\..\\Minecraft.World\\SharedConstants.h"
#include "..\\..\\Minecraft.World\\compression.h"

#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <string>
#include <share.h>

namespace
{
	volatile LONG g_botsRunning = 1;
	CRITICAL_SECTION g_logCS;
	bool g_logCSInit = false;
	FILE *g_log = NULL;

	std::wstring g_botHost;
	int g_botPort = 25565;
	std::wstring g_botPrefix = L"Bot";

	void BotSendPacket(DataOutputStream *dos, DataOutputStream *payloadDos,
		ByteArrayOutputStream *payload, BufferedOutputStream *bos,
		shared_ptr<Packet> packet)
	{
		Packet::writeTcpFrame(packet, dos, payloadDos, payload);
		bos->flush();
	}

	void BotLog(const char *fmt, ...)
	{
		if (g_log == NULL) return;
		char message[1024];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(message, sizeof(message), fmt, ap);
		va_end(ap);

		SYSTEMTIME st;
		GetLocalTime(&st);
		EnterCriticalSection(&g_logCS);
		fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n",
			st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, message);
		fflush(g_log);
		LeaveCriticalSection(&g_logCS);
	}

	struct BotContext
	{
		int index;
	};

	DWORD WINAPI BotThread(LPVOID param)
	{
		BotContext *ctx = (BotContext *)param;
		int index = ctx->index;
		delete ctx;

		Compression::UseDefaultThreadStorage();

		wchar_t name[64];
		swprintf_s(name, 64, L"%ls%03d", g_botPrefix.c_str(), index);
		for (int i = 0; name[i] != 0; ++i)
		{
			if (name[i] == L' ' || name[i] == L'\t') name[i] = L'_';
		}

		double angle = ((double)(index % 64) / 64.0) * 6.28318530718;
		int reconnectDelay = 2000 + (index % 10) * 250;

		while (InterlockedCompareExchange(&g_botsRunning, 1, 1) == 1)
		{
			Socket *socket = new Socket(g_botHost, g_botPort);
			if (socket == NULL || !socket->createdOk)
			{
				BotLog("%ls: connect failed", name);
				if (socket != NULL) delete socket;
				Sleep(reconnectDelay);
				continue;
			}

			DataInputStream *dis = new DataInputStream(socket->getInputStream(false));
			BufferedOutputStream *bos = new BufferedOutputStream(socket->getOutputStream(false), 8192);
			DataOutputStream *dos = new DataOutputStream(bos);
			ByteArrayOutputStream *payload = new ByteArrayOutputStream(8192);
			DataOutputStream *payloadDos = new DataOutputStream(payload);

			BotSendPacket(dos, payloadDos, payload, bos,
				shared_ptr<Packet>(new PreLoginPacket(std::wstring(name))));
			BotLog("%ls: prelogin sent", name);

			bool loginSent = false;
			bool loggedIn = false;
			bool havePosition = false;
			int entityId = -1;
			double spawnX = 0.0, spawnY = 64.0, spawnZ = 0.0;
			unsigned int chunks = 0;
			unsigned int ticks = 0;
			bool alive = true;

			while (alive && InterlockedCompareExchange(&g_botsRunning, 1, 1) == 1)
			{
				shared_ptr<Packet> packet = Packet::readTcpFrame(dis, false);
				if (packet == NULL)
				{
					BotLog("%ls: EOF after %u chunks", name, chunks);
					break;
				}

				switch (packet->getId())
				{
				case 2: // PreLogin response
					{
						shared_ptr<PreLoginPacket> pre = dynamic_pointer_cast<PreLoginPacket>(packet);
						if (!loginSent && pre != NULL)
						{
							shared_ptr<Packet> login(new LoginPacket(std::wstring(name),
								SharedConstants::NETWORK_PROTOCOL_VERSION,
								INVALID_XUID, INVALID_XUID, false,
								pre->m_ugcPlayersVersion, 0, 0, false));
							BotSendPacket(dos, payloadDos, payload, bos, login);
							loginSent = true;
							BotLog("%ls: login sent", name);
						}
					}
					break;
				case 1: // Server -> client login (entity id is in clientVersion)
					{
						shared_ptr<LoginPacket> login = dynamic_pointer_cast<LoginPacket>(packet);
						loggedIn = true;
						entityId = login != NULL ? login->clientVersion : -1;
						BotLog("%ls: logged in entity=%d dim=%d", name,
							entityId,
							login != NULL ? (int)login->dimension : 0);
					}
					break;
				case 6: // Set spawn position (respawn point, not the player position)
					break;
				case 13: // MovePlayerPacket::PosRot: server teleport (y=eye, yView=feet)
					{
						shared_ptr<MovePlayerPacket> tp = dynamic_pointer_cast<MovePlayerPacket>(packet);
						if (tp != NULL && tp->hasPos)
						{
							spawnX = tp->x;
							spawnY = tp->yView; // feet
							spawnZ = tp->z;
							havePosition = true;
							BotLog("%ls: position %.2f %.2f %.2f", name, spawnX, spawnY, spawnZ);
							// First packet must match the server's xLastOk/yLastOk/zLastOk
							// exactly to mark the connection as synched.
							BotSendPacket(dos, payloadDos, payload, bos, shared_ptr<Packet>(
								new MovePlayerPacket::Pos(spawnX, spawnY, spawnY + 1.62, spawnZ, true, false)));
						}
					}
					break;
				case 34: // TeleportEntityPacket
					{
						shared_ptr<TeleportEntityPacket> tp = dynamic_pointer_cast<TeleportEntityPacket>(packet);
						if (tp != NULL && tp->id == entityId)
						{
							spawnX = tp->x / 32.0;
							spawnY = tp->y / 32.0;
							spawnZ = tp->z / 32.0;
							havePosition = true;
							BotLog("%ls: teleport %.2f %.2f %.2f", name, spawnX, spawnY, spawnZ);
							BotSendPacket(dos, payloadDos, payload, bos, shared_ptr<Packet>(
								new MovePlayerPacket::Pos(spawnX, spawnY, spawnY + 1.62, spawnZ, true, false)));
						}
					}
					break;
				case 0: // Keep alive
					{
						shared_ptr<KeepAlivePacket> keepAlive = dynamic_pointer_cast<KeepAlivePacket>(packet);
						if (keepAlive != NULL)
						{
							BotSendPacket(dos, payloadDos, payload, bos,
								shared_ptr<Packet>(new KeepAlivePacket(keepAlive->id)));
						}
					}
					break;
				case 51: // Chunk data
					chunks++;
					break;
				case 255: // Disconnect
					BotLog("%ls: disconnected by server", name);
					alive = false;
					break;
				default:
					break;
				}

				if (loggedIn && havePosition)
				{
					// Circle around the spawn point. The server runs in creative
					// mode for these tests, so the 0.25-block walk check is off.
					angle += 0.02;
					double x = spawnX + cos(angle) * 3.0;
					double z = spawnZ + sin(angle) * 3.0;
					BotSendPacket(dos, payloadDos, payload, bos, shared_ptr<Packet>(
						new MovePlayerPacket::Pos(x, spawnY, spawnY + 1.62, z, true, false)));
					ticks++;
					if ((ticks % 600) == 0)
					{
						// Exercise client -> server chat and the broadcast path.
						BotSendPacket(dos, payloadDos, payload, bos,
							shared_ptr<Packet>(new ChatPacket(L"bot heartbeat")));
					}
					if ((ticks % 200) == 0)
					{
						BotLog("%ls: alive chunks=%u pos=%.1f %.1f %.1f", name, chunks, x, spawnY, z);
					}
				}
			}

			delete dos;
			delete bos;
			delete payloadDos;
			delete payload;
			delete dis;
			socket->close(false);
			delete socket;

			if (InterlockedCompareExchange(&g_botsRunning, 1, 1) == 1)
			{
				Sleep(reconnectDelay);
			}
		}

		BotLog("%ls: thread exit", name);
		return 0;
	}
}

void RunProtocolBots(int count, const std::wstring& prefix,
	const std::wstring& host, int port)
{
	if (count < 1) count = 1;
	if (count > 1000) count = 1000;

	// The packet ID -> factory map is normally built during the full game
	// init. Bot mode runs before that, so build it explicitly; otherwise
	// Packet::readPacket cannot create the server's PreLogin/Login responses.
	Packet::staticCtor();
	// LevelType::getLevelType is used by LoginPacket::read.
	LevelType::staticCtor();

	g_botHost = host;
	g_botPort = port;
	g_botPrefix = prefix.empty() ? L"Bot" : prefix;

	InitializeCriticalSection(&g_logCS);
	g_logCSInit = true;
	g_log = _fsopen("H:\\Temp\\protocol_bots.log", "a", _SH_DENYNO);
	BotLog("starting %d bots -> %ls:%d prefix=%ls", count, g_botHost.c_str(), g_botPort, g_botPrefix.c_str());

	// The compression TLS default must exist before bot threads use it.
	Compression::CreateNewThreadStorage();

	for (int i = 1; i <= count; ++i)
	{
		BotContext *ctx = new BotContext();
		ctx->index = i;
		CreateThread(NULL, 0, BotThread, ctx, 0, NULL);
		if ((i % 25) == 0)
		{
			Sleep(100);
		}
	}

	// Run until the stop file appears or the process is terminated.
	while (true)
	{
		if (GetFileAttributesW(L"H:\\Temp\\bots_stop.txt") != INVALID_FILE_ATTRIBUTES)
		{
			break;
		}
		Sleep(1000);
	}

	InterlockedExchange(&g_botsRunning, 0);
	BotLog("stop requested");
	Sleep(3000);
}
