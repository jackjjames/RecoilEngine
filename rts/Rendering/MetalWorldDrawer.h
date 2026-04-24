/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <cstdint>
#include <memory>

class IBuffer;
class IShaderPipeline;
class ITexture;

// Stage 9 minimum-viable world drawer for the Metal backend. Builds a
// flat XZ grid mesh at a fixed resolution and displaces Y in the
// vertex stage by sampling an R32F heightmap texture uploaded from
// CReadMap::GetCornerHeightMapUnsynced. Mesh density is decoupled
// from corner-heightmap density so later slices can swap in higher-
// res grids (or drop to a coarser grid on MBP integrated GPUs)
// without touching the map-data path.
//
// Deliberately skips everything SMFGroundDrawer does beyond producing
// geometry on screen: no SMF tile textures, no shadow pass, no detail
// normal map, no water, no LOD. Those land with the C3b/C3c/C3d
// slices. Goal here is "the map is visible" so the rest of the GL →
// Metal drawer ports have something to sit on top of.
class MetalWorldDrawer
{
public:
	MetalWorldDrawer();
	~MetalWorldDrawer();

	MetalWorldDrawer(const MetalWorldDrawer&) = delete;
	MetalWorldDrawer& operator=(const MetalWorldDrawer&) = delete;

	bool IsValid() const { return valid; }

	// Issue the draw against the current Metal frame's encoder. No-op
	// when the drawer failed to init (caller can still draw the splash
	// fallback). Expects BeginFrame/PresentFrame to bracket this from
	// SpringApp::Update as usual.
	void Draw() const;

private:
	std::unique_ptr<IShaderPipeline> pipeline;
	std::unique_ptr<IBuffer>         vertexBuffer;
	std::unique_ptr<IBuffer>         indexBuffer;
	std::unique_ptr<IBuffer>         uniformBuffer;
	std::unique_ptr<ITexture>        heightmapTexture;

	uint32_t indexCount       = 0;
	float    cachedMinHeight  = 0.0f;
	float    cachedMaxHeight  = 0.0f;
	bool     valid            = false;
};

#endif // RENDER_BACKEND_METAL
