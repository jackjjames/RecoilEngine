/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <cstdint>
#include <memory>

class IBuffer;
class IShaderPipeline;
class ITexture;

// Bottom-left corner minimap. Draws the SMF minimap top-mip (1024x1024
// DXT1 -> RGBA8) into a fixed-size NDC quad and overlays a coloured
// dot per active unit at its projected world XZ -> map UV position.
// Team color comes from teamHandler->Team()->color.
//
// All screen-space, no projection matrix: positions are emitted
// directly in NDC. Unit positions are read from unitHandler each
// frame; the dot vertex buffer grows in 1 KB power-of-two steps so
// most frames recycle a single allocation.
//
// This is a passive minimap (no click handling, no selection box,
// no fog-of-war). The RTT-driven CMiniMap port lands later once
// IRenderTarget grows colour attachments on Metal; until then this
// gives the player situational awareness without touching common
// code.
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

	bool valid = false;
};

#endif // RENDER_BACKEND_METAL
