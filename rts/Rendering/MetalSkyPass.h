/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <memory>

class IBuffer;
class IShaderPipeline;

// Full-screen sky gradient pass. Draws a single triangle that covers
// the viewport and lets the fragment shader compute a horizon-zenith
// gradient with a soft sun disc based on the live ISkyLight direction.
//
// Vertex-less: no vertex buffer, the shader synthesises positions from
// gl_VertexIndex. Runs first in the Metal frame so the terrain draws
// on top of the sky without needing a depth buffer.
//
// Stage 9/C5b replaces this with a real ModernSky port (cube-map + sky
// atmospherics); until then this keeps the background from being
// pitch-black and gives the terrain a believable skydome to sit in.
class MetalSkyPass
{
public:
	MetalSkyPass();
	~MetalSkyPass();

	MetalSkyPass(const MetalSkyPass&) = delete;
	MetalSkyPass& operator=(const MetalSkyPass&) = delete;

	bool IsValid() const { return valid; }

	void Draw() const;

private:
	std::unique_ptr<IShaderPipeline> pipeline;
	std::unique_ptr<IBuffer>         uniformBuffer;

	bool valid = false;
};

#endif // RENDER_BACKEND_METAL
