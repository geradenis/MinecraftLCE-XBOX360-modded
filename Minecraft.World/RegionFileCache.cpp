#include "stdafx.h"
#include "File.h"
#include "RegionFileCache.h"
#include "ConsoleSaveFileIO.h"

RegionFileCache RegionFileCache::s_defaultCache;

RegionFileCache::RegionFileCache()
{
	InitializeCriticalSectionAndSpinCount(&m_lock, 5120);
}

RegionFileCache::~RegionFileCache()
{
	// M2: Only release the cached region files when the cache itself goes away.
	// _clear() takes the lock, so calling it here is safe.
	_clear();
	DeleteCriticalSection(&m_lock);
}

bool RegionFileCache::useSplitSaves(ESavePlatform platform)
{
	switch(platform)
	{
	case SAVE_FILE_PLATFORM_XBONE:
	case SAVE_FILE_PLATFORM_PS4:
		return true;
	default:
		return false;
	};
}

RegionFile *RegionFileCache::_getRegionFile(ConsoleSaveFile *saveFile, const wstring &prefix, int chunkX, int chunkZ)		// 4J - TODO was synchronized
{
	// M2: Lookup and insertion have to be atomic - saves run on three threads and
	// they all share this cache.
	EnterCriticalSection(&m_lock);

	// 4J Jev - changed back to use of the File class.
	//char file[MAX_PATH_SIZE];
	//sprintf(file,"%s\\region\\r.%d.%d.mcr",basePath,chunkX >> 5,chunkZ >> 5);

	//File regionDir(basePath, L"region");

	//File file(regionDir, wstring(L"r.") + _toString(chunkX>>5) + L"." + _toString(chunkZ>>5) + L".mcr" );
	MemSect(31);
	File file;
	if(useSplitSaves(saveFile->getSavePlatform()))
	{
		file = File( prefix + wstring(L"r.") + _toString(chunkX>>4) + L"." + _toString(chunkZ>>4) + L".mcr" );
	}
	else
	{
		file = File( prefix + wstring(L"r.") + _toString(chunkX>>5) + L"." + _toString(chunkZ>>5) + L".mcr" );
	}
	MemSect(0);

	RegionFile *ref = NULL;
	AUTO_VAR(it, cache.find(file));
	if( it != cache.end() )
		ref = it->second;

	// 4J Jev, put back in.
    if (ref != NULL)
	{
		LeaveCriticalSection(&m_lock);
		return ref;
    }

	// 4J Stu - Remove for new save files
	/*
    if (!regionDir.exists())
	{
        regionDir.mkdirs();
    }
	*/

	// M2: Deliberately no _clear() when the cache is full. _clear() deletes the
	// cached RegionFile objects, but another save thread may still hold a pointer
	// returned by an earlier call, which would then dangle and be used after free.
	// In practice there is only one region file per 1024 chunks, so the cache
	// stays small and the memory it retains is negligible. It is only cleared on
	// shutdown (and by explicit clear() calls) now.

	RegionFile *reg = new RegionFile(saveFile, &file);
    cache[file] = reg;	   // 4J - this was originally a softReferenc

	LeaveCriticalSection(&m_lock);
    return reg;

}

void RegionFileCache::_clear()															// 4J - TODO was synchronized
{
	EnterCriticalSection(&m_lock);

	AUTO_VAR(itEnd, cache.end());
	for( AUTO_VAR(it, cache.begin()); it != itEnd; it++ )
	{
		// 4J - removed try/catch
//        try {
        RegionFile *regionFile = it->second;
        if (regionFile != NULL)
		{
            regionFile->close();
        }
		delete regionFile;
//        } catch (IOException e) {
//            e.printStackTrace();
//        }	
	}
	cache.clear();

	LeaveCriticalSection(&m_lock);
}

int RegionFileCache::_getSizeDelta(ConsoleSaveFile *saveFile, const wstring &prefix, int chunkX, int chunkZ)
{
	// M2: EnterCriticalSection is recursive, so the nested lock in _getRegionFile
	// is fine; holding the lock here also covers the RegionFile call.
	EnterCriticalSection(&m_lock);
    RegionFile *r = _getRegionFile(saveFile, prefix, chunkX, chunkZ);
	int sizeDelta = r->getSizeDelta();
	LeaveCriticalSection(&m_lock);

	return sizeDelta;
}

DataInputStream *RegionFileCache::_getChunkDataInputStream(ConsoleSaveFile *saveFile, const wstring &prefix, int chunkX, int chunkZ)
{
	EnterCriticalSection(&m_lock);

	RegionFile* r = _getRegionFile(saveFile, prefix, chunkX, chunkZ);
	DataInputStream *ret;
	if(useSplitSaves(saveFile->getSavePlatform()))
	{
		ret = r->getChunkDataInputStream(chunkX & 15, chunkZ & 15);
	}
	else
	{
		ret = r->getChunkDataInputStream(chunkX & 31, chunkZ & 31);
	}

	LeaveCriticalSection(&m_lock);

	return ret;
}

DataOutputStream *RegionFileCache::_getChunkDataOutputStream(ConsoleSaveFile *saveFile, const wstring &prefix, int chunkX, int chunkZ)
{
	EnterCriticalSection(&m_lock);

	RegionFile* r = _getRegionFile(saveFile, prefix, chunkX, chunkZ);
	DataOutputStream *ret;
	if(useSplitSaves(saveFile->getSavePlatform()))
	{
		ret = r->getChunkDataOutputStream(chunkX & 15, chunkZ & 15);
	}
	else
	{
		ret = r->getChunkDataOutputStream(chunkX & 31, chunkZ & 31);
	}

	LeaveCriticalSection(&m_lock);

	return ret;
}
