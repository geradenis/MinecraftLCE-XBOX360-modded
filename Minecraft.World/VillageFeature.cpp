#include "stdafx.h"
#include "VillageFeature.h"
#include "VillagePieces.h"
#include "net.minecraft.world.level.h"
#include "net.minecraft.world.level.biome.h"
#include "net.minecraft.world.level.dimension.h"

vector<Biome *> VillageFeature::allowedBiomes;

void VillageFeature::staticCtor()
{
	allowedBiomes.push_back( Biome::plains );
	allowedBiomes.push_back( Biome::desert );
}


VillageFeature::VillageFeature(int villageSizeModifier, int iXZSize) : StructureFeature(), villageSizeModifier(villageSizeModifier)
{
	m_iXZSize=iXZSize;
}

bool VillageFeature::isFeatureChunk(int x, int z,bool bIsSuperflat)
{
    int townSpacing;

#ifdef _LARGE_WORLDS
	if(level->dimension->getXZSize() > 128)
	{
		townSpacing = 32;
	}
	else
#endif
	if(bIsSuperflat) 
	{
		townSpacing= 32;
	}
	else
	{
		townSpacing= 16;// 4J change 32;
	}

    int minTownSeparation = 8;

    int xx = x;
    int zz = z;
    if (x < 0) x -= townSpacing - 1;
    if (z < 0) z -= townSpacing - 1;

    int xCenterTownChunk = x / townSpacing;
    int zCenterTownChunk = z / townSpacing;
    Random *r = level->getRandomFor(xCenterTownChunk, zCenterTownChunk, 10387312);
    xCenterTownChunk *= townSpacing;
    zCenterTownChunk *= townSpacing;
    xCenterTownChunk += r->nextInt(townSpacing - minTownSeparation);
    zCenterTownChunk += r->nextInt(townSpacing - minTownSeparation);
    x = xx;
    z = zz;

	bool forcePlacement = false;
	LevelGenerationOptions *levelGenOptions = app.getLevelGenerationOptions();
	if( levelGenOptions != NULL )
	{
		forcePlacement = levelGenOptions->isFeatureChunk(x,z,eFeature_Village);
	}

    if (forcePlacement || (x == xCenterTownChunk && z == zCenterTownChunk) )
	{
        bool biomeOk = level->getBiomeSource()->containsOnly(x * 16 + 8, z * 16 + 8, 0, allowedBiomes);
        if (biomeOk)
		{
			//app.DebugPrintf("Biome ok for Village at %d, %d\n",(x * 16 + 8),(z * 16 + 8));
            return true;
        }
    }

    return false;
}

StructureStart *VillageFeature::createStructureStart(int x, int z)
{
	// 4J added
	app.AddTerrainFeaturePosition(eTerrainFeature_Village,x,z);

	return new VillageStart(level, random, x, z, villageSizeModifier, m_iXZSize);
}

VillageFeature::VillageStart::VillageStart(Level *level, Random *random, int chunkX, int chunkZ, int villageSizeModifier, int iXZSize)
{
	valid = false;	// 4J added initialiser
	m_iXZSize=iXZSize;
	m_level=level;

    list<VillagePieces::PieceWeight *> *pieceSet = VillagePieces::createPieceSet(random, villageSizeModifier);

    VillagePieces::StartPiece *startRoom = new VillagePieces::StartPiece(level->getBiomeSource(), 0, random, (chunkX << 4) + 2, (chunkZ << 4) + 2, pieceSet, villageSizeModifier, level);
	pieces.push_back(startRoom);
    startRoom->addChildren(startRoom, &pieces, random);

    vector<StructurePiece *> *pendingRoads = &startRoom->pendingRoads;
    vector<StructurePiece *> *pendingHouses = &startRoom->pendingHouses;
    while (!pendingRoads->empty() || !pendingHouses->empty())
	{

        // prioritize roads
        if (pendingRoads->empty())
		{
            int pos = random->nextInt((int)pendingHouses->size());
			AUTO_VAR(it, pendingHouses->begin() + pos);
            StructurePiece *structurePiece = *it;
			pendingHouses->erase(it);
            structurePiece->addChildren(startRoom, &pieces, random);
        }
		else
		{
            int pos = random->nextInt((int)pendingRoads->size());
			AUTO_VAR(it, pendingRoads->begin() + pos);
            StructurePiece *structurePiece = *it;
			pendingRoads->erase(it);
            structurePiece->addChildren(startRoom, &pieces, random);
        }
    }

    calculateBoundingBox();

    int count = 0;
	for( AUTO_VAR(it, pieces.begin()); it != pieces.end(); it++ )
	{
		StructurePiece *piece = *it;
        if (dynamic_cast<VillagePieces::VillageRoadPiece *>(piece) == NULL)
		{
            count++;
        }
	}
	valid = count > 2;
}

bool VillageFeature::VillageStart::isValid()
{
	// 4J-PB - Adding a bounds check to ensure a village isn't over the edge of our world - we end up with half houses in that case
	// M2: bound the village by the dimension's real world size in blocks instead
	// of the legacy LevelData::m_xzSize (still clamped to the save-compatible
	// 320 chunks). m_iXZSize is intentionally not used here anymore.
	int blockMax = (m_level->dimension->getXZSize() * 16) / 2;
	int blockMin = -blockMax;
	if((boundingBox->x0<blockMin) || (boundingBox->x1>blockMax) || (boundingBox->z0<blockMin) || (boundingBox->z1>blockMax))
	{
		valid=false;
	}
	return valid;
}
