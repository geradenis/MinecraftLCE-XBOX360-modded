#include "stdafx.h"
#include "..\Common\Consoles_App.h"
#include "..\User.h"
#include "..\..\Minecraft.Client\Minecraft.h"
#include "..\..\Minecraft.Client\MinecraftServer.h"
#include "..\..\Minecraft.Client\PlayerList.h"
#include "..\..\Minecraft.Client\ServerPlayer.h"
#include "..\..\Minecraft.World\Level.h"
#include "..\..\Minecraft.World\LevelSettings.h"
#include "..\..\Minecraft.World\BiomeSource.h"
#include "..\..\Minecraft.World\LevelType.h"
#include "..\Common\UI\UI.h"
#include "..\Common\Network\GameNetworkManager.h"

CConsoleMinecraftApp app;

CConsoleMinecraftApp::CConsoleMinecraftApp() : CMinecraftApp()
{
}

void CConsoleMinecraftApp::SetRichPresenceContext(int iPad, int contextId)
{
	ProfileManager.SetRichPresenceContextValue(iPad,CONTEXT_GAME_STATE,contextId);
}

void CConsoleMinecraftApp::StoreLaunchData()
{
}
void CConsoleMinecraftApp::ExitGame()
{
}
void CConsoleMinecraftApp::FatalLoadError()
{
}

void CConsoleMinecraftApp::CaptureSaveThumbnail()
{
}
void CConsoleMinecraftApp::GetSaveThumbnail(PBYTE *pbData,DWORD *pdwSize)
{
}
void CConsoleMinecraftApp::ReleaseSaveThumbnail()
{
}

void CConsoleMinecraftApp::GetScreenshot(int iPad,PBYTE *pbData,DWORD *pdwSize)
{
}

void CConsoleMinecraftApp::TemporaryCreateGameStart()
{
	////////////////////////////////////////////////////////////////////////////////////////////// From CScene_Main::OnInit

	app.setLevelGenerationOptions(NULL);

	// From CScene_Main::RunPlayGame
	Minecraft *pMinecraft=Minecraft::GetInstance();
	app.ReleaseSaveThumbnail();
	ProfileManager.SetLockedProfile(0);
	pMinecraft->user->name = L"Windows";
	app.ApplyGameSettingsChanged(0);

	////////////////////////////////////////////////////////////////////////////////////////////// From CScene_MultiGameJoinLoad::OnInit
	MinecraftServer::resetFlags();

	// From CScene_MultiGameJoinLoad::OnNotifyPressEx
	app.SetTutorialMode( false );
	app.SetCorruptSaveDeleted(false);

	////////////////////////////////////////////////////////////////////////////////////////////// From CScene_MultiGameCreate::CreateGame

	app.ClearTerrainFeaturePosition();
	wstring wWorldName = L"TestWorld";

	StorageManager.ResetSaveData();
	StorageManager.SetSaveTitle(wWorldName.c_str());

	bool isFlat = false;
	__int64 seedValue = 0; // BiomeSource::findSeed(isFlat?LevelType::lvl_flat:LevelType::lvl_normal);	// 4J - was (new Random())->nextLong() - now trying to actually find a seed to suit our requirements

	NetworkGameInitData *param = new NetworkGameInitData();
	param->seed = seedValue;
	param->saveData = NULL;

	// M2 test hook: the default test world is peaceful, which disables enemy
	// spawning entirely. Allow automated runs to request a difficulty.
	int testDifficulty = 0;
	const char *difficultyOverride = getenv("MC_TEST_DIFFICULTY");
	if (difficultyOverride != NULL) testDifficulty = atoi(difficultyOverride);
	if (testDifficulty < 0) testDifficulty = 0;
	if (testDifficulty > 3) testDifficulty = 3;
	app.SetGameHostOption(eGameHostOption_Difficulty, testDifficulty);
	app.SetGameHostOption(eGameHostOption_FriendsOfFriends,0);
	app.SetGameHostOption(eGameHostOption_Gamertags,1);
	app.SetGameHostOption(eGameHostOption_BedrockFog,1);

	app.SetGameHostOption(eGameHostOption_GameType,GameType::CREATIVE->getId() ); // LevelSettings::GAMETYPE_SURVIVAL
	app.SetGameHostOption(eGameHostOption_LevelType, 0 );
	app.SetGameHostOption(eGameHostOption_Structures, 1 );
	app.SetGameHostOption(eGameHostOption_BonusChest, 0 );

	app.SetGameHostOption(eGameHostOption_PvP, 1);
	app.SetGameHostOption(eGameHostOption_TrustPlayers, 1 );
	app.SetGameHostOption(eGameHostOption_FireSpreads, 1 );
	app.SetGameHostOption(eGameHostOption_TNT, 1 );
	app.SetGameHostOption(eGameHostOption_HostCanFly, 1);
	app.SetGameHostOption(eGameHostOption_HostCanChangeHunger, 1);
	app.SetGameHostOption(eGameHostOption_HostCanBeInvisible, 1 );

	param->settings = app.GetGameHostOption( eGameHostOption_All );

	// M2: create a real (stub) local session. Without HostGame the network
	// manager stays in QNET_STATE_IDLE, IsInSession() returns false and the
	// server bails out right after printing the default game type.
	g_NetworkManager.HostGame(1, false, true, MINECRAFT_NET_MAX_PLAYERS, 0);

	param->xzSize = LEVEL_MAX_WIDTH;
	param->hellScale = HELL_LEVEL_MAX_SCALE;

	param->settings = app.GetGameHostOption( eGameHostOption_All );

	g_NetworkManager.FakeLocalPlayerJoined();

	LoadingInputParams *loadingParams = new LoadingInputParams();
	loadingParams->func = &CGameNetworkManager::RunNetworkGameThreadProc;
	loadingParams->lpParam = (LPVOID)param;

	// Reset the autosave time
	app.SetAutosaveTimerTime();

	UIFullscreenProgressCompletionData *completionData = new UIFullscreenProgressCompletionData();
	completionData->bShowBackground = TRUE;
	completionData->bShowLogo = TRUE;
	completionData->type = e_ProgressCompletion_CloseAllPlayersUIScenes;
	completionData->iPad = DEFAULT_XUI_MENU_USER;
	loadingParams->completionData = completionData;

	ui.NavigateToScene(0, eUIScene_FullscreenProgress, loadingParams);
}

int CConsoleMinecraftApp::GetLocalTMSFileIndex(WCHAR *wchTMSFile,bool bFilenameIncludesExtension,eFileExtensionType eEXT)
{
	return -1;
}

int CConsoleMinecraftApp::LoadLocalTMSFile(WCHAR *wchTMSFile)
{
	return -1;
}

int CConsoleMinecraftApp::LoadLocalTMSFile(WCHAR *wchTMSFile, eFileExtensionType eExt)
{
	return -1;
}

void CConsoleMinecraftApp::FreeLocalTMSFiles(eTMSFileType eType)
{
}
