/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <cstdint>
#include <memory>

class IBuffer;
class IShaderPipeline;

// Selection rings overlay. Walks unitHandler each frame and emits a
// thin white ring on the ground at the base of every selected unit,
// sized by the unit's collision radius; alpha-blended so it tints the
// underlying terrain rather than overpainting it.
//
// Both pass through a single pipeline (vec3 position + vec4 colour)
// keyed by view-proj matrix - no per-marker UBO. Drawn after units
// + projectiles and before the HUD text so markers read cleanly on
// top of the silhouette.
//
// All inputs come through public unitHandler / teamHandler / camera
// surface; common code stays untouched. Lua/UI health bars are not
// duplicated here: they need the Lua render bridge rather than a
// Metal-only fallback.
class MetalSelectionMarkers
{
public:
	MetalSelectionMarkers();
	~MetalSelectionMarkers();

	MetalSelectionMarkers(const MetalSelectionMarkers&) = delete;
	MetalSelectionMarkers& operator=(const MetalSelectionMarkers&) = delete;

	bool IsValid() const { return valid; }

	void Draw();

private:
	std::unique_ptr<IShaderPipeline> pipeline;
	std::unique_ptr<IBuffer>         vertexBuffer;
	std::unique_ptr<IBuffer>         uniformBuffer;

	uint32_t bufferCapacity = 0;

	bool valid = false;
};

#endif // RENDER_BACKEND_METAL
