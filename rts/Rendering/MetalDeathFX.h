/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <memory>
#include <unordered_map>
#include <vector>

#include "System/float3.h"

class IBuffer;
class IShaderPipeline;

// Death-event FX: each frame we snapshot the active unit IDs +
// positions, then on the next frame any ID that was present last
// frame but not this frame counts as a death and spawns a short-lived
// expanding billboard at its last-known position. The shader paints a
// hot core that fades outward into smoke through alpha falloff +
// radial gradient.
//
// Pure observer pattern; we only read public unitHandler state. No
// hooks into the eventHandler death pipeline (that's common code) -
// the snapshot diff catches every disappearance regardless of cause
// (killed, scuttled, transferred, removed by Lua) which is what we
// want for a visual cue.
//
// Particles age out on a fixed lifetime and the active list compacts
// in place each frame. The pool is uncapped but bounded in practice
// by death rate; a runaway count would be self-limiting since the
// vertex buffer auto-grows from a 4 KiB seed and drops trailing
// particles when growth fails.
class MetalDeathFX
{
public:
	MetalDeathFX();
	~MetalDeathFX();

	MetalDeathFX(const MetalDeathFX&) = delete;
	MetalDeathFX& operator=(const MetalDeathFX&) = delete;

	bool IsValid() const { return valid; }

	void Draw();

private:
	struct Particle {
		float3 pos;
		float  birthTime = 0.0f;
		float  baseRadius = 16.0f;
	};

	std::unique_ptr<IShaderPipeline> pipeline;
	std::unique_ptr<IBuffer>         vertexBuffer;
	std::unique_ptr<IBuffer>         uniformBuffer;
	uint32_t                         bufferCapacity = 0;
	std::vector<Particle>            particles;
	std::unordered_map<int, float3>  prevUnits;
	bool                             valid = false;
};

#endif // RENDER_BACKEND_METAL
