/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef _3DO_TEXTURE_HANDLER_H
#define _3DO_TEXTURE_HANDLER_H

#include <memory>
#include <string>
#include <vector>

#include "Rendering/GL/myGL.h"
#include "Rendering/Textures/AtlasedTexture.hpp"
#include "Rendering/Textures/ITexture.h"
#include "Rendering/Textures/TAPalette.h"
#include "System/float4.h"
#include "System/UnorderedMap.hpp"

struct TexFile;

class C3DOTextureHandler
{
public:
	using UnitTexture = float4;

	void Init();
	void Kill();

	// NOTE: safe with unordered_map after all textures have been loaded
	UnitTexture* Get3DOTexture(const std::string& name);

	unsigned int GetAtlasTex1ID() const { return atlas3do1Handle ? atlas3do1Handle->GetNativeId() : 0; }
	unsigned int GetAtlasTex2ID() const { return atlas3do2Handle ? atlas3do2Handle->GetNativeId() : 0; }
	unsigned int GetAtlasTexSizeX() const { return bigTexX; }
	unsigned int GetAtlasTexSizeY() const { return bigTexY; }

	const spring::unordered_map<std::string, UnitTexture>& GetAtlasTextures() const { return textures; }

	void DumpAtlasTextures(const std::string& fileExt = "png") const;

private:
	std::vector<TexFile> LoadTexFiles();

	static TexFile CreateTex(const std::string& name, const std::string& name2, bool teamcolor = false);

private:
	spring::unordered_map<std::string, UnitTexture> textures;

	CTAPalette palette;

	std::unique_ptr<ITexture> atlas3do1Handle;
	std::unique_ptr<ITexture> atlas3do2Handle;
	int bigTexX = 0;
	int bigTexY = 0;
	int numLevels = 0;
};

extern C3DOTextureHandler textureHandler3DO;

#endif /* _3DO_TEXTURE_HANDLER_H */
