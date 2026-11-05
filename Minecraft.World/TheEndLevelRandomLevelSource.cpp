#include "stdafx.h"
#include "net.minecraft.world.level.h"
#include "net.minecraft.world.level.biome.h"
#include "net.minecraft.world.level.levelgen.h"
#include "net.minecraft.world.level.levelgen.feature.h"
#include "net.minecraft.world.level.levelgen.structure.h"
#include "net.minecraft.world.level.levelgen.synth.h"
#include "net.minecraft.world.level.tile.h"
#include "net.minecraft.world.level.storage.h"
#include "TheEndLevelRandomLevelSource.h"

// M2: Java/Bedrock-style outer End islands.
//
// The original LCE generator only built the central island: its height field was
// a cone centred on (0,0), so everything outside roughly 100 blocks was void.
// We keep that central island exactly as before and add deterministic islands on
// a 1024-block grid around it. Coordinates in getHeights() are in 8-block units,
// so the grid cell size is 128 units.
static int endFloorDiv(int a, int b)
{
	int q = a / b;
	int r = a % b;
	if (r != 0 && ((r < 0) != (b < 0))) --q;
	return q;
}

static unsigned int endIslandHash(int x, int z)
{
	unsigned int h = (unsigned int)x * 374761393u + (unsigned int)z * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

static double endIslandDoffs(int x, int z)
{
	static const int CELL = 128;	// 128 * 8 = 1024 blocks between island cells
	int cellX = endFloorDiv(x, CELL);
	int cellZ = endFloorDiv(z, CELL);

	double best = -100.0;
	// Check the neighbouring cells too, so islands can overlap cell boundaries
	// without being cut off.
	for (int dx = -1; dx <= 1; ++dx)
	{
		for (int dz = -1; dz <= 1; ++dz)
		{
			int cx = cellX + dx;
			int cz = cellZ + dz;

			double centerX, centerZ, top, slope;
			if (cx == 0 && cz == 0)
			{
				// The classic central island, unchanged.
				centerX = 0.0;
				centerZ = 0.0;
				top = 100.0;
				slope = 8.0;
			}
			else
			{
				unsigned int h = endIslandHash(cx, cz);
				centerX = (cx + 0.25 + 0.5 * ((h & 0xff) / 255.0)) * CELL;
				centerZ = (cz + 0.25 + 0.5 * (((h >> 8) & 0xff) / 255.0)) * CELL;
				top = 55.0 + 35.0 * (((h >> 16) & 0xff) / 255.0);
				slope = 8.0 + 4.0 * (((h >> 24) & 0xff) / 255.0);
			}

			double ddx = x - centerX;
			double ddz = z - centerZ;
			double doffs = top - sqrt(ddx * ddx + ddz * ddz) * slope;
			if (doffs > best) best = doffs;
		}
	}

	if (best > 80.0) best = 80.0;
	if (best < -100.0) best = -100.0;
	return best;
}

TheEndLevelRandomLevelSource::TheEndLevelRandomLevelSource(Level *level, __int64 seed)
{
	m_XZSize = END_LEVEL_MIN_WIDTH;

    this->level = level;

    random = new Random(seed);
	pprandom = new Random(seed);	// 4J added
    lperlinNoise1 = new PerlinNoise(random, 16);
    lperlinNoise2 = new PerlinNoise(random, 16);
    perlinNoise1 = new PerlinNoise(random, 8);

    scaleNoise = new PerlinNoise(random, 10);
    depthNoise = new PerlinNoise(random, 16);
}

TheEndLevelRandomLevelSource::~TheEndLevelRandomLevelSource()
{
	delete random;
	delete pprandom;
	delete lperlinNoise1;
	delete lperlinNoise2;
	delete perlinNoise1;
	delete scaleNoise;
	delete depthNoise;
}

void TheEndLevelRandomLevelSource::prepareHeights(int xOffs, int zOffs, byteArray blocks, BiomeArray biomes)
{
	doubleArray buffer;	// 4J - used to be declared with class level scope but tidying up for thread safety reasons

    int xChunks = 16 / CHUNK_WIDTH;

    int xSize = xChunks + 1;
    int ySize = Level::genDepth / CHUNK_HEIGHT + 1;
    int zSize = xChunks + 1;
    buffer = getHeights(buffer, xOffs * xChunks, 0, zOffs * xChunks, xSize, ySize, zSize);

    for (int xc = 0; xc < xChunks; xc++)
	{
        for (int zc = 0; zc < xChunks; zc++)
		{
            for (int yc = 0; yc < Level::genDepth / CHUNK_HEIGHT; yc++)
			{
                double yStep = 1 / (double) CHUNK_HEIGHT;
                double s0 = buffer[((xc + 0) * zSize + (zc + 0)) * ySize + (yc + 0)];
                double s1 = buffer[((xc + 0) * zSize + (zc + 1)) * ySize + (yc + 0)];
                double s2 = buffer[((xc + 1) * zSize + (zc + 0)) * ySize + (yc + 0)];
                double s3 = buffer[((xc + 1) * zSize + (zc + 1)) * ySize + (yc + 0)];

                double s0a = (buffer[((xc + 0) * zSize + (zc + 0)) * ySize + (yc + 1)] - s0) * yStep;
                double s1a = (buffer[((xc + 0) * zSize + (zc + 1)) * ySize + (yc + 1)] - s1) * yStep;
                double s2a = (buffer[((xc + 1) * zSize + (zc + 0)) * ySize + (yc + 1)] - s2) * yStep;
                double s3a = (buffer[((xc + 1) * zSize + (zc + 1)) * ySize + (yc + 1)] - s3) * yStep;

                for (int y = 0; y < CHUNK_HEIGHT; y++)
				{
                    double xStep = 1 / (double) CHUNK_WIDTH;

                    double _s0 = s0;
                    double _s1 = s1;
                    double _s0a = (s2 - s0) * xStep;
                    double _s1a = (s3 - s1) * xStep;

                    for (int x = 0; x < CHUNK_WIDTH; x++)
					{
                        int offs = (x + xc * CHUNK_WIDTH) << Level::genDepthBitsPlusFour | (0 + zc * CHUNK_WIDTH) << Level::genDepthBits | (yc * CHUNK_HEIGHT + y);
                        int step = 1 << Level::genDepthBits;
                        double zStep = 1 / (double) CHUNK_WIDTH;

                        double val = _s0;
                        double vala = (_s1 - _s0) * zStep;
                        for (int z = 0; z < CHUNK_WIDTH; z++)
						{
                            int tileId = 0;
                            if (val > 0)
							{
                                tileId = Tile::whiteStone_Id;
                            } else {
                            }

                            blocks[offs] = (byte) tileId;
                            offs += step;
                            val += vala;
                        }
                        _s0 += _s0a;
                        _s1 += _s1a;
                    }

                    s0 += s0a;
                    s1 += s1a;
                    s2 += s2a;
                    s3 += s3a;
                }
            }
        }
    }
	delete [] buffer.data;

}

void TheEndLevelRandomLevelSource::buildSurfaces(int xOffs, int zOffs, byteArray blocks, BiomeArray biomes)
{
    for (int x = 0; x < 16; x++)
	{
        for (int z = 0; z < 16; z++)
		{
            int runDepth = 1;
            int run = -1;

            byte top = (byte) Tile::whiteStone_Id;
            byte material = (byte) Tile::whiteStone_Id;

            for (int y = Level::genDepthMinusOne; y >= 0; y--)
			{
                int offs = (z * 16 + x) * Level::genDepth + y;

                int old = blocks[offs];

                if (old == 0)
				{
                    run = -1;
                }
				else if (old == Tile::rock_Id)
				{
                    if (run == -1)
					{
                        if (runDepth <= 0)
						{
                            top = 0;
                            material = (byte) Tile::whiteStone_Id;
                        }

                        run = runDepth;
                        if (y >= 0) blocks[offs] = top;
                        else blocks[offs] = material;
                    }
					else if (run > 0)
					{
                        run--;
                        blocks[offs] = material;
                    }
                }
            }
        }
    }
}

LevelChunk *TheEndLevelRandomLevelSource::create(int x, int z)
{
	return getChunk(x, z);
}

LevelChunk *TheEndLevelRandomLevelSource::getChunk(int xOffs, int zOffs)
{
    random->setSeed(xOffs * 341873128712l + zOffs * 132897987541l);

	BiomeArray biomes;
	// 4J - now allocating this with a physical alloc & bypassing general memory management so that it will get cleanly freed
	unsigned int blocksSize = Level::genDepth * 16 * 16;
	byte *tileData = (byte *)XPhysicalAlloc(blocksSize, MAXULONG_PTR, 4096, PAGE_READWRITE);
	XMemSet128(tileData,0,blocksSize);
	byteArray blocks = byteArray(tileData,blocksSize);
//    byteArray blocks = byteArray(16 * level->depth * 16);

//    LevelChunk *levelChunk = new LevelChunk(level, blocks, xOffs, zOffs);			// 4J moved below
    level->getBiomeSource()->getBiomeBlock(biomes, xOffs * 16, zOffs * 16, 16, 16, true);

    prepareHeights(xOffs, zOffs, blocks, biomes);
    buildSurfaces(xOffs, zOffs, blocks, biomes);

	// 4J - this now creates compressed block data from the blocks array passed in, so moved it until after the blocks are actually finalised. We also
	// now need to free the passed in blocks as the LevelChunk doesn't use the passed in allocation anymore.
	LevelChunk *levelChunk = new LevelChunk(level, blocks, xOffs, zOffs);
	XPhysicalFree(tileData);

    levelChunk->recalcHeightmap();

	//delete blocks.data; // Don't delete the blocks as the array data is actually owned by the chunk now
	delete biomes.data;

    return levelChunk;
}

doubleArray TheEndLevelRandomLevelSource::getHeights(doubleArray buffer, int x, int y, int z, int xSize, int ySize, int zSize)
{
    if (buffer.data == NULL)
	{
        buffer = doubleArray(xSize * ySize * zSize);
    }

    double s = 1 * 684.412;
    double hs = 1 * 684.412;

	doubleArray pnr, ar, br, sr, dr, fi, fis;	// 4J - used to be declared with class level scope but moved here for thread safety

    sr = scaleNoise->getRegion(sr, x, z, xSize, zSize, 1.121, 1.121, 0.5);
    dr = depthNoise->getRegion(dr, x, z, xSize, zSize, 200.0, 200.0, 0.5);

    s *= 2;

    pnr = perlinNoise1->getRegion(pnr, x, y, z, xSize, ySize, zSize, s / 80.0, hs / 160.0, s / 80.0);
    ar = lperlinNoise1->getRegion(ar, x, y, z, xSize, ySize, zSize, s, hs, s);
    br = lperlinNoise2->getRegion(br, x, y, z, xSize, ySize, zSize, s, hs, s);

    int p = 0;
    int pp = 0;

    for (int xx = 0; xx < xSize; xx++)
	{
        for (int zz = 0; zz < zSize; zz++)
		{
            double scale = ((sr[pp] + 256.0) / 512);
            if (scale > 1) scale = 1;


            double depth = (dr[pp] / 8000.0);
            if (depth < 0) depth = -depth * 0.3;
            depth = depth * 3.0 - 2.0;

            // M2: central island plus outer islands on a 1024-block grid.
            float doffs = (float)endIslandDoffs(xx + x, zz + z);
            if (depth > 1) depth = 1;
            depth = depth / 8;
            depth = 0;

            if (scale < 0) scale = 0;
            scale = (scale) + 0.5;
            depth = depth * ySize / 16;

            pp++;

            double yCenter = ySize / 2.0;


            for (int yy = 0; yy < ySize; yy++)
			{
                double val = 0;
                double yOffs = (yy - (yCenter)) * 8 / scale;

                if (yOffs < 0) yOffs *= -1;

                double bb = ar[p] / 512;
                double cc = br[p] / 512;

                double v = (pnr[p] / 10 + 1) / 2;
                if (v < 0) val = bb;
                else if (v > 1) val = cc;
                else val = bb + (cc - bb) * v;
                val -= 8;
                val += doffs;

                int r = 2;
                if (yy > ySize / 2 - r)
				{
                    double slide = (yy - (ySize / 2 - r)) / (64.0f);
                    if (slide < 0) slide = 0;
                    if (slide > 1) slide = 1;
                    val = val * (1 - slide) + -3000 * slide;
                }
                r = 8;
                if (yy < r)
				{
                    double slide = (r - yy) / (r - 1.0f);
                    val = val * (1 - slide) + -30 * slide;
                }


                buffer[p] = val;
                p++;
            }
        }
    }

	delete [] pnr.data;
	delete [] ar.data;
	delete [] br.data;
	delete [] sr.data;
	delete [] dr.data;
	delete [] fi.data;
	delete [] fis.data;

    return buffer;

}

bool TheEndLevelRandomLevelSource::hasChunk(int x, int y)
{
	return true;
}

void TheEndLevelRandomLevelSource::calcWaterDepths(ChunkSource *parent, int xt, int zt)
{
    int xo = xt * 16;
    int zo = zt * 16;
    for (int x = 0; x < 16; x++)
	{
        int y = level->getSeaLevel();
        for (int z = 0; z < 16; z++)
		{
            int xp = xo + x + 7;
            int zp = zo + z + 7;
            int h = level->getHeightmap(xp, zp);
            if (h <= 0)
			{
                if (level->getHeightmap(xp - 1, zp) > 0 || level->getHeightmap(xp + 1, zp) > 0 || level->getHeightmap(xp, zp - 1) > 0 || level->getHeightmap(xp, zp + 1) > 0)
				{
                    bool hadWater = false;
                    if (hadWater || (level->getTile(xp - 1, y, zp) == Tile::calmWater_Id && level->getData(xp - 1, y, zp) < 7)) hadWater = true;
                    if (hadWater || (level->getTile(xp + 1, y, zp) == Tile::calmWater_Id && level->getData(xp + 1, y, zp) < 7)) hadWater = true;
                    if (hadWater || (level->getTile(xp, y, zp - 1) == Tile::calmWater_Id && level->getData(xp, y, zp - 1) < 7)) hadWater = true;
                    if (hadWater || (level->getTile(xp, y, zp + 1) == Tile::calmWater_Id && level->getData(xp, y, zp + 1) < 7)) hadWater = true;
                    if (hadWater)
					{
                        for (int x2 = -5; x2 <= 5; x2++)
						{
                            for (int z2 = -5; z2 <= 5; z2++)
							{
                                int d = (x2 > 0 ? x2 : -x2) + (z2 > 0 ? z2 : -z2);

                                if (d <= 5)
								{
                                    d = 6 - d;
                                    if (level->getTile(xp + x2, y, zp + z2) == Tile::calmWater_Id)
									{
                                        int od = level->getData(xp + x2, y, zp + z2);
                                        if (od < 7 && od < d)
										{
                                            level->setData(xp + x2, y, zp + z2, d);
                                        }
                                    }
                                }
                            }
                        }
                        if (hadWater)
						{
                            level->setTileAndDataNoUpdate(xp, y, zp, Tile::calmWater_Id, 7);
                            for (int y2 = 0; y2 < y; y2++)
							{
                                level->setTileAndDataNoUpdate(xp, y2, zp, Tile::calmWater_Id, 8);
                            }
                        }
                    }
                }
            }
        }
    }

}

void TheEndLevelRandomLevelSource::postProcess(ChunkSource *parent, int xt, int zt)
{
    HeavyTile::instaFall = true;
    int xo = xt * 16;
    int zo = zt * 16;

	// 4J - added. The original java didn't do any setting of the random seed here, and passes the level random to the biome decorator.
	// We'll be running our postProcess in parallel with getChunk etc. so we need to use a separate random - have used the same initialisation code as
	// used in RandomLevelSource::postProcess to make sure this random value is consistent for each world generation. 
	pprandom->setSeed(level->getSeed());
	__int64 xScale = pprandom->nextLong() / 2 * 2 + 1;
	__int64 zScale = pprandom->nextLong() / 2 * 2 + 1;
	pprandom->setSeed(((xt * xScale) + (zt * zScale)) ^ level->getSeed());

    Biome *biome = level->getBiome(xo + 16, zo + 16);
    biome->decorate(level, pprandom, xo, zo);		// 4J - passing pprandom rather than level->random here to make this consistent with our parallel world generation

    HeavyTile::instaFall = false;
	
	app.processSchematics(parent->getChunk(xt,zt));
}

bool TheEndLevelRandomLevelSource::save(bool force, ProgressListener *progressListener)
{
	return true;
}

bool TheEndLevelRandomLevelSource::tick()
{
	return false;
}

bool TheEndLevelRandomLevelSource::shouldSave()
{
	return true;
}

wstring TheEndLevelRandomLevelSource::gatherStats()
{
	return L"RandomLevelSource";
}

vector<Biome::MobSpawnerData *> *TheEndLevelRandomLevelSource::getMobsAt(MobCategory *mobCategory, int x, int y, int z)
{
    Biome *biome = level->getBiome(x, z);
    if (biome == NULL)
	{
        return NULL;
    }
    return biome->getMobs(mobCategory);
}

TilePos *TheEndLevelRandomLevelSource::findNearestMapFeature(Level *level, const wstring& featureName, int x, int y, int z)
{
	return NULL;
}
 
