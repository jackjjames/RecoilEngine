/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <memory>
#include <unordered_map>
#include <vector>

#include "System/float3.h"

class IBuffer;
class IShaderPipeline;

// Persistent ground scorch decals at every unit-death site. Same
// snapshot-diff observer pattern as MetalDeathFX so the two stay
// visually paired (the bright fireball burns for ~1.4s, the scorch
// stays for the rest of the match). Craters tessellate to a small
// terrain-conforming grid at spawn so they lie flush on slopes; the
// fragment shader paints a dark brown splotch with radial falloff
// that darkens the ground texture beneath through standard alpha
// blending.
//
// Common code is read-only (unitHandler, CGround heightmap). Crater
// pool is FIFO-capped so the buffer doesn't grow unbounded over very
// long matches; the cap is generous so it only kicks in well past
// the point where individual decals would be visually distinguishable.
class MetalCraters
{
public:
	MetalCraters();
	~MetalCraters();

	MetalCraters(const MetalCraters&) = delete;
	MetalCraters& operator=(const MetalCraters&) = delete;

	bool IsValid() const { return valid; }

	void Draw();

private:
	struct Crater {
		float3 center;
		float  radius = 24.0f;
	};

	std::unique_ptr<IShaderPipeline> pipeline;
	std::unique_ptr<IBuffer>         vertexBuffer;
	std::unique_ptr<IBuffer>         uniformBuffer;
	uint32_t                         vertexBufferCapacity = 0;
	std::vector<Crater>              craters;
	std::unordered_map<int, float3>  prevUnits;
	bool                             valid = false;
};

#endif // RENDER_BACKEND_METAL
