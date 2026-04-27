/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <cstdint>
#include <memory>

class IBuffer;
class IShaderPipeline;
class ITexture;

// Metal renderer for the engine minimap. Draws the SMF minimap
// top-mip (1024x1024 DXT1 -> RGBA8) into the geometry owned by CMiniMap
// and overlays a coloured dot per active unit at its projected world XZ
// -> map UV position. Team color comes from teamHandler->Team()->color.
//
// Important: layout/state come from the existing minimap path
// (MiniMapGeometry config, Lua/PIP widget geometry changes). Metal must
// not invent a separate minimap layout.
//
// Skipped (IsValid() false) on synthetic / non-SMF maps that have
// no minimap mip in the .smf payload.
class MetalMinimap
{
public:
	MetalMinimap();
	~MetalMinimap();

	MetalMinimap(const MetalMinimap&) = delete;
	MetalMinimap& operator=(const MetalMinimap&) = delete;

	bool IsValid() const { return valid; }

	void Draw();

private:
	std::unique_ptr<IShaderPipeline> bgPipeline;
	std::unique_ptr<IShaderPipeline> dotPipeline;
	std::unique_ptr<IBuffer>         bgVertexBuffer;
	std::unique_ptr<IBuffer>         dotVertexBuffer;
	std::unique_ptr<ITexture>        minimapTexture;

	uint32_t dotBufferCapacity = 0;

	bool restoredCommonGeometry = false;
	bool valid = false;
};

#endif // RENDER_BACKEND_METAL
