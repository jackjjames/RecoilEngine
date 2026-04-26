/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <memory>

class IBuffer;
class IShaderPipeline;

// Ground-plane mouse cursor for the Metal backend. Reads
// CMouseHandler::GetWorldMapPos() each frame and draws a thin pulsing
// ring on the terrain at that point so the player has any kind of
// pointer feedback before the GL CMouseCursor sprite renderer is
// ported. Safely no-ops when the cursor is over the skybox / off the
// ground (GetWorldMapPos returns -OnesVector in that case).
//
// Pure overlay; common code is read-only via the public mouse pointer.
class MetalWorldCursor
{
public:
	MetalWorldCursor();
	~MetalWorldCursor();

	MetalWorldCursor(const MetalWorldCursor&) = delete;
	MetalWorldCursor& operator=(const MetalWorldCursor&) = delete;

	bool IsValid() const { return valid; }

	void Draw();

private:
	std::unique_ptr<IShaderPipeline> pipeline;
	std::unique_ptr<IBuffer>         vertexBuffer;
	std::unique_ptr<IBuffer>         uniformBuffer;
	uint32_t                         vertCount = 0;
	bool                             valid = false;
};

#endif // RENDER_BACKEND_METAL
