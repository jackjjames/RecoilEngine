/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <memory>

class IBuffer;
class IShaderPipeline;

// Weapon range rings around every selected unit. For each selection
// the longest-range non-zero weapon contributes a thin ground-plane
// circle so the player can see firing radius at a glance. Visualises
// only the primary weapon to avoid the clutter you'd get from
// overplotting every weapon on commanders / cruisers.
//
// Pure observer; reads CUnit::isSelected, CUnit::pos, the public
// weapons vector and CWeapon::range. No common-code mutation.
class MetalRangeRings
{
public:
	MetalRangeRings();
	~MetalRangeRings();

	MetalRangeRings(const MetalRangeRings&) = delete;
	MetalRangeRings& operator=(const MetalRangeRings&) = delete;

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
