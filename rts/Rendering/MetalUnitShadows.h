/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <memory>

class IBuffer;
class IShaderPipeline;

// Cheap fake-shadow drawer: for every active unit / feature, projects
// the unit's bounding sphere onto the ground along the inverse sun
// direction and draws a soft elliptical patch there. Not a real
// shadow map - the silhouette isn't sampled and self-shadowing /
// terrain-on-unit shadows don't appear - but the ellipse stretches
// and rotates with the sun, which gives the scene strong directional
// lighting context. The proper sun-projected shadow pass (S9-C5a
// part 2) replaces this once IRenderTarget grows real depth
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
