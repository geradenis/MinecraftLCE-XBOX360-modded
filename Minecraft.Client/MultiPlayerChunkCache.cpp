#include "stdafx.h"
#include "MultiPlayerChunkCache.h"
#include "ServerChunkCache.h"
#include "..\Minecraft.World\net.minecraft.world.level.chunk.h"
#include "..\Minecraft.World\net.minecraft.world.level.dimension.h"
#include "..\Minecraft.World\Arrays.h"
#include "..\Minecraft.World\StringHelpers.h"
#include "MinecraftServer.h"
#include "ServerLevel.h"
#include "..\Minecraft.World\Tile.h"
#include "..\Minecraft.World\WaterLevelChunk.h"

MultiPlayerChunkCache::MultiPlayerChunkCache(Level *level)
{
	m_XZSize = level->dimension->getXZSize(); // 4J Added

	emptyChunk = new EmptyLevelChunk(level, byteArray(16 * 16 * Level::maxBuildHeight), 0, 0);

	// For normal world dimension, create a chunk that can be used to create the illusion of infinite water at the edge of the world
	if( level->dimension->id == 0 )
	{
		byteArray bytes = byteArray(16 * 16 * 128);

		// Superflat.... make grass, not water...
		if(level->getLevelData()->getGenerator() == LevelType::lvl_flat)
		{
			for( int x = 0; x < 16; x++ )
				for( int y = 0; y < 128; y++ )
					for( int z = 0; z < 16; z++ )
					{
						unsigned char tileId = 0;
						if( y == 3 ) tileId = Tile::grass_Id;
						else if( y <= 2 ) tileId = Tile::dirt_Id;

						bytes[x << 11 | z << 7 | y] = tileId;
					}
		}
		else
		{
			for( int x = 0; x < 16; x++ )
				for( int y = 0; y < 128; y++ )
					for( int z = 0; z < 16; z++ )
					{
						unsigned char tileId = 0;
						if( y <= ( level->getSeaLevel() - 10 ) ) tileId = Tile::rock_Id;
						else if( y < level->getSeaLevel() ) tileId = Tile::calmWater_Id;

						bytes[x << 11 | z << 7 | y] = tileId;
					}
		}

		waterChunk = new WaterLevelChunk(level, bytes, 0, 0);

		delete[] bytes.data;

		if(level->getLevelData()->getGenerator() == LevelType::lvl_flat)
		{
			for( int x = 0; x < 16; x++ )
				for( int y = 0; y < 128; y++ )
					for( int z = 0; z < 16; z++ )
					{
						if( y >= 3 )
						{
							((WaterLevelChunk *)waterChunk)->setLevelChunkBrightness(LightLayer::Sky,x,y,z,15);
						}
					}
		}
		else
		{
			for( int x = 0; x < 16; x++ )
				for( int y = 0; y < 128; y++ )
					for( int z = 0; z < 16; z++ )
					{
						if( y >= ( level->getSeaLevel() - 1 ) )
						{
							((WaterLevelChunk *)waterChunk)->setLevelChunkBrightness(LightLayer::Sky,x,y,z,15);
						}
						else
						{
							((WaterLevelChunk *)waterChunk)->setLevelChunkBrightness(LightLayer::Sky,x,y,z,2);
						}
					}
		}
	}
	else
	{
		waterChunk = NULL;
	}

	this->level = level;

	InitializeCriticalSectionAndSpinCount(&m_csLoadCreate,4000);

#ifdef _LARGE_WORLDS
	// M3: upper bound for the client-side chunk cache. The visible window is at
	// most 25x25 = 625 chunks (one player); with split-screen the per-player
	// window shrinks so the total stays around 625. 2048 leaves room for
	// chunks that were just dropped and may still be referenced for a frame.
	m_maxCachedChunks = 2048;
#ifdef _WINDOWS64
	wchar_t maxClientBuf[64];
	if (GetEnvironmentVariableW(L"MC_MAX_CLIENT_CHUNKS", maxClientBuf, 64) > 0)
	{
		m_maxCachedChunks = _wtoi(maxClientBuf);
	}
#endif
#endif
}

MultiPlayerChunkCache::~MultiPlayerChunkCache()
{
	delete emptyChunk;
	delete waterChunk;

#ifdef _LARGE_WORLDS
	// These only hold pointers to chunks owned by loadedChunkList.
	m_droppedLRU.clear();
	m_droppedSet.clear();
#endif

	AUTO_VAR(itEnd, loadedChunkList.end());
	for (AUTO_VAR(it, loadedChunkList.begin()); it != itEnd; it++)
		delete *it;
	cache.clear();

	DeleteCriticalSection(&m_csLoadCreate);
}


bool MultiPlayerChunkCache::hasChunk(int x, int z)
{
	// This cache always claims to have chunks, although it might actually just return empty data if it doesn't have anything
	return true;
}

// 4J  added - find out if we actually really do have a chunk in our cache
bool MultiPlayerChunkCache::reallyHasChunk(int x, int z)
{
	EnterCriticalSection(&m_csLoadCreate);
	AUTO_VAR(it, cache.find(chunkKey(x, z)));
	bool has = (it != cache.end() && it->second.chunk != NULL && it->second.hasData);
	LeaveCriticalSection(&m_csLoadCreate);
	return has;
}

void MultiPlayerChunkCache::drop(int x, int z)
{
	// 4J Stu - We do want to drop any entities in the chunks, especially for the case when a player is dead as they will
	// not get the RemoveEntity packet if an entity is removed.
    LevelChunk *chunk = getChunk(x, z);
	// M3: never unload the shared sentinels.
	if (chunk == NULL || chunk == emptyChunk || chunk == waterChunk || chunk->isEmpty())
	{
		return;
	}

#ifdef SHARING_ENABLED
	// 4J-M1: the chunk is leaving our view, so stop sharing its block storage with
	// the server chunk. This lets ServerChunkCache actually delete the server chunk
	// once it falls out of its bounded unloaded-chunk cache.
	if( g_NetworkManager.IsHost() )
	{
		chunk->stopSharingTilesAndData(true);
	}
#endif

	// Added parameter here specifies that we don't want to delete tile entities, as they won't get recreated unless they've got update packets
	// The tile entities are in general only created on the client by virtue of the chunk rebuild
	chunk->unload(false);

	// 4J - We just want to clear out the entities in the chunk, but everything else should be valid
	chunk->loaded = true;

#ifdef _LARGE_WORLDS
	// M3: keep the chunk in the cache but mark it as a candidate for eviction.
	EnterCriticalSection(&m_csLoadCreate);
	markDroppedLocked(chunk);
	trimCacheLocked();
	LeaveCriticalSection(&m_csLoadCreate);
#endif
}

#ifdef _LARGE_WORLDS
void MultiPlayerChunkCache::markDroppedLocked(LevelChunk *chunk)
{
	if (chunk == NULL) return;
	if (m_droppedSet.insert(chunk).second)
	{
		m_droppedLRU.push_back(chunk);
	}
}

void MultiPlayerChunkCache::markActiveLocked(LevelChunk *chunk)
{
	if (chunk == NULL) return;
	if (m_droppedSet.erase(chunk) > 0)
	{
		m_droppedLRU.remove(chunk);
	}
}

void MultiPlayerChunkCache::trimCacheLocked()
{
	if (m_maxCachedChunks <= 0) return;

	while ((int)loadedChunkList.size() > m_maxCachedChunks && !m_droppedLRU.empty())
	{
		LevelChunk *victim = m_droppedLRU.front();
		m_droppedLRU.pop_front();
		m_droppedSet.erase(victim);

		AUTO_VAR(it, cache.find(chunkKey(victim->x, victim->z)));
		if (it != cache.end() && it->second.chunk == victim)
		{
			cache.erase(it);
		}

		for (size_t i = 0; i < loadedChunkList.size(); ++i)
		{
			if (loadedChunkList[i] == victim)
			{
				loadedChunkList[i] = loadedChunkList.back();
				loadedChunkList.pop_back();
				break;
			}
		}

		delete victim;
	}
}
#endif

LevelChunk *MultiPlayerChunkCache::create(int x, int z)
{
	const __int64 key = chunkKey(x, z);

	EnterCriticalSection(&m_csLoadCreate);
	AUTO_VAR(it, cache.find(key));
	if (it != cache.end() && it->second.chunk != NULL)
	{
		LevelChunk *existing = it->second.chunk;
#ifdef _LARGE_WORLDS
		// The chunk is visible again; it must not be evicted by trimCacheLocked().
		markActiveLocked(existing);
#endif
		LeaveCriticalSection(&m_csLoadCreate);
		existing->load();
		return existing;
	}

	// M2: create the chunk while holding the lock so that two threads cannot
	// create it twice. The old code used an atomic compare-exchange on the flat
	// array; a hash map needs the lock for the lookup+insert pair.
	if( g_NetworkManager.IsHost() )		// force here to disable sharing of data
	{
		// 4J-JEV: We are about to use shared data, abort if the server is stopped and the data is deleted.
		if (MinecraftServer::getInstance()->serverHalted())
		{
			LeaveCriticalSection(&m_csLoadCreate);
			return NULL;
		}

		// If we're the host, then don't create the chunk, share data from the server's copy 
#ifdef _LARGE_WORLDS
		ServerChunkCache *serverCache = MinecraftServer::getInstance()->getLevel(level->dimension->id)->cache;
		// M2: pin the server chunk while the client chunk is constructed. The
		// LevelChunk ctor increments m_clientShareCount only at its very end, so
		// without the pin trimUnloadedCache() could free the server chunk in the
		// window between the lookup and the constructor.
		LevelChunk *serverChunk = serverCache->getChunkLoadedOrUnloadedPinned(x,z);
		if (serverChunk == NULL || serverCache->isSentinelChunk(serverChunk))
		{
			// M3: the server chunk was evicted between the visibility packet and
			// this call. Sharing the all-air sentinel would make the client chunk
			// permanently empty, so fall through to a normal empty chunk instead.
			app.DebugPrintf("MultiPlayerChunkCache::create: no server chunk for %d,%d\n", x, z);
			serverCache->releaseChunk(serverChunk);
			serverChunk = NULL;
		}
#else
		LevelChunk *serverChunk = MinecraftServer::getInstance()->getLevel(level->dimension->id)->cache->getChunk(x,z);
#endif
		if (serverChunk != NULL)
		{
			LevelChunk *chunk = new LevelChunk(level, x, z, serverChunk);
			chunk->loaded = true;
#ifdef _LARGE_WORLDS
			serverCache->releaseChunk(serverChunk);
#endif

			ClientChunkEntry &entry = cache[key];
			entry.chunk = chunk;
			entry.hasData = true;
			loadedChunkList.push_back(chunk);
			LeaveCriticalSection(&m_csLoadCreate);

			// If we're sharing with the server, we'll need to calculate our heightmap now, which isn't shared. If we aren't sharing with the server,
			// then this will be calculated when the chunk data arrives.
			chunk->recalcHeightmapOnly();

			// M3: mark the chunk dirty only after the heightmap exists. Marking it
			// before the recalc let the render thread build the chunk as empty and
			// nothing re-dirtied it afterwards (invisible chunks after a reload).
			level->setTilesDirty( x * 16 , 0 , z * 16 , x * 16 + 15, 127, z * 16 + 15);
			return chunk;
		}
	}

	// Non-host, or the host could not provide a server chunk for this position.
	{
		// Passing an empty array into the LevelChunk ctor, which it now detects and sets up the chunk as compressed & empty
		byteArray bytes;

		LevelChunk *chunk = new LevelChunk(level, bytes, x, z);

		// 4J - changed to use new methods for lighting
		chunk->setSkyLightDataAllBright(); 
//			Arrays::fill(chunk->skyLight->data, (byte) 255);
		chunk->loaded = true;

		ClientChunkEntry &entry = cache[key];
		entry.chunk = chunk;
		// Do not reset entry.hasData: dataReceived() may have already been called
		// for this chunk before the renderer created it.
		loadedChunkList.push_back(chunk);
		LeaveCriticalSection(&m_csLoadCreate);
		return chunk;
	}
}

LevelChunk *MultiPlayerChunkCache::getChunk(int x, int z)
{
	EnterCriticalSection(&m_csLoadCreate);
	AUTO_VAR(it, cache.find(chunkKey(x, z)));
	LevelChunk *chunk = (it != cache.end()) ? it->second.chunk : NULL;
	LeaveCriticalSection(&m_csLoadCreate);
	return chunk != NULL ? chunk : emptyChunk;
}

LevelChunk *MultiPlayerChunkCache::getChunkIfLoaded(int x, int z)
{
	EnterCriticalSection(&m_csLoadCreate);
	AUTO_VAR(it, cache.find(chunkKey(x, z)));
	LevelChunk *chunk = (it != cache.end()) ? it->second.chunk : NULL;
	LeaveCriticalSection(&m_csLoadCreate);
	return chunk;
}

bool MultiPlayerChunkCache::save(bool force, ProgressListener *progressListener)
{
	return true;
}

bool MultiPlayerChunkCache::tick()
{
	return false;
}

bool MultiPlayerChunkCache::shouldSave()
{
	return false;
}

void MultiPlayerChunkCache::postProcess(ChunkSource *parent, int x, int z)
{
}

vector<Biome::MobSpawnerData *> *MultiPlayerChunkCache::getMobsAt(MobCategory *mobCategory, int x, int y, int z)
{
	return NULL;
}

TilePos *MultiPlayerChunkCache::findNearestMapFeature(Level *level, const wstring &featureName, int x, int y, int z)
{
	return NULL;
}

wstring MultiPlayerChunkCache::gatherStats()
{
	EnterCriticalSection(&m_csLoadCreate);
	int size = (int)loadedChunkList.size();
	LeaveCriticalSection(&m_csLoadCreate);
	return L"MultiplayerChunkCache: " + _toString<int>(size);
	
}

void MultiPlayerChunkCache::dataReceived(int x, int z)
{
	EnterCriticalSection(&m_csLoadCreate);
	cache[chunkKey(x, z)].hasData = true;
	LeaveCriticalSection(&m_csLoadCreate);
}