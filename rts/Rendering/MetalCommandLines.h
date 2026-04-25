/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <memory>

class IBuffer;
class IShaderPipeline;

// Command-queue overlay for selected units. Walks every selected
// CUnit's CCommandAI::commandQue each frame and emits a chained
// world-space polyline from the unit's current position through each
// position-bearing command target (move / attack / fight / patrol /
// repair / guard / build). Lines are colour-keyed by command type and
// alpha-blended on top of the world so they read clearly without
// burying terrain. Pure overlay - no game-state modification, common
// code is read-only via public unitHandler / commandAI accessors.
//
// Intentionally missing for this slice:
//   - target-unit lookup for unit-targeted commands (CMD_GUARD with a
//     unitID, CMD_REPAIR pointed at a unit). Lands when the renderer
//     starts caching CFeature / CUnit lookups by ID.
//   - depth-aware draw: the line list draws in NDC z order from the
//     viewProj transform but does not sample the depth buffer for
//     occlusion fades. Good enough to read against terrain; once
//     IRenderTarget grows depth attachments on Metal we can add the
//     X-ray dim style the GL build uses.
class MetalCommandLines
{
public:
	MetalCommandLines();
	~MetalCommandLines();

	MetalCommandLines(const MetalCommandLines&) = delete;
	MetalCommandLines& operator=(const MetalCommandLines&) = delete;

	bool IsValid() const { return valid; }

	void Draw();

private:
	std::unique_ptr<IShaderPipeline> pipeline;
	std::unique_ptr<IBuffer>         vertexBuffer;
	std::unique_ptr<IBuffer>         uniformBuffer;
	uint32_t                         bufferCapacity = 0;
	bool                             valid = false;
};

#endif // RENDER_BACKEND_METAL
