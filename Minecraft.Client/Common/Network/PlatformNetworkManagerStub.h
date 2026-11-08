#pragma once
using namespace std;
#include <vector>
#include "..\..\..\Minecraft.World\C4JThread.h"
#include "NetworkPlayerInterface.h"
#include "PlatformNetworkManagerInterface.h"
#include "SessionInfo.h"

// TCP player used by the Windows64 dedicated-server prototype. The console
// network layer normally wraps IQNetPlayer; this implementation carries a
// plain TCP Socket instead.
class NetworkPlayerTcp : public INetworkPlayer
{
public:
	NetworkPlayerTcp(const wstring& name, unsigned char smallId, int userIndex, bool local, bool host)
		: m_name(name), m_smallId(smallId), m_userIndex(userIndex), m_local(local), m_host(host), m_socket(NULL), m_uid(0) {}
	virtual ~NetworkPlayerTcp() {}

	virtual unsigned char GetSmallId() { return m_smallId; }
	virtual void SendData(INetworkPlayer *player, const void *pvData, int dataSize, bool lowPriority) {}
	virtual bool IsSameSystem(INetworkPlayer *player) { return player == this; }
	virtual int GetSendQueueSizeBytes(INetworkPlayer *player, bool lowPriority) { return 0; }
	virtual int GetSendQueueSizeMessages(INetworkPlayer *player, bool lowPriority) { return 0; }
	virtual int GetCurrentRtt() { return 0; }
	virtual bool IsHost() { return m_host; }
	virtual bool IsGuest() { return false; }
	virtual bool IsLocal() { return m_local; }
	virtual int GetSessionIndex() { return 0; }
	virtual bool IsTalking() { return false; }
	virtual bool IsMutedByLocalUser(int userIndex) { return false; }
	virtual bool HasVoice() { return false; }
	virtual bool HasCamera() { return false; }
	virtual int GetUserIndex() { return m_userIndex; }
	virtual void SetSocket(Socket *pSocket) { m_socket = pSocket; }
	virtual Socket *GetSocket() { return m_socket; }
	virtual const wchar_t *GetOnlineName() { return m_name.c_str(); }
	virtual wstring GetDisplayName() { return m_name; }
	virtual PlayerUID GetUID() { return m_uid; }
	virtual void SetSmallId(unsigned char id) { m_smallId = id; }
	virtual void SetOnlineName(const wstring& name) { m_name = name; }

public:
	wstring m_name;
	unsigned char m_smallId;
	int m_userIndex;
	bool m_local;
	bool m_host;
	Socket *m_socket;
	PlayerUID m_uid;
};

class CPlatformNetworkManagerStub : public CPlatformNetworkManager
{	friend class CGameNetworkManager;
public:
	virtual bool Initialise(CGameNetworkManager *pGameNetworkManager, int flagIndexSize);
	virtual void Terminate();
	virtual int GetJoiningReadyPercentage();
	virtual int CorrectErrorIDS(int IDS);

	virtual void DoWork();
	virtual int GetPlayerCount();
	virtual int GetOnlinePlayerCount();
	virtual int GetLocalPlayerMask(int playerIndex);
	virtual bool AddLocalPlayerByUserIndex( int userIndex );
	virtual bool RemoveLocalPlayerByUserIndex( int userIndex );
	virtual INetworkPlayer *GetLocalPlayerByUserIndex( int userIndex );
	virtual INetworkPlayer *GetPlayerByIndex(int playerIndex);
	virtual INetworkPlayer * GetPlayerByXuid(PlayerUID xuid);
	virtual INetworkPlayer * GetPlayerBySmallId(unsigned char smallId);
	virtual bool ShouldMessageForFullSession();

	virtual INetworkPlayer *GetHostPlayer();
	virtual bool IsHost();
	virtual bool JoinGameFromInviteInfo( int userIndex, int userMask, const INVITE_INFO *pInviteInfo);
	virtual bool LeaveGame(bool bMigrateHost);

	virtual bool IsInSession();
	virtual bool IsInGameplay();
	virtual bool IsReadyToPlayOrIdle();
	virtual bool IsInStatsEnabledSession();
	virtual bool SessionHasSpace(unsigned int spaceRequired = 1);
	virtual void SendInviteGUI(int quadrant);
	virtual bool IsAddingPlayer();

	virtual void HostGame(int localUsersMask, bool bOnlineGame, bool bIsPrivate, unsigned char publicSlots = MINECRAFT_NET_MAX_PLAYERS, unsigned char privateSlots = 0);
	virtual int  JoinGame(FriendSessionInfo *searchResult, int localUsersMask, int primaryUserIndex );
	virtual bool SetLocalGame(bool isLocal);
	virtual bool IsLocalGame() { return m_bIsOfflineGame; }
	virtual void SetPrivateGame(bool isPrivate);
	virtual bool IsPrivateGame() { return m_bIsPrivateGame; }
	virtual bool IsLeavingGame() { return m_bLeavingGame; }
	virtual void ResetLeavingGame() { m_bLeavingGame = false; }

	virtual void RegisterPlayerChangedCallback(int iPad, void (*callback)(void *callbackParam, INetworkPlayer *pPlayer, bool leaving), void *callbackParam);
	virtual void UnRegisterPlayerChangedCallback(int iPad, void (*callback)(void *callbackParam, INetworkPlayer *pPlayer, bool leaving), void *callbackParam);

	virtual void HandleSignInChange();

	virtual bool _RunNetworkGame();
	
private:
	bool isSystemPrimaryPlayer(IQNetPlayer *pQNetPlayer);
	virtual bool _LeaveGame(bool bMigrateHost, bool bLeaveRoom);
	virtual void _HostGame(int dwUsersMask, unsigned char publicSlots = MINECRAFT_NET_MAX_PLAYERS, unsigned char privateSlots = 0);
	virtual bool _StartGame();

    IQNet *             m_pIQNet;             // pointer to QNet interface

	HANDLE m_notificationListener;

	vector<IQNetPlayer *> m_machineQNetPrimaryPlayers; // collection of players that we deem to be the main one for that system

	bool			m_bLeavingGame;
	bool			m_bLeaveGameOnTick;
	bool			m_migrateHostOnLeave;
	bool			m_bHostChanged;

	bool			m_bIsOfflineGame;
	bool			m_bIsPrivateGame;
	int				m_flagIndexSize;

	// This is only maintained by the host, and is not valid on client machines
	GameSessionData m_hostGameSessionData;
	CGameNetworkManager *m_pGameNetworkManager;
public:
	virtual void UpdateAndSetGameSessionData(INetworkPlayer *pNetworkPlayerLeaving = NULL);

private:
	// TODO 4J Stu - Do we need to be able to have more than one of these?
	void (*playerChangedCallback[XUSER_MAX_COUNT])(void *callbackParam, INetworkPlayer *pPlayer, bool leaving);
	void *playerChangedCallbackParam[XUSER_MAX_COUNT];

	static int RemovePlayerOnSocketClosedThreadProc( void* lpParam );
	virtual bool RemoveLocalPlayer( INetworkPlayer *pNetworkPlayer );

	// Things for handling per-system flags
	class PlayerFlags
	{
	public:
		INetworkPlayer *m_pNetworkPlayer;
		unsigned char *flags;
		unsigned int count;
		PlayerFlags(INetworkPlayer *pNetworkPlayer, unsigned int count);
		~PlayerFlags();
	};
	vector<PlayerFlags *> m_playerFlags;
	void SystemFlagAddPlayer(INetworkPlayer *pNetworkPlayer);
	void SystemFlagRemovePlayer(INetworkPlayer *pNetworkPlayer);
	void SystemFlagReset();
public:
	virtual void SystemFlagSet(INetworkPlayer *pNetworkPlayer, int index);
	virtual bool SystemFlagGet(INetworkPlayer *pNetworkPlayer, int index);

	// For telemetry
private:
	float m_lastPlayerEventTimeStart;

public:
	wstring GatherStats();
	wstring GatherRTTStats();

private:	
	vector<FriendSessionInfo *> friendsSessions[XUSER_MAX_COUNT];
	int m_searchResultsCount[XUSER_MAX_COUNT];
	int m_lastSearchStartTime[XUSER_MAX_COUNT];

	// The results that will be filled in with the current search
	XSESSION_SEARCHRESULT_HEADER *m_pSearchResults[XUSER_MAX_COUNT];
	XNQOS *m_pQoSResult[XUSER_MAX_COUNT];

	// The results from the previous search, which are currently displayed in the game
	XSESSION_SEARCHRESULT_HEADER *m_pCurrentSearchResults[XUSER_MAX_COUNT];
	XNQOS *m_pCurrentQoSResult[XUSER_MAX_COUNT];
	int m_currentSearchResultsCount[XUSER_MAX_COUNT];
	
	int m_lastSearchPad;
	bool m_bSearchResultsReady;
	bool m_bSearchPending;
	LPVOID m_pSearchParam;
	void (*m_SessionsUpdatedCallback)(LPVOID pParam);

	C4JThread* m_SearchingThread;

	void TickSearch();
	void SearchForGames();
	static int SearchForGamesThreadProc( void* lpParameter );

	void SetSearchResultsReady(int resultCount = 0);

	vector<INetworkPlayer *>currentNetworkPlayers;
	INetworkPlayer *addNetworkPlayer(IQNetPlayer *pQNetPlayer);
	void removeNetworkPlayer(IQNetPlayer *pQNetPlayer);
	static INetworkPlayer *getNetworkPlayer(IQNetPlayer *pQNetPlayer);

	// TCP players for the Windows64 dedicated-server prototype.
	bool m_bTcpClient;
	vector<NetworkPlayerTcp *> m_tcpPlayers;
	// Removed players are kept alive until shutdown: UI code may still hold
	// INetworkPlayer pointers when a client disconnects.
	vector<NetworkPlayerTcp *> m_retiredTcpPlayers;
	CRITICAL_SECTION m_tcpPlayersCS;
	wstring m_tcpServerIp;
	int m_tcpServerPort;

	virtual void SetSessionTexturePackParentId( int id );
	virtual void SetSessionSubTexturePackId( int id );
	virtual void Notify(int ID, ULONG_PTR Param);

public:
	virtual vector<FriendSessionInfo *> *GetSessionList(int iPad, int localPlayers, bool partyOnly);
	virtual bool AddTcpPlayer(Socket *socket, const wstring& name, bool local, bool host);
	virtual void RegisterTcpPlayer(unsigned char smallId, const wstring& name, bool host);
	virtual void RemoveTcpPlayer(unsigned char smallId);
	virtual bool GetGameSessionInfo(int iPad, SessionID sessionId,FriendSessionInfo *foundSession);	virtual void SetSessionsUpdatedCallback( void (*SessionsUpdatedCallback)(LPVOID pParam), LPVOID pSearchParam );
	virtual void GetFullFriendSessionInfo( FriendSessionInfo *foundSession, void (* FriendSessionUpdatedFn)(bool success, void *pParam), void *pParam );
	virtual void ForceFriendsSessionRefresh();

private:
	void NotifyPlayerJoined( IQNetPlayer *pQNetPlayer );

#ifndef _XBOX
	void FakeLocalPlayerJoined() { NotifyPlayerJoined(m_pIQNet->GetLocalPlayerByUserIndex(0)); }
#endif
};
