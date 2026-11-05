#include "stdafx.h"
#include "ServerChunkCache.h"
#include "ServerLevel.h"
#include "MinecraftServer.h"
#include "PlayerList.h"
#include "ServerPlayer.h"
#include "..\Minecraft.World\net.minecraft.world.level.h"
#include "..\Minecraft.World\net.minecraft.world.level.dimension.h"
#include "..\Minecraft.World\net.minecraft.world.level.storage.h"
#include "..\Minecraft.World\net.minecraft.world.level.chunk.h"
#include "..\Minecraft.World\Pos.h"
#include "..\Minecraft.World\ProgressListener.h"
#include "..\Minecraft.World\ThreadName.h"
#include "..\Minecraft.World\compression.h"
#include "..\Minecraft.World\OldChunkStorage.h"

#ifdef _WINDOWS64
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#pragma comment(lib, "psapi.lib")

// M2 diagnostics: the chunk cache must never delete a chunk that is still
// reachable from m_cache. Any violation is logged here before the cache
// self-heals, so the root cause can be identified without another crash dump.
static void chunkDebugLog(const char *fmt, ...)
{
	FILE *f = NULL;
	fopen_s(&f, "H:\\Temp\\m2_chunks.log", "a");
	if (f == NULL) return;
	va_list args;
	va_start(args, fmt);
	vfprintf(f, fmt, args);
	va_end(args);
	fputc('\n', f);
	fclose(f);
}

static bool chunkDebugEnabled()
{
	static int enabled = -1;
	if (enabled < 0)
	{
		const char *value = getenv("MC_CHUNK_LOG");
		enabled = (value != NULL && value[0] == '1') ? 1 : 0;
	}
	return enabled != 0;
}
#define CHUNK_LOG(...) do { if (chunkDebugEnabled()) chunkDebugLog(__VA_ARGS__); } while (0)
#else
#define CHUNK_LOG(...) ((void)0)
#endif

ServerChunkCache::ServerChunkCache(ServerLevel *level, ChunkStorage *storage, ChunkSource *source)
{
	autoCreate = false;	// 4J added
    
	emptyChunk = new EmptyLevelChunk(level, byteArray( Level::CHUNK_TILE_COUNT ), 0, 0);

    this->level = level;
    this->storage = storage;
    this->source = source;
	this->m_XZSize = source->m_XZSize;

	// M2: m_cache / m_unloadedCache are hash maps, no fixed-size allocation.

#ifdef _LARGE_WORLDS
	// M1: by default keep a bounded number of unloaded chunks around. Setting
	// MC_MAX_UNLOADED=0 restores the original unlimited behaviour (useful as an M0 baseline).
	m_maxUnloadedChunks = 256;
	m_unloadTick = 0;
	m_evictedChunks = 0;
#ifdef _WINDOWS64
	wchar_t maxUnloadedBuf[64];
	if (GetEnvironmentVariableW(L"MC_MAX_UNLOADED", maxUnloadedBuf, 64) > 0)
	{
		m_maxUnloadedChunks = _wtoi(maxUnloadedBuf);
	}
#endif
#endif

	InitializeCriticalSectionAndSpinCount(&m_csLoadCreate,4000);
	InitializeCriticalSectionAndSpinCount(&m_csCreate,4000);
}

// 4J-PB added
ServerChunkCache::~ServerChunkCache()
{
	delete emptyChunk;
	delete source;

#ifdef _LARGE_WORLDS
	// Chunks in m_unloadedCache are not in m_loadedChunkList, so deleting both
	// sets does not double-delete anything.
	for (AUTO_VAR(it, m_unloadedCache.begin()); it != m_unloadedCache.end(); ++it)
	{
		delete it->second;
	}
	m_unloadedCache.clear();
	m_unloadedLRU.clear();
	m_unloadedLRUIndex.clear();
#endif

	AUTO_VAR(itEnd, m_loadedChunkList.end());
	for (AUTO_VAR(it, m_loadedChunkList.begin()); it != itEnd; it++)
		delete *it;
	m_cache.clear();
	DeleteCriticalSection(&m_csCreate);
	DeleteCriticalSection(&m_csLoadCreate);
}

// M2: helpers used to keep m_cache, m_loadedChunkList, m_unloadedCache and the
// unloaded LRU consistent. The caller must hold m_csLoadCreate.
bool ServerChunkCache::findCacheKeyForChunk(LevelChunk *chunk, __int64 *outKey)
{
	for (AUTO_VAR(it, m_cache.begin()); it != m_cache.end(); ++it)
	{
		if (it->second == chunk)
		{
			if (outKey != NULL) *outKey = it->first;
			return true;
		}
	}
	return false;
}

bool ServerChunkCache::eraseCacheEntryForChunk(LevelChunk *chunk, __int64 *outKey)
{
	for (AUTO_VAR(it, m_cache.begin()); it != m_cache.end(); ++it)
	{
		if (it->second == chunk)
		{
			if (outKey != NULL) *outKey = it->first;
			m_cache.erase(it);
			return true;
		}
	}
	return false;
}

void ServerChunkCache::restoreUnloadedChunk(LevelChunk *chunk)
{
	if (chunk == NULL || chunk == emptyChunk) return;

	const __int64 ownKey = chunkKey(chunk->x, chunk->z);
	AUTO_VAR(cit, m_unloadedCache.find(ownKey));
	if (cit == m_unloadedCache.end() || cit->second == chunk)
	{
		m_unloadedCache[ownKey] = chunk;
	}
	else
	{
		CHUNK_LOG("RESTORE_KEY_OCCUPIED tick=%u dim=%d chunk=%d,%d ptr=%p other=%p",
			m_unloadTick, level->dimension->id, chunk->x, chunk->z, chunk, cit->second);
	}
	onChunkUnloaded(chunk);
}

void ServerChunkCache::discardChunkLocked(LevelChunk *chunk)
{
	if (chunk == NULL || chunk == emptyChunk) return;

	if (chunk->getClientShareCount() != 0)
	{
		// A client copy still points at this storage, so it cannot be deleted.
		// Keep it in the unloaded LRU instead.
		restoreUnloadedChunk(chunk);
		return;
	}

	onChunkReloaded(chunk);
	const __int64 ownKey = chunkKey(chunk->x, chunk->z);
	AUTO_VAR(cit, m_unloadedCache.find(ownKey));
	if (cit != m_unloadedCache.end() && cit->second == chunk) m_unloadedCache.erase(cit);
	delete chunk;
}

void ServerChunkCache::removeFromLoadedList(LevelChunk *chunk)
{
	if (chunk == NULL) return;
	for (size_t i = 0; i < m_loadedChunkList.size(); )
	{
		if (m_loadedChunkList[i] == chunk)
		{
			m_loadedChunkList[i] = m_loadedChunkList.back();
			m_loadedChunkList.pop_back();
			// Do not advance: the element swapped in may also match.
		}
		else
		{
			++i;
		}
	}
}

bool ServerChunkCache::hasChunk(int x, int z)
{
	EnterCriticalSection(&m_csLoadCreate);
	AUTO_VAR(it, m_cache.find(chunkKey(x, z)));
	bool found = false;
	if (it != m_cache.end() && it->second != NULL)
	{
		LevelChunk *lc = it->second;
		if (lc->x == x && lc->z == z)
		{
			found = true;
		}
		else
		{
			CHUNK_LOG("HASCHUNK_COORD_MISMATCH tick=%u dim=%d req=%d,%d got=%d,%d ptr=%p",
				m_unloadTick, level->dimension->id, x, z, lc->x, lc->z, lc);
		}
	}
	LeaveCriticalSection(&m_csLoadCreate);
	return found;
}

vector<LevelChunk *> *ServerChunkCache::getLoadedChunkList()
{
	return &m_loadedChunkList;
}

vector<LevelChunk *> ServerChunkCache::copyLoadedChunkList()
{
	// M2: callers must not iterate the live vector while another thread (the
	// post-process thread) can push newly created chunks into it.
	vector<LevelChunk *> chunks;
	EnterCriticalSection(&m_csLoadCreate);
	chunks = m_loadedChunkList;
	LeaveCriticalSection(&m_csLoadCreate);
	return chunks;
}

void ServerChunkCache::drop(int x, int z)
{
	// 4J - we're not dropping things anymore now that we have a fixed sized cache
#ifdef _LARGE_WORLDS

	bool canDrop = false;
//	if (level->dimension->mayRespawn())
//	{
//		Pos *spawnPos = level->getSharedSpawnPos();
//		int xd = x * 16 + 8 - spawnPos->x;
//		int zd = z * 16 + 8 - spawnPos->z;
//		delete spawnPos;
//		int r = 128;
//		if (xd < -r || xd > r || zd < -r || zd > r)
//		{
//			canDrop = true;
//}
//	}
//	else
	{
		canDrop = true;
	}
	if(canDrop)
	{
		EnterCriticalSection(&m_csLoadCreate);
		AUTO_VAR(it, m_cache.find(chunkKey(x, z)));
		if (it != m_cache.end() && it->second != NULL)
		{
			LevelChunk *chunk = it->second;
			if (chunk->x == x && chunk->z == z)
			{
				// Avoid queueing the same chunk more than once per tick.
				if (m_toDropSet.insert(chunk).second)
				{
					m_toDrop.push_back(chunk);
				}
			}
			else
			{
				CHUNK_LOG("DROP_COORD_MISMATCH tick=%u dim=%d req=%d,%d got=%d,%d ptr=%p",
					m_unloadTick, level->dimension->id, x, z, chunk->x, chunk->z, chunk);
			}
		}
		LeaveCriticalSection(&m_csLoadCreate);
	}
#endif
}

void ServerChunkCache::dropAll()
{
#ifdef _LARGE_WORLDS
	// Copy the list under the lock; drop() takes the same lock.
	vector<LevelChunk *> chunks;
	EnterCriticalSection(&m_csLoadCreate);
	chunks = m_loadedChunkList;
	LeaveCriticalSection(&m_csLoadCreate);
	for (size_t i = 0; i < chunks.size(); ++i)
	{
		drop(chunks[i]->x, chunks[i]->z);
	}
#endif
}

// 4J - this is the original (and virtual) interface to create
LevelChunk *ServerChunkCache::create(int x, int z)
{
	return create(x, z, false);
}

LevelChunk *ServerChunkCache::create(int x, int z, bool asyncPostProcess)	// 4J - added extra parameter
{
	const __int64 key = chunkKey(x, z);

	// Fast path: the chunk is already fully in the map.
	EnterCriticalSection(&m_csLoadCreate);
	AUTO_VAR(it, m_cache.find(key));
	if (it != m_cache.end() && it->second != NULL && it->second->x == x && it->second->z == z)
	{
		LevelChunk *existing = it->second;
		LeaveCriticalSection(&m_csLoadCreate);
		return existing;
	}
	LeaveCriticalSection(&m_csLoadCreate);

	// M2: terrain generation/loading is serialised by m_csCreate, not by the map
	// lock. m_csLoadCreate is held only for short map operations: the client
	// render thread calls getChunkLoadedOrUnloaded() and must never wait for a
	// chunk to generate.
	EnterCriticalSection(&m_csCreate);

	// Another thread may have created the chunk while we waited for m_csCreate.
	EnterCriticalSection(&m_csLoadCreate);
	it = m_cache.find(key);
	if (it != m_cache.end() && it->second != NULL && it->second->x == x && it->second->z == z)
	{
		LevelChunk *existing = it->second;
		LeaveCriticalSection(&m_csLoadCreate);
		LeaveCriticalSection(&m_csCreate);
		return existing;
	}
	LeaveCriticalSection(&m_csLoadCreate);

	LevelChunk *chunk = load(x, z);
	if (chunk == NULL && source != NULL)
	{
		chunk = source->getChunk(x, z);
	}

	// M2: a chunk loaded from storage carries its own coordinates in the save
	// data. Never insert a chunk under a key that does not match its position:
	// the unload path derives the key from the chunk, so a mismatch would leave
	// a dangling pointer in m_cache once the chunk is evicted.
	if (chunk != NULL && chunk != emptyChunk && (chunk->x != x || chunk->z != z))
	{
		CHUNK_LOG("CREATE_COORD_MISMATCH tick=%u dim=%d req=%d,%d got=%d,%d ptr=%p",
			m_unloadTick, level->dimension->id, x, z, chunk->x, chunk->z, chunk);
		EnterCriticalSection(&m_csLoadCreate);
		discardChunkLocked(chunk);
		LeaveCriticalSection(&m_csLoadCreate);
		chunk = NULL;
	}

	if (chunk == NULL || chunk == emptyChunk)
	{
		// emptyChunk has x=z=0, so it must never be stored under an arbitrary
		// coordinate. Returning it is enough for the callers that need it.
		LeaveCriticalSection(&m_csCreate);
		return emptyChunk;
	}

	chunk->load();

	EnterCriticalSection(&m_csLoadCreate);
	AUTO_VAR(cit, m_cache.find(key));
	if (cit != m_cache.end() && cit->second != NULL && cit->second != chunk)
	{
		LevelChunk *existing = cit->second;
		if (existing->x == x && existing->z == z)
		{
			CHUNK_LOG("CREATE_DUPLICATE tick=%u dim=%d at=%d,%d ours=%p existing=%p",
				m_unloadTick, level->dimension->id, x, z, chunk, existing);
			discardChunkLocked(chunk);
			LeaveCriticalSection(&m_csLoadCreate);
			LeaveCriticalSection(&m_csCreate);
			return existing;
		}

		// A stale entry with the wrong coordinates occupies our key. Move it to
		// the unloaded LRU (so any in-flight reader keeps valid memory for the
		// grace period) and replace it.
		CHUNK_LOG("CREATE_REPLACE_BAD_KEY tick=%u dim=%d at=%d,%d bad=%p got=%d,%d",
			m_unloadTick, level->dimension->id, x, z, existing, existing->x, existing->z);
		m_cache.erase(cit);
		removeFromLoadedList(existing);
		restoreUnloadedChunk(existing);
	}

	m_cache[key] = chunk;
	if (std::find(m_loadedChunkList.begin(), m_loadedChunkList.end(), chunk) == m_loadedChunkList.end())
	{
		m_loadedChunkList.push_back(chunk);
	}
	// An active chunk must never be tracked by the unloaded LRU. This is
	// normally a no-op; it repairs the case where a reload raced with trim.
	onChunkReloaded(chunk);
	LeaveCriticalSection(&m_csLoadCreate);
	LeaveCriticalSection(&m_csCreate);

	// 4J - this will run a recalcHeightmap if source is a randomlevelsource, which has been split out from source::getChunk so that
	// we are doing it after the chunk has been added to the cache - otherwise a lot of the lighting fails as lights aren't added if the chunk
	// they are in fail ServerChunkCache::hasChunk.
	source->lightChunk(chunk);
	updatePostProcessFlags( x, z );

	// 4J - If post-processing is to be async, then let the server know about requests rather than processing directly here. Note that
	// these hasChunk() checks appear to be incorrect - the chunks checked by these map out as:
	//
	// 1.		2.		3.		4.
	// oxx		xxo		ooo		ooo
	// oPx		Poo		oox		xoo
	// ooo		ooo		oPx		Pxo
	//
	// where P marks the chunk that is being considered for postprocessing, and x marks chunks that needs to be loaded. It would seem that the
	// chunks which need to be loaded should stay the same relative to the chunk to be processed, but the hasChunk checks in 3 cases check again
	// the chunk which is to be processed itself rather than (what I presume to be) the correct position.
	// Don't think we should change in case it alters level creation.

	if( asyncPostProcess )
	{
		// 4J Stu - TODO This should also be calling the same code as chunk->checkPostProcess, but then we cannot guarantee we are in the server add the post-process request
		if ( ( (chunk->terrainPopulated & LevelChunk::sTerrainPopulatedFromHere) == 0) && hasChunk(x + 1, z + 1) && hasChunk(x, z + 1) && hasChunk(x + 1, z)) MinecraftServer::getInstance()->addPostProcessRequest(this, x, z);
		if (hasChunk(x - 1, z) && ((getChunk(x - 1, z)->terrainPopulated & LevelChunk::sTerrainPopulatedFromHere ) == 0 ) && hasChunk(x - 1, z + 1) && hasChunk(x, z + 1) && hasChunk(x - 1, z)) MinecraftServer::getInstance()->addPostProcessRequest(this, x - 1, z);
		if (hasChunk(x, z - 1) && ((getChunk(x, z - 1)->terrainPopulated & LevelChunk::sTerrainPopulatedFromHere ) == 0 ) && hasChunk(x + 1, z - 1) && hasChunk(x, z - 1) && hasChunk(x + 1, z)) MinecraftServer::getInstance()->addPostProcessRequest(this, x, z - 1);
		if (hasChunk(x - 1, z - 1) && ((getChunk(x - 1, z - 1)->terrainPopulated & LevelChunk::sTerrainPopulatedFromHere ) == 0 ) && hasChunk(x - 1, z - 1) && hasChunk(x, z - 1) && hasChunk(x - 1, z)) MinecraftServer::getInstance()->addPostProcessRequest(this, x - 1, z - 1);
	}
	else
	{
		chunk->checkPostProcess(this, this, x, z);
	}

	// 4J - Now try and fix up any chests that were saved pre-1.8.2. We don't want to do this to this particular chunk as we don't know if all its neighbours are loaded yet, and we
	// need the neighbours to be able to work out the facing direction for the chests. Therefore process any neighbouring chunk that loading this chunk would be the last neighbour for.
	// 5 cases illustrated below, where P is the chunk to be processed, T is this chunk, and x are other chunks that need to be checked for being present

	// 1.		2.		3.		4.		5.
	// ooooo	ooxoo	ooooo	ooooo	ooooo
	// oxooo	oxPxo	oooxo	ooooo	ooxoo
	// xPToo	ooToo	ooTPx	ooToo	oxPxo	(in 5th case P and T are same)
	// oxooo	ooooo	oooxo	oxPxo	ooxoo
	// ooooo	ooooo	ooooo	ooxoo	ooooo

	if( hasChunk( x - 1, z ) && hasChunk( x - 2, z ) && hasChunk( x - 1, z + 1 ) && hasChunk( x - 1, z - 1 ) ) chunk->checkChests( this, x - 1, z );
	if( hasChunk( x, z + 1) && hasChunk( x , z + 2 ) && hasChunk( x - 1, z + 1 ) && hasChunk( x + 1, z + 1 ) ) chunk->checkChests( this, x, z + 1);
	if( hasChunk( x + 1, z ) && hasChunk( x + 2, z ) && hasChunk( x + 1, z + 1 ) && hasChunk( x + 1, z - 1 ) ) chunk->checkChests( this, x + 1, z );
	if( hasChunk( x, z - 1) && hasChunk( x , z - 2 ) && hasChunk( x - 1, z - 1 ) && hasChunk( x + 1, z - 1 ) ) chunk->checkChests( this, x, z - 1);
	if( hasChunk( x - 1, z ) && hasChunk( x + 1, z ) && hasChunk ( x, z - 1 ) && hasChunk( x, z + 1 ) ) chunk->checkChests( this, x, z );

#ifdef __PS3__
	Sleep(1);
#endif // __PS3__
    return chunk;

}

// 4J Stu - Split out this function so that we get a chunk without loading entities
// This is used when sharing server chunk data on the main thread
LevelChunk *ServerChunkCache::getChunk(int x, int z)
{
	EnterCriticalSection(&m_csLoadCreate);
	AUTO_VAR(it, m_cache.find(chunkKey(x, z)));
	LevelChunk *lc = (it != m_cache.end()) ? it->second : NULL;
	if (lc != NULL && (lc->x != x || lc->z != z))
	{
		CHUNK_LOG("GETCHUNK_COORD_MISMATCH tick=%u dim=%d req=%d,%d got=%d,%d ptr=%p",
			m_unloadTick, level->dimension->id, x, z, lc->x, lc->z, lc);
		lc = NULL;
	}
	LeaveCriticalSection(&m_csLoadCreate);

	if( lc )
	{
		return lc;
	}

	if( level->isFindingSpawn || autoCreate )
	{
		return create(x, z);
	}

	return emptyChunk;
}

LevelChunk *ServerChunkCache::getChunkIfLoaded(int x, int z)
{
	EnterCriticalSection(&m_csLoadCreate);
	AUTO_VAR(it, m_cache.find(chunkKey(x, z)));
	LevelChunk *lc = (it != m_cache.end()) ? it->second : NULL;
	if (lc != NULL && (lc->x != x || lc->z != z))
	{
		CHUNK_LOG("GETCHUNKIFLOADED_COORD_MISMATCH tick=%u dim=%d req=%d,%d got=%d,%d ptr=%p",
			m_unloadTick, level->dimension->id, x, z, lc->x, lc->z, lc);
		lc = NULL;
	}
	LeaveCriticalSection(&m_csLoadCreate);
	return lc;
}

#ifdef _LARGE_WORLDS
// 4J added - this special variation on getChunk also checks the unloaded chunk cache. It is called on a host machine from the client-side level when:
// (1) Trying to determine whether the client blocks and data are the same as those on the server, so we can start sharing them
// (2) Trying to resync the lighting data from the server to the client
// As such it is really important that we don't return emptyChunk in these situations, when we actually still have the block/data/lighting in the unloaded cache
LevelChunk *ServerChunkCache::getChunkLoadedOrUnloaded(int x, int z)
{
	EnterCriticalSection(&m_csLoadCreate);
	AUTO_VAR(it, m_cache.find(chunkKey(x, z)));
	LevelChunk *lc = (it != m_cache.end()) ? it->second : NULL;
	if (lc != NULL && (lc->x != x || lc->z != z))
	{
		CHUNK_LOG("GETCHUNKLOADED_COORD_MISMATCH tick=%u dim=%d req=%d,%d got=%d,%d ptr=%p",
			m_unloadTick, level->dimension->id, x, z, lc->x, lc->z, lc);
		lc = NULL;
	}
	if (lc == NULL)
	{
		AUTO_VAR(uit, m_unloadedCache.find(chunkKey(x, z)));
		if (uit != m_unloadedCache.end()) lc = uit->second;
		if (lc != NULL && (lc->x != x || lc->z != z))
		{
			CHUNK_LOG("GETUNLOADED_COORD_MISMATCH tick=%u dim=%d req=%d,%d got=%d,%d ptr=%p",
				m_unloadTick, level->dimension->id, x, z, lc->x, lc->z, lc);
			lc = NULL;
		}
	}
	LeaveCriticalSection(&m_csLoadCreate);

	if( lc )
	{
		return lc;
	}

	if( level->isFindingSpawn || autoCreate )
	{
		return create(x, z);
	}

	return emptyChunk;
}

LevelChunk *ServerChunkCache::getChunkLoadedOrUnloadedPinned(int x, int z)
{
	LevelChunk *lc = NULL;
	EnterCriticalSection(&m_csLoadCreate);
	AUTO_VAR(it, m_cache.find(chunkKey(x, z)));
	if (it != m_cache.end()) lc = it->second;
	if (lc != NULL && (lc->x != x || lc->z != z))
	{
		CHUNK_LOG("GETPINNED_COORD_MISMATCH tick=%u dim=%d req=%d,%d got=%d,%d ptr=%p",
			m_unloadTick, level->dimension->id, x, z, lc->x, lc->z, lc);
		lc = NULL;
	}
	if (lc == NULL)
	{
		AUTO_VAR(uit, m_unloadedCache.find(chunkKey(x, z)));
		if (uit != m_unloadedCache.end()) lc = uit->second;
		if (lc != NULL && (lc->x != x || lc->z != z))
		{
			CHUNK_LOG("GETPINNED_UNLOADED_COORD_MISMATCH tick=%u dim=%d req=%d,%d got=%d,%d ptr=%p",
				m_unloadTick, level->dimension->id, x, z, lc->x, lc->z, lc);
			lc = NULL;
		}
	}
	if (lc != NULL && lc != emptyChunk)
	{
		// Keep the chunk alive until releaseChunk() is called.
		InterlockedIncrement(&lc->m_clientShareCount);
	}
	LeaveCriticalSection(&m_csLoadCreate);

	if (lc != NULL) return lc;

	if (level->isFindingSpawn || autoCreate)
	{
		lc = create(x, z);
		if (lc != NULL && lc != emptyChunk)
		{
			EnterCriticalSection(&m_csLoadCreate);
			InterlockedIncrement(&lc->m_clientShareCount);
			LeaveCriticalSection(&m_csLoadCreate);
		}
		return lc;
	}

	return emptyChunk;
}

void ServerChunkCache::releaseChunk(LevelChunk *chunk)
{
	if (chunk == NULL || chunk == emptyChunk) return;
	InterlockedDecrement(&chunk->m_clientShareCount);
}
#endif

// 4J Added //
#ifdef _LARGE_WORLDS
void ServerChunkCache::dontDrop(int x, int z)
{
	EnterCriticalSection(&m_csLoadCreate);
	AUTO_VAR(it, m_cache.find(chunkKey(x, z)));
	if (it != m_cache.end() && it->second != NULL)
	{
		LevelChunk *chunk = it->second;
		if (m_toDropSet.erase(chunk) > 0)
		{
			m_toDrop.erase(std::remove(m_toDrop.begin(), m_toDrop.end(), chunk), m_toDrop.end());
		}
	}
	LeaveCriticalSection(&m_csLoadCreate);
}
#endif

LevelChunk *ServerChunkCache::load(int x, int z)
{
    if (storage == NULL) return NULL;

    LevelChunk *levelChunk = NULL;
	bool fromUnloadedCache = false;

#ifdef _LARGE_WORLDS
	// M2: only the unloaded-cache lookup is protected by the map lock. Storage
	// I/O and terrain generation happen outside it so lookups never block on them.
	EnterCriticalSection(&m_csLoadCreate);
	AUTO_VAR(it, m_unloadedCache.find(chunkKey(x, z)));
	if (it != m_unloadedCache.end())
	{
		levelChunk = it->second;
		m_unloadedCache.erase(it);
		fromUnloadedCache = true;
		if (levelChunk != NULL) onChunkReloaded(levelChunk);
	}
	LeaveCriticalSection(&m_csLoadCreate);
	if (levelChunk == NULL)
#endif
	{
		levelChunk = storage->load(level, x, z);
	}

	// M2: the compressed chunk format stores its own x/z inside the data. If
	// those coordinates do not match the requested chunk, the chunk must never
	// be inserted into m_cache under this key: the unload path computes the key
	// from chunk->x/z, so the entry would survive the unload and later point at
	// freed memory. Put the chunk back into the unloaded LRU (if it came from
	// there) and let the caller generate/load the correct one.
	if (levelChunk != NULL && (levelChunk->x != x || levelChunk->z != z))
	{
		CHUNK_LOG("LOAD_COORD_MISMATCH tick=%u dim=%d req=%d,%d got=%d,%d ptr=%p fromUnloaded=%d",
			m_unloadTick, level->dimension->id, x, z, levelChunk->x, levelChunk->z, levelChunk,
			fromUnloadedCache ? 1 : 0);
#ifdef _LARGE_WORLDS
		if (fromUnloadedCache)
		{
			EnterCriticalSection(&m_csLoadCreate);
			restoreUnloadedChunk(levelChunk);
			LeaveCriticalSection(&m_csLoadCreate);
		}
		else
#endif
		{
			delete levelChunk;
		}
		levelChunk = NULL;
	}

    if (levelChunk != NULL)
	{
        levelChunk->lastSaveTime = level->getTime();
    }
    return levelChunk;
}

#ifdef _LARGE_WORLDS
void ServerChunkCache::onChunkUnloaded(LevelChunk *chunk)
{
	// Defensive: if this chunk is already tracked, remove the stale entry first
	onChunkReloaded(chunk);

	// Diagnostic: an unloaded chunk must not remain reachable from the active map.
	AUTO_VAR(cit, m_cache.find(chunkKey(chunk->x, chunk->z)));
	if (cit != m_cache.end() && cit->second == chunk)
	{
		CHUNK_LOG("LRU_ADD_IN_CACHE tick=%u dim=%d chunk=%d,%d ptr=%p",
			m_unloadTick, level->dimension->id, chunk->x, chunk->z, chunk);
	}

	UnloadedChunkEntry entry;
	entry.chunk = chunk;
	entry.unloadTick = m_unloadTick;
	m_unloadedLRU.push_back(entry);
	// M2: std::prev is C++11 and the Xbox 360 toolset (VC10) does not provide it.
	std::list<UnloadedChunkEntry>::iterator lastEntry = m_unloadedLRU.end();
	--lastEntry;
	m_unloadedLRUIndex[chunk] = lastEntry;
}

void ServerChunkCache::onChunkReloaded(LevelChunk *chunk)
{
	AUTO_VAR(it, m_unloadedLRUIndex.find(chunk));
	if (it != m_unloadedLRUIndex.end())
	{
		m_unloadedLRU.erase(it->second);
		m_unloadedLRUIndex.erase(it);
	}
}

void ServerChunkCache::trimUnloadedCache()
{
	if (m_maxUnloadedChunks <= 0) return;	// unlimited (M0 baseline)

	// Don't evict chunks that were unloaded very recently: the local client processes
	// visibility changes asynchronously and may still be using the shared storage.
	static const unsigned int UNLOAD_GRACE_TICKS = 200;
	// M3: bound the work per tick. Entries that cannot be evicted yet stay in
	// the list and are retried on the next tick, so this is a scan budget
	// rather than an eviction count.
	static const int MAX_SCAN_PER_TICK = 256;

	EnterCriticalSection(&m_csLoadCreate);
	int scanned = 0;
	AUTO_VAR(it, m_unloadedLRU.begin());
	while (it != m_unloadedLRU.end() && (int)m_unloadedLRU.size() > m_maxUnloadedChunks && scanned < MAX_SCAN_PER_TICK)
	{
		++scanned;

		// Do not rely on strict LRU ordering for the grace check: entries that
		// were skipped earlier stay in the list, so a young entry can appear
		// before an older one.
		if ((m_unloadTick - it->unloadTick) < UNLOAD_GRACE_TICKS)
		{
			++it;
			continue;
		}

		LevelChunk *chunk = it->chunk;
		bool skip = false;

		// Still shared with a client-side LevelChunk (or pinned by a reader)?
		// Then its storage must stay alive.
		if (chunk->getClientShareCount() != 0)
		{
			skip = true;
		}
		else
		{
			// M2: never delete a chunk that is still reachable from m_cache.
			// This is the invariant whose violation caused a use-after-free in
			// Level::getTile. Check the chunk's own key first (O(1)); only fall
			// back to a full scan if that key does not point at this chunk.
			const __int64 key = chunkKey(chunk->x, chunk->z);
			AUTO_VAR(cit, m_cache.find(key));
			if (cit != m_cache.end() && cit->second == chunk)
			{
				CHUNK_LOG("TRIM_SKIP_IN_CACHE tick=%u dim=%d chunk=%d,%d ptr=%p key=%lld",
					m_unloadTick, level->dimension->id, chunk->x, chunk->z, chunk, key);
				skip = true;
			}
			else
			{
				__int64 otherKey = 0;
				if (findCacheKeyForChunk(chunk, &otherKey))
				{
					CHUNK_LOG("TRIM_SKIP_IN_CACHE_OTHER tick=%u dim=%d chunk=%d,%d ptr=%p key=%lld",
						m_unloadTick, level->dimension->id, chunk->x, chunk->z, chunk, otherKey);
					skip = true;
				}
				else if (std::find(m_loadedChunkList.begin(), m_loadedChunkList.end(), chunk) != m_loadedChunkList.end())
				{
					// The loaded list must not contain a chunk that is not in m_cache.
					CHUNK_LOG("TRIM_SKIP_IN_LOADED_LIST tick=%u dim=%d chunk=%d,%d ptr=%p",
						m_unloadTick, level->dimension->id, chunk->x, chunk->z, chunk);
					skip = true;
				}
			}
		}

		if (skip)
		{
			// M3: leave the entry in the LRU so it is retried after the client
			// releases it. The old code erased it here, so a chunk that happened
			// to be shared at this moment could never be evicted again and the
			// memory cap stopped working.
			++it;
			continue;
		}

		it = m_unloadedLRU.erase(it);
		m_unloadedLRUIndex.erase(chunk);

		AUTO_VAR(cit, m_unloadedCache.find(chunkKey(chunk->x, chunk->z)));
		if (cit != m_unloadedCache.end() && cit->second == chunk) m_unloadedCache.erase(cit);
		delete chunk;
		m_evictedChunks++;
	}
	LeaveCriticalSection(&m_csLoadCreate);
}
#endif

void ServerChunkCache::logChunkStats()
{
#if defined(_WINDOWS64) && defined(_LARGE_WORLDS)
	if ((m_unloadTick % 100) != 0) return;

	wchar_t path[512];
	if (GetEnvironmentVariableW(L"MC_STATS_FILE", path, 512) <= 0) return;

	// Open/append/close every time: keeping the FILE* open locks the log for the
	// whole session, which makes it impossible to watch the benchmark live.
	FILE *statsFile = NULL;
	_wfopen_s(&statsFile, path, L"a");
	if (statsFile == NULL) return;

	PROCESS_MEMORY_COUNTERS pmc;
	memset(&pmc, 0, sizeof(pmc));
	GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));

	double px = 0.0, pz = 0.0;
	MinecraftServer *server = MinecraftServer::getInstance();
	if (server != NULL && server->getPlayers() != NULL && !server->getPlayers()->players.empty())
	{
		shared_ptr<ServerPlayer> p = server->getPlayers()->players[0];
		if (p != NULL) { px = p->x; pz = p->z; }
	}

	EnterCriticalSection(&m_csLoadCreate);
	unsigned int loadedCount = (unsigned int)m_loadedChunkList.size();
	unsigned int unloadedCount = (unsigned int)m_unloadedLRU.size();
	LeaveCriticalSection(&m_csLoadCreate);

	fprintf(statsFile, "tick=%u dim=%d max=%d loaded=%u unloaded=%u evicted=%u px=%.0f pz=%.0f ws=%.1fMB priv=%.1fMB\n",
		m_unloadTick, level->dimension->id, m_maxUnloadedChunks, loadedCount,
		unloadedCount, m_evictedChunks, px, pz,
		pmc.WorkingSetSize / (1024.0 * 1024.0), pmc.PagefileUsage / (1024.0 * 1024.0));
	fclose(statsFile);
#endif
}

void ServerChunkCache::saveEntities(LevelChunk *levelChunk)
{
    if (storage == NULL) return;

    storage->saveEntities(level, levelChunk);
}

void ServerChunkCache::save(LevelChunk *levelChunk)
{
	if (storage == NULL) return;

	levelChunk->lastSaveTime = level->getTime();
	storage->save(level, levelChunk);
}

// 4J added
void ServerChunkCache::updatePostProcessFlag(short flag, int x, int z, int xo, int zo, LevelChunk *lc)
{
	if( hasChunk( x + xo, z + zo ) )
	{
		LevelChunk *lc2 = getChunk(x + xo, z + zo);
		if( lc2 != emptyChunk )		// Will only be empty chunk of this is the edge (we've already checked hasChunk so won't just be a missing chunk)
		{
			if( lc2->terrainPopulated & LevelChunk::sTerrainPopulatedFromHere )
			{
				lc->terrainPopulated |= flag;
			}
		}
		else
		{
			// The edge - always consider as post-processed
			lc->terrainPopulated |= flag;
		}
	}
}

// 4J added - normally we try and set these flags when a chunk is post-processed. However, when setting in a north or easterly direction the
// affected chunks might not themselves exist, so we need to check the flags also when creating new chunks.
void ServerChunkCache::updatePostProcessFlags(int x, int z)
{
	LevelChunk *lc = getChunk(x, z);
	if( lc != emptyChunk )
	{
		// First check if any of our neighbours are post-processed, that should affect OUR flags
		updatePostProcessFlag( LevelChunk::sTerrainPopulatedFromS,  x, z,  0, -1, lc );
		updatePostProcessFlag( LevelChunk::sTerrainPopulatedFromSW, x, z, -1, -1, lc );
		updatePostProcessFlag( LevelChunk::sTerrainPopulatedFromW,  x, z, -1,  0, lc );
		updatePostProcessFlag( LevelChunk::sTerrainPopulatedFromNW, x, z, -1,  1, lc );
		updatePostProcessFlag( LevelChunk::sTerrainPopulatedFromN,  x, z,  0,  1, lc );
		updatePostProcessFlag( LevelChunk::sTerrainPopulatedFromNE, x, z,  1,  1, lc );
		updatePostProcessFlag( LevelChunk::sTerrainPopulatedFromE,  x, z,  1,  0, lc );
		updatePostProcessFlag( LevelChunk::sTerrainPopulatedFromSE, x, z,  1, -1, lc );

		// Then, if WE are post-processed, check that our neighbour's flags are also set
		if( lc->terrainPopulated & LevelChunk::sTerrainPopulatedFromHere )
		{
			flagPostProcessComplete(LevelChunk::sTerrainPopulatedFromW,  x + 1, z + 0 );
			flagPostProcessComplete(LevelChunk::sTerrainPopulatedFromSW, x + 1, z + 1 );
			flagPostProcessComplete(LevelChunk::sTerrainPopulatedFromS,  x + 0, z + 1 );
			flagPostProcessComplete(LevelChunk::sTerrainPopulatedFromSE, x - 1, z + 1 );
			flagPostProcessComplete(LevelChunk::sTerrainPopulatedFromE,  x - 1, z + 0 );
			flagPostProcessComplete(LevelChunk::sTerrainPopulatedFromNE, x - 1, z - 1 );
			flagPostProcessComplete(LevelChunk::sTerrainPopulatedFromN,  x + 0, z - 1 );
			flagPostProcessComplete(LevelChunk::sTerrainPopulatedFromNW, x + 1, z - 1 );
		}
	}

	flagPostProcessComplete(0, x, z);
}

// 4J added - add a flag to a chunk to say that one of its neighbours has completed post-processing. If this completes the set of
// chunks which can actually set tile tiles in this chunk (sTerrainPopulatedAllAffecting), then this is a good point to compress this chunk.
// If this completes the set of all 8 neighbouring chunks that have been fully post-processed, then this is a good time to fix up some
// lighting things that need all the tiles to be in place in the region into which they might propagate.
void ServerChunkCache::flagPostProcessComplete(short flag, int x, int z)
{
	// Set any extra flags for this chunk to indicate which neighbours have now had their post-processing done
	if( !hasChunk(x, z) ) return;

	LevelChunk *lc = level->getChunk( x, z );
	if( lc == emptyChunk ) return;

	lc->terrainPopulated |= flag;

	// Are all neighbouring chunks which could actually place tiles on this chunk complete? (This is ones to W, SW, S)
	if( ( lc->terrainPopulated & LevelChunk::sTerrainPopulatedAllAffecting ) == LevelChunk::sTerrainPopulatedAllAffecting )
	{
		// Do the compression of data & lighting at this point
		PIXBeginNamedEvent(0,"Compressing lighting/blocks");

		// Check, using lower blocks as a reference, if we've already compressed - no point doing this multiple times, which
		// otherwise we will do as we aren't checking for the flags transitioning in the if statement we're in here
		if( !lc->isLowerBlockStorageCompressed() )
			lc->compressBlocks();
		if( !lc->isLowerBlockLightStorageCompressed() )
			lc->compressLighting();
		if( !lc->isLowerDataStorageCompressed() )
			lc->compressData();
		
		PIXEndNamedEvent();
	}

	// Are all neighbouring chunks And this one now post-processed?
	if( lc->terrainPopulated == LevelChunk::sTerrainPopulatedAllNeighbours )
	{
		// Special lighting patching for schematics first
		app.processSchematicsLighting(lc);

		// This would be a good time to fix up any lighting for this chunk since all the geometry that could affect it should now be in place
		PIXBeginNamedEvent(0,"Recheck gaps");
		if( lc->level->dimension->id != 1 )
		{
			lc->recheckGaps(true);
		}
		PIXEndNamedEvent();

		// Do a checkLight on any tiles which are lava.
		PIXBeginNamedEvent(0,"Light lava (this)");
		lc->lightLava();
		PIXEndNamedEvent();

		// Flag as now having this post-post-processing stage completed
		lc->terrainPopulated |= LevelChunk::sTerrainPostPostProcessed;
	}
}

void ServerChunkCache::postProcess(ChunkSource *parent, int x, int z )
{
    LevelChunk *chunk = getChunk(x, z);
	if (chunk == NULL || chunk == emptyChunk) return;

	// M2: post-processing can take a long time (structures/decorations). Pin the
	// 3x3 neighbourhood so the unloaded-chunk LRU cannot free any chunk that the
	// decorator may read while it runs.
	vector<LevelChunk *> pinned;
	EnterCriticalSection(&m_csLoadCreate);
	for (int dx = -1; dx <= 1; ++dx)
	{
		for (int dz = -1; dz <= 1; ++dz)
		{
			AUTO_VAR(it, m_cache.find(chunkKey(x + dx, z + dz)));
			if (it != m_cache.end() && it->second != NULL && it->second != emptyChunk)
			{
				InterlockedIncrement(&it->second->m_clientShareCount);
				pinned.push_back(it->second);
			}
		}
	}
	LeaveCriticalSection(&m_csLoadCreate);

    if ( (chunk->terrainPopulated & LevelChunk::sTerrainPopulatedFromHere) == 0 )
	{	
		if (source != NULL)
		{
			PIXBeginNamedEvent(0,"Main post processing");
            source->postProcess(parent, x, z);
			PIXEndNamedEvent();

            chunk->markUnsaved();
        }

		// Flag not only this chunk as being post-processed, but also all the chunks that this post-processing might affect. We can guarantee that these
		// chunks exist as that's determined before post-processing can even run
		chunk->terrainPopulated |= LevelChunk::sTerrainPopulatedFromHere;

		// M2: the world no longer has a small fixed edge, so there are no special
		// edge chunks that need their missing neighbour flags filled in.

		// Set flags for post-processing being complete for neighbouring chunks. This also performs actions if this post-processing completes
		// a full set of post-processing flags for one of these neighbours.
		flagPostProcessComplete(0, x, z );
		flagPostProcessComplete(LevelChunk::sTerrainPopulatedFromW,  x + 1, z + 0 );
		flagPostProcessComplete(LevelChunk::sTerrainPopulatedFromSW, x + 1, z + 1 );
		flagPostProcessComplete(LevelChunk::sTerrainPopulatedFromS,  x + 0, z + 1 );
		flagPostProcessComplete(LevelChunk::sTerrainPopulatedFromSE, x - 1, z + 1 );
		flagPostProcessComplete(LevelChunk::sTerrainPopulatedFromE,  x - 1, z + 0 );
		flagPostProcessComplete(LevelChunk::sTerrainPopulatedFromNE, x - 1, z - 1 );
		flagPostProcessComplete(LevelChunk::sTerrainPopulatedFromN,  x + 0, z - 1 );
		flagPostProcessComplete(LevelChunk::sTerrainPopulatedFromNW, x + 1, z - 1 );
    }

	for (size_t i = 0; i < pinned.size(); ++i)
	{
		InterlockedDecrement(&pinned[i]->m_clientShareCount);
	}
}

// 4J Added for suspend
bool ServerChunkCache::saveAllEntities()
{
	PIXBeginNamedEvent(0, "Save all entities");

	PIXBeginNamedEvent(0, "saving to NBT");
	// M2: copy the list under the map lock and do the entity writes outside it.
	vector<LevelChunk *> chunks;
	EnterCriticalSection(&m_csLoadCreate);
	chunks = m_loadedChunkList;
	LeaveCriticalSection(&m_csLoadCreate);
	for(AUTO_VAR(it,chunks.begin()); it != chunks.end(); ++it)
	{
		storage->saveEntities(level, *it);
	}
	PIXEndNamedEvent();

	PIXBeginNamedEvent(0,"Flushing");
	storage->flush();
	PIXEndNamedEvent();

	PIXEndNamedEvent();
	return true;
}

bool ServerChunkCache::save(bool force, ProgressListener *progressListener)
{
	// M2: copy the list under the map lock, then do all disk I/O outside it.
	// Otherwise every client chunk lookup waits for the whole save.
	vector<LevelChunk *> chunks;
	EnterCriticalSection(&m_csLoadCreate);
	chunks = m_loadedChunkList;
	LeaveCriticalSection(&m_csLoadCreate);

    int saves = 0;

	// 4J - added this to support progressListner
	int count = 0;
    if (progressListener != NULL)
	{
        AUTO_VAR(itEnd, chunks.end());
		for (AUTO_VAR(it, chunks.begin()); it != itEnd; it++)
		{
			LevelChunk *chunk = *it;
            if (chunk->shouldSave(force))
			{
                count++;
            }
        }
    }
    int cc = 0;

	bool maxSavesReached = false;

	if(!force)
	{
		//app.DebugPrintf("Unsaved chunks = %d\n", level->getUnsavedChunkCount() );
		// Single threaded implementation for small saves
		for (unsigned int i = 0; i < chunks.size(); i++)
		{
			LevelChunk *chunk = chunks[i];
#ifndef SPLIT_SAVES
			if (force && !chunk->dontSave) saveEntities(chunk);
#endif
			if (chunk->shouldSave(force))
			{
				save(chunk);
				chunk->setUnsaved(false);
				if (++saves == MAX_SAVES && !force)
				{
					return false;
				}

				// 4J - added this to support progressListener
				if (progressListener != NULL)
				{
					if (++cc % 10 == 0)
					{
						progressListener->progressStagePercentage(cc * 100 / count);
					}
				}
			}
		}
	}
	else
	{
#if 1 //_LARGE_WORLDS
		// 4J Stu - We have multiple for threads for all saving as part of the storage, so use that rather than new threads here

		// Created a roughly sorted list to match the order that the files were created in 	McRegionChunkStorage::McRegionChunkStorage.
		// This is to minimise the amount of data that needs to be moved round when creating a new level.

		vector<LevelChunk *> sortedChunkList;

		for( int i = 0; i < chunks.size(); i++ )
		{
			if( ( chunks[i]->x < 0 ) && ( chunks[i]->z < 0 ) ) sortedChunkList.push_back(chunks[i]);
		}
		for( int i = 0; i < chunks.size(); i++ )
		{
			if( ( chunks[i]->x >= 0 ) && ( chunks[i]->z < 0 ) ) sortedChunkList.push_back(chunks[i]);
		}
		for( int i = 0; i < chunks.size(); i++ )
		{
			if( ( chunks[i]->x >= 0 ) && ( chunks[i]->z >= 0 ) ) sortedChunkList.push_back(chunks[i]);
		}
		for( int i = 0; i < chunks.size(); i++ )
		{
			if( ( chunks[i]->x < 0 ) && ( chunks[i]->z >= 0 ) ) sortedChunkList.push_back(chunks[i]);
		}

		// Push all the chunks to be saved to the compression threads
		for (unsigned int i = 0; i < sortedChunkList.size();++i)
		{
			LevelChunk *chunk = sortedChunkList[i];
			if (force && !chunk->dontSave) saveEntities(chunk);
			if (chunk->shouldSave(force))
			{
				save(chunk);
				chunk->setUnsaved(false);
				if (++saves == MAX_SAVES && !force)
				{
					return false;
				}

				// 4J - added this to support progressListener
				if (progressListener != NULL)
				{
					if (++cc % 10 == 0)
					{
						progressListener->progressStagePercentage(cc * 100 / count);
					}
				}
			}
			// Wait if we are building up too big a queue of chunks to be written - on PS3 this has been seen to cause so much data to be queued that we run out of
			// out of memory when saving after exploring a full map
			storage->WaitIfTooManyQueuedChunks();
		}

		// Wait for the storage threads to be complete
		storage->WaitForAll();
#else
		// Multithreaded implementation for larger saves

		C4JThread::Event *wakeEvent[3]; // This sets off the threads that are waiting to continue
		C4JThread::Event *notificationEvent[3]; // These are signalled by the threads to let us know they are complete
		C4JThread *saveThreads[3];
		DWORD threadId[3];
		SaveThreadData threadData[3];
		ZeroMemory(&threadData[0], sizeof(SaveThreadData));
		ZeroMemory(&threadData[1], sizeof(SaveThreadData));
		ZeroMemory(&threadData[2], sizeof(SaveThreadData));

		for(unsigned int i = 0; i < 3; ++i)
		{
			saveThreads[i] = NULL;
		
			threadData[i].cache = this;

			wakeEvent[i] = new C4JThread::Event(); //CreateEvent(NULL,FALSE,FALSE,NULL);
			threadData[i].wakeEvent = wakeEvent[i];
			
			notificationEvent[i] = new C4JThread::Event(); //CreateEvent(NULL,FALSE,FALSE,NULL);
			threadData[i].notificationEvent = notificationEvent[i];

			if(i==0) threadData[i].useSharedThreadStorage = true;
			else threadData[i].useSharedThreadStorage = false;
		}

		LevelChunk *chunk = NULL;
		byte workingThreads;
		bool chunkSet = false;

		// Created a roughly sorted list to match the order that the files were created in 	McRegionChunkStorage::McRegionChunkStorage.
		// This is to minimise the amount of data that needs to be moved round when creating a new level.

		vector<LevelChunk *> sortedChunkList;

		for( int i = 0; i < m_loadedChunkList.size(); i++ )
		{
			if( ( m_loadedChunkList[i]->x < 0 ) && ( m_loadedChunkList[i]->z < 0 ) ) sortedChunkList.push_back(m_loadedChunkList[i]);
		}
		for( int i = 0; i < m_loadedChunkList.size(); i++ )
		{
			if( ( m_loadedChunkList[i]->x >= 0 ) && ( m_loadedChunkList[i]->z < 0 ) ) sortedChunkList.push_back(m_loadedChunkList[i]);
		}
		for( int i = 0; i < m_loadedChunkList.size(); i++ )
		{
			if( ( m_loadedChunkList[i]->x >= 0 ) && ( m_loadedChunkList[i]->z >= 0 ) ) sortedChunkList.push_back(m_loadedChunkList[i]);
		}
		for( int i = 0; i < m_loadedChunkList.size(); i++ )
		{
			if( ( m_loadedChunkList[i]->x < 0 ) && ( m_loadedChunkList[i]->z >= 0 ) ) sortedChunkList.push_back(m_loadedChunkList[i]);
		}
		
		for (unsigned int i = 0; i < sortedChunkList.size();)
		{
			workingThreads = 0;
			PIXBeginNamedEvent(0,"Setting tasks for save threads\n");
			for(unsigned int j = 0; j < 3; ++j)
			{
				chunkSet = false;

				while(!chunkSet && i < sortedChunkList.size())
				{
					chunk = sortedChunkList[i];

					threadData[j].saveEntities = (force && !chunk->dontSave);

					if (chunk->shouldSave(force) || threadData[j].saveEntities)
					{
						chunkSet = true;
						++workingThreads;

						threadData[j].chunkToSave = chunk;

						//app.DebugPrintf("Chunk to save set for thread %d\n", j);

						if(saveThreads[j] == NULL)
						{
							char threadName[256];
							sprintf(threadName,"Save thread %d\n",j);
							SetThreadName(threadId[j], threadName);

							//saveThreads[j] = CreateThread(NULL,0,runSaveThreadProc,&threadData[j],CREATE_SUSPENDED,&threadId[j]);
							saveThreads[j] = new C4JThread(runSaveThreadProc,(void *)&threadData[j],threadName);


							//app.DebugPrintf("Created new thread: %s\n",threadName);

							// Threads 1,3 and 5 are generally idle so use them (this call waits on thread 2)
							if(j == 0) saveThreads[j]->SetProcessor(CPU_CORE_SAVE_THREAD_A); //XSetThreadProcessor( saveThreads[j], 1);
							else if(j == 1) saveThreads[j]->SetProcessor(CPU_CORE_SAVE_THREAD_B); //XSetThreadProcessor( saveThreads[j], 3);
							else if(j == 2) saveThreads[j]->SetProcessor(CPU_CORE_SAVE_THREAD_C); //XSetThreadProcessor( saveThreads[j], 5);

							//ResumeThread( saveThreads[j] );
							saveThreads[j]->Run();
						}

						if (++saves == MAX_SAVES && !force)
						{
							maxSavesReached = true;
							break;

							//LeaveCriticalSection(&m_csLoadCreate);
							// TODO Should we be returning from here? Probably not
							//return false;
						}

						// 4J - added this to support progressListener
						if (progressListener != NULL)
						{
							if (count > 0 && ++cc % 10 == 0)
							{
								progressListener->progressStagePercentage(cc * 100 / count);
							}
						}
					}

					++i;
				}

				if( !chunkSet )
				{
					threadData[j].chunkToSave = NULL;
					//app.DebugPrintf("No chunk to save set for thread %d\n",j);
				}
			}
			PIXEndNamedEvent();
			PIXBeginNamedEvent(0,"Waking save threads\n");
			// Start the worker threads going
			for(unsigned int k = 0; k < 3; ++k)
			{
				//app.DebugPrintf("Waking save thread %d\n",k);
				threadData[k].wakeEvent->Set(); //SetEvent(threadData[k].wakeEvent);
			}
			PIXEndNamedEvent();
			PIXBeginNamedEvent(0,"Waiting for completion of save threads\n");
			//app.DebugPrintf("Waiting for %d save thread(s) to complete\n", workingThreads);

			// Wait for the worker threads to complete
			//WaitForMultipleObjects(workingThreads,notificationEvent,TRUE,INFINITE);
			// 4J Stu - TODO This isn't ideal as it's not a perfect re-implmentation of the Xbox behaviour
			for(unsigned int k = 0; k < workingThreads; ++k)
			{
				threadData[k].notificationEvent->WaitForSignal(INFINITE);
			}
			PIXEndNamedEvent();
			if( maxSavesReached ) break;
		}

		//app.DebugPrintf("Clearing up worker threads\n");
		// Stop all the worker threads by giving them nothing to process then telling them to start
		unsigned char validThreads = 0;
		for(unsigned int i = 0; i < 3; ++i)
		{
			//app.DebugPrintf("Settings chunk to NULL for save thread %d\n", i);
			threadData[i].chunkToSave = NULL;

			//app.DebugPrintf("Setting wake event for save thread %d\n",i);
			threadData[i].wakeEvent->Set(); //SetEvent(threadData[i].wakeEvent);

			if(saveThreads[i] != NULL) ++validThreads;
		}

		//WaitForMultipleObjects(validThreads,saveThreads,TRUE,INFINITE);
		// 4J Stu - TODO This isn't ideal as it's not a perfect re-implmentation of the Xbox behaviour
		for(unsigned int k = 0; k < validThreads; ++k)
		{
			saveThreads[k]->WaitForCompletion(INFINITE);;
		}

		for(unsigned int i = 0; i < 3; ++i)
		{
			//app.DebugPrintf("Closing handles for save thread %d\n", i);
			delete threadData[i].wakeEvent; //CloseHandle(threadData[i].wakeEvent);
			delete threadData[i].notificationEvent; //CloseHandle(threadData[i].notificationEvent);
			delete saveThreads[i]; //CloseHandle(saveThreads[i]);
		}
#endif
	}

    if (force)
	{
        if (storage == NULL)
		{
			return true;
		}
        storage->flush();
    }

    return !maxSavesReached;

}

bool ServerChunkCache::tick()
{
#ifdef _LARGE_WORLDS
	m_unloadTick++;
#endif
    if (!level->noSave)
	{
#ifdef _LARGE_WORLDS
		for (int i = 0; i < 100; i++)
		{
			LevelChunk *chunk = NULL;
			EnterCriticalSection(&m_csLoadCreate);
			if (!m_toDrop.empty())
			{
				chunk = m_toDrop.front();
				m_toDrop.pop_front();
				m_toDropSet.erase(chunk);
			}
			LeaveCriticalSection(&m_csLoadCreate);

			if (chunk == NULL) break;

			if(!chunk->isUnloaded())
			{
				// Saving is done without the cache lock; the chunk stays visible
				// in m_cache until it has been written out.
				save(chunk);
				saveEntities(chunk);

				EnterCriticalSection(&m_csLoadCreate);
				// M3: unload under the lock. create() must never observe a chunk
				// that has already been unloaded but is still present in m_cache;
				// the old code did that and returned a chunk with no entities.
				chunk->unload(true);
				removeFromLoadedList(chunk);

				const __int64 key = chunkKey(chunk->x, chunk->z);
				AUTO_VAR(cit, m_cache.find(key));
				if (cit != m_cache.end() && cit->second == chunk)
				{
					m_cache.erase(cit);
				}
				else
				{
					// The entry under our key is missing or belongs to another
					// chunk. Remove any stale entry pointing at this chunk so it
					// cannot be returned after it has been unloaded.
					__int64 foundKey = 0;
					bool erasedByValue = eraseCacheEntryForChunk(chunk, &foundKey);
					CHUNK_LOG("UNLOAD_KEY_MISMATCH tick=%u dim=%d chunk=%d,%d ptr=%p mapEntry=%p erasedByValue=%d foundKey=%lld",
						m_unloadTick, level->dimension->id, chunk->x, chunk->z, chunk,
						(cit != m_cache.end()) ? cit->second : NULL,
						erasedByValue ? 1 : 0, foundKey);
				}

				AUTO_VAR(uit, m_unloadedCache.find(key));
				if (uit == m_unloadedCache.end() || uit->second == chunk)
				{
					m_unloadedCache[key] = chunk;
				}
				else
				{
					CHUNK_LOG("UNLOAD_KEY_OCCUPIED tick=%u dim=%d chunk=%d,%d ptr=%p other=%p",
						m_unloadTick, level->dimension->id, chunk->x, chunk->z, chunk, uit->second);
				}
				onChunkUnloaded(chunk);
				LeaveCriticalSection(&m_csLoadCreate);
			}
		}
		trimUnloadedCache();
#endif
        if (storage != NULL) storage->tick();
    }

	logChunkStats();

    return source->tick();

}

bool ServerChunkCache::shouldSave()
{
	return !level->noSave;
}

wstring ServerChunkCache::gatherStats()
{
	 return L"ServerChunkCache: ";// + _toString<int>(loadedChunks.size()) + L" Drop: " + _toString<int>(toDrop.size());
}

vector<Biome::MobSpawnerData *> *ServerChunkCache::getMobsAt(MobCategory *mobCategory, int x, int y, int z)
{
	return source->getMobsAt(mobCategory, x, y, z);
}

TilePos *ServerChunkCache::findNearestMapFeature(Level *level, const wstring &featureName, int x, int y, int z)
{
	return source->findNearestMapFeature(level, featureName, x, y, z);
}

int ServerChunkCache::runSaveThreadProc(LPVOID lpParam)
{
	SaveThreadData *params = (SaveThreadData *)lpParam;

	if(params->useSharedThreadStorage)
	{
		Compression::UseDefaultThreadStorage();
		OldChunkStorage::UseDefaultThreadStorage();
	}
	else
	{
		Compression::CreateNewThreadStorage();
		OldChunkStorage::CreateNewThreadStorage();
	}

	// Wait for the producer thread to tell us to start
	params->wakeEvent->WaitForSignal(INFINITE); //WaitForSingleObject(params->wakeEvent,INFINITE);

	//app.DebugPrintf("Save thread has started\n");

	while(params->chunkToSave != NULL)
	{
		PIXBeginNamedEvent(0,"Saving entities");
		//app.DebugPrintf("Save thread has started processing a chunk\n");
		if (params->saveEntities) params->cache->saveEntities(params->chunkToSave);
		PIXEndNamedEvent();
		PIXBeginNamedEvent(0,"Saving chunk");

		params->cache->save(params->chunkToSave);
		params->chunkToSave->setUnsaved(false);

		PIXEndNamedEvent();
		PIXBeginNamedEvent(0,"Notifying and waiting for next chunk");

		// Inform the producer thread that we are done with this chunk
		params->notificationEvent->Set(); //SetEvent(params->notificationEvent);

		//app.DebugPrintf("Save thread has alerted producer that it is complete\n");

		// Wait for the producer thread to tell us to go again
		params->wakeEvent->WaitForSignal(INFINITE); //WaitForSingleObject(params->wakeEvent,INFINITE);
		PIXEndNamedEvent();
	}

	//app.DebugPrintf("Thread is exiting as it has no chunk to process\n");

	if(!params->useSharedThreadStorage)
	{
		Compression::ReleaseThreadStorage();
		OldChunkStorage::ReleaseThreadStorage();
	}

	return 0;
}
