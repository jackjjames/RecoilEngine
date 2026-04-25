/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <memory>

class IBuffer;
class IShaderPipeline;

// Cheap fake-shadow drawer: for every active unit / feature, drops a
// soft circular dark blob on the ground at its footprint. This is not
// a real shadow map - the sun direction isn't sampled and tall
// silhouettes don't elongate - but it's the single biggest "ground
// contact" cue you can deliver without standing up render-to-texture
// + a depth-only pre-pass. The proper sun-projected shadow pass
// (S9-C5a part 2) replaces this once IRenderTarget grows real depth
// attachments on Metal.
//
// Drawn between the terrain and unit mesh passes so units overdraw
// their own shadow at the contact point and the blob still covers the
// ground beneath them.
class MetalUnitShadows
{
public:
	MetalUnitShadows();
	~MetalUnitShadows();

	MetalUnitShadows(const MetalUnitShadows&) = delete;
	MetalUnitShadows& operator=(const MetalUnitShadows&) = delete;

	bool IsValid() const { return valid; }

	void Draw();

private:
	std::unique_ptr<IShaderPipeline> pipeline;
	std::unique_ptr<IBuffer>         vertexBuffer;
	std::unique_ptr<IBuffer>         indexBuffer;
	std::unique_ptr<IBuffer>         uniformBuffer;

	// Capacity (in shadow quads) the dynamic vertex/index buffers can
	// hold. Re-allocated when the active unit count grows past it.
	uint32_t bufferCapacity = 0;

	bool valid = false;
};

#endif // RENDER_BACKEND_METAL
