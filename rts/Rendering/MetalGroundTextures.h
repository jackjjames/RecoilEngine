/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <cstdint>
#include <memory>

class ITexture;

// Tiled ground-texture cache for the Metal terrain pass. Reads the
// SMF tile stream (.smf -> .smt files referenced by the map) directly
// via the existing CSMFMapFile / CFileHandler API, decompresses each
// 32x32 DXT1 tile to RGBA8 on the CPU (no Apple-Silicon BC support),
// and packs every tile into one big atlas texture. Also builds a
// tile-index lookup texture (R32F, one entry per 32x32 ground cell,
// value = atlas index of the tile to draw at that cell).
//
// The fragment shader reads (mapUV) -> atlasIndex -> atlasUV and
// composites the proper SMF ground texture per cell, replacing the
// minimap-as-diffuse stand-in MetalWorldDrawer ships with by default.
//
// All file I/O is done on the Metal side via public CSMFMapFile API
// (no modifications to common code). The proper streaming /
// per-bigtex LOD path that CSMFGroundTextures implements for the GL
// build lands in a later slice once the IRenderTarget colour-
// attachment infrastructure is in place; until then this is a one-
// shot upload that's good enough for full-map skirmish framing.
//
// The renderer skips itself (IsValid() == false) when:
//   - readMap is not a CSMFReadMap (legacy SM3 / synthetic map)
//   - tileCount is 0 or absurdly large (atlas would exceed
//     Metal's 16384x16384 max texture dimension)
//   - any .smt file fails to open or has the wrong magic / version
class MetalGroundTextures
{
public:
	MetalGroundTextures();
	~MetalGroundTextures();

	MetalGroundTextures(const MetalGroundTextures&) = delete;
	MetalGroundTextures& operator=(const MetalGroundTextures&) = delete;

	bool IsValid() const { return valid; }

	// Big atlas of all tiles, RGBA8. tilesPerRow tiles wide,
	// (numTiles / tilesPerRow) rows high (rounded up). Each tile
	// occupies a 32x32 texel block; the fragment shader recovers
	// per-cell UVs from (atlasIndex, fractionalCellUV).
	ITexture* GetAtlasTexture()    const { return atlasTexture.get(); }
	// Tile-index lookup, R32F. Width = tileMapSizeX, height =
	// tileMapSizeY. Sampled with NEAREST so each ground cell reads
	// its exact integer tile index.
	ITexture* GetTileIndexTexture() const { return tileIndexTexture.get(); }

	int32_t GetTilesPerRow()   const { return tilesPerRow; }
	int32_t GetTilesPerCol()   const { return tilesPerCol; }
	int32_t GetTileMapSizeX()  const { return tileMapSizeX; }
	int32_t GetTileMapSizeY()  const { return tileMapSizeY; }

private:
	std::unique_ptr<ITexture> atlasTexture;
	std::unique_ptr<ITexture> tileIndexTexture;

	int32_t tilesPerRow  = 0;
	int32_t tilesPerCol  = 0;
	int32_t tileMapSizeX = 0;
	int32_t tileMapSizeY = 0;

	bool valid = false;
};

#endif // RENDER_BACKEND_METAL
