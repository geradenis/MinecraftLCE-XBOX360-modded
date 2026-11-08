#include "stdafx.h"
#include "PlayerList.h"
#include "PlayerChunkMap.h"
#include "MinecraftServer.h"
#include "Settings.h"
#include "ServerLevel.h"
#include "ServerChunkCache.h"
#include "ServerPlayer.h"
#include "ServerPlayerGameMode.h"
#include "ServerConnection.h"
#include "PendingConnection.h"
#include "PlayerConnection.h"
#include "EntityTracker.h"
#include "..\Minecraft.World\net.minecraft.world.level.storage.h"
#include "..\Minecraft.World\net.minecraft.world.level.dimension.h"
#include "..\Minecraft.World\ArrayWithLength.h"
#include "..\Minecraft.World\net.minecraft.network.packet.h"
#include "..\Minecraft.World\net.minecraft.network.h"
#include "..\Minecraft.World\Pos.h"
#include "..\Minecraft.World\ProgressListener.h"
#include "..\Minecraft.World\HellRandomLevelSource.h"
#include "..\Minecraft.World\net.minecraft.world.phys.h"
#include "..\Minecraft.World\net.minecraft.world.item.h"
#include "..\Minecraft.World\net.minecraft.world.level.tile.h"
#include "..\Minecraft.World\net.minecraft.world.level.storage.h"
#include "..\Minecraft.World\net.minecraft.world.level.saveddata.h"
#include "..\Minecraft.World\JavaMath.h"
#include "..\Minecraft.World\LevelData.h"
#include "..\Minecraft.World\net.minecraft.world.entity.boss.enderdragon.h"
#ifdef _XBOX
#include "Xbox\Network\NetworkPlayerXbox.h"
#elif defined(__PS3__) || defined(__ORBIS__)
#include "Common\Network\Sony\NetworkPlayerSony.h"
#endif

// 4J - this class is fairly substantially altered as there didn't seem any point in porting code for banning, whitelisting, ops etc.

PlayerList::PlayerList(MinecraftServer *server)
{
	playerIo = NULL;

    this->server = server;

	sendAllPlayerInfoIn = 0;
	overrideGameMode = NULL;
	allowCheatsForAllPlayers = false;

#ifdef _PSVITA__
	viewDistance = 3;
#elif defined _LARGE_WORLDS
	// M2: keep this equal to LevelRenderer::PLAYER_VIEW_DISTANCE (12) so the
	// server loads exactly the chunks the client wants to draw.
	viewDistance = server->settings->getInt(L"view-distance", 12);
	// PlayerChunkMap requires at least MIN_VIEW_DISTANCE (3).
	if (viewDistance < PlayerChunkMap::MIN_VIEW_DISTANCE) viewDistance = PlayerChunkMap::MIN_VIEW_DISTANCE;
	if (viewDistance > 12) viewDistance = 12;
#else
	viewDistance = 10;
#endif

#ifdef _WINDOWS64
	// Number of chunks sent to a player per server tick. Java's
	// PlayerChunkSender starts at 9 chunks/tick; the vanilla LCE path sends
	// one, which makes a 12-chunk radius take ~31 seconds to stream.
	chunksPerTick = server->settings->getInt(L"chunks-per-tick", 9);
	if (chunksPerTick < 1) chunksPerTick = 1;
	if (chunksPerTick > 16) chunksPerTick = 16;

	// Java separates the chunk view distance from the simulation distance.
	// Entities only tick within simulation-distance chunks of a player.
	simulationDistance = server->settings->getInt(L"simulation-distance", viewDistance);
	if (simulationDistance < 2) simulationDistance = 2;
	if (simulationDistance > viewDistance) simulationDistance = viewDistance;
#else
	chunksPerTick = 1;
	simulationDistance = viewDistance;
#endif

    //int viewDistance = server->settings->getInt(L"view-distance", 10);

#ifdef _WINDOWS64
    maxPlayers = server->settings->getInt(L"max-players", MINECRAFT_NET_MAX_PLAYERS);
    if (maxPlayers > MINECRAFT_NET_MAX_PLAYERS) maxPlayers = MINECRAFT_NET_MAX_PLAYERS;
    if (maxPlayers < 1) maxPlayers = 1;
#else
    maxPlayers = server->settings->getInt(L"max-players", 20);
#endif
    doWhiteList = false;
	
	InitializeCriticalSection(&m_kickPlayersCS);
	InitializeCriticalSection(&m_closePlayersCS);
}

PlayerList::~PlayerList()
{
	for( AUTO_VAR(it, players.begin()); it < players.end(); it++ )
	{
		(*it)->connection = nullptr;	// Must remove reference to connection, or else there is a circular dependency
		delete (*it)->gameMode;		// Gamemode also needs deleted as it references back to this player
		(*it)->gameMode = NULL;
	}

	DeleteCriticalSection(&m_kickPlayersCS);
	DeleteCriticalSection(&m_closePlayersCS);
}

void PlayerList::placeNewPlayer(Connection *connection, shared_ptr<ServerPlayer> player, shared_ptr<LoginPacket> packet)
{
	app.DebugPrintf("PlayerList::placeNewPlayer name=%ls\n", player->name.c_str());
	bool newPlayer = load(player);
	app.DebugPrintf("PlayerList::placeNewPlayer load done new=%d\n", (int)newPlayer);
        player->setLevel(server->getLevel(player->dimension));
		player->gameMode->setLevel((ServerLevel *) player->level);

		// Make sure these privileges are always turned off for the host player
		INetworkPlayer *networkPlayer = connection->getSocket()->getPlayer();
		if(networkPlayer != NULL && networkPlayer->IsHost())
		{
			player->enableAllPlayerPrivileges(true);			
			player->setPlayerGamePrivilege(Player::ePlayerGamePrivilege_HOST,1);
		}

#if defined(__PS3__) || defined(__ORBIS__)
		// PS3 networking library doesn't automatically assign PlayerUIDs to the network players for anything remote, so need to tell it what to set from the data in this packet now
		if( !g_NetworkManager.IsLocalGame() )
		{
			if( networkPlayer != NULL )
			{
				((NetworkPlayerSony *)networkPlayer)->SetUID( packet->m_onlineXuid );
			}
		}
#endif

		// 4J Stu - TU-1 hotfix
		// Fix for #13150 - When a player loads/joins a game after saving/leaving in the nether, sometimes they are spawned on top of the nether and cannot mine down
		validatePlayerSpawnPosition(player);

//        logger.info(getName() + " logged in with entity id " + playerEntity.entityId + " at (" + playerEntity.x + ", " + playerEntity.y + ", " + playerEntity.z + ")");

        ServerLevel *level = server->getLevel(player->dimension);

		DWORD playerIndex = 0;
#ifdef _WINDOWS64
		// TCP prototype: the server-assigned small id is also the player
		// (colour) index. The client learns it from the LoginPacket.
		if (networkPlayer != NULL)
		{
			playerIndex = networkPlayer->GetSmallId();
			networkPlayer->SetOnlineName(player->name);
		}
#else
		{
			bool usedIndexes[MINECRAFT_NET_MAX_PLAYERS];
			ZeroMemory( &usedIndexes, MINECRAFT_NET_MAX_PLAYERS * sizeof(bool) );
			for(AUTO_VAR(it, players.begin()); it < players.end(); ++it)
			{
				usedIndexes[ (int)(*it)->getPlayerIndex() ] = true;
			}
			for(unsigned int i = 0; i < MINECRAFT_NET_MAX_PLAYERS; ++i)
			{
				if(!usedIndexes[i])
				{
					playerIndex = i;
					break;
				}
			}
		}
#endif
		player->setPlayerIndex( playerIndex );
		player->setCustomSkin( packet->m_playerSkinId );
		player->setCustomCape( packet->m_playerCapeId );

		// 4J-JEV: Moved this here so we can send player-model texture and geometry data.
		shared_ptr<PlayerConnection> playerConnection = shared_ptr<PlayerConnection>(new PlayerConnection(server, connection, player));
		//player->connection = playerConnection;	// Used to be assigned in PlayerConnection ctor but moved out so we can use shared_ptr

		if(newPlayer)
		{
			int mapScale = 3;
#ifdef _LARGE_WORLDS
			int scale = MapItemSavedData::MAP_SIZE * 2 * (1 << mapScale);
			int centreXC = (int) (Math::round(player->x / scale) * scale);
			int centreZC = (int) (Math::round(player->z / scale) * scale);
#else
			// 4J-PB - for Xbox maps, we'll centre them on the origin of the world, since we can fit the whole world in our map
			int centreXC = 0;
			int centreZC = 0;
#endif
			// 4J Added - Give every player a map the first time they join a server
			player->inventory->setItem( 9, shared_ptr<ItemInstance>( new ItemInstance(Item::map_Id, 1, level->getAuxValueForMap(player->getXuid(),0,centreXC, centreZC, mapScale ) ) ) );
			if(app.getGameRuleDefinitions() != NULL)
			{
				app.getGameRuleDefinitions()->postProcessPlayer(player);
			}
		}

		if(!player->customTextureUrl.empty() && player->customTextureUrl.substr(0,3).compare(L"def") != 0 && !app.IsFileInMemoryTextures(player->customTextureUrl))
		{
			if(	server->getConnection()->addPendingTextureRequest(player->customTextureUrl))
			{
#ifndef _CONTENT_PACKAGE
				wprintf(L"Sending texture packet to get custom skin %ls from player %ls\n",player->customTextureUrl.c_str(), player->name.c_str());
#endif
				playerConnection->send(shared_ptr<TextureAndGeometryPacket>( new TextureAndGeometryPacket(player->customTextureUrl,NULL,0) ) );
			}
		}
		else if(!player->customTextureUrl.empty() && app.IsFileInMemoryTextures(player->customTextureUrl))
		{			
			// Update the ref count on the memory texture data
			app.AddMemoryTextureFile(player->customTextureUrl,NULL,0);
		}

		if(!player->customTextureUrl2.empty() && player->customTextureUrl2.substr(0,3).compare(L"def") != 0 && !app.IsFileInMemoryTextures(player->customTextureUrl2))
		{
			if(	server->getConnection()->addPendingTextureRequest(player->customTextureUrl2))
			{
#ifndef _CONTENT_PACKAGE
				wprintf(L"Sending texture packet to get custom skin %ls from player %ls\n",player->customTextureUrl2.c_str(), player->name.c_str());
#endif
				playerConnection->send(shared_ptr<TexturePacket>( new TexturePacket(player->customTextureUrl2,NULL,0) ) );
			}
		}
		else if(!player->customTextureUrl2.empty() && app.IsFileInMemoryTextures(player->customTextureUrl2))
		{			
			// Update the ref count on the memory texture data
			app.AddMemoryTextureFile(player->customTextureUrl2,NULL,0);
		}

		player->setIsGuest( packet->m_isGuest );

        Pos *spawnPos = level->getSharedSpawnPos();

		updatePlayerGameMode(player, nullptr, level);

		// Update the privileges with the correct game mode
		GameType *gameType = Player::getPlayerGamePrivilege(player->getAllPlayerGamePrivileges(),Player::ePlayerGamePrivilege_CreativeMode) ? GameType::CREATIVE : GameType::SURVIVAL;
		gameType = LevelSettings::validateGameType(gameType->getId());
		if (player->gameMode->getGameModeForPlayer() != gameType)
		{
			player->setPlayerGamePrivilege(Player::ePlayerGamePrivilege_CreativeMode,player->gameMode->getGameModeForPlayer()->getId() );
		}

		//shared_ptr<PlayerConnection> playerConnection = shared_ptr<PlayerConnection>(new PlayerConnection(server, connection, player));
		player->connection = playerConnection;	// Used to be assigned in PlayerConnection ctor but moved out so we can use shared_ptr

		// 4J Added to store UGC settings
		playerConnection->m_friendsOnlyUGC = packet->m_friendsOnlyUGC;
		playerConnection->m_offlineXUID = packet->m_offlineXuid;
		playerConnection->m_onlineXUID = packet->m_onlineXuid;

		// This player is now added to the list, so incrementing this value invalidates all previous PreLogin packets
		if(packet->m_friendsOnlyUGC) ++server->m_ugcPlayersVersion;

		addPlayerToReceiving( player );

		playerConnection->send( shared_ptr<LoginPacket>( new LoginPacket(L"", player->entityId, level->getLevelData()->getGenerator(), level->getSeed(), player->gameMode->getGameModeForPlayer()->getId(),
																		(byte) level->dimension->id, (byte) level->getMaxBuildHeight(), (byte) getMaxPlayers(),
																		level->difficulty, TelemetryManager->GetMultiplayerInstanceID(), (BYTE)playerIndex, level->useNewSeaLevel(), player->getAllPlayerGamePrivileges(),
																		level->getLevelData()->getXZSize(), level->getLevelData()->getHellScale() ) ) );
        playerConnection->send( shared_ptr<SetSpawnPositionPacket>( new SetSpawnPositionPacket(spawnPos->x, spawnPos->y, spawnPos->z) ) );
		playerConnection->send( shared_ptr<PlayerAbilitiesPacket>( new PlayerAbilitiesPacket(&player->abilities)) );
		delete spawnPos;

        sendLevelInfo(player, level);

        // 4J-PB - removed, since it needs to be localised in the language the client is in
		//server->players->broadcastAll( shared_ptr<ChatPacket>( new ChatPacket(L"�e" + playerEntity->name + L" joined the game.") ) );
		broadcastAll( shared_ptr<ChatPacket>( new ChatPacket(player->name, ChatPacket::e_ChatPlayerJoinedGame) ) );

		MemSect(14);
        add(player);
		MemSect(0);

		player->doTick(true, true, false);	// 4J - added - force sending of the nearest chunk before the player is teleported, so we have somewhere to arrive on...
        playerConnection->teleport(player->x, player->y, player->z, player->yRot, player->xRot);

        server->getConnection()->addPlayerConnection(playerConnection);
        playerConnection->send( shared_ptr<SetTimePacket>( new SetTimePacket(level->getTime()) ) );

		AUTO_VAR(activeEffects, player->getActiveEffects());
		for(AUTO_VAR(it, activeEffects->begin()); it != activeEffects->end(); ++it)
		{
			MobEffectInstance *effect = *it;
			playerConnection->send(shared_ptr<UpdateMobEffectPacket>( new UpdateMobEffectPacket(player->entityId, effect) ) );
		}

        player->initMenu();

		// If we are joining at the same time as someone in the end on this system is travelling through the win portal,
		// then we should set our wonGame flag to true so that respawning works when the EndPoem is closed
		INetworkPlayer *thisPlayer = player->connection->getNetworkPlayer();
		if( thisPlayer != NULL )
		{
			for(AUTO_VAR(it, players.begin()); it != players.end(); ++it)
			{
				shared_ptr<ServerPlayer> servPlayer = *it;
				INetworkPlayer *checkPlayer = servPlayer->connection->getNetworkPlayer();
				if(thisPlayer != checkPlayer && checkPlayer != NULL && thisPlayer->IsSameSystem( checkPlayer ) && servPlayer->wonGame )
				{
					player->wonGame = true;
					break;
				}
			}
		}
}

void PlayerList::setLevel(ServerLevelArray levels)
{
	playerIo = levels[0]->getLevelStorage()->getPlayerIO();
}

void PlayerList::changeDimension(shared_ptr<ServerPlayer> player, ServerLevel *from)
{
	ServerLevel *to = player->getLevel();

	// M2: the dragon is defeated only once. If the End chunks were unloaded and
	// later regenerated (or the world was reloaded), stale dragon/crystal
	// entities must not survive. The decorator already refuses to spawn new
	// ones, so drop anything that is still around when a player enters the End.
	if (to->dimension->id == 1 && to->getLevelData() != NULL && to->getLevelData()->getDragonKilled())
	{
		vector<shared_ptr<Entity> > stale;
		EnterCriticalSection(&to->m_entitiesCS);
		for (AUTO_VAR(it, to->entities.begin()); it != to->entities.end(); ++it)
		{
			if (((*it)->GetType() & eTYPE_ENDERDRAGON) != 0 ||
				((*it)->GetType() & eTYPE_ENDER_CRYSTAL) != 0)
			{
				stale.push_back(*it);
			}
		}
		LeaveCriticalSection(&to->m_entitiesCS);

		for (AUTO_VAR(it, stale.begin()); it != stale.end(); ++it)
		{
			app.DebugPrintf("END_CLEANUP: removing stale boss entity type=%d\n", (*it)->GetType());
			to->removeEntityImmediately(*it);
		}
	}

	if (from != NULL) from->getChunkMap()->remove(player);
	to->getChunkMap()->add(player);

	to->cache->create(((int) player->x) >> 4, ((int) player->z) >> 4);
}

int PlayerList::getMaxRange()
{
	return PlayerChunkMap::convertChunkRangeToBlock(getSimulationDistance());
}

 // 4J Changed return val to bool to check if new player or loaded player
bool PlayerList::load(shared_ptr<ServerPlayer> player)
{
	return playerIo->load(player);
}

void PlayerList::save(shared_ptr<ServerPlayer> player)
{
	playerIo->save(player);
}

// 4J Stu - TU-1 hotifx
// Add this function to take some of the code from the PlayerList::add function with the fixes
// for checking spawn area, especially in the nether. These needed to be done in a different order from before
// Fix for #13150 - When a player loads/joins a game after saving/leaving in the nether, sometimes they are spawned on top of the nether and cannot mine down
void PlayerList::validatePlayerSpawnPosition(shared_ptr<ServerPlayer> player)
{
	// 4J Stu - Some adjustments to make sure the current players position is correct
	// Make sure that the player is on the ground, and in the centre x/z of the current column
	app.DebugPrintf("Original pos is %f, %f, %f in dimension %d\n", player->x, player->y, player->z, player->dimension);

	double targetX = 0;
	if(player->x < 0) targetX = Mth::ceil(player->x) - 0.5;
	else targetX = Mth::floor(player->x) + 0.5;

	double targetY = floor(player->y);

	double targetZ = 0;
	if(player->z < 0) targetZ = Mth::ceil(player->z) - 0.5;
	else targetZ = Mth::floor(player->z) + 0.5;

	player->setPos(targetX, targetY, targetZ);

	app.DebugPrintf("New pos is %f, %f, %f in dimension %d\n", player->x, player->y, player->z, player->dimension);

	ServerLevel *level = server->getLevel(player->dimension);
    while (level->getCubes(player, player->bb)->size() != 0)
	{
        player->setPos(player->x, player->y + 1, player->z);
    }
	app.DebugPrintf("Final pos is %f, %f, %f in dimension %d\n", player->x, player->y, player->z, player->dimension);

	// 4J Stu - If we are in the nether and the above while loop has put us above the nether then we have a problem
	// Finding a valid, safe spawn point is potentially computationally expensive (may have to hunt through a large part
	// of the nether) so move the player to their spawn position in the overworld so that they do not lose their inventory
	// 4J Stu - We also use this mechanism to force a spawn point in the overworld for players who were in the save when the reset nether option was applied
	if(level->dimension->id == -1 && player->y > 125)
	{
		app.DebugPrintf("Player in the nether tried to spawn at y = %f, moving to overworld\n", player->y);
		player->setLevel(server->getLevel(0));
		player->gameMode->setLevel(server->getLevel(0));
		player->dimension = 0;

		level = server->getLevel(player->dimension);

		Pos *levelSpawn = level->getSharedSpawnPos();
		player->setPos(levelSpawn->x, levelSpawn->y, levelSpawn->z);
		delete levelSpawn;

		Pos *bedPosition = player->getRespawnPosition();
		if (bedPosition != NULL)
		{
			Pos *respawnPosition = Player::checkBedValidRespawnPosition(server->getLevel(player->dimension), bedPosition);
			if (respawnPosition != NULL)
			{
				player->moveTo(respawnPosition->x + 0.5f, respawnPosition->y + 0.1f, respawnPosition->z + 0.5f, 0, 0);
				player->setRespawnPosition(bedPosition);
			}
			delete bedPosition;
		}
		while (level->getCubes(player, player->bb)->size() != 0)
		{
			player->setPos(player->x, player->y + 1, player->z);
		}
		
		app.DebugPrintf("Updated pos is %f, %f, %f in dimension %d\n", player->x, player->y, player->z, player->dimension);
	}
}

void PlayerList::add(shared_ptr<ServerPlayer> player)
{
	//broadcastAll(shared_ptr<PlayerInfoPacket>( new PlayerInfoPacket(player->name, true, 1000) ) );
	if( player->connection->getNetworkPlayer() )
	{
		broadcastAll(shared_ptr<PlayerInfoPacket>( new PlayerInfoPacket( player ) ) );
	}

    players.push_back(player);

	// The new player must be visible to the same-system scan before
	// level->addEntity() starts sending tracked entities.
	updateSameSystemFlags();

	// 4J Added
	addPlayerToReceiving(player);

    // Ensure the area the player is spawning in is loaded!
	ServerLevel *level = server->getLevel(player->dimension);

	// 4J Stu - TU-1 hotfix
	// Fix for #13150 - When a player loads/joins a game after saving/leaving in the nether, sometimes they are spawned on top of the nether and cannot mine down
	// Some code from here has been moved to the above validatePlayerSpawnPosition function

	// 4J Stu - Swapped these lines about so that we get the chunk visiblity packet way ahead of all the add tracked entity packets
	// Fix for #9169 - ART : Sign text is replaced with the words �Awaiting approval�.
    changeDimension(player, NULL);
    level->addEntity(player);

	for (int i = 0; i < players.size(); i++)
	{
		shared_ptr<ServerPlayer> op = players.at(i);
		//player->connection->send(shared_ptr<PlayerInfoPacket>( new PlayerInfoPacket(op->name, true, op->latency) ) );
		if( op->connection->getNetworkPlayer() )
		{
			player->connection->send(shared_ptr<PlayerInfoPacket>( new PlayerInfoPacket( op ) ) );
		}
	}

	if(level->isAtLeastOnePlayerSleeping())
	{
		shared_ptr<ServerPlayer> firstSleepingPlayer = nullptr;
		for (unsigned int i = 0; i < players.size(); i++)
		{
			shared_ptr<ServerPlayer> thisPlayer = players[i];
			if(thisPlayer->isSleeping())
			{
				if(firstSleepingPlayer == NULL) firstSleepingPlayer = thisPlayer;				
				thisPlayer->connection->send(shared_ptr<ChatPacket>( new ChatPacket(thisPlayer->name, ChatPacket::e_ChatBedMeSleep)));
			}
		}
		player->connection->send(shared_ptr<ChatPacket>( new ChatPacket(firstSleepingPlayer->name, ChatPacket::e_ChatBedPlayerSleep)));
	}

	updateSameSystemFlags();
}

void PlayerList::move(shared_ptr<ServerPlayer> player)
{
	player->getLevel()->getChunkMap()->move(player);
}

void PlayerList::remove(shared_ptr<ServerPlayer> player)
{
#ifdef _WINDOWS64
	// Remember the TCP small id; free it only after the player has been fully
	// removed so lookups through the connection still resolve correctly.
	unsigned char tcpSmallId = 0;
	bool removeTcpPlayer = false;
	if (player->connection != NULL)
	{
		INetworkPlayer *np = player->connection->getNetworkPlayer();
		if (np != NULL && !np->IsHost())
		{
			tcpSmallId = np->GetSmallId();
			removeTcpPlayer = true;
		}
	}
#endif
    save(player);
	//4J Stu - We don't want to save the map data for guests, so when we are sure that the player is gone delete the map
	if(player->isGuest()) playerIo->deleteMapFilesForPlayer(player);
	ServerLevel *level = player->getLevel();
    level->removeEntity(player);
    level->getChunkMap()->remove(player);
	AUTO_VAR(it, find(players.begin(),players.end(),player));
	if( it != players.end() )
	{
		players.erase(it);
	}
	//broadcastAll(shared_ptr<PlayerInfoPacket>( new PlayerInfoPacket(player->name, false, 9999) ) );	

	removePlayerFromReceiving(player);
	player->connection = nullptr;		// Must remove reference to connection, or else there is a circular dependency
	delete player->gameMode;		// Gamemode also needs deleted as it references back to this player
	player->gameMode = NULL;

	// Do not save every player here. This method runs once per disconnect;
	// with 50-255 players leaving together it queued O(players^2) saves and
	// made the server look like it still had ghost players. The player's own
	// data was saved above, autosave runs periodically, and stopServer()
	// saves everyone.

#ifdef _WINDOWS64
	if (removeTcpPlayer)
	{
		g_NetworkManager.RemoveTcpPlayer(tcpSmallId);
	}
#endif

	// On a TCP server no player has a same-system sibling, so the O(P^2)
	// rescan is pure overhead for every disconnect. Only run it when at
	// least one remaining player still has the flag set (splitscreen).
	bool anySameSystem = player->m_hasSameSystemPlayers;
	for (unsigned int i = 0; i < players.size() && !anySameSystem; ++i)
	{
		if (players[i]->m_hasSameSystemPlayers)
		{
			anySameSystem = true;
			break;
		}
	}
	if (anySameSystem) updateSameSystemFlags();
}

shared_ptr<ServerPlayer> PlayerList::getPlayerForLogin(PendingConnection *pendingConnection, const wstring& userName, PlayerUID xuid, PlayerUID onlineXuid)
{
    if (players.size() >= maxPlayers)
	{
        pendingConnection->disconnect(DisconnectPacket::eDisconnect_ServerFull);
        return shared_ptr<ServerPlayer>();
    }
	
	shared_ptr<ServerPlayer> player = shared_ptr<ServerPlayer>(new ServerPlayer(server, server->getLevel(0), userName, new ServerPlayerGameMode(server->getLevel(0)) ));
	player->gameMode->player = player; // 4J added as had to remove this assignment from ServerPlayer ctor
	player->setXuid( xuid ); // 4J Added
	player->setOnlineXuid( onlineXuid ); // 4J Added

	// Work out the base server player settings
	INetworkPlayer *networkPlayer = pendingConnection->connection->getSocket()->getPlayer();
	if(networkPlayer != NULL && !networkPlayer->IsHost())
	{
		player->enableAllPlayerPrivileges( app.GetGameHostOption(eGameHostOption_TrustPlayers)>0 );
	}

	// 4J Added
	LevelRuleset *serverRuleDefs = app.getGameRuleDefinitions();
	if(serverRuleDefs != NULL)
	{
		player->gameMode->setGameRules( GameRuleDefinition::generateNewGameRulesInstance(GameRulesInstance::eGameRulesInstanceType_ServerPlayer, serverRuleDefs, pendingConnection->connection) );
	}

    return player;
}

shared_ptr<ServerPlayer> PlayerList::respawn(shared_ptr<ServerPlayer> serverPlayer, int targetDimension, bool keepAllPlayerData)
{
	// How we handle the entity tracker depends on whether we are the primary player currently, and whether there will be any player in the same system in the same dimension once we finish respawning. 
	bool isPrimary = canReceiveAllPackets(serverPlayer);	// Is this the primary player in its current dimension?
	int oldDimension = serverPlayer->dimension;
	bool isEmptying = ( targetDimension != oldDimension);		// We're not emptying this dimension on this machine if this player is going back into the same dimension

	// Also consider if there is another player on this machine which is in the same dimension and can take over as primary player
	if( isEmptying )
	{
		INetworkPlayer *thisPlayer = serverPlayer->connection->getNetworkPlayer();

		for( unsigned int i = 0; thisPlayer != NULL && i < players.size(); i++ )
		{
			shared_ptr<ServerPlayer> ep = players[i];
			if( ep == serverPlayer ) continue;
			if( ep->dimension !=  oldDimension ) continue;

			INetworkPlayer * otherPlayer = ep->connection->getNetworkPlayer();
			if( otherPlayer != NULL && thisPlayer->IsSameSystem(otherPlayer) )
			{
				// There's another player here in the same dimension - we're not the last one out
				isEmptying = false;
			}
		}
	}

	// Now we know where we stand, the actions to take are as follows:
	// (1) if this isn't the primary player, then we just need to remove it from the entity tracker
	// (2) if this Is the primary player then:
	//		(a) if isEmptying is true, then remove the player from the tracker, and send "remove entity" packets for anything seen (this is the original behaviour of the code)
	//		(b) if isEmptying is false, then we'll be transferring control of entity tracking to another player

	if( isPrimary )
	{
		if( isEmptying )
		{
			app.DebugPrintf("Emptying this dimension\n");
			serverPlayer->getLevel()->getTracker()->clear(serverPlayer);
		}
		else
		{
			app.DebugPrintf("Transferring... storing flags\n");
			serverPlayer->getLevel()->getTracker()->removeEntity(serverPlayer);
		}
	}
	else
	{
		app.DebugPrintf("Not primary player\n");
		serverPlayer->getLevel()->getTracker()->removeEntity(serverPlayer);
	}

	serverPlayer->getLevel()->getChunkMap()->remove(serverPlayer);
	AUTO_VAR(it, find(players.begin(),players.end(),serverPlayer));
	if( it != players.end() )
	{
	    players.erase(it);
	}
    server->getLevel(serverPlayer->dimension)->removeEntityImmediately(serverPlayer);

    Pos *bedPosition = serverPlayer->getRespawnPosition();

	removePlayerFromReceiving(serverPlayer);
    serverPlayer->dimension = targetDimension;

	EDefaultSkins skin = serverPlayer->getPlayerDefaultSkin();
	DWORD playerIndex = serverPlayer->getPlayerIndex();

	PlayerUID playerXuid = serverPlayer->getXuid();
	PlayerUID playerOnlineXuid = serverPlayer->getOnlineXuid();
	
    shared_ptr<ServerPlayer> player = shared_ptr<ServerPlayer>(new ServerPlayer(server, server->getLevel(serverPlayer->dimension), serverPlayer->name, new ServerPlayerGameMode(server->getLevel(serverPlayer->dimension))));
	player->restoreFrom(serverPlayer, keepAllPlayerData);
	if (keepAllPlayerData)
	{
		// Fix for #81759 - TU9: Content: Gameplay: Entering The End Exit Portal replaces the Player's currently held item with the first one from the Quickbar
		player->inventory->selected = serverPlayer->inventory->selected;
	}
	player->gameMode->player = player; // 4J added as had to remove this assignment from ServerPlayer ctor
	player->setXuid( playerXuid ); // 4J Added
	player->setOnlineXuid( playerOnlineXuid ); // 4J Added

	// 4J Stu - Don't reuse the id. If we do, then the player can be re-added after being removed, but the add packet gets sent before the remove packet
    //player->entityId = serverPlayer->entityId;

    player->connection = serverPlayer->connection;
	player->setPlayerDefaultSkin( skin );
	player->setIsGuest( serverPlayer->isGuest() );
	player->setPlayerIndex( playerIndex );
	player->setCustomSkin( serverPlayer->getCustomSkin() );
	player->setCustomCape( serverPlayer->getCustomCape() );
	player->setPlayerGamePrivilege(Player::ePlayerGamePrivilege_All, serverPlayer->getAllPlayerGamePrivileges());
	player->gameMode->setGameRules( serverPlayer->gameMode->getGameRules() );
	player->dimension = targetDimension;

	// 4J Stu - Added this as we need to know earlier if the player is the player for this connection so that
	// we can work out if they are the primary for the system and can receive all packets
	player->connection->setPlayer( player );

	addPlayerToReceiving(player);

    ServerLevel *level = server->getLevel(serverPlayer->dimension);

	// reset the player's game mode (first pick from old, then copy level if
	// necessary)
	updatePlayerGameMode(player, serverPlayer, level);

	if(serverPlayer->wonGame && targetDimension == oldDimension && serverPlayer->getHealth() > 0)
	{
		// If the player is still alive and respawning to the same dimension, they are just being added back from someone else viewing the Win screen
		player->moveTo(serverPlayer->x, serverPlayer->y, serverPlayer->z, serverPlayer->yRot, serverPlayer->xRot);
		if(bedPosition != NULL)
		{
			player->setRespawnPosition(bedPosition);
			delete bedPosition;
		}
		// Fix for #81759 - TU9: Content: Gameplay: Entering The End Exit Portal replaces the Player's currently held item with the first one from the Quickbar
		player->inventory->selected = serverPlayer->inventory->selected;
	}
    else if (bedPosition != NULL)
	{
        Pos *respawnPosition = Player::checkBedValidRespawnPosition(level, bedPosition);
        if (respawnPosition != NULL)
		{
            player->moveTo(respawnPosition->x + 0.5f, respawnPosition->y + 0.1f, respawnPosition->z + 0.5f, 0, 0);
            player->setRespawnPosition(bedPosition);
        }
		else
		{
			// M2: getRespawnPosition() falls back to the shared world spawn when
			// the player has no bed, so an invalid bed here must not leave the
			// player at the old position. Returning from the End used to drop
			// the player at the End coordinates (0, ~64, 0) in the Overworld,
			// which is often inside stone. Only show the "no respawn bed"
			// message if there really was a bed; otherwise use the world spawn
			// and let the collision loop below lift the player out.
			bool wasBed = level->getTile(bedPosition->x, bedPosition->y, bedPosition->z) == Tile::bed_Id;
			if (wasBed)
			{
				player->connection->send( shared_ptr<GameEventPacket>( new GameEventPacket(GameEventPacket::NO_RESPAWN_BED_AVAILABLE, 0) ) );
			}

			Pos *levelSpawn = level->getSharedSpawnPos();
			player->moveTo(levelSpawn->x + 0.5f, levelSpawn->y, levelSpawn->z + 0.5f, 0, 0);
			delete levelSpawn;
        }
		delete bedPosition;
    }
	else if (targetDimension != oldDimension || serverPlayer->wonGame || serverPlayer->getHealth() <= 0)
	{
		// M2: no bed and we are not simply returning to the same spot (End exit
		// portal, win screen, or death). restoreFrom() kept the old position,
		// which for the End exit is (0, ~64, 0) in the Overworld - often inside
		// stone or bedrock. Use the world spawn and let the collision loop below
		// lift the player out.
		Pos *levelSpawn = level->getSharedSpawnPos();
		player->moveTo(levelSpawn->x + 0.5f, levelSpawn->y, levelSpawn->z + 0.5f, 0, 0);
		delete levelSpawn;
	}

    // Ensure the area the player is spawning in is loaded!
    level->cache->create(((int) player->x) >> 4, ((int) player->z) >> 4);

    while (!level->getCubes(player, player->bb)->empty())
	{
        player->setPos(player->x, player->y + 1, player->z);
    }

	player->connection->send( shared_ptr<RespawnPacket>( new RespawnPacket((char) player->dimension, player->level->getSeed(), player->level->getMaxBuildHeight(),
																				player->gameMode->getGameModeForPlayer(), level->difficulty, level->getLevelData()->getGenerator(),
																				player->level->useNewSeaLevel(), player->entityId, level->getLevelData()->getXZSize(), level->getLevelData()->getHellScale()) ) );
    player->connection->teleport(player->x, player->y, player->z, player->yRot, player->xRot);

	if(keepAllPlayerData)
	{
		vector<MobEffectInstance *> *activeEffects = player->getActiveEffects();
		for(AUTO_VAR(it, activeEffects->begin()); it != activeEffects->end(); ++it)
		{
			MobEffectInstance *effect = *it;

			player->connection->send(shared_ptr<UpdateMobEffectPacket>( new UpdateMobEffectPacket(player->entityId, effect) ) );
		}
		delete activeEffects;
		player->getEntityData()->markDirty(Mob::DATA_EFFECT_COLOR_ID);
	}

    sendLevelInfo(player, level);

    level->getChunkMap()->add(player);
    level->addEntity(player);
    players.push_back(player);

    player->initMenu();

	// 4J-JEV - Dying before this point in the tutorial is pretty annoying,
	// making sure to remove health/hunger and give you back your meat.
	if( Minecraft::GetInstance()->isTutorial() 
		&& (!Minecraft::GetInstance()->gameMode->getTutorial()->isStateCompleted(e_Tutorial_State_Food_Bar)) )
	{
		app.getGameRuleDefinitions()->postProcessPlayer(player);
	}

	if( oldDimension == 1 && player->dimension != 1 )
	{
		player->displayClientMessage(IDS_PLAYER_LEFT_END);
	}

    return player;

}

void PlayerList::toggleDimension(shared_ptr<ServerPlayer> player, int targetDimension)
{
	int lastDimension = player->dimension;
	// How we handle the entity tracker depends on whether we are the primary player currently, and whether there will be any player in the same system in the same dimension once we finish respawning. 
	bool isPrimary = canReceiveAllPackets(player);	// Is this the primary player in its current dimension?
	bool isEmptying = true;

	// Also consider if there is another player on this machine which is in the same dimension and can take over as primary player
	INetworkPlayer *thisPlayer = player->connection->getNetworkPlayer();

	for( unsigned int i = 0; thisPlayer != NULL && i < players.size(); i++ )
	{
		shared_ptr<ServerPlayer> ep = players[i];
		if( ep == player ) continue;
		if( ep->dimension !=  lastDimension ) continue;

		INetworkPlayer * otherPlayer = ep->connection->getNetworkPlayer();
		if( otherPlayer != NULL && thisPlayer->IsSameSystem(otherPlayer) )
		{
			// There's another player here in the same dimension - we're not the last one out
			isEmptying = false;
		}
	}


	// Now we know where we stand, the actions to take are as follows:
	// (1) if this isn't the primary player, then we just need to remove it from the entity tracker
	// (2) if this Is the primary player then:
	//		(a) if isEmptying is true, then remove the player from the tracker, and send "remove entity" packets for anything seen (this is the original behaviour of the code)
	//		(b) if isEmptying is false, then we'll be transferring control of entity tracking to another player

	if( isPrimary )
	{
		if( isEmptying )
		{
			app.DebugPrintf("Toggle... Emptying this dimension\n");
			player->getLevel()->getTracker()->clear(player);
		}
		else
		{
			app.DebugPrintf("Toggle...  transferring\n");
			player->getLevel()->getTracker()->removeEntity(player);
		}
	}
	else
	{
		app.DebugPrintf("Toggle...  Not primary player\n");
		player->getLevel()->getTracker()->removeEntity(player);
	}

    ServerLevel *oldLevel = server->getLevel(player->dimension);

	// 4J Stu - Do this much earlier so we don't end up unloading chunks in the wrong dimension
	player->getLevel()->getChunkMap()->remove(player);

	if(player->dimension != 1 && targetDimension == 1)
	{
		player->displayClientMessage(IDS_PLAYER_ENTERED_END);
	}
	else if( player->dimension == 1 )
	{
		player->displayClientMessage(IDS_PLAYER_LEFT_END);
	}

    player->dimension = targetDimension;

    ServerLevel *newLevel = server->getLevel(player->dimension);

	// 4J Stu - Fix for #46423 - TU5: Art: Code: No burning animation visible after entering The Nether while burning
	player->clearFire(); // Stop burning if travelling through a portal

	// 4J Stu Added so that we remove entities from the correct level, after the respawn packet we will be in the wrong level
	player->flushEntitiesToRemove();

	player->connection->send( shared_ptr<RespawnPacket>( new RespawnPacket((char) player->dimension, newLevel->getSeed(), newLevel->getMaxBuildHeight(),
																		player->gameMode->getGameModeForPlayer(), newLevel->difficulty, newLevel->getLevelData()->getGenerator(),
																		newLevel->useNewSeaLevel(), player->entityId, newLevel->getLevelData()->getXZSize(), newLevel->getLevelData()->getHellScale()) ) );

    oldLevel->removeEntityImmediately(player);
    player->removed = false;

    double xt = player->x;
    double zt = player->z;
    double srcX = xt;
    double srcZ = zt;
    double scale = newLevel->getLevelData()->getHellScale(); // 4J Scale was 8 but this is all we can fit in
    if (player->dimension == -1)
	{
        xt /= scale;
        zt /= scale;
        player->moveTo(xt, player->y, zt, player->yRot, player->xRot);
        if (player->isAlive())
		{
            oldLevel->tick(player, false);
        }
    }
	else if (player->dimension == 0)
	{
        xt *= scale;
        zt *= scale;
        player->moveTo(xt, player->y, zt, player->yRot, player->xRot);
        if (player->isAlive())
		{
            oldLevel->tick(player, false);
        }
    }
	else
	{
		Pos *p = newLevel->getDimensionSpecificSpawn();

		xt = p->x;
		player->y = p->y;
		zt = p->z;
		delete p;
		player->moveTo(xt, player->y, zt, 90, 0);
		if (player->isAlive())
		{
			oldLevel->tick(player, false);
		}
	}
	
#ifdef _WINDOWS64
	// M2 test hook: log the portal coordinate scaling so the 1:8 Overworld <->
	// Nether ratio can be verified in an automated run.
	if (getenv("MC_PORTAL_LOG") != NULL)
	{
		app.DebugPrintf("PORTAL_SCALE: dim %d -> %d src %.1f,%.1f scale %d dst %.1f,%.1f\n",
			lastDimension, targetDimension, srcX, srcZ, (int) scale, xt, zt);
	}
#endif

	removePlayerFromReceiving(player, false, lastDimension);
	addPlayerToReceiving(player);

	if (lastDimension == 1)
	{
	}
	else
	{
		xt = (double) Mth::clamp((int) xt, -Level::MAX_LEVEL_SIZE + 128, Level::MAX_LEVEL_SIZE - 128);
		zt = (double) Mth::clamp((int) zt, -Level::MAX_LEVEL_SIZE + 128, Level::MAX_LEVEL_SIZE - 128);
		if (player->isAlive())
		{
			newLevel->addEntity(player);
			player->moveTo(xt, player->y, zt, player->yRot, player->xRot);
			newLevel->tick(player, false);
			newLevel->cache->autoCreate = true;
			(new PortalForcer())->force(newLevel, player);
			newLevel->cache->autoCreate = false;
		}
	}


    player->setLevel(newLevel);
    // 4J - chunk coordinates from the previous dimension must not block sending in the new one
    player->sentChunks.clear();
    changeDimension(player, oldLevel);

	player->gameMode->setLevel(newLevel);

	// Resend the teleport if we haven't yet sent the chunk they will land on
	if( player->sentChunks.find(ChunkPos(player->xChunk,player->zChunk)) == player->sentChunks.end() )
	{
		player->connection->teleport(player->x, player->y, player->z, player->yRot, player->xRot, false);
		// Force sending of the current chunk
		player->doTick(true, true, true);
	}
	
	player->connection->teleport(player->x, player->y, player->z, player->yRot, player->xRot);

	// 4J Stu - Fix for #64683 - Customer Encountered: TU7: Content: Gameplay: Potion effects are removed after using the Nether Portal
	vector<MobEffectInstance *> *activeEffects = player->getActiveEffects();
	for(AUTO_VAR(it, activeEffects->begin()); it != activeEffects->end(); ++it)
	{
		MobEffectInstance *effect = *it;

		player->connection->send(shared_ptr<UpdateMobEffectPacket>( new UpdateMobEffectPacket(player->entityId, effect) ) );
	}
	delete activeEffects;
	player->getEntityData()->markDirty(Mob::DATA_EFFECT_COLOR_ID);

    sendLevelInfo(player, newLevel);
    sendAllPlayerInfo(player);
}

void PlayerList::tick()
{
	// 4J - brought changes to how often this is sent forward from 1.2.3
    if (++sendAllPlayerInfoIn > SEND_PLAYER_INFO_INTERVAL)
	{
        sendAllPlayerInfoIn = 0;
    }

	if (sendAllPlayerInfoIn < players.size()) 
	{
		shared_ptr<ServerPlayer> op = players[sendAllPlayerInfoIn];
		//broadcastAll(shared_ptr<PlayerInfoPacket>( new PlayerInfoPacket(op->name, true, op->latency) ) );		
		if( op->connection->getNetworkPlayer() )
		{
			broadcastAll(shared_ptr<PlayerInfoPacket>( new PlayerInfoPacket( op ) ) );
		}
	}

	EnterCriticalSection(&m_closePlayersCS);
	while(!m_smallIdsToClose.empty())
	{
		BYTE smallId = m_smallIdsToClose.front();
		m_smallIdsToClose.pop_front();

		shared_ptr<ServerPlayer> player = nullptr;

		for(unsigned int i = 0; i < players.size(); i++)
		{
			shared_ptr<ServerPlayer> p = players.at(i);
			// 4J Stu - May be being a bit overprotective with all the NULL checks, but adding late in TU7 so want to be safe
			if (p != NULL && p->connection != NULL && p->connection->connection != NULL && p->connection->connection->getSocket() != NULL && p->connection->connection->getSocket()->getSmallId() == smallId )
			{
				player = p;
				break;
			}
		}

		if (player != NULL)
		{
			player->connection->disconnect( DisconnectPacket::eDisconnect_Closed );
		}
	}
	LeaveCriticalSection(&m_closePlayersCS);

	EnterCriticalSection(&m_kickPlayersCS);
	while(!m_smallIdsToKick.empty())
	{
		BYTE smallId = m_smallIdsToKick.front();
		m_smallIdsToKick.pop_front();
		INetworkPlayer *selectedPlayer = g_NetworkManager.GetPlayerBySmallId(smallId);
		if( selectedPlayer != NULL )
		{
			if( selectedPlayer->IsLocal() != TRUE )
			{
//#ifdef _XBOX
				PlayerUID xuid = selectedPlayer->GetUID();
				// Kick this player from the game
				shared_ptr<ServerPlayer> player = nullptr;

				for(unsigned int i = 0; i < players.size(); i++)
				{
					shared_ptr<ServerPlayer> p = players.at(i);
					PlayerUID playersXuid = p->getOnlineXuid();
					if (p != NULL && ProfileManager.AreXUIDSEqual(playersXuid, xuid ) )
					{
						player = p;
						break;
					}
				}

				if (player != NULL)
				{
					m_bannedXuids.push_back( player->getOnlineXuid() );
					// 4J Stu - If we have kicked a player, make sure that they have no privileges if they later try to join the world when trust players is off
					player->enableAllPlayerPrivileges( false );
					player->connection->setWasKicked();
					player->connection->send( shared_ptr<DisconnectPacket>( new DisconnectPacket(DisconnectPacket::eDisconnect_Kicked) ));
				}
//#endif
			}
		}
	}
	LeaveCriticalSection(&m_kickPlayersCS);

	// Check our receiving players, and if they are dead see if we can replace them
	for(unsigned int dim = 0; dim < 2; ++dim)
	{
		for(unsigned int i = 0; i < receiveAllPlayers[dim].size(); ++i)
		{
			shared_ptr<ServerPlayer> currentPlayer  = receiveAllPlayers[dim][i];
			if(currentPlayer->removed)
			{
				shared_ptr<ServerPlayer> newPlayer = findAlivePlayerOnSystem(currentPlayer);
				if(newPlayer != NULL)
				{
					receiveAllPlayers[dim][i] = newPlayer;
					app.DebugPrintf("Replacing primary player %ls with %ls in dimension %d\n", currentPlayer->name.c_str(), newPlayer->name.c_str(), dim);
				}
			}
		}
	}
}

bool PlayerList::isTrackingTile(int x, int y, int z, int dimension)
{
	return server->getLevel(dimension)->getChunkMap()->isTrackingTile(x, y, z);
}

// 4J added - make sure that any tile updates for the chunk at this location get prioritised for sending
void PlayerList::prioritiseTileChanges(int x, int y, int z, int dimension)
{
	server->getLevel(dimension)->getChunkMap()->prioritiseTileChanges(x, y, z);
}

void PlayerList::broadcastAll(shared_ptr<Packet> packet)
{
    for (unsigned int i = 0; i < players.size(); i++)
	{
        shared_ptr<ServerPlayer> player = players[i];
        player->connection->send(packet);
    }
}

void PlayerList::broadcastAll(shared_ptr<Packet> packet, int dimension)
{
    for (unsigned int i = 0; i < players.size(); i++)
	{
        shared_ptr<ServerPlayer> player = players[i];
        if (player->dimension == dimension) player->connection->send(packet);
    }
}

wstring PlayerList::getPlayerNames()
{
    wstring msg;
    for (unsigned int i = 0; i < players.size(); i++)
	{
        if (i > 0) msg += L", ";
        msg += players[i]->name;
    }
    return msg;
}

bool PlayerList::isWhiteListed(const wstring& name)
{
	return true;
}

bool PlayerList::isOp(const wstring& name)
{
	return false;
}

bool PlayerList::isOp(shared_ptr<ServerPlayer> player)
{
	bool cheatsEnabled = app.GetGameHostOption(eGameHostOption_CheatsEnabled);
#ifdef _DEBUG_MENUS_ENABLED
	cheatsEnabled = cheatsEnabled || app.GetUseDPadForDebug();
#endif
	INetworkPlayer *networkPlayer = player->connection->getNetworkPlayer();
	bool isOp = cheatsEnabled && (player->isModerator() || (networkPlayer != NULL && networkPlayer->IsHost()));
	return isOp;
}

shared_ptr<ServerPlayer> PlayerList::getPlayer(const wstring& name)
{
    for (unsigned int i = 0; i < players.size(); i++) 
	{
        shared_ptr<ServerPlayer> p = players[i];
        if (p->name == name)	// 4J - used to be case insensitive (using equalsIgnoreCase) - imagine we'll be shifting to XUIDs anyway
		{
            return p;
        }
    }
    return nullptr;
}

// 4J Added
shared_ptr<ServerPlayer> PlayerList::getPlayer(PlayerUID uid)
{
    for (unsigned int i = 0; i < players.size(); i++) 
	{
        shared_ptr<ServerPlayer> p = players[i];
        if (p->getXuid() == uid || p->getOnlineXuid() == uid)	// 4J - used to be case insensitive (using equalsIgnoreCase) - imagine we'll be shifting to XUIDs anyway
		{
            return p;
        }
    }
    return nullptr;
}

void PlayerList::sendMessage(const wstring& name, const wstring& message)
{
    shared_ptr<ServerPlayer> player = getPlayer(name);
    if (player != NULL)
	{
        player->connection->send( shared_ptr<ChatPacket>( new ChatPacket(message) ) );
    }
}

void PlayerList::broadcast(double x, double y, double z, double range, int dimension, shared_ptr<Packet> packet)
{
	broadcast(nullptr, x, y, z, range, dimension, packet);
}

void PlayerList::broadcast(shared_ptr<Player> except, double x, double y, double z, double range, int dimension, shared_ptr<Packet> packet)
{
	// 4J - altered so that we don't send to the same machine more than once. Add the source player to the machines we have "sent" to as it doesn't need to go to that
	// machine either
	vector< shared_ptr<ServerPlayer> > sentTo;
	if( except != NULL )
	{
		sentTo.push_back(dynamic_pointer_cast<ServerPlayer>(except));
	}

    for (unsigned int i = 0; i < players.size(); i++)
	{
        shared_ptr<ServerPlayer> p = players[i];
        if (p == except) continue;
        if (p->dimension != dimension) continue;

		// 4J - don't send to the same machine more than once
		bool dontSend = false;
		if( sentTo.size() )
		{
			INetworkPlayer *thisPlayer = p->connection->getNetworkPlayer();
			if( thisPlayer == NULL )
			{
				dontSend = true;
			}
			else
			{
				for(unsigned int j = 0; j < sentTo.size(); j++ )	
				{
					shared_ptr<ServerPlayer> player2 = sentTo[j];
					INetworkPlayer *otherPlayer = player2->connection->getNetworkPlayer();
					if( otherPlayer != NULL && thisPlayer->IsSameSystem(otherPlayer) )
					{
						dontSend = true;
					}
				}
			}
		}
		if( dontSend )
		{
			continue;
		}


        double xd = x - p->x;
        double yd = y - p->y;
        double zd = z - p->z;
        if (xd * xd + yd * yd + zd * zd < range * range)
		{
#if 0 // _DEBUG
			shared_ptr<LevelSoundPacket> SoundPacket= dynamic_pointer_cast<LevelSoundPacket>(packet);

			if(SoundPacket)
			{

				app.DebugPrintf("---broadcast - eSoundType_[%d] ",SoundPacket->getSound());
				OutputDebugStringW(ConsoleSoundEngine::wchSoundNames[SoundPacket->getSound()]);
				app.DebugPrintf("\n");
			}
#endif
            p->connection->send(packet);
			sentTo.push_back( p );
        }
    }

}

void PlayerList::broadcastToAllOps(const wstring& message)
{
    shared_ptr<Packet> chatPacket = shared_ptr<ChatPacket>( new ChatPacket(message) );
    for (unsigned int i = 0; i < players.size(); i++)
	{
        shared_ptr<ServerPlayer> p = players[i];
        if (isOp(p->name))
		{
            p->connection->send(chatPacket);
        }
    }
}

bool PlayerList::sendTo(const wstring& name, shared_ptr<Packet> packet)
{
    shared_ptr<ServerPlayer> player = getPlayer(name);
    if (player != NULL)
	{
        player->connection->send(packet);
        return true;
    }
    return false;
}

void PlayerList::saveAll(ProgressListener *progressListener, bool bDeleteGuestMaps /*= false*/)
{
	if(progressListener != NULL) progressListener->progressStart(IDS_PROGRESS_SAVING_PLAYERS);
	// 4J - playerIo can be NULL if we have have to exit a game really early on due to network failure
	if(playerIo)
	{
		playerIo->saveAllCachedData();
		for (unsigned int i = 0; i < players.size(); i++)
		{
			playerIo->save(players[i]);
		
			//4J Stu - We don't want to save the map data for guests, so when we are sure that the player is gone delete the map
			if(bDeleteGuestMaps && players[i]->isGuest()) playerIo->deleteMapFilesForPlayer(players[i]);

			if(progressListener != NULL) progressListener->progressStagePercentage((i * 100)/ ((int)players.size()));
		}
		playerIo->clearOldPlayerFiles();
		playerIo->saveMapIdLookup();
	}
}

void PlayerList::whiteList(const wstring& playerName)
{
}

void PlayerList::blackList(const wstring& playerName)
{
}

void PlayerList::reloadWhitelist()
{
}

void PlayerList::sendLevelInfo(shared_ptr<ServerPlayer> player, ServerLevel *level)
{
    player->connection->send( shared_ptr<SetTimePacket>( new SetTimePacket(level->getTime()) ) );
    if (level->isRaining())
	{
        player->connection->send( shared_ptr<GameEventPacket>( new GameEventPacket(GameEventPacket::START_RAINING, 0) ) );
    }
	else
	{
		// 4J Stu - Fix for #44836 - Customer Encountered: Out of Sync Weather [A-10]
		// If it was raining when the player left the level, and is now not raining we need to make sure that state is updated
        player->connection->send( shared_ptr<GameEventPacket>( new GameEventPacket(GameEventPacket::STOP_RAINING, 0) ) );
	}

	// send the stronghold position if there is one 
	if((level->dimension->id==0) && level->getLevelData()->getHasStronghold())
	{
		player->connection->send( shared_ptr<XZPacket>( new XZPacket(XZPacket::STRONGHOLD,level->getLevelData()->getXStronghold(),level->getLevelData()->getZStronghold()) ) );
	}
}

void PlayerList::sendAllPlayerInfo(shared_ptr<ServerPlayer> player)
{
    player->refreshContainer(player->inventoryMenu);
    player->resetSentInfo();
}

int PlayerList::getPlayerCount()
{
	return (int)players.size();
}

int PlayerList::getPlayerCount(ServerLevel *level)
{
	int count = 0;

	for(AUTO_VAR(it, players.begin()); it != players.end(); ++it)
	{
		if( (*it)->level == level ) ++count;
	}

	return count;
}

int PlayerList::getMaxPlayers()
{
	return maxPlayers;
}

MinecraftServer *PlayerList::getServer()
{
	return server;
}

int PlayerList::getViewDistance()
{
	return viewDistance;
}

int PlayerList::getChunksPerTick()
{
	return chunksPerTick;
}

void PlayerList::setOverrideGameMode(GameType *gameMode)
{
	this->overrideGameMode = gameMode;
}

void PlayerList::updatePlayerGameMode(shared_ptr<ServerPlayer> newPlayer, shared_ptr<ServerPlayer> oldPlayer, Level *level)
{

	// reset the player's game mode (first pick from old, then copy level if
	// necessary)
	if (oldPlayer != NULL)
	{
		newPlayer->gameMode->setGameModeForPlayer(oldPlayer->gameMode->getGameModeForPlayer());
	}
	else if (overrideGameMode != NULL)
	{
		newPlayer->gameMode->setGameModeForPlayer(overrideGameMode);
	}
	newPlayer->gameMode->updateGameMode(level->getLevelData()->getGameType());
}

void PlayerList::setAllowCheatsForAllPlayers(bool allowCommands)
{
	this->allowCheatsForAllPlayers = allowCommands;
}

shared_ptr<ServerPlayer> PlayerList::findAlivePlayerOnSystem(shared_ptr<ServerPlayer> player)
{
	int dimIndex, playerDim;
	dimIndex = playerDim = player->dimension;
	if( dimIndex == -1 ) dimIndex = 1;
	else if( dimIndex == 1) dimIndex = 2;

	INetworkPlayer *thisPlayer = player->connection->getNetworkPlayer();
	if( thisPlayer != NULL )
	{
		for(AUTO_VAR(itP, players.begin()); itP != players.end(); ++itP)
		{
			shared_ptr<ServerPlayer> newPlayer = *itP;

			INetworkPlayer *otherPlayer = newPlayer->connection->getNetworkPlayer();

			if( !newPlayer->removed &&
				newPlayer != player &&
				newPlayer->dimension == playerDim &&
				otherPlayer != NULL &&
				otherPlayer->IsSameSystem( thisPlayer )
				)
			{
				return newPlayer;
			}
		}
	}

	return nullptr;
}

void PlayerList::removePlayerFromReceiving(shared_ptr<ServerPlayer> player, bool usePlayerDimension /*= true*/, int dimension /*= 0*/)
{
	int dimIndex, playerDim;
	dimIndex = playerDim = usePlayerDimension ? player->dimension : dimension;
	if( dimIndex == -1 ) dimIndex = 1;
	else if( dimIndex == 1) dimIndex = 2;

#ifndef _CONTENT_PACKAGE
	app.DebugPrintf("Requesting remove player %ls as primary in dimension %d\n", player->name.c_str(), dimIndex);
#endif
	bool playerRemoved = false;

	AUTO_VAR(it, find( receiveAllPlayers[dimIndex].begin(), receiveAllPlayers[dimIndex].end(), player));
	if( it != receiveAllPlayers[dimIndex].end() )
	{
#ifndef _CONTENT_PACKAGE
		app.DebugPrintf("Remove: Removing player %ls as primary in dimension %d\n", player->name.c_str(), dimIndex);
#endif
		receiveAllPlayers[dimIndex].erase(it);
		playerRemoved = true;
		player->m_receivesAllPackets = false;
	}

	INetworkPlayer *thisPlayer = player->connection->getNetworkPlayer();
	if( thisPlayer != NULL && playerRemoved )
	{
		for(AUTO_VAR(itP, players.begin()); itP != players.end(); ++itP)
		{
			shared_ptr<ServerPlayer> newPlayer = *itP;

			INetworkPlayer *otherPlayer = newPlayer->connection->getNetworkPlayer();

			if( newPlayer != player &&
				newPlayer->dimension == playerDim &&
				otherPlayer != NULL &&
				otherPlayer->IsSameSystem( thisPlayer )
				)
			{
#ifndef _CONTENT_PACKAGE
				app.DebugPrintf("Remove: Adding player %ls as primary in dimension %d\n", newPlayer->name.c_str(), dimIndex);
#endif
				receiveAllPlayers[dimIndex].push_back( newPlayer );
				newPlayer->m_receivesAllPackets = true;
				break;
			}
		}
	}
	else if( thisPlayer == NULL )
	{
#ifndef _CONTENT_PACKAGE
		app.DebugPrintf("Remove: Qnet player for %ls was NULL so re-checking all players\n", player->name.c_str() );
#endif
		// 4J Stu - Something went wrong, or possibly the QNet player left before we got here.
		// Re-check all active players and make sure they have someone on their system to receive all packets
		for(AUTO_VAR(itP, players.begin()); itP != players.end(); ++itP)
		{
			shared_ptr<ServerPlayer> newPlayer = *itP;
			INetworkPlayer *checkingPlayer = newPlayer->connection->getNetworkPlayer();

			if( checkingPlayer != NULL )
			{
				int newPlayerDim = 0;
				if( newPlayer->dimension == -1 ) newPlayerDim = 1;
				else if( newPlayer->dimension == 1) newPlayerDim = 2;
				bool foundPrimary = false;
				for(AUTO_VAR(it, receiveAllPlayers[newPlayerDim].begin()); it != receiveAllPlayers[newPlayerDim].end(); ++it)
				{
					shared_ptr<ServerPlayer> primaryPlayer = *it;
					INetworkPlayer *primPlayer = primaryPlayer->connection->getNetworkPlayer();
					if(primPlayer != NULL && checkingPlayer->IsSameSystem( primPlayer ) )
					{
						foundPrimary = true;
						break;
					}
				}
				if(!foundPrimary)
				{
#ifndef _CONTENT_PACKAGE
					app.DebugPrintf("Remove: Adding player %ls as primary in dimension %d\n", newPlayer->name.c_str(), newPlayerDim);
#endif
					receiveAllPlayers[newPlayerDim].push_back( newPlayer );
					newPlayer->m_receivesAllPackets = true;
				}
			}
		}
	}
}

void PlayerList::addPlayerToReceiving(shared_ptr<ServerPlayer> player)
{
	int playerDim = 0;
	if( player->dimension == -1 ) playerDim = 1;
	else if( player->dimension == 1) playerDim = 2;

#ifndef _CONTENT_PACKAGE
	app.DebugPrintf("Requesting add player %ls as primary in dimension %d\n", player->name.c_str(), playerDim);
#endif

	bool shouldAddPlayer = true;

	INetworkPlayer *thisPlayer = player->connection->getNetworkPlayer();

	if( thisPlayer == NULL )
	{
#ifndef _CONTENT_PACKAGE
		app.DebugPrintf("Add: Qnet player for player %ls is NULL so not adding them\n", player->name.c_str() );
#endif
		shouldAddPlayer = false;
	}
	else
	{
		for(AUTO_VAR(it, receiveAllPlayers[playerDim].begin()); it != receiveAllPlayers[playerDim].end(); ++it)
		{
			shared_ptr<ServerPlayer> oldPlayer = *it;
			INetworkPlayer *checkingPlayer = oldPlayer->connection->getNetworkPlayer();
			if(checkingPlayer != NULL && checkingPlayer->IsSameSystem( thisPlayer ) ) 
			{
				shouldAddPlayer = false;
				break;
			}
		}
	}
	
	if( shouldAddPlayer )
	{
#ifndef _CONTENT_PACKAGE
		app.DebugPrintf("Add: Adding player %ls as primary in dimension %d\n", player->name.c_str(), playerDim);
#endif
		receiveAllPlayers[playerDim].push_back( player );
	}
	// Cache the result for the hot send path (O(1) afterwards).
	player->m_receivesAllPackets =
		find(receiveAllPlayers[playerDim].begin(), receiveAllPlayers[playerDim].end(), player) != receiveAllPlayers[playerDim].end();

#ifdef _WINDOWS64
	// TCP prototype diagnostics: whether this player is allowed to receive
	// per-system packets (AddPlayerPacket and friends) on the host.
	{
		FILE *f = NULL;
		fopen_s(&f, "H:\\Temp\\tcp_players.log", "a");
		if (f)
		{
			fprintf(f, "addPlayerToReceiving: name=%ls netPlayer=%p shouldAdd=%d\n",
				player->name.c_str(), (void *)thisPlayer, shouldAddPlayer ? 1 : 0);
			fclose(f);
		}
	}
#endif
}

bool PlayerList::canReceiveAllPackets(shared_ptr<ServerPlayer> player)
{
	// O(1): the flag is maintained by add/removePlayerFromReceiving. The old
	// linear scan was O(players) on every PlayerConnection::send and became a
	// serious bottleneck with hundreds of TCP players.
	return player != NULL && player->m_receivesAllPackets;
}

void PlayerList::updateSameSystemFlags()
{
	// TrackedEntity::isVisible used to scan every player to find splitscreen
	// siblings whenever an entity was out of range. With 150+ TCP players that
	// made entity tracking O(players^2) per entity. Cache the answer on the
	// player; TCP players are always their own system, so the scan is skipped
	// entirely for them.
	for (unsigned int i = 0; i < players.size(); ++i)
	{
		shared_ptr<ServerPlayer> p = players[i];
		bool hasSameSystem = false;
		INetworkPlayer *np = (p->connection != NULL) ? p->connection->getNetworkPlayer() : NULL;
		if (np != NULL)
		{
			for (unsigned int j = 0; j < players.size(); ++j)
			{
				if (i == j) continue;
				shared_ptr<ServerPlayer> q = players[j];
				INetworkPlayer *nq = (q->connection != NULL) ? q->connection->getNetworkPlayer() : NULL;
				if (nq != NULL && np->IsSameSystem(nq))
				{
					hasSameSystem = true;
					break;
				}
			}
		}
		p->m_hasSameSystemPlayers = hasSameSystem;
	}
}

void PlayerList::kickPlayerByShortId(BYTE networkSmallId)
{
	EnterCriticalSection(&m_kickPlayersCS);
	m_smallIdsToKick.push_back(networkSmallId);
	LeaveCriticalSection(&m_kickPlayersCS);
}

void  PlayerList::closePlayerConnectionBySmallId(BYTE networkSmallId)
{
	EnterCriticalSection(&m_closePlayersCS);
	m_smallIdsToClose.push_back(networkSmallId);
	LeaveCriticalSection(&m_closePlayersCS);
}

bool PlayerList::isXuidBanned(PlayerUID xuid)
{
	if( xuid == INVALID_XUID ) return false;

	bool banned = false;

	for( AUTO_VAR(it, m_bannedXuids.begin()); it != m_bannedXuids.end(); ++it )
	{
		if( ProfileManager.AreXUIDSEqual( xuid, *it ) )
		{
			banned = true;
			break;
		}
	}

	return banned;
}

// AP added for Vita so the range can be increased once the level starts
void PlayerList::setViewDistance(int newViewDistance)
{
	viewDistance = newViewDistance;
}
