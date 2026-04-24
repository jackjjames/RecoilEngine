/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <memory>

class IBuffer;
class IShaderPipeline;

// Minimum-viable "unit positions are visible on the map" drawer. Draws
// a camera-facing quad at every active unit's position, sized by the
// unit's radius and coloured with its team colour. One dynamic vertex
// buffer re-uploaded each frame (capped at kMaxUnits).
//
// Real CUnitDrawer port (mesh loading, LOD, team-color replacement,
// bone animation) lands with S9-C4a/b. Until then this at least makes
// the simulation state visible so the terrain + LuaRules can be
// debugged visually.
class MetalUnitMarkers
{
public:
	MetalUnitMarkers();
	~MetalUnitMarkers();

	MetalUnitMarkers(const MetalUnitMarkers&) = delete;
	MetalUnitMarkers& operator=(const MetalUnitMarkers&) = delete;

	bool IsValid() const { return valid; }

	void Draw();

private:
	std::unique_ptr<IShaderPipeline> pipeline;
	std::unique_ptr<IBuffer>         vertexBuffer;
	std::unique_ptr<IBuffer>         uniformBuffer;

	size_t vertexBufferCapacity = 0;
	bool   valid                = false;
};

#endif // RENDER_BACKEND_METAL
