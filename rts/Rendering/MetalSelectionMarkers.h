/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <cstdint>
#include <memory>

class IBuffer;
class IShaderPipeline;

// Selection rings + health bars overlay. Walks unitHandler each
// frame and emits, in world space:
//   1) a thin white ring on the ground at the base of every selected
//      unit, sized by the unit's collision radius; alpha-blended so
//      it tints the underlying terrain rather than overpainting it
//   2) a small camera-billboarded health bar above every unit that
//      is damaged or owned by the local ally team, split red/green
//      proportional to health/maxHealth
//
// Both pass through a single pipeline (vec3 position + vec4 colour)
// keyed by view-proj matrix - no per-marker UBO. Drawn after units
// + projectiles and before the HUD text so markers read cleanly on
// top of the silhouette.
//
// All inputs come through public unitHandler / teamHandler / camera
// surface; common code stays untouched. The proper CSelectionDrawer
// port (with command queues, build squares, formation lines) lands
// later once IRenderTarget grows colour attachments on Metal.
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
