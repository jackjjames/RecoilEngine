/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalGroundTextures.h"

#include "Game/GameSetup.h"
#include "Map/MapInfo.h"
#include "Map/ReadMap.h"
#include "Map/SMF/SMFFormat.h"
#include "Map/SMF/SMFMapFile.h"
#include "Map/SMF/SMFReadMap.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/Textures/ITexture.h"
#include "Rendering/Textures/MetalDXTDecoder.h"
#include "Rendering/Textures/TextureCreationParams.hpp"
#include "System/FileSystem/FileHandler.h"
#include "System/FileSystem/FileSystem.h"
#include "System/Log/ILog.h"
#include "System/type2.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>


namespace {

// Apple Silicon Metal accepts up to 16384x16384 textures, but we cap
// the atlas to 8192x8192 so a single allocation stays under the
// 256 MB-per-resource budget that the integrated GPU keeps loose.
constexpr int kMaxAtlasSide   = 8192;
constexpr int kTilePixelSide  = 32;
constexpr uint32_t kGL_RGBA8  = 0x8058;
constexpr uint32_t kGL_R32F   = 0x822E;

// SMT files are streamed into one big tile-byte buffer, then walked
// tile-by-tile to fill an RGBA8 atlas. We only keep mip 0 of each
// tile - mips 1-3 stay in the file and the GPU can mipmap the atlas
// itself if it wants smaller lookups (linear filtering on the atlas
// is opt-in, see the MetalWorldDrawer constructor).
//
// Returns true on success and fills `outTileBytes` (size =
// numTiles * SMALL_TILE_SIZE), `outTileMap` (size = tileCount), and
// the tilesPerRow / tilesPerCol layout the atlas uses.
bool LoadSMFTilePayload(CSMFReadMap*                 smfMap,
                        std::vector<char>&           outTileBytes,
                        std::vector<int32_t>&        outTileMap,
                        int32_t&                     outNumTiles)
{
	if (smfMap == nullptr)
		return false;

	CSMFMapFile& mapFile = smfMap->GetMapFile();
	CFileHandler* ifs = mapFile.GetFileHandler();
	if (ifs == nullptr)
		return false;

	const SMFHeader& header = mapFile.GetHeader();
	ifs->Seek(header.tilesPtr);

	MapTileHeader tileHeader;
	CSMFMapFile::ReadMapTileHeader(tileHeader, *ifs);

	if (smfMap->tileCount <= 0 || tileHeader.numTiles <= 0)
		return false;

	outNumTiles = tileHeader.numTiles;
	outTileBytes.assign(static_cast<size_t>(tileHeader.numTiles) * SMALL_TILE_SIZE, 0);
	outTileMap.assign(smfMap->tileCount, 0);

	// SMT path resolution mirrors CSMFGroundTextures::LoadTiles:
	// either follow the .smt names baked into the .smf file, or use
	// the override list from mapInfo->smf.smtFileNames when the map
	// description ships explicit paths. Same fallback chain as the
	// GL build so we hit the same files for the same maps.
	const std::string smfDir = (gameSetup != nullptr)
	                         ? FileSystem::GetDirectory(gameSetup->MapFileName())
	                         : std::string();
	const std::vector<std::string>* overrideNames = nullptr;
	if (mapInfo != nullptr && !mapInfo->smf.smtFileNames.empty()
	    && static_cast<int>(mapInfo->smf.smtFileNames.size()) == tileHeader.numTileFiles)
	{
		overrideNames = &mapInfo->smf.smtFileNames;
	}

	int curTile = 0;
	for (int a = 0; a < tileHeader.numTileFiles; ++a) {
		int  numSmallTiles = 0;
		char fileNameBuffer[256] = {0};

		ifs->Read(&numSmallTiles, sizeof(int));
		ifs->ReadString(&fileNameBuffer[0], sizeof(char) * (sizeof(fileNameBuffer) - 1));

		const std::string smtFileName  = fileNameBuffer;
		const std::string smtFilePath  = (overrideNames == nullptr)
		                               ? (smfDir + smtFileName)
		                               : (smfDir + (*overrideNames)[a]);

		CFileHandler tileFile(smtFilePath);
		if (!tileFile.FileExists()) {
			// Try the path without the smfDir prefix (some maps ship
			// .smt directly in the maps/ root rather than alongside
			// the .smf). Same fallback the GL build does.
			tileFile.Open((overrideNames == nullptr) ? smtFileName : (*overrideNames)[a]);
		}

		if (!tileFile.FileExists()) {
			LOG_L(L_WARNING,
				"[MetalGroundTextures] missing .smt tile-file '%s'; %d tiles will be magenta",
				smtFilePath.c_str(), numSmallTiles);
			std::memset(&outTileBytes[curTile * SMALL_TILE_SIZE], 0xaa,
			            static_cast<size_t>(numSmallTiles) * SMALL_TILE_SIZE);
			curTile += numSmallTiles;
			continue;
		}

		TileFileHeader tfh;
		CSMFMapFile::ReadMapTileFileHeader(tfh, tileFile);
		if (std::strcmp(tfh.magic, "spring tilefile") != 0
		    || tfh.version != 1 || tfh.tileSize != 32 || tfh.compressionType != 1)
		{
			LOG_L(L_ERROR,
				"[MetalGroundTextures] tile-file '%s' magic/version/size/comprType mismatch",
				smtFilePath.c_str());
			return false;
		}

		for (int b = 0; b < numSmallTiles; ++b) {
			tileFile.Read(&outTileBytes[(curTile++) * SMALL_TILE_SIZE], SMALL_TILE_SIZE);
		}
	}

	ifs->Read(outTileMap.data(), static_cast<size_t>(smfMap->tileCount) * sizeof(int));
	return true;
}

} // namespace


MetalGroundTextures::MetalGroundTextures()
{
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr)
		return;
	auto& backend = *globalRendering->renderBackend;

	auto* smfMap = dynamic_cast<CSMFReadMap*>(readMap);
	if (smfMap == nullptr) {
		LOG_L(L_INFO, "[MetalGroundTextures] readMap is not SMF; tiled diffuse disabled");
		return;
	}

	std::vector<char>    tileBytes;
	std::vector<int32_t> tileMap;
	int32_t numTiles = 0;
	if (!LoadSMFTilePayload(smfMap, tileBytes, tileMap, numTiles))
		return;

	tileMapSizeX = smfMap->tileMapSizeX;
	tileMapSizeY = smfMap->tileMapSizeY;
	if (tileMapSizeX <= 0 || tileMapSizeY <= 0 || numTiles <= 0)
		return;

	// Pick a roughly square atlas layout. Width = ceil(sqrt(N)),
	// height = ceil(N / width). Both rounded up to the next power of
	// two on the pixel side so Metal can keep the texture tiled
	// efficiently. Bail if the chosen layout would exceed the cap.
	tilesPerRow = static_cast<int32_t>(std::ceil(std::sqrt(static_cast<float>(numTiles))));
	if (tilesPerRow < 1)
		tilesPerRow = 1;
	tilesPerCol = (numTiles + tilesPerRow - 1) / tilesPerRow;

	const int32_t atlasW = tilesPerRow * kTilePixelSide;
	const int32_t atlasH = tilesPerCol * kTilePixelSide;
	if (atlasW > kMaxAtlasSide || atlasH > kMaxAtlasSide) {
		LOG_L(L_WARNING,
			"[MetalGroundTextures] %d tiles would need %dx%d atlas (cap %d); falling back to minimap diffuse",
			numTiles, atlasW, atlasH, kMaxAtlasSide);
		return;
	}

	// Build the atlas RGBA8 buffer one tile at a time. For each tile
	// the SMT stores 4 mip levels packed as DXT1: only mip 0 (the
	// 32x32 image, first 512 bytes) goes into the atlas; finer mips
	// would need a chained mip layout this slice doesn't bother
	// with - linear filtering between adjacent tiles is the bigger
	// quality lever and we get that with the next slice's per-tile
	// padding pass.
	std::vector<uint8_t> atlas(static_cast<size_t>(atlasW) * atlasH * 4, 0);

	for (int32_t t = 0; t < numTiles; ++t) {
		const int32_t tx = t % tilesPerRow;
		const int32_t ty = t / tilesPerRow;
		const uint8_t* src = reinterpret_cast<const uint8_t*>(
			&tileBytes[static_cast<size_t>(t) * SMALL_TILE_SIZE]);

		// Decompress 32x32 DXT1 -> 32x32 RGBA8 (4 KB) into a scratch
		// buffer, then copy row-by-row into the atlas. Direct
		// decompression into the atlas would need the BC1 decoder
		// to know the destination stride; the scratch+copy path
		// keeps MetalDXT::DecompressBC1Image's contract simple.
		uint8_t tile[kTilePixelSide * kTilePixelSide * 4];
		MetalDXT::DecompressBC1Image(src, tile, kTilePixelSide, kTilePixelSide);

		for (int32_t row = 0; row < kTilePixelSide; ++row) {
			const uint8_t* s = &tile[row * kTilePixelSide * 4];
			uint8_t*       d = &atlas[((ty * kTilePixelSide + row) * atlasW
			                            + tx * kTilePixelSide) * 4];
			std::memcpy(d, s, kTilePixelSide * 4);
		}
	}

	GL::TextureCreationParams atlasParams;
	atlasParams.linearTextureFilter = false; // NEAREST: avoid bleeding from neighbour tiles in the atlas
	atlasParams.linearMipMapFilter  = false;
	atlasParams.reqNumLevels = 1;
	atlasTexture = backend.CreateTexture2D(int2(atlasW, atlasH), kGL_RGBA8, atlasParams, /*wantCompress=*/false);
	if (!atlasTexture || !atlasTexture->IsValid()) {
		LOG_L(L_ERROR, "[MetalGroundTextures] atlas texture creation failed (%dx%d)", atlasW, atlasH);
		return;
	}
	atlasTexture->UploadImage(atlas.data());

	// Tile-index texture: one float per ground cell, value = tile
	// index into the atlas. R32F because float can represent every
	// integer up to 2^24 exactly and we never approach that ceiling
	// here. NEAREST so the per-cell read snaps to the right tile.
	std::vector<float> tileIndexF(static_cast<size_t>(tileMapSizeX) * tileMapSizeY, 0.0f);
	for (size_t i = 0; i < tileIndexF.size(); ++i) {
		const int32_t idx = tileMap[i];
		tileIndexF[i] = (idx >= 0 && idx < numTiles) ? static_cast<float>(idx) : 0.0f;
	}

	GL::TextureCreationParams idxParams;
	idxParams.linearTextureFilter = false;
	idxParams.linearMipMapFilter  = false;
	idxParams.reqNumLevels = 1;
	tileIndexTexture = backend.CreateTexture2D(int2(tileMapSizeX, tileMapSizeY), kGL_R32F, idxParams, /*wantCompress=*/false);
	if (!tileIndexTexture || !tileIndexTexture->IsValid()) {
		LOG_L(L_ERROR, "[MetalGroundTextures] tile-index texture creation failed (%dx%d)", tileMapSizeX, tileMapSizeY);
		atlasTexture.reset();
		return;
	}
	tileIndexTexture->UploadImage(tileIndexF.data());

	LOG("[MetalGroundTextures] atlas=%dx%d (%d tiles, %dx%d grid) tileMap=%dx%d",
		atlasW, atlasH, numTiles, tilesPerRow, tilesPerCol, tileMapSizeX, tileMapSizeY);
	valid = true;
}

MetalGroundTextures::~MetalGroundTextures() = default;

#endif // RENDER_BACKEND_METAL
