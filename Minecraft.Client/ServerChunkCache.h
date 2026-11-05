#pragma once
#include "..\Minecraft.World\net.minecraft.world.level.h"
#include "..\Minecraft.World\File.h"
#include "..\Minecraft.World\net.minecraft.world.level.storage.h"
#include "..\Minecraft.World\JavaIntHash.h"
#include "..\Minecraft.World\RandomLevelSource.h"
#include "..\Minecraft.World\C4JThread.h"
#include <list>
#include <unordered_map>
#include <unordered_set>
using namespace std;
class ServerLevel;

class ServerChunkCache : public ChunkSource
{

private:
//	unordered_set<int,IntKeyHash, IntKeyEq> toDrop;
private:
	LevelChunk *emptyChunk;
    ChunkSource *source;
    ChunkStorage *storage;
public:
	bool autoCreate;
private:
	// M2: bookkeeping helpers. All of these expect m_csLoadCreate to be held.
	bool findCacheKeyForChunk(LevelChunk *chunk, __int64 *outKey);
	bool eraseCacheEntryForChunk(LevelChunk *chunk, __int64 *outKey);
	void restoreUnloadedChunk(LevelChunk *chunk);
	void discardChunkLocked(LevelChunk *chunk);
	// M3: removes one occurrence without preserving order (O(n) worst case,
	// but no longer O(n) moves for every unloaded chunk).
	void removeFromLoadedList(LevelChunk *chunk);
	// M2: the old fixed XZSIZE x XZSIZE arrays limited the world to a small square.
	// Chunks are now stored in hash maps keyed by their global chunk coordinates,
	// so the cache can hold chunks anywhere within Level::MAX_LEVEL_SIZE.
	unordered_map<__int64, LevelChunk *, LongKeyHash, LongKeyEq> m_cache;
    vector<LevelChunk *> m_loadedChunkList;
    ServerLevel *level;

#ifdef _LARGE_WORLDS
	deque<LevelChunk *> m_toDrop;
	// M3: O(1) membership test for m_toDrop. The old code scanned the whole
	// deque on every drop()/dontDrop(), which became quadratic when a player
	// flew quickly and hundreds of chunks were queued per tick.
	unordered_set<LevelChunk *> m_toDropSet;
	unordered_map<__int64, LevelChunk *, LongKeyHash, LongKeyEq> m_unloadedCache;

	// M1: bounded LRU of chunks that have been unloaded from the active cache.
	// The original code kept every unloaded chunk alive forever (in m_unloadedCache),
	// which is the main source of unbounded memory growth when a player explores.
	// Now unloaded chunks are saved to storage and, once the limit is exceeded, the
	// least recently used ones are deleted and will be reloaded from storage on demand.
	struct UnloadedChunkEntry
	{
		LevelChunk *chunk;
		unsigned int unloadTick;
	};
	std::list<UnloadedChunkEntry> m_unloadedLRU;	// oldest at the front, newest at the back
	std::unordered_map<LevelChunk *, std::list<UnloadedChunkEntry>::iterator> m_unloadedLRUIndex;
	int m_maxUnloadedChunks;						// <= 0 means unlimited (original M0 behaviour)
	unsigned int m_unloadTick;
	unsigned int m_evictedChunks;

	void onChunkUnloaded(LevelChunk *chunk);
	void onChunkReloaded(LevelChunk *chunk);
	void trimUnloadedCache();
#endif

	// 4J - added for multithreaded support
	// M2: m_csLoadCreate protects only the maps/lists. Terrain generation and
	// entity loading are serialised by m_csCreate instead, so the client/render
	// thread never waits for a chunk to generate.
	CRITICAL_SECTION m_csLoadCreate;
	CRITICAL_SECTION m_csCreate;

	// M2: chunk key in the same format as PlayerChunkMap (biased x in the low
	// 32 bits, biased z in the high 32 bits). This is unique for all coordinates
	// that fit in a signed 32-bit int.
	static __int64 chunkKey(int x, int z)
	{
		return (x + 0x7fffffffLL) | ((z + 0x7fffffffLL) << 32);
	}

public:
	ServerChunkCache(ServerLevel *level, ChunkStorage *storage, ChunkSource *source);
	virtual ~ServerChunkCache();
	// M3: true for the shared all-air sentinel returned by
	// getChunkLoadedOrUnloadedPinned() when the chunk is not cached at all.
	// Callers that copy storage from the returned chunk must check this.
	bool isSentinelChunk(LevelChunk *chunk) const { return chunk == emptyChunk; }
    virtual bool hasChunk(int x, int z);
	vector<LevelChunk *> *getLoadedChunkList();
    void drop(int x, int z);
	void dropAll();
    virtual LevelChunk *create(int x, int z);
	LevelChunk *create(int x, int z, bool asyncPostProcess );	// 4J added
    virtual LevelChunk *getChunk(int x, int z);
	virtual LevelChunk *getChunkIfLoaded(int x, int z);		// M2 added
	vector<LevelChunk *> copyLoadedChunkList();			// M2: snapshot under the map lock
	// M2: returns a chunk that is pinned for the caller (m_clientShareCount is
	// incremented) so the unloaded-chunk LRU cannot delete it while the caller
	// is still reading its storage. Call releaseChunk() when done.
	LevelChunk *getChunkLoadedOrUnloadedPinned(int x, int z);
	void releaseChunk(LevelChunk *chunk);
#ifdef _LARGE_WORLDS	
	LevelChunk *getChunkLoadedOrUnloaded(int x, int z);		// 4J added
#endif

	// 4J-JEV Added; Remove chunk from the toDrop queue.
#ifdef _LARGE_WORLDS	
	void dontDrop(int x, int z); 
#endif

private:
	 LevelChunk *load(int x, int z);
    void saveEntities(LevelChunk *levelChunk);
    void save(LevelChunk *levelChunk);

	void updatePostProcessFlag(short flag, int x, int z, int xo, int zo, LevelChunk *lc);	// 4J added
	void updatePostProcessFlags(int x, int z);								// 4J added
	void flagPostProcessComplete(short flag, int x, int z);					// 4J added
public:
	virtual void postProcess(ChunkSource *parent, int x, int z);


private:
#ifdef _LARGE_WORLDS
	static const int MAX_SAVES = 20;
#else
	// 4J Stu - Was 24, but lowering it drastically so that we can trickle save chunks
	static const int MAX_SAVES = 1;
#endif

	// M1: diagnostics (Windows64 testbed only)
	void logChunkStats();

public:
	virtual bool saveAllEntities();
	virtual bool save(bool force, ProgressListener *progressListener);
    virtual bool tick();
    virtual bool shouldSave();
    virtual wstring gatherStats();

	virtual vector<Biome::MobSpawnerData *> *getMobsAt(MobCategory *mobCategory, int x, int y, int z);
	virtual TilePos *findNearestMapFeature(Level *level, const wstring &featureName, int x, int y, int z);

private:
	typedef struct _SaveThreadData
	{
		ServerChunkCache *cache;
		LevelChunk *chunkToSave;
		bool saveEntities;
		bool useSharedThreadStorage;
		C4JThread::Event *notificationEvent;
		C4JThread::Event *wakeEvent; // This is a handle to the one fired by the producer thread
	} SaveThreadData;

public:
	static int runSaveThreadProc(LPVOID lpParam);
};
