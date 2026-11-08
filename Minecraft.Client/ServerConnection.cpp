#include "stdafx.h"
#include "Options.h"
#include "ServerConnection.h"
#include "PendingConnection.h"
#include "PlayerConnection.h"
#include "ServerPlayer.h"
#include "PlayerList.h"
#include "MinecraftServer.h"
#include "..\Minecraft.World\net.minecraft.network.h"
#include "..\Minecraft.World\Socket.h"
#include "..\Minecraft.World\net.minecraft.world.level.h"
#include "MultiPlayerLevel.h"

ServerConnection::ServerConnection(MinecraftServer *server)
{
	// 4J - added initialiser
	connectionCounter = 0;
	InitializeCriticalSection(&pending_cs);

	this->server = server;
}

ServerConnection::~ServerConnection()
{
	DeleteCriticalSection(&pending_cs);
}

// 4J - added to handle incoming connections, to replace thread that original used to have
void ServerConnection::NewIncomingSocket(Socket *socket)
{
	shared_ptr<PendingConnection> unconnectedClient = shared_ptr<PendingConnection>(new PendingConnection(server, socket, L"Connection #" + _toString<int>(connectionCounter++)));
	handleConnection(unconnectedClient);	
}

void ServerConnection::addPlayerConnection(shared_ptr<PlayerConnection> uc)
{
	players.push_back(uc);
}

void ServerConnection::handleConnection(shared_ptr<PendingConnection> uc)
{
	EnterCriticalSection(&pending_cs);
	pending.push_back(uc);
	LeaveCriticalSection(&pending_cs);
}

void ServerConnection::stop()
{
	EnterCriticalSection(&pending_cs);
    for (unsigned int i = 0; i < pending.size(); i++)
	{
        shared_ptr<PendingConnection> uc = pending[i];
        uc->connection->close(DisconnectPacket::eDisconnect_Closed);
    }
	LeaveCriticalSection(&pending_cs);

    for (unsigned int i = 0; i < players.size(); i++)
	{
        shared_ptr<PlayerConnection> player = players[i];
        player->connection->close(DisconnectPacket::eDisconnect_Closed);
    }
}

void ServerConnection::tick()
{
	{
		// MGH - changed this so that the the CS lock doesn't cover the tick (was causing a lockup when 2 players tried to join)
		EnterCriticalSection(&pending_cs);
		vector< shared_ptr<PendingConnection> > tempPending = pending;
		LeaveCriticalSection(&pending_cs);

		// Accepting a login is expensive: the new player is added to every
		// chunk and every nearby entity sends an add packet. With 255 bots
		// connecting at once this used to run in a single tick and stalled the
		// server for minutes. Spread the accepted logins over ticks.
		static const int MAX_LOGINS_PER_TICK = 2;
		int loginsStarted = 0;
		for (unsigned int i = 0; i < tempPending.size(); i++)
		{
			shared_ptr<PendingConnection> uc = tempPending[i];
			bool allowLogin = loginsStarted < MAX_LOGINS_PER_TICK;
			if (allowLogin && uc->hasAcceptedLogin()) loginsStarted++;
	//        try {	// 4J - removed try/catch
				uc->tick(allowLogin);
	//        } catch (Exception e) {
	//            uc.disconnect("Internal server error");
	//            logger.log(Level.WARNING, "Failed to handle packet: " + e, e);
	//        }
			if(uc->connection != NULL) uc->connection->flush();
		}
	}

	// now remove from the pending list
	EnterCriticalSection(&pending_cs);
	for (unsigned int i = 0; i < pending.size(); i++)
	if (pending[i]->done)
	{
		pending.erase(pending.begin()+i);
		i--;
	}
	LeaveCriticalSection(&pending_cs);

    // Global per-tick chunk budget (the Java server bounds this per player
    // via PlayerChunkSender acks; without acks we bound the whole server tick
    // instead). Players with nothing to send do not consume the budget, so
    // later players are not starved.
    int chunkBudget;
    if (players.size() <= 8) chunkBudget = 1000;		// <= 8 players
    else if (players.size() <= 64) chunkBudget = 64;	// 9..64
    else if (players.size() <= 128) chunkBudget = 32;	// 65..128
    else chunkBudget = 24;								// > 128

    // Tearing down a connection waits for its read/write threads. If 255
    // clients disconnect together, doing all of them in one tick froze the
    // server for ~25 seconds. Process at most one disconnect per tick; the
    // rest stay in the list and are handled on following ticks.
    static const int MAX_DISCONNECTS_PER_TICK = 1;
    int disconnectsThisTick = 0;

    for (unsigned int i = 0; i < players.size(); i++)
	{
        shared_ptr<PlayerConnection> player = players[i];
		bool deferDisconnect = false;
		if (player->isDisconnected())
		{
			if (disconnectsThisTick >= MAX_DISCONNECTS_PER_TICK)
			{
				deferDisconnect = true;
			}
			else
			{
				disconnectsThisTick++;
			}
		}

		shared_ptr<ServerPlayer> serverPlayer = player->getPlayer();
		if( !deferDisconnect && serverPlayer )
		{
			int chunksPerTick = 1;
			if (server != NULL && server->getPlayers() != NULL)
			{
				chunksPerTick = server->getPlayers()->getChunksPerTick();
			}
			// Large sessions stay conservative: serialising hundreds of
			// chunks per tick would stall the server thread.
			if (players.size() > 8 && chunksPerTick > 1) chunksPerTick = 1;

			for (int n = 0; n < chunksPerTick && chunkBudget > 0; ++n)
			{
				if (serverPlayer->doChunkSendingTick(false)) chunkBudget--;
			}
		}
		if (!deferDisconnect)
		{
//        try {	// 4J - removed try/catch
            player->tick();
//        } catch (Exception e) {
//            logger.log(Level.WARNING, "Failed to handle packet: " + e, e);
//            player.disconnect("Internal server error");
//        }
			if (player->done)
			{
				players.erase(players.begin()+i);
				i--;
			}
		}
        if (player->connection != NULL) player->connection->flush();
    }

}

bool ServerConnection::addPendingTextureRequest(const wstring &textureName)
{
	AUTO_VAR(it, find( m_pendingTextureRequests.begin(), m_pendingTextureRequests.end(), textureName));
	if( it == m_pendingTextureRequests.end() )
	{
		m_pendingTextureRequests.push_back(textureName);
		return true;
	}

	// 4J Stu - We want to request this texture from everyone, if we have a duplicate it's most likely because the first person we asked for it didn't have it
	// eg They selected a skin then deleted the skin pack. The side effect of this change is that in certain cases we can send a few more requests, and receive
	// a few more responses if people join with the same skin in a short space of time
	return true;
}

void ServerConnection::handleTextureReceived(const wstring &textureName)
{
	AUTO_VAR(it, find( m_pendingTextureRequests.begin(), m_pendingTextureRequests.end(), textureName));
	if( it != m_pendingTextureRequests.end() )
	{
		m_pendingTextureRequests.erase(it);
	}
	for (unsigned int i = 0; i < players.size(); i++)
	{
        shared_ptr<PlayerConnection> player = players[i];
        if (!player->done)
		{
			player->handleTextureReceived(textureName);
        }
    }
}

void ServerConnection::handleTextureAndGeometryReceived(const wstring &textureName)
{
	AUTO_VAR(it, find( m_pendingTextureRequests.begin(), m_pendingTextureRequests.end(), textureName));
	if( it != m_pendingTextureRequests.end() )
	{
		m_pendingTextureRequests.erase(it);
	}
	for (unsigned int i = 0; i < players.size(); i++)
	{
		shared_ptr<PlayerConnection> player = players[i];
		if (!player->done)
		{
			player->handleTextureAndGeometryReceived(textureName);
		}
	}
}

void ServerConnection::handleServerSettingsChanged(shared_ptr<ServerSettingsChangedPacket> packet)
{
	Minecraft *pMinecraft = Minecraft::GetInstance();

	if(packet->action==ServerSettingsChangedPacket::HOST_DIFFICULTY)
	{
		for(unsigned int i = 0; i < pMinecraft->levels.length; ++i)
		{
			if( pMinecraft->levels[i] != NULL )
			{
				app.DebugPrintf("ClientConnection::handleServerSettingsChanged - Difficulty = %d",packet->data);
				pMinecraft->levels[i]->difficulty = packet->data;
			}
		}	
	}
// 	else if(packet->action==ServerSettingsChangedPacket::HOST_IN_GAME_SETTINGS)// options
// 	{
// 		app.SetGameHostOption(eGameHostOption_All,packet->m_serverSettings)
// 	}
// 	else
// 	{
// 		unsigned char ucData=(unsigned char)packet->data;
// 		if(ucData&1)
// 		{
// 			// hide gamertags
// 			pMinecraft->options->SetGamertagSetting(true);
// 		}
// 		else
// 		{
// 			pMinecraft->options->SetGamertagSetting(false);
// 		}
// 
// 		for (unsigned int i = 0; i < players.size(); i++)
// 		{
// 			shared_ptr<PlayerConnection> playerconnection = players[i];
// 			playerconnection->setShowOnMaps(pMinecraft->options->GetGamertagSetting());				
// 		}
// 	}
}