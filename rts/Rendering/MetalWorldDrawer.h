/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <cstdint>
#include <memory>

class IBuffer;
class IShaderPipeline;

// Stage 9 minimum-viable world drawer for the Metal backend. Builds a
// subsampled heightmap mesh (one vertex per N×N corner block) from
// CReadMap::GetCornerHeightMapUnsynced on construction, compiles a
// matmul + height-shaded pipeline through the shared glslang /
// spirv-cross path, and issues one indexed draw per frame using the
// active camera's view-projection matrix.
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

	uint32_t indexCount       = 0;
	float    cachedMinHeight  = 0.0f;
	float    cachedMaxHeight  = 0.0f;
	bool     valid            = false;
};

#endif // RENDER_BACKEND_METAL
