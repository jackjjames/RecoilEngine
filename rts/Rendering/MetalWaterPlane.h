/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <memory>

class IBuffer;
class IShaderPipeline;

// Cheap analytic water plane. Draws a single map-sized quad at y = 0
// and lets the fragment shader synthesise wavelets, fresnel sky
// reflection and depth-based opacity from the live waterRendering
// palette + ISky direction. No render-target reflections, no caustics
// texture; the proper CBumpWater port (later S9 slice) replaces this
// once IRenderTarget grows colour attachments on Metal.
//
// Drawn after the terrain pass and before unit shadows so partially
// submerged units still cast a visible shadow on the terrain through
// the alpha-blended water surface. Skipped on dry maps
// (currentMinHeight >= 0 and forceRendering == false), matching the
// GL build's "don't bother allocating water on land-only maps"
// guard.
class MetalWaterPlane
{
public:
	MetalWaterPlane();
	~MetalWaterPlane();

	MetalWaterPlane(const MetalWaterPlane&) = delete;
	MetalWaterPlane& operator=(const MetalWaterPlane&) = delete;

	bool IsValid() const { return valid; }

	void Draw() const;

private:
	std::unique_ptr<IShaderPipeline> pipeline;
	std::unique_ptr<IBuffer>         uniformBuffer;

	bool valid = false;
};

#endif // RENDER_BACKEND_METAL
