/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <memory>

class IBuffer;
class IShaderPipeline;

// Pulsing build-progress halo on the ground around every unit
// currently under construction (CUnit::beingBuilt). Colour shifts
// from yellow at 0% progress through to green at 100%, and the alpha
// pulses so the player's eye is drawn to active build sites without
// it being visually noisy when the field is busy.
//
// Pure observer; reads unitHandler + CUnit public state (beingBuilt,
// buildProgress, pos, radius). No common-code mutation.
//
// Limitations vs. BAR's chobby HUD: no progress arc (full ring all
// the way around), no nanolathe beam visualisation, no separate
// repair / reclaim distinction. All three layer on once the Metal
// HUD has more advanced widget primitives.
class MetalBuildHalo
{
public:
	MetalBuildHalo();
	~MetalBuildHalo();

	MetalBuildHalo(const MetalBuildHalo&) = delete;
	MetalBuildHalo& operator=(const MetalBuildHalo&) = delete;

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
