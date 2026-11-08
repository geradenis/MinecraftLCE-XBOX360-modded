#include "stdafx.h"
#include "PendingConnection.h"
#include "PlayerConnection.h"
#include "ServerConnection.h"
#include "ServerPlayer.h"
#include "ServerPlayerGameMode.h"
#include "ServerLevel.h"
#include "PlayerList.h"
#include "MinecraftServer.h"
#include "..\Minecraft.World\net.minecraft.network.h"
#include "..\Minecraft.World\pos.h"
#include "..\Minecraft.World\net.minecraft.world.level.dimension.h"
#include "..\Minecraft.World\net.minecraft.world.level.storage.h"
#include "..\Minecraft.World\net.minecraft.world.item.h"
#include "..\Minecraft.World\SharedConstants.h"
#include "Settings.h"
// #ifdef __PS3__
// #include "PS3\Network\NetworkPlayerSony.h"
// #endif

Random *PendingConnection::random = new Random();

static wstring SanitizePlayerName(const wstring& raw)
{
	wstring out;
	for (size_t i = 0; i < raw.length(); ++i)
	{
		wchar_t c = raw[i];
		if (c < 32) continue;
		out.push_back(c);
	}
	while (!out.empty() && out[0] == L' ') out.erase(out.begin());
	while (!out.empty() && out[out.length() - 1] == L' ') out.pop_back();
	if (out.empty()) out = L"Player";
	if (out.length() > (size_t)Player::MAX_NAME_LENGTH)
	{
		out = out.substr(0, Player::MAX_NAME_LENGTH);
	}
	return out;
}

static wstring MakeUniquePlayerName(PlayerList *players, const wstring& base)
{
	if (players == NULL || players->getPlayer(base) == NULL) return base;
	for (int n = 2; n < 10000; ++n)
	{
		wchar_t suffix[16];
		swprintf_s(suffix, 16, L"_%d", n);
		size_t suffixLen = wcslen(suffix);
		size_t keep = base.length();
		if (keep + suffixLen > (size_t)Player::MAX_NAME_LENGTH)
		{
			keep = (size_t)Player::MAX_NAME_LENGTH - suffixLen;
		}
		wstring candidate = base.substr(0, keep) + suffix;
		if (players->getPlayer(candidate) == NULL) return candidate;
	}
	return base;
}

PendingConnection::PendingConnection(MinecraftServer *server, Socket *socket, const wstring& id)
{
	// 4J - added initialisers
	done = false;
    _tick = 0;
    name = L"";
    acceptedLogin = nullptr;
	loginKey = L"";

    this->server = server;
    connection = new Connection(socket, id, this);
    connection->fakeLag = FAKE_LAG;
}

PendingConnection::~PendingConnection()
{
#ifdef _WINDOWS64
	// A client can disconnect before logging in; free its TCP small id so a
	// long-running server does not leak ids.
	if (connection != NULL && connection->getSocket() != NULL)
	{
		INetworkPlayer *np = connection->getSocket()->getPlayer();
		if (np != NULL && !np->IsHost())
		{
			g_NetworkManager.RemoveTcpPlayer(np->GetSmallId());
		}
	}
#endif
	delete connection;
}

void PendingConnection::tick(bool allowLogin)
{
    if (acceptedLogin != NULL && allowLogin)
	{
        this->handleAcceptedLogin(acceptedLogin);
        acceptedLogin = nullptr;
    }
    if (_tick++ == MAX_TICKS_BEFORE_LOGIN)
	{
        disconnect(DisconnectPacket::eDisconnect_LoginTooLong);
    }
	else
	{
        connection->tick();
    }
}

void PendingConnection::disconnect(DisconnectPacket::eDisconnectReason reason)
{
 //   try {	// 4J - removed try/catch
//        logger.info("Disconnecting " + getName() + ": " + reason);
		app.DebugPrintf("Pending connection disconnect: %d\n", reason );
        connection->send( shared_ptr<DisconnectPacket>( new DisconnectPacket(reason) ) );
        connection->sendAndQuit();
        done = true;
//    } catch (Exception e) {
//        e.printStackTrace();
//    }
}

void PendingConnection::handlePreLogin(shared_ptr<PreLoginPacket> packet)
{
	app.DebugPrintf("PendingConnection::handlePreLogin name=%ls netcode=%d (server=%d)\n",
		packet->loginKey.c_str(), (int)packet->m_netcodeVersion, (int)MINECRAFT_NET_VERSION);
    if (packet->m_netcodeVersion != MINECRAFT_NET_VERSION)
	{
		app.DebugPrintf("Netcode version is %d not equal to %d\n", packet->m_netcodeVersion, MINECRAFT_NET_VERSION);
        if (packet->m_netcodeVersion > MINECRAFT_NET_VERSION)
		{
            disconnect(DisconnectPacket::eDisconnect_OutdatedServer);
        }
		else
		{
            disconnect(DisconnectPacket::eDisconnect_OutdatedClient);
        }
        return;
    }
//	printf("Server: handlePreLogin\n");
	// 4J Stu - Change from the login packet as we know better on client end during the pre-login packet.
	// Sanitize and make the name unique: two PC clients may choose the same
	// nickname. The suffix is server-side only; the roster shows the final
	// name.
	name = MakeUniquePlayerName(server->getPlayers(), SanitizePlayerName(packet->loginKey));
	sendPreLoginResponse();
}

void PendingConnection::sendPreLoginResponse()
{
	// 4J Stu - Calculate the players with UGC privileges set
	PlayerUID *ugcXuids = new PlayerUID[MINECRAFT_NET_MAX_PLAYERS];
	DWORD ugcXuidCount = 0;
	DWORD hostIndex = 0;
	BYTE ugcFriendsOnlyBits = 0;
	char szUniqueMapName[14];

	StorageManager.GetSaveUniqueFilename(szUniqueMapName);

	PlayerList *playerList = MinecraftServer::getInstance()->getPlayers();
	for(AUTO_VAR(it, playerList->players.begin()); it != playerList->players.end(); ++it)
	{
		shared_ptr<ServerPlayer> player = *it;
		// If the offline Xuid is invalid but the online one is not then that's guest which we should ignore
		// If the online Xuid is invalid but the offline one is not then we are definitely an offline game so dont care about UGC
		
		// PADDY - this is failing when a local player with chat restrictions joins an online game

		if( player != NULL && player->connection->m_offlineXUID != INVALID_XUID && player->connection->m_onlineXUID != INVALID_XUID )
		{
			if( player->connection->m_friendsOnlyUGC )
			{
				ugcFriendsOnlyBits |= (1<<ugcXuidCount);
			}
			// Need to use the online XUID otherwise friend checks will fail on the client
			ugcXuids[ugcXuidCount] = player->connection->m_onlineXUID;

			if( player->connection->getNetworkPlayer() != NULL && player->connection->getNetworkPlayer()->IsHost() ) hostIndex = ugcXuidCount;

			++ugcXuidCount;
		}
	}

#if 0
    if (false)//	server->onlineMode) // 4J - removed
	{
        loginKey = L"TOIMPLEMENT"; // 4J - todo Long.toHexString(random.nextLong());
        connection->send( shared_ptr<PreLoginPacket>( new PreLoginPacket(loginKey, ugcXuids, ugcXuidCount, ugcFriendsOnlyBits, server->m_ugcPlayersVersion, szUniqueMapName,app.GetGameHostOption(eGameHostOption_All),hostIndex) ) );
    }
	else
#endif
	{
        connection->send( shared_ptr<PreLoginPacket>( new PreLoginPacket(L"-", ugcXuids, ugcXuidCount, ugcFriendsOnlyBits, server->m_ugcPlayersVersion,szUniqueMapName,app.GetGameHostOption(eGameHostOption_All),hostIndex, server->m_texturePackId) ) );
    }
}

void PendingConnection::handleLogin(shared_ptr<LoginPacket> packet)
{
//	printf("Server: handleLogin\n");
    //name = packet->userName;
    if (packet->clientVersion != SharedConstants::NETWORK_PROTOCOL_VERSION)
	{
		app.DebugPrintf("Client version is %d not equal to %d\n", packet->clientVersion, SharedConstants::NETWORK_PROTOCOL_VERSION);
        if (packet->clientVersion > SharedConstants::NETWORK_PROTOCOL_VERSION)
		{
            disconnect(DisconnectPacket::eDisconnect_OutdatedServer);
        }
		else
		{
            disconnect(DisconnectPacket::eDisconnect_OutdatedClient);
        }
        return;
    }

    //if (true)// 4J removed !server->onlineMode)
	bool sentDisconnect = false;

	if( sentDisconnect )
	{
		// Do nothing
	}
	else if( server->getPlayers()->isXuidBanned( packet->m_onlineXuid ) )
	{
		disconnect(DisconnectPacket::eDisconnect_Banned);
	}
	else
	{
        handleAcceptedLogin(packet);
    }
	//else
	{
		//4J - removed
#if 0 
        new Thread() {
            public void run() {
                try {
                    String key = loginKey;
                    URL url = new URL("http://www.minecraft.net/game/checkserver.jsp?user=" + URLEncoder.encode(packet.userName, "UTF-8") + "&serverId=" + URLEncoder.encode(key, "UTF-8"));
                    BufferedReader br = new BufferedReader(new InputStreamReader(url.openStream()));
                    String msg = br.readLine();
                    br.close();
                    if (msg.equals("YES")) {
                        acceptedLogin = packet;
                    } else {
                        disconnect("Failed to verify username!");
                    }
                } catch (Exception e) {
                    disconnect("Failed to verify username! [internal error " + e + "]");
                    e.printStackTrace();
                }
            }
        }.start();
#endif
    }

}

void PendingConnection::handleAcceptedLogin(shared_ptr<LoginPacket> packet)
{
	app.DebugPrintf("PendingConnection::handleAcceptedLogin name=%ls ugc=%u/%u\n",
		name.c_str(), packet->m_ugcPlayersVersion, server->m_ugcPlayersVersion);
	if(packet->m_ugcPlayersVersion != server->m_ugcPlayersVersion)
	{
		// Send the pre-login packet again with the new list of players
		sendPreLoginResponse();
		return;
	}

	// Guests use the online xuid, everyone else uses the offline one
	PlayerUID playerXuid = packet->m_offlineXuid;
	if(playerXuid == INVALID_XUID) playerXuid = packet->m_onlineXuid;

	// Re-check at login time: two clients can both pass PreLogin before the
	// first one is added to the player list.
	name = MakeUniquePlayerName(server->getPlayers(), SanitizePlayerName(name));

    shared_ptr<ServerPlayer> playerEntity = server->getPlayers()->getPlayerForLogin(this, name, playerXuid,packet->m_onlineXuid);
	app.DebugPrintf("PendingConnection::handleAcceptedLogin getPlayerForLogin -> %p\n", playerEntity.get());
    if (playerEntity != NULL)
	{
        server->getPlayers()->placeNewPlayer(connection, playerEntity, packet);
		app.DebugPrintf("PendingConnection::handleAcceptedLogin placeNewPlayer returned\n");
		connection = NULL;	// We've moved responsibility for this over to the new PlayerConnection, NULL so we don't delete our reference to it here in our dtor
    }
    done = true;

}

void PendingConnection::onDisconnect(DisconnectPacket::eDisconnectReason reason, void *reasonObjects)
{
	app.DebugPrintf("PendingConnection::onDisconnect reason=%d done=%d\n", (int)reason, (int)done);
//    logger.info(getName() + " lost connection");
    done = true;
}

void PendingConnection::handleGetInfo(shared_ptr<GetInfoPacket> packet)
{
	//try {
	//String message = server->motd + "�" + server->players->getPlayerCount() + "�" + server->players->getMaxPlayers();
	//connection->send(new DisconnectPacket(message));
	connection->send(shared_ptr<DisconnectPacket>(new DisconnectPacket(DisconnectPacket::eDisconnect_ServerFull) ) );
	connection->sendAndQuit();
	server->connection->removeSpamProtection(connection->getSocket());
	done = true;
	//} catch (Exception e) {
	//	e.printStackTrace();
	//}
}

void PendingConnection::handleKeepAlive(shared_ptr<KeepAlivePacket> packet)
{
	// Ignore
}

void PendingConnection::onUnhandledPacket(shared_ptr<Packet> packet)
{
	disconnect(DisconnectPacket::eDisconnect_UnexpectedPacket);
}

void PendingConnection::send(shared_ptr<Packet> packet)
{
	connection->send(packet);
}

wstring PendingConnection::getName()
{
	return L"Unimplemented";
//        if (name != null) return name + " [" + connection.getRemoteAddress().toString() + "]";
//        return connection.getRemoteAddress().toString();
}

bool PendingConnection::isServerPacketListener()
{
	return true;
}