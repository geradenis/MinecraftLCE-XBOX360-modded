#pragma once
#include "StructureFeature.h"
#include "StructureStart.h"
class Biome;

class VillageFeature : public StructureFeature
{
private:
	const int villageSizeModifier;
public:
	static void staticCtor();
	static vector<Biome *> allowedBiomes;
    VillageFeature(int villageSizeModifier, int iXZSize);

protected:
	virtual bool isFeatureChunk(int x, int z, bool bIsSuperflat=false);
    virtual StructureStart *createStructureStart(int x, int z);

private:
	class VillageStart : public StructureStart
	{
	private:
		bool valid;
		// Legacy chunk-based size kept only for save compatibility; world bounds
		// are taken from m_level->dimension->getXZSize() instead.
		int m_iXZSize;
		Level *m_level;
	public:
		VillageStart(Level *level, Random *random, int chunkX, int chunkZ, int villageSizeModifier,int iXZSize);
        bool isValid();
    };

	int m_iXZSize;
};
