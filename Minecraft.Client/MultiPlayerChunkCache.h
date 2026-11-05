#pragma once
#include "..\Minecraft.World\net.minecraft.world.level.h"
#include "..\Minecraft.World\net.minecraft.world.level.chunk.h"
#include "..\Minecraft.World\RandomLevelSource.h"
#include "..\Minecraft.World\JavaIntHash.h"
#include <unordered_map>
#include <list>
#include <set>

using namespace std;
class ServerChunkCache;

// 4J - various alterations here to make this thread safe, and operate as a fixed sized cache
class MultiPlayerChunkCache : public ChunkSource
{
	friend class LevelRenderer;
private:
	LevelChunk *emptyChunk;
	LevelChunk *waterChunk;

	vector<LevelChunk *> loadedChunkList;

	// M2: hash-map cache keyed by global chunk coordinates. The old fixed-size
	// array (and its hasData array) limited the client to the same small square
	// as the server.
	struct ClientChunkEntry
	{
		LevelChunk *chunk;
		bool hasData;
		ClientChunkEntry() : chunk(NULL), hasData(false) {}
	};
	unordered_map<__int64, ClientChunkEntry, LongKeyHash, LongKeyEq> cache;
	// 4J - added for multithreaded support
	CRITICAL_SECTION m_csLoadCreate;

	static __int64 chunkKey(int x, int z)
	{
		return (x + 0x7fffffffLL) | ((z + 0x7fffffffLL) << 32);
	}

    Level *level;

#ifdef _LARGE_WORLDS
	// M3: the client cache used to keep every chunk it had ever seen (drop()
	// only unloaded the entities). On an infinite world that grows without
	// bound. Chunks that leave the view are now tracked in this LRU and the
	// oldest ones are deleted once the cache exceeds m_maxCachedChunks.
	std::list<LevelChunk *> m_droppedLRU;	// oldest at the front
	std::set<LevelChunk *> m_droppedSet;
	int m_maxCachedChunks;
	void markDroppedLocked(LevelChunk *chunk);
	void markActiveLocked(LevelChunk *chunk);
	void trimCacheLocked();
#endif

public:
	MultiPlayerChunkCache(Level *level);
	~MultiPlayerChunkCache();
    virtual bool hasChunk(int x, int z);
	virtual bool reallyHasChunk(int x, int z);
    virtual void drop(int x, int z);
    virtual LevelChunk *create(int x, int z);
    virtual LevelChunk *getChunk(int x, int z);
	virtual LevelChunk *getChunkIfLoaded(int x, int z);		// M2 added
    virtual bool save(bool force, ProgressListener *progressListener);
    virtual bool tick();
    virtual bool shouldSave();
    virtual void postProcess(ChunkSource *parent, int x, int z);
    virtual wstring gatherStats();
	virtual vector<Biome::MobSpawnerData *> *getMobsAt(MobCategory *mobCategory, int x, int y, int z);
	virtual TilePos *findNearestMapFeature(Level *level, const wstring &featureName, int x, int y, int z);
	virtual void dataReceived(int x, int z);	// 4J added
};