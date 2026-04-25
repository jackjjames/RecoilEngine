/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalCommandLines.h"

#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Game/GlobalUnsynced.h"
#include "Map/Ground.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/IBuffer.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "Sim/Units/CommandAI/Command.h"
#include "Sim/Units/CommandAI/CommandAI.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitHandler.h"
#include "System/Log/ILog.h"
#include "System/float3.h"

#include <cstring>
#include <vector>


namespace {

struct LineVertex {
	float pos[3];
	float color[4];
};

struct alignas(16) UBOLayout {
	float viewProj[16];
};

constexpr const char* kVertexGlsl = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 0) out vec4 vColor;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 uViewProj;
} ubo;

void main() {
    vColor = aColor;
    gl_Position = ubo.uViewProj * vec4(aPos, 1.0);
}
)";

constexpr const char* kFragmentGlsl = R"(#version 450
layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = vColor;
}
)";


struct CommandStyle {
	float r, g, b, a;
};

// Order-line colour palette. Hue maps to command intent: green = move,
// red = attack, cyan = patrol, yellow = guard, lime = repair / build.
// Alpha kept at 0.85 so lines read on light terrain without saturating
// over snow / ice tiles.
CommandStyle StyleForCommand(int cmdID)
{
	if (cmdID < 0)
		return {0.20f, 1.00f, 0.20f, 0.85f}; // build (negative = -unitDefID)

	switch (cmdID) {
		case CMD_MOVE:    return {0.20f, 1.00f, 0.30f, 0.85f};
		case CMD_FIGHT:   return {0.10f, 0.80f, 1.00f, 0.85f};
		case CMD_PATROL:  return {0.30f, 0.80f, 1.00f, 0.85f};
		case CMD_ATTACK:  return {1.00f, 0.20f, 0.20f, 0.90f};
		case CMD_GUARD:   return {1.00f, 0.95f, 0.20f, 0.85f};
		case CMD_REPAIR:  return {0.30f, 1.00f, 0.50f, 0.85f};
	}
	// Unknown / non-positional commands (stop, wait, selfd, etc.) get
	// drawn dim white so the chain visually continues without making
	// up a colour for a category we don't model yet.
	return {0.85f, 0.85f, 0.85f, 0.55f};
}

// Pull the world-space target out of a Command. Most positional
// commands keep XYZ in params[0..2]; some build queue entries (negative
// id) follow the same convention. Returns false for unit-targeted
// commands (single param = unit ID) so we don't draw lines to (0,0,0).
bool ExtractTargetPos(const Command& c, float3& out)
{
	const auto* params = c.GetParams();
	const int   numP   = static_cast<int>(c.GetNumParams());
	if (params == nullptr)
		return false;

	if (c.IsBuildCommand()) {
		// build orders carry XYZ + facing; XYZ are valid even when
		// the build site is on water (y can be negative)
		if (numP >= 3) {
			out = float3(params[0], params[1], params[2]);
			return true;
		}
		return false;
	}

	switch (c.GetID()) {
		case CMD_MOVE:
		case CMD_FIGHT:
		case CMD_PATROL:
			if (numP >= 3) {
				out = float3(params[0], params[1], params[2]);
				return true;
			}
			break;
		case CMD_ATTACK:
		case CMD_REPAIR:
			// these can be either unit-targeted (1 param = unitID) or
			// position-targeted (3 params). Use the position form
			// only; unit-targeted variants land with the ID-lookup
			// follow-up.
			if (numP >= 3) {
				out = float3(params[0], params[1], params[2]);
				return true;
			}
			break;
	}
	return false;
}

constexpr float kLift = 6.0f; // raise lines slightly above ground for visibility

float3 GroundClampedPos(const float3& p)
{
	float3 r = p;
	r.y = std::max(p.y, CGround::GetHeightReal(p.x, p.z, false)) + kLift;
	return r;
}

} // namespace


MetalCommandLines::MetalCommandLines()
{
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr)
		return;
	auto& backend = *globalRendering->renderBackend;

	UBOLayout seed{};
	seed.viewProj[0] = 1.0f;
	seed.viewProj[5] = 1.0f;
	seed.viewProj[10] = 1.0f;
	seed.viewProj[15] = 1.0f;
	uniformBuffer = backend.CreateBuffer(sizeof(UBOLayout), &seed);
	if (!uniformBuffer || !uniformBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalCommandLines] uniform buffer creation failed");
		return;
	}

	bufferCapacity = 4096; // 2048 line segments before the first growth
	const std::vector<LineVertex> seedVerts(bufferCapacity);
	vertexBuffer = backend.CreateBuffer(bufferCapacity * sizeof(LineVertex), seedVerts.data());
	if (!vertexBuffer || !vertexBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalCommandLines] vertex buffer creation failed");
		return;
	}

	PipelineDesc pd;
	pd.name           = "metal_command_lines";
	pd.vertexSource   = kVertexGlsl;
	pd.fragmentSource = kFragmentGlsl;
	pd.vertexAttributes = {
		VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = offsetof(LineVertex, pos),   .format = VertexFormat::Float3 },
		VertexAttribute{ .location = 1, .bufferSlot = 0, .offset = offsetof(LineVertex, color), .format = VertexFormat::Float4 },
	};
	pd.vertexBindings = {
		VertexBindingLayout{ .slot = 0, .stride = sizeof(LineVertex) },
	};
	// alpha-blended overlay so fight + move from overlapping selections
	// composite cleanly without obliterating the terrain underneath
	pd.blendState.enabled  = true;
	pd.blendState.srcColor = GL_SRC_ALPHA;
	pd.blendState.dstColor = GL_ONE_MINUS_SRC_ALPHA;
	pd.blendState.srcAlpha = GL_ONE;
	pd.blendState.dstAlpha = GL_ONE_MINUS_SRC_ALPHA;

	pipeline = backend.CreatePipeline(pd);
	if (!pipeline || !pipeline->IsValid()) {
		LOG_L(L_ERROR, "[MetalCommandLines] pipeline link failed: %s",
			pipeline ? pipeline->GetLog().c_str() : "(null)");
		return;
	}

	valid = true;
}

MetalCommandLines::~MetalCommandLines() = default;

void MetalCommandLines::Draw()
{
	if (!valid)
		return;

	const CCamera* cam = CCameraHandler::GetActiveCamera();
	if (cam == nullptr)
		return;

	const auto& units = unitHandler.GetActiveUnits();
	if (units.empty())
		return;

	std::vector<LineVertex> verts;
	// Each selected unit contributes (queueLen + 1) points worth of
	// segment endpoints. 64 segments / unit upfront bound covers a
	// long patrol queue without reallocating mid-loop.
	verts.reserve(units.size() * 8);

	for (const CUnit* u : units) {
		if (u == nullptr || !u->isSelected)
			continue;
		// Hide queues for units the player can't actually command.
		// Allyteam covers shared control + spectators-of-friendlies.
		if (u->allyteam != gu->myAllyTeam && !gu->spectating)
			continue;
		if (u->commandAI == nullptr)
			continue;

		const auto& q = u->commandAI->commandQue;
		if (q.empty())
			continue;

		float3 prev = GroundClampedPos(u->pos);
		for (const Command& c : q) {
			float3 target;
			if (!ExtractTargetPos(c, target))
				continue;
			const float3 next = GroundClampedPos(target);
			const CommandStyle s = StyleForCommand(c.GetID());

			verts.push_back(LineVertex{
				{ prev.x, prev.y, prev.z },
				{ s.r, s.g, s.b, s.a },
			});
			verts.push_back(LineVertex{
				{ next.x, next.y, next.z },
				{ s.r, s.g, s.b, s.a },
			});
			prev = next;
		}
	}

	if (verts.empty())
		return;

	auto& backend = *globalRendering->renderBackend;
	if (verts.size() > bufferCapacity) {
		uint32_t newCap = bufferCapacity;
		while (newCap < verts.size())
			newCap *= 2;
		auto bigger = backend.CreateBuffer(newCap * sizeof(LineVertex), verts.data());
		if (bigger && bigger->IsValid()) {
			vertexBuffer = std::move(bigger);
			bufferCapacity = newCap;
		} else {
			// fall back: clamp to existing capacity, last few segments
			// drop this frame
			verts.resize(bufferCapacity);
		}
	} else {
		vertexBuffer->UpdateData(verts.data(), verts.size() * sizeof(LineVertex), 0);
	}

	UBOLayout ubo{};
	std::memcpy(ubo.viewProj, cam->GetViewProjectionMatrix().m, sizeof(ubo.viewProj));
	uniformBuffer->UpdateData(&ubo, sizeof(ubo), 0);

	pipeline->Enable();
	pipeline->BindUniformBuffer(0, *uniformBuffer, 0, sizeof(ubo));
	pipeline->BindVertexBuffer(0, *vertexBuffer);
	pipeline->Draw(PrimitiveTopology::Lines, 0, static_cast<uint32_t>(verts.size()));
	pipeline->Disable();
}

#endif // RENDER_BACKEND_METAL
