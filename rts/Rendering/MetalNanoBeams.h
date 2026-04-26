/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <memory>

class IBuffer;
class IShaderPipeline;

// Animated nanolathe beams between every active CBuilder and its
// current build / repair / reclaim / resurrect target. Colour-codes
// the action so the player can read what each constructor is doing
// at a glance, and the shader scrolls a dashed alpha mask along the
// beam to give the spray a sense of motion without needing per-tick
// CEG particle simulation.
//
// Pure observer; reads CBuilder::curBuild / curReclaim / curResurrect
// + the targets' world positions through the public CSolidObject
// surface. No common-code mutation, no hooks into the unit script
// pipeline (no nanoPieceCache / NanoFireEvent traversal yet - those
// queue behind a separate slice once a Metal CEG renderer lands).
class MetalNanoBeams
{
public:
	MetalNanoBeams();
	~MetalNanoBeams();

	MetalNanoBeams(const MetalNanoBeams&) = delete;
	MetalNanoBeams& operator=(const MetalNanoBeams&) = delete;

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
