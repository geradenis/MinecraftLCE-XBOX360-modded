#include "stdafx.h"	
#include "..\..\..\Minecraft.World\Socket.h"
#include "..\..\..\Minecraft.World\StringHelpers.h"
#include "PlatformNetworkManagerStub.h"
#include "..\..\User.h"
#include "..\..\Xbox\Network\NetworkPlayerXbox.h"		// TODO - stub version of this?

CPlatformNetworkManagerStub *g_pPlatformNetworkManager;


void CPlatformNetworkManagerStub::NotifyPlayerJoined(IQNetPlayer *pQNetPlayer	)
{
	const char * pszDescription;

	// 4J Stu - We create a fake socket for every where that we need an INBOUND queue of game data. Outbound
	// is all handled by QNet so we don't need that. Therefore each client player has one, and the host has one
	// for each client player.
	bool createFakeSocket = false;
	bool localPlayer = false;

	NetworkPlayerXbox *networkPlayer = (NetworkPlayerXbox *)addNetworkPlayer(pQNetPlayer);

    if( pQNetPlayer->IsLocal() )
    {
		localPlayer = true;
        if( pQNetPlayer->IsHost() )
        {
            pszDescription = "local host";
			// 4J Stu - No socket for the localhost as it uses a special loopback queue

			m_machineQNetPrimaryPlayers.push_back( pQNetPlayer );
        }
        else
        {
            pszDescription = "local";

			// We need an inbound queue on all local players to receive data from the host
			createFakeSocket = true;
        }
    }
    else
    {
        if( pQNetPlayer->IsHost() )
        {
            pszDescription = "remote host";
        }
        else
        {
            pszDescription = "remote";

			// If we are the host, then create a fake socket for every remote player
			if( m_pIQNet->IsHost() )
			{
				createFakeSocket = true;
			}
        }

		if( m_pIQNet->IsHost() && !m_bHostChanged )
		{
			// Do we already have a primary player for this system?
			bool systemHasPrimaryPlayer = false;
			for(AUTO_VAR(it, m_machineQNetPrimaryPlayers.begin()); it < m_machineQNetPrimaryPlayers.end(); ++it)
			{
				IQNetPlayer *pQNetPrimaryPlayer = *it;
				if( pQNetPlayer->IsSameSystem(pQNetPrimaryPlayer) )
				{
					systemHasPrimaryPlayer = true;
					break;
				}
			}
			if( !systemHasPrimaryPlayer )
				m_machineQNetPrimaryPlayers.push_back( pQNetPlayer );
		}
    }
	g_NetworkManager.PlayerJoining( networkPlayer );
	
	if( createFakeSocket == true && !m_bHostChanged )
	{
		g_NetworkManager.CreateSocket( networkPlayer, localPlayer );
	}

    app.DebugPrintf( "Player 0x%p \"%ls\" joined; %s; voice %i; camera %i.\n",
        pQNetPlayer,
        pQNetPlayer->GetGamertag(),
        pszDescription,
        (int) pQNetPlayer->HasVoice(),
        (int) pQNetPlayer->HasCamera() );


	if( m_pIQNet->IsHost() )
	{
		// 4J-PB - only the host should do this
//		g_NetworkManager.UpdateAndSetGameSessionData();
		SystemFlagAddPlayer( networkPlayer );
	}
	
	for( int idx = 0; idx < XUSER_MAX_COUNT; ++idx)
	{
		if(playerChangedCallback[idx] != NULL)
			playerChangedCallback[idx]( playerChangedCallbackParam[idx], networkPlayer, false );
	}

	if(m_pIQNet->GetState() == QNET_STATE_GAME_PLAY)
	{
		int localPlayerCount = 0;
		for(unsigned int idx = 0; idx < XUSER_MAX_COUNT; ++idx)
		{
			if( m_pIQNet->GetLocalPlayerByUserIndex(idx) != NULL ) ++localPlayerCount;
		}

		float appTime = app.getAppTime();

		// Only record stats for the primary player here
		m_lastPlayerEventTimeStart = appTime;
	}
}

bool CPlatformNetworkManagerStub::Initialise(CGameNetworkManager *pGameNetworkManager, int flagIndexSize)
{
	m_pGameNetworkManager = pGameNetworkManager;
	m_flagIndexSize = flagIndexSize;
	g_pPlatformNetworkManager = this;
	for( int i = 0; i < XUSER_MAX_COUNT; i++ )
	{
		playerChangedCallback[ i ] = NULL;
	}
	
	m_bLeavingGame = false;
	m_bLeaveGameOnTick = false;
	m_bHostChanged = false;

	m_bSearchResultsReady = false;
	m_bSearchPending = false;

	m_bIsOfflineGame = false;
	m_bTcpClient = false;
	InitializeCriticalSection(&m_tcpPlayersCS);
	m_tcpServerIp = L"127.0.0.1";
	m_tcpServerPort = 25565;
#ifdef _XBOX
	// Direct TCP client PoC default. The optional server.txt below overrides
	// this if the platform can read it.
	m_tcpServerIp = L"192.168.0.209";
	m_tcpServerPort = 25565;
#endif

	// Optional client configuration: a server.txt file next to the executable
	// with "ip" or "ip:port" on the first line. The --connect/--join command
	// line overrides this.
	{
		FILE *sf = NULL;
#ifdef _XBOX
		// Xenia mounts the game's res directory; the root path has no device.
		fopen_s(&sf, "\\res\\server.txt", "r");
#else
		fopen_s(&sf, "server.txt", "r");
#endif
		if (sf != NULL)
		{
			char narrow[128] = {0};
			if (fgets(narrow, sizeof(narrow), sf) != NULL)
			{
				for (int i = 0; narrow[i] != 0; ++i)
				{
					if (narrow[i] == '\r' || narrow[i] == '\n') { narrow[i] = 0; break; }
				}
				wstring addr = convStringToWstring(narrow);
				size_t colon = addr.find(L':');
				if (colon != wstring::npos)
				{
					m_tcpServerPort = _wtoi(addr.substr(colon + 1).c_str());
					addr = addr.substr(0, colon);
				}
				if (!addr.empty()) m_tcpServerIp = addr;
			}
			fclose(sf);
		}
	}
	m_pSearchParam = NULL;
	m_SessionsUpdatedCallback = NULL;

	for(unsigned int i = 0; i < XUSER_MAX_COUNT; ++i)
	{
		m_searchResultsCount[i] = 0;
		m_lastSearchStartTime[i] = 0;

		// The results that will be filled in with the current search
		m_pSearchResults[i] = NULL;
		m_pQoSResult[i] = NULL;
		m_pCurrentSearchResults[i] = NULL;
		m_pCurrentQoSResult[i] = NULL;
		m_currentSearchResultsCount[i] = 0;
	}

    // Success!
    return true;
}

void CPlatformNetworkManagerStub::Terminate()
{
	EnterCriticalSection(&m_tcpPlayersCS);
	for (size_t i = 0; i < m_tcpPlayers.size(); ++i) delete m_tcpPlayers[i];
	m_tcpPlayers.clear();
	for (size_t i = 0; i < m_retiredTcpPlayers.size(); ++i) delete m_retiredTcpPlayers[i];
	m_retiredTcpPlayers.clear();
	LeaveCriticalSection(&m_tcpPlayersCS);
	DeleteCriticalSection(&m_tcpPlayersCS);
}

int CPlatformNetworkManagerStub::GetJoiningReadyPercentage()
{
	return 100;
}

int CPlatformNetworkManagerStub::CorrectErrorIDS(int IDS)
{
	return IDS;
}

bool CPlatformNetworkManagerStub::isSystemPrimaryPlayer(IQNetPlayer *pQNetPlayer)
{
	return true;
}

// We call this twice a frame, either side of the render call so is a good place to "tick" things
void CPlatformNetworkManagerStub::DoWork()
{
}

int CPlatformNetworkManagerStub::GetPlayerCount()
{
	EnterCriticalSection(&m_tcpPlayersCS);
	int tcpCount = (int)m_tcpPlayers.size();
	LeaveCriticalSection(&m_tcpPlayersCS);
	// On a TCP client m_tcpPlayers already contains the local player, the
	// host and every remote player the server told us about.
	if (m_bTcpClient) return tcpCount;
	return m_pIQNet->GetPlayerCount() + tcpCount;
}

bool CPlatformNetworkManagerStub::ShouldMessageForFullSession()
{
	return false;
}

int CPlatformNetworkManagerStub::GetOnlinePlayerCount()
{
	EnterCriticalSection(&m_tcpPlayersCS);
	int tcpCount = (int)m_tcpPlayers.size();
	LeaveCriticalSection(&m_tcpPlayersCS);
	if (m_bTcpClient) return tcpCount;
	return 1 + tcpCount;
}

int CPlatformNetworkManagerStub::GetLocalPlayerMask(int playerIndex)
{
	return 1 << playerIndex;
}

bool CPlatformNetworkManagerStub::AddLocalPlayerByUserIndex( int userIndex )
{
	// On a TCP client the local player already exists in m_tcpPlayers
	// (created by JoinGame), and there is no QNet session to add it to.
	if (m_bTcpClient) return true;
	NotifyPlayerJoined(m_pIQNet->GetLocalPlayerByUserIndex(userIndex));
	return ( m_pIQNet->AddLocalPlayerByUserIndex(userIndex) == S_OK );
}

bool CPlatformNetworkManagerStub::RemoveLocalPlayerByUserIndex( int userIndex )
{
	return true;
}

bool CPlatformNetworkManagerStub::IsInStatsEnabledSession()
{
	return true;
}

bool CPlatformNetworkManagerStub::SessionHasSpace(unsigned int spaceRequired /*= 1*/)
{
	return true;
}

void CPlatformNetworkManagerStub::SendInviteGUI(int quadrant)
{
}

bool CPlatformNetworkManagerStub::IsAddingPlayer()
{
	return false;
}

bool CPlatformNetworkManagerStub::LeaveGame(bool bMigrateHost)
{
	app.DebugPrintf("LeaveGame: entered (tcpClient=%d, leaving=%d)\n", (int)m_bTcpClient, (int)m_bLeavingGame);
	if( m_bLeavingGame ) return true;

	m_bLeavingGame = true;

#if defined(_WINDOWS64) || defined(_XBOX)
	if (m_bTcpClient)
	{
		// Dedicated-server client: there is no local QNet session to end and
		// no server to wait for. Close every TCP socket so the server sees
		// EOF immediately (instead of keeping a frozen player), then clear
		// the TCP session state. Without this IsInSession() stays true and
		// IUIScene_PauseMenu::_ExitWorld waits forever before showing the
		// main menu.
		EnterCriticalSection(&m_tcpPlayersCS);
		for (size_t i = 0; i < m_tcpPlayers.size(); ++i)
		{
			Socket *socket = m_tcpPlayers[i]->GetSocket();
			if (socket != NULL)
			{
				socket->close(false);
				m_tcpPlayers[i]->SetSocket(NULL);
			}
			// Keep the INetworkPlayer objects alive: UI callbacks may still
			// hold pointers to them until the world teardown completes.
			m_retiredTcpPlayers.push_back(m_tcpPlayers[i]);
		}
		m_tcpPlayers.clear();
		LeaveCriticalSection(&m_tcpPlayersCS);

		m_bTcpClient = false;
		m_bLeavingGame = false;
		return true;
	}
#endif

	// If we are the host wait for the game server to end
	if(m_pIQNet->IsHost() && g_NetworkManager.ServerStoppedValid())
	{
		m_pIQNet->EndGame();
		g_NetworkManager.ServerStoppedWait();
		g_NetworkManager.ServerStoppedDestroy();
	}
	return true;
}

bool CPlatformNetworkManagerStub::_LeaveGame(bool bMigrateHost, bool bLeaveRoom)
{
	return true;
}

void CPlatformNetworkManagerStub::HostGame(int localUsersMask, bool bOnlineGame, bool bIsPrivate, unsigned char publicSlots /*= MINECRAFT_NET_MAX_PLAYERS*/, unsigned char privateSlots /*= 0*/)
{
// #ifdef _XBOX
	// 4J Stu - We probably did this earlier as well, but just to be sure!
	SetLocalGame( !bOnlineGame );
	SetPrivateGame( bIsPrivate );
	SystemFlagReset();

	// Make sure that the Primary Pad is in by default
	localUsersMask |= GetLocalPlayerMask( g_NetworkManager.GetPrimaryPad() );

	m_bLeavingGame = false;

#ifndef _XBOX
	m_pIQNet->HostGame();
#endif

	_HostGame( localUsersMask, publicSlots, privateSlots );
//#endif
}

void CPlatformNetworkManagerStub::_HostGame(int usersMask, unsigned char publicSlots /*= MINECRAFT_NET_MAX_PLAYERS*/, unsigned char privateSlots /*= 0*/)
{
}

bool CPlatformNetworkManagerStub::_StartGame()
{
	return true;
}

int CPlatformNetworkManagerStub::JoinGame(FriendSessionInfo *searchResult, int localUsersMask, int primaryUserIndex)
{
	// Windows64 dedicated-server prototype: join the TCP server directly.
#ifdef _WINDOWS64
	extern wstring g_connectIp;
	extern int g_connectPort;
	extern wstring g_playerName;
	if (!g_connectIp.empty())
	{
		m_tcpServerIp = g_connectIp;
		m_tcpServerPort = g_connectPort;
	}
#endif

	FILE *jf = NULL;
	fopen_s(&jf, "H:\\Temp\\tcp_join.log", "a");
	if (jf) { fprintf(jf, "JoinGame called ip=%ls port=%d\n", m_tcpServerIp.c_str(), m_tcpServerPort); fflush(jf); }

	Socket *socket = new Socket(m_tcpServerIp, m_tcpServerPort);
	if (jf) { fprintf(jf, "socket=%p createdOk=%d\n", socket, socket ? socket->createdOk : -1); fflush(jf); }
	if (socket == NULL || !socket->createdOk)
	{
		app.DebugPrintf("TCP JoinGame: connect failed\n");
		if (jf) { fprintf(jf, "connect failed\n"); fclose(jf); }
		if (socket != NULL) delete socket;
		return CGameNetworkManager::JOINGAME_FAIL_GENERAL;
	}

	// Give every client process a unique display name. All PC clients share
	// the same local profile, so without this every remote player is "stub".
	wchar_t nameBuf[64];
	nameBuf[0] = 0;
	FILE *nf = NULL;
	fopen_s(&nf, "client_name.txt", "r");
	if (nf != NULL)
	{
		char narrow[64] = {0};
		if (fgets(narrow, sizeof(narrow), nf) != NULL)
		{
			for (int i = 0; narrow[i] != 0; ++i)
			{
				if (narrow[i] == '\r' || narrow[i] == '\n') { narrow[i] = 0; break; }
			}
			if (narrow[0] != 0)
			{
				wstring wname = convStringToWstring(narrow);
				wcsncpy_s(nameBuf, 64, wname.c_str(), _TRUNCATE);
			}
		}
		fclose(nf);
	}
	// --name / --nick has priority over client_name.txt.
#ifdef _WINDOWS64
	if (!g_playerName.empty())
	{
		wcsncpy_s(nameBuf, 64, g_playerName.c_str(), _TRUNCATE);
	}
#endif
	if (nameBuf[0] == 0)
	{
#if defined(_XBOX)
		// Use the signed-in Xbox profile's gamertag, like the normal console
		// path. Fall back to a fixed name if the profile is unavailable.
		wstring gamertag = convStringToWstring(ProfileManager.GetGamertag(ProfileManager.GetPrimaryPad()));
		if (!gamertag.empty())
		{
			wcsncpy_s(nameBuf, 64, gamertag.c_str(), _TRUNCATE);
		}
		if (nameBuf[0] == 0)
		{
			wcsncpy_s(nameBuf, 64, L"XboxPlayer", _TRUNCATE);
		}
#else
		swprintf_s(nameBuf, 64, L"Player%u", (unsigned int)GetCurrentProcessId());
#endif
	}
	// LoginPacket::read accepts at most Player::MAX_NAME_LENGTH (20).
	if (wcslen(nameBuf) > 20) nameBuf[20] = 0;
	wstring localName = nameBuf;
	if (Minecraft::GetInstance() != NULL && Minecraft::GetInstance()->user != NULL)
	{
		Minecraft::GetInstance()->user->name = localName;
	}

	// The server will confirm the real small id in the LoginPacket.
	// Use the primary pad as the local user index; the Xbox UI does not
	// always keep LockedProfile in sync with the pad used to start the join.
	NetworkPlayerTcp *local = new NetworkPlayerTcp(localName, 1, ProfileManager.GetPrimaryPad(), true, false);
	local->SetSocket(socket);
	socket->setPlayer(local);

	// Do not create a fake host entry: a dedicated server has no host player,
	// and a normal host is added from its PlayerInfoPacket (small id 0).
	EnterCriticalSection(&m_tcpPlayersCS);
	m_tcpPlayers.push_back(local);
	LeaveCriticalSection(&m_tcpPlayersCS);

	m_bTcpClient = true;
	if (jf) { fprintf(jf, "join success, local name=%ls\n", localName.c_str()); fclose(jf); }
	return CGameNetworkManager::JOINGAME_SUCCESS;
}

bool CPlatformNetworkManagerStub::AddTcpPlayer(Socket *socket, const wstring& name, bool local, bool host)
{
	EnterCriticalSection(&m_tcpPlayersCS);
	int smallId = 1;
	bool freeFound = false;
	for (; smallId <= (int)MINECRAFT_NET_MAX_PLAYERS; ++smallId)
	{
		bool used = false;
		for (size_t i = 0; i < m_tcpPlayers.size(); ++i)
		{
			if (m_tcpPlayers[i]->m_smallId == (unsigned char)smallId) { used = true; break; }
		}
		if (!used) { freeFound = true; break; }
	}
	if (!freeFound)
	{
		// All 255 TCP small ids are in use. Reject the pending socket; the
		// login would fail anyway and a duplicate id would corrupt lookups.
		LeaveCriticalSection(&m_tcpPlayersCS);
		return false;
	}

	NetworkPlayerTcp *player = new NetworkPlayerTcp(name, (unsigned char)smallId, local ? 0 : -1, local, host);
	player->SetSocket(socket);
	if (socket != NULL) socket->setPlayer(player);
	m_tcpPlayers.push_back(player);
#ifdef _WINDOWS64
	{
		FILE *f = NULL;
		fopen_s(&f, "H:\\Temp\\tcp_players.log", "a");
		if (f)
		{
			fprintf(f, "AddTcpPlayer: socket=%p smallId=%u total=%d\n", (void *)socket, (unsigned int)smallId, (int)m_tcpPlayers.size());
			fclose(f);
		}
	}
#endif
	LeaveCriticalSection(&m_tcpPlayersCS);
	return true;
}

// Client-side roster: the server sends a PlayerInfoPacket (small id + name)
// for every session player, including players outside tracking range. This
// keeps the pause/player menus consistent with the server.
void CPlatformNetworkManagerStub::RegisterTcpPlayer(unsigned char smallId, const wstring& name, bool host)
{
	bool changed = false;
	EnterCriticalSection(&m_tcpPlayersCS);
	bool found = false;
	for (size_t i = 0; i < m_tcpPlayers.size(); ++i)
	{
		if (m_tcpPlayers[i]->m_smallId == smallId)
		{
			found = true;
			if (!name.empty() && m_tcpPlayers[i]->m_name != name)
			{
				m_tcpPlayers[i]->m_name = name;
				changed = true;
			}
			if (host && !m_tcpPlayers[i]->m_host)
			{
				m_tcpPlayers[i]->m_host = true;
				changed = true;
			}
			break;
		}
	}
	if (!found)
	{
		// On the host every TCP player already exists in m_tcpPlayers; only a
		// client needs to materialise remote players it learned about.
		if (m_bTcpClient)
		{
			NetworkPlayerTcp *player = new NetworkPlayerTcp(name, smallId, -1, false, host);
			m_tcpPlayers.push_back(player);
			changed = true;
		}
	}
	LeaveCriticalSection(&m_tcpPlayersCS);

#ifdef _WINDOWS64
	// Only log roster changes: PlayerInfoPacket is re-sent on privilege
	// changes, and with 255 players logging every packet would be costly.
	if (changed)
	{
		INetworkPlayer *localPlayer = GetLocalPlayerByUserIndex(0);
		const wchar_t *localName = (localPlayer != NULL) ? localPlayer->GetOnlineName() : L"?";
		FILE *f = NULL;
		fopen_s(&f, "H:\\Temp\\tcp_players.log", "a");
		if (f)
		{
			fprintf(f, "RegisterTcpPlayer[%ls]: smallId=%u name=%ls host=%d\n", localName, smallId, name.c_str(), host ? 1 : 0);
			fclose(f);
		}
	}
#endif
}

void CPlatformNetworkManagerStub::RemoveTcpPlayer(unsigned char smallId)
{
	EnterCriticalSection(&m_tcpPlayersCS);
	for (size_t i = 0; i < m_tcpPlayers.size(); ++i)
	{
		if (m_tcpPlayers[i]->m_smallId == smallId)
		{
			// Do not delete: the UI may still hold this INetworkPlayer.
			m_tcpPlayers[i]->SetSocket(NULL);
			m_retiredTcpPlayers.push_back(m_tcpPlayers[i]);
			m_tcpPlayers.erase(m_tcpPlayers.begin() + i);
#ifdef _WINDOWS64
			FILE *f = NULL;
			fopen_s(&f, "H:\\Temp\\tcp_players.log", "a");
			if (f)
			{
				fprintf(f, "RemoveTcpPlayer: smallId=%u total=%d\n", smallId, (int)m_tcpPlayers.size());
				fclose(f);
			}
#endif
			break;
		}
	}
	LeaveCriticalSection(&m_tcpPlayersCS);
}

bool CPlatformNetworkManagerStub::SetLocalGame(bool isLocal)
{
	m_bIsOfflineGame = isLocal;

	return true;
}

void CPlatformNetworkManagerStub::SetPrivateGame(bool isPrivate)
{
	app.DebugPrintf("Setting as private game: %s\n", isPrivate ? "yes" : "no" );
	m_bIsPrivateGame = isPrivate;
}

void CPlatformNetworkManagerStub::RegisterPlayerChangedCallback(int iPad, void (*callback)(void *callbackParam, INetworkPlayer *pPlayer, bool leaving), void *callbackParam)
{
	playerChangedCallback[iPad] = callback;
	playerChangedCallbackParam[iPad] = callbackParam;
}

void CPlatformNetworkManagerStub::UnRegisterPlayerChangedCallback(int iPad, void (*callback)(void *callbackParam, INetworkPlayer *pPlayer, bool leaving), void *callbackParam)
{
	if(playerChangedCallbackParam[iPad] == callbackParam)
	{
		playerChangedCallback[iPad] = NULL;
		playerChangedCallbackParam[iPad] = NULL;
	}
}

void CPlatformNetworkManagerStub::HandleSignInChange()
{
	return;	
}

bool CPlatformNetworkManagerStub::_RunNetworkGame()
{
	return true;
}

void CPlatformNetworkManagerStub::UpdateAndSetGameSessionData(INetworkPlayer *pNetworkPlayerLeaving /*= NULL*/)
{
// 	DWORD playerCount = m_pIQNet->GetPlayerCount();
// 
// 	if( this->m_bLeavingGame )
// 		return;
// 
// 	if( GetHostPlayer() == NULL )
// 		return;
// 
// 	for(unsigned int i = 0; i < MINECRAFT_NET_MAX_PLAYERS; ++i)
// 	{
// 		if( i < playerCount )
// 		{
// 			INetworkPlayer *pNetworkPlayer = GetPlayerByIndex(i);
// 
// 			// We can call this from NotifyPlayerLeaving but at that point the player is still considered in the session
// 			if( pNetworkPlayer != pNetworkPlayerLeaving )
// 			{
// 				m_hostGameSessionData.players[i] = ((NetworkPlayerXbox *)pNetworkPlayer)->GetUID();
// 
// 				char *temp;
// 				temp = (char *)wstringtofilename( pNetworkPlayer->GetOnlineName() );
// 				memcpy(m_hostGameSessionData.szPlayers[i],temp,XUSER_NAME_SIZE);
// 			}
// 			else
// 			{
// 				m_hostGameSessionData.players[i] = NULL;
// 				memset(m_hostGameSessionData.szPlayers[i],0,XUSER_NAME_SIZE);
// 			}
// 		}
// 		else
// 		{
// 			m_hostGameSessionData.players[i] = NULL;
// 			memset(m_hostGameSessionData.szPlayers[i],0,XUSER_NAME_SIZE);
// 		}
// 	}
// 
// 	m_hostGameSessionData.hostPlayerUID = ((NetworkPlayerXbox *)GetHostPlayer())->GetQNetPlayer()->GetXuid();
// 	m_hostGameSessionData.m_uiGameHostSettings = app.GetGameHostOption(eGameHostOption_All);
}

int CPlatformNetworkManagerStub::RemovePlayerOnSocketClosedThreadProc( void* lpParam )
{
	INetworkPlayer *pNetworkPlayer = (INetworkPlayer *)lpParam;

	Socket *socket = pNetworkPlayer->GetSocket();

	if( socket != NULL )
	{
		//printf("Waiting for socket closed event\n");
		socket->m_socketClosedEvent->WaitForSignal(INFINITE);

		//printf("Socket closed event has fired\n");
		// 4J Stu - Clear our reference to this socket
		pNetworkPlayer->SetSocket( NULL );
		delete socket;
	}

	return g_pPlatformNetworkManager->RemoveLocalPlayer( pNetworkPlayer );
}

bool CPlatformNetworkManagerStub::RemoveLocalPlayer( INetworkPlayer *pNetworkPlayer )
{
	return true;
}

CPlatformNetworkManagerStub::PlayerFlags::PlayerFlags(INetworkPlayer *pNetworkPlayer, unsigned int count)
{
	// 4J Stu - Don't assert, just make it a multiple of 8! This count is calculated from a load of separate values,
	// and makes tweaking world/render sizes a pain if we hit an assert here
	count = (count + 8 - 1) & ~(8 - 1);
	//assert( ( count % 8 ) == 0 );
	this->m_pNetworkPlayer = pNetworkPlayer;
	this->flags = new unsigned char [ count / 8 ];
	memset( this->flags, 0, count / 8 );
	this->count = count;
}
CPlatformNetworkManagerStub::PlayerFlags::~PlayerFlags()
{
	delete [] flags;
}

// Add a player to the per system flag storage - if we've already got a player from that system, copy its flags over
void CPlatformNetworkManagerStub::SystemFlagAddPlayer(INetworkPlayer *pNetworkPlayer)
{
	PlayerFlags *newPlayerFlags = new PlayerFlags( pNetworkPlayer,  m_flagIndexSize);
	// If any of our existing players are on the same system, then copy over flags from that one
	for( unsigned int i = 0; i < m_playerFlags.size(); i++ )
	{
		if( pNetworkPlayer->IsSameSystem(m_playerFlags[i]->m_pNetworkPlayer) )
		{
			memcpy( newPlayerFlags->flags, m_playerFlags[i]->flags, m_playerFlags[i]->count / 8 );
			break;
		}
	}
	m_playerFlags.push_back(newPlayerFlags);
}

// Remove a player from the per system flag storage - just maintains the m_playerFlags vector without any gaps in it
void CPlatformNetworkManagerStub::SystemFlagRemovePlayer(INetworkPlayer *pNetworkPlayer)
{
	for( unsigned int i = 0; i < m_playerFlags.size(); i++ )
	{
		if( m_playerFlags[i]->m_pNetworkPlayer == pNetworkPlayer )
		{
			delete m_playerFlags[i];
			m_playerFlags[i] = m_playerFlags.back();
			m_playerFlags.pop_back();
			return;
		}
	}
}

void CPlatformNetworkManagerStub::SystemFlagReset()
{
	for( unsigned int i = 0; i < m_playerFlags.size(); i++ )
	{
		delete m_playerFlags[i];
	}
	m_playerFlags.clear();
}

// Set a per system flag - this is done by setting the flag on every player that shares that system
void CPlatformNetworkManagerStub::SystemFlagSet(INetworkPlayer *pNetworkPlayer, int index)
{
	if( ( index < 0 ) || ( index >= m_flagIndexSize ) ) return;
	if( pNetworkPlayer == NULL ) return;

	for( unsigned int i = 0; i < m_playerFlags.size(); i++ )
	{
		if( pNetworkPlayer->IsSameSystem(m_playerFlags[i]->m_pNetworkPlayer) )
		{
			m_playerFlags[i]->flags[ index / 8 ] |= ( 128 >> ( index % 8 ) );
		}
	}
}

// Get value of a per system flag - can be read from the flags of the passed in player as anything else sent to that
// system should also have been duplicated here
bool CPlatformNetworkManagerStub::SystemFlagGet(INetworkPlayer *pNetworkPlayer, int index)
{
	if( ( index < 0 ) || ( index >= m_flagIndexSize ) ) return false;
	if( pNetworkPlayer == NULL )
	{
		return false;
	}

	for( unsigned int i = 0; i < m_playerFlags.size(); i++ )
	{
		if( m_playerFlags[i]->m_pNetworkPlayer == pNetworkPlayer )
		{
			return ( ( m_playerFlags[i]->flags[ index / 8 ] & ( 128 >> ( index % 8 ) ) ) != 0 );
		}
	}
	return false;
}

wstring CPlatformNetworkManagerStub::GatherStats()
{
	return L"";
}

wstring CPlatformNetworkManagerStub::GatherRTTStats()
{
	wstring stats(L"Rtt: ");

	wchar_t stat[32];

	for(unsigned int i = 0; i < GetPlayerCount(); ++i)
	{
		IQNetPlayer *pQNetPlayer = ((NetworkPlayerXbox *)GetPlayerByIndex( i ))->GetQNetPlayer();

		if(!pQNetPlayer->IsLocal())
		{
			ZeroMemory(stat,32*sizeof(WCHAR));
			swprintf(stat, 32, L"%d: %d/", i, pQNetPlayer->GetCurrentRtt() );
			stats.append(stat);
		}
	}
	return stats;
}

void CPlatformNetworkManagerStub::TickSearch()
{
}

void CPlatformNetworkManagerStub::SearchForGames()
{
}

int CPlatformNetworkManagerStub::SearchForGamesThreadProc( void* lpParameter )
{
	return 0;
}

void CPlatformNetworkManagerStub::SetSearchResultsReady(int resultCount)
{
	m_bSearchResultsReady = true;
	m_searchResultsCount[m_lastSearchPad] = resultCount;
}

vector<FriendSessionInfo *> *CPlatformNetworkManagerStub::GetSessionList(int iPad, int localPlayers, bool partyOnly)
{
	vector<FriendSessionInfo *> *filteredList = new vector<FriendSessionInfo *>();
	// Windows64 dedicated-server prototype: always advertise the direct TCP server.
	FriendSessionInfo *info = new FriendSessionInfo();
#ifdef _XBOX
	// Xbox SessionID is an XNKID structure, not an integer. A zeroed id is
	// fine for the direct TCP entry because JoinGame ignores it and uses
	// m_tcpServerIp/m_tcpServerPort from server.txt.
	memset(&info->sessionId, 0, sizeof(info->sessionId));
#else
	info->sessionId = 1;
#endif
	wchar_t labelBuf[160];
	swprintf_s(labelBuf, 160, L"Direct TCP Server (%ls:%d)", m_tcpServerIp.c_str(), m_tcpServerPort);
	size_t len = wcslen(labelBuf);
	info->displayLabel = new wchar_t[len + 1];
	wcscpy_s(info->displayLabel, len + 1, labelBuf);
	info->displayLabelLength = (unsigned char)len;
	info->displayLabelViewableStartIndex = 0;
	info->data.netVersion = 1;
#ifdef _XBOX
	info->data.isJoinable = true;
#else
	info->data.isReadyToJoin = true;
#endif
	info->data.m_uiGameHostSettings = app.GetGameHostOption(eGameHostOption_All);
	filteredList->push_back(info);
	return filteredList;
}

bool CPlatformNetworkManagerStub::GetGameSessionInfo(int iPad, SessionID sessionId, FriendSessionInfo *foundSessionInfo)
{
#ifdef _XBOX
	// Xbox SessionID is an XNKID structure. The stub advertises exactly one
	// direct-TCP entry with a zeroed id.
	XNKID zero;
	memset(&zero, 0, sizeof(zero));
	if (memcmp(&sessionId, &zero, sizeof(zero)) != 0) return false;
	memset(&foundSessionInfo->sessionId, 0, sizeof(foundSessionInfo->sessionId));
#else
	if (sessionId != 1) return false;
	foundSessionInfo->sessionId = 1;
#endif
	foundSessionInfo->data.netVersion = 1;
#ifdef _XBOX
	foundSessionInfo->data.isJoinable = true;
#else
	foundSessionInfo->data.isReadyToJoin = true;
#endif
	return true;
}

void CPlatformNetworkManagerStub::SetSessionsUpdatedCallback( void (*SessionsUpdatedCallback)(LPVOID pParam), LPVOID pSearchParam )
{
	m_SessionsUpdatedCallback = SessionsUpdatedCallback; m_pSearchParam = pSearchParam;
}

void CPlatformNetworkManagerStub::GetFullFriendSessionInfo( FriendSessionInfo *foundSession, void (* FriendSessionUpdatedFn)(bool success, void *pParam), void *pParam )
{
	FriendSessionUpdatedFn(true, pParam);
}

void CPlatformNetworkManagerStub::ForceFriendsSessionRefresh()
{
	app.DebugPrintf("Resetting friends session search data\n");
	
	for(unsigned int i = 0; i < XUSER_MAX_COUNT; ++i)
	{
		m_searchResultsCount[i] = 0;
		m_lastSearchStartTime[i] = 0;
		delete m_pSearchResults[i];
		m_pSearchResults[i] = NULL;
	}
}

INetworkPlayer *CPlatformNetworkManagerStub::addNetworkPlayer(IQNetPlayer *pQNetPlayer)
{
	NetworkPlayerXbox *pNetworkPlayer = new NetworkPlayerXbox(pQNetPlayer);
	pQNetPlayer->SetCustomDataValue((ULONG_PTR)pNetworkPlayer);
	currentNetworkPlayers.push_back( pNetworkPlayer );
	return pNetworkPlayer;
}

void CPlatformNetworkManagerStub::removeNetworkPlayer(IQNetPlayer *pQNetPlayer)
{
	INetworkPlayer *pNetworkPlayer = getNetworkPlayer(pQNetPlayer);
	for( AUTO_VAR(it, currentNetworkPlayers.begin()); it != currentNetworkPlayers.end(); it++ )
	{
		if( *it == pNetworkPlayer )
		{
			currentNetworkPlayers.erase(it);
			return;
		}
	}
}

INetworkPlayer *CPlatformNetworkManagerStub::getNetworkPlayer(IQNetPlayer *pQNetPlayer)
{
	return pQNetPlayer ? (INetworkPlayer *)(pQNetPlayer->GetCustomDataValue()) : NULL;
}


INetworkPlayer *CPlatformNetworkManagerStub::GetLocalPlayerByUserIndex(int userIndex )
{
	EnterCriticalSection(&m_tcpPlayersCS);
	for (size_t i = 0; i < m_tcpPlayers.size(); ++i)
	{
		if (m_tcpPlayers[i]->m_local && m_tcpPlayers[i]->m_userIndex == userIndex)
		{
			INetworkPlayer *result = m_tcpPlayers[i];
			LeaveCriticalSection(&m_tcpPlayersCS);
			return result;
		}
	}
	LeaveCriticalSection(&m_tcpPlayersCS);
	if (m_bTcpClient) return NULL;
	return getNetworkPlayer(m_pIQNet->GetLocalPlayerByUserIndex(userIndex)); 
}

INetworkPlayer *CPlatformNetworkManagerStub::GetPlayerByIndex(int playerIndex)
{
	EnterCriticalSection(&m_tcpPlayersCS);
	if (playerIndex >= 0 && playerIndex < (int)m_tcpPlayers.size())
	{
		INetworkPlayer *result = m_tcpPlayers[playerIndex];
		LeaveCriticalSection(&m_tcpPlayersCS);
		return result;
	}
	LeaveCriticalSection(&m_tcpPlayersCS);
	if (m_bTcpClient) return NULL;
	return getNetworkPlayer(m_pIQNet->GetPlayerByIndex(playerIndex));
}

INetworkPlayer * CPlatformNetworkManagerStub::GetPlayerByXuid(PlayerUID xuid)
{
	if (m_bTcpClient) return NULL;
	return getNetworkPlayer( m_pIQNet->GetPlayerByXuid(xuid)) ;
}

INetworkPlayer * CPlatformNetworkManagerStub::GetPlayerBySmallId(unsigned char smallId)
{
	EnterCriticalSection(&m_tcpPlayersCS);
	for (size_t i = 0; i < m_tcpPlayers.size(); ++i)
	{
		if (m_tcpPlayers[i]->m_smallId == smallId)
		{
			INetworkPlayer *result = m_tcpPlayers[i];
			LeaveCriticalSection(&m_tcpPlayersCS);
			return result;
		}
	}
	LeaveCriticalSection(&m_tcpPlayersCS);
	if (m_bTcpClient) return NULL;
	// The QNet stub returns m_player[0] for any id; only small id 0 (the
	// host) is meaningful here. Unknown ids must resolve to NULL, otherwise a
	// disconnected TCP socket would be mistaken for the host player.
	if (smallId == 0) return getNetworkPlayer(m_pIQNet->GetPlayerBySmallId(smallId));
	return NULL;
}

INetworkPlayer *CPlatformNetworkManagerStub::GetHostPlayer()
{
	EnterCriticalSection(&m_tcpPlayersCS);
	for (size_t i = 0; i < m_tcpPlayers.size(); ++i)
	{
		if (m_tcpPlayers[i]->m_host)
		{
			INetworkPlayer *result = m_tcpPlayers[i];
			LeaveCriticalSection(&m_tcpPlayersCS);
			return result;
		}
	}
	LeaveCriticalSection(&m_tcpPlayersCS);
	if (m_bTcpClient) return NULL;
	return getNetworkPlayer(m_pIQNet->GetHostPlayer());
}

bool CPlatformNetworkManagerStub::IsHost()
{
	if (m_bTcpClient) return false;
	return m_pIQNet->IsHost() && !m_bHostChanged;
}

bool CPlatformNetworkManagerStub::JoinGameFromInviteInfo( int userIndex, int userMask, const INVITE_INFO *pInviteInfo)
{
	if (m_bTcpClient) return false;
	return ( m_pIQNet->JoinGameFromInviteInfo( userIndex, userMask, pInviteInfo ) == S_OK);
}

void CPlatformNetworkManagerStub::SetSessionTexturePackParentId( int id )
{
	m_hostGameSessionData.texturePackParentId = id;
}

void CPlatformNetworkManagerStub::SetSessionSubTexturePackId( int id )
{
	m_hostGameSessionData.subTexturePackId = id;
}

void CPlatformNetworkManagerStub::Notify(int ID, ULONG_PTR Param)
{
}

bool CPlatformNetworkManagerStub::IsInSession()
{
	if (m_bTcpClient) return true;
	return m_pIQNet->GetState() != QNET_STATE_IDLE;
}

bool CPlatformNetworkManagerStub::IsInGameplay()
{
	if (m_bTcpClient) return true;
	return m_pIQNet->GetState() == QNET_STATE_GAME_PLAY;
}

bool CPlatformNetworkManagerStub::IsReadyToPlayOrIdle()
{
	return true;
}
