/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalNanoBeams.h"

#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Game/GlobalUnsynced.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/IBuffer.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "Sim/Features/Feature.h"
#include "Sim/Objects/SolidObject.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Units/UnitTypes/Builder.h"
#include "System/Log/ILog.h"
#include "System/float3.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>


namespace {

// Beam vertex carries world position, RGBA colour, and a `seg` 0..1
// parameter that the fragment shader uses for scrolling-dash alpha
// masking. Two verts per beam (start/end), drawn as a line list.
struct BeamVertex {
	float pos[3];
	float color[4];
	float seg;
};

struct alignas(16) UBOLayout {
	float viewProj[16];
	float uTime[4]; // .x = scroll phase
};

constexpr const char* kVertexGlsl = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in float aSeg;

layout(location = 0) out vec4  vColor;
layout(location = 1) out float vSeg;

layout(set = 0, binding = 0) uniform NanoUBO {
    mat4 uViewProj;
    vec4 uTime;
} ubo;

void main() {
    vColor = aColor;
    vSeg   = aSeg;
    gl_Position = ubo.uViewProj * vec4(aPos, 1.0);
}
)";

constexpr const char* kFragmentGlsl = R"(#version 450
layout(location = 0) in vec4  vColor;
layout(location = 1) in float vSeg;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform NanoUBO {
    mat4 uViewProj;
    vec4 uTime;
} ubo;

void main() {
    // Dashed beam: phase the segment coordinate by uTime.x so dashes
    // appear to scroll from constructor toward target. 4 dashes per
    // beam keeps the visual rhythm legible at common zoom levels.
    float phase = fract(vSeg * 4.0 + ubo.uTime.x);
    float dash  = step(0.45, phase);

    // Soften the leading / trailing edge of each dash so the beam
    // doesn't strobe.
    float edge  = smoothstep(0.45, 0.55, phase) * smoothstep(1.00, 0.85, phase);
    float alpha = vColor.a * (dash * 0.85 + edge * 0.15);
    fragColor = vec4(vColor.rgb * alpha, alpha);
}
)";


struct BeamColor { float r, g, b; };
// Build / repair / reclaim / resurrect colours roughly match the GL
// builder beam conventions BAR players are used to.
constexpr BeamColor kColBuild     { 0.30f, 1.00f, 0.40f }; // green
constexpr BeamColor kColRepair    { 0.30f, 0.80f, 1.00f }; // cyan
constexpr BeamColor kColReclaim   { 0.50f, 0.65f, 1.00f }; // soft blue
constexpr BeamColor kColResurrect { 0.85f, 0.55f, 1.00f }; // violet

void PushBeam(std::vector<BeamVertex>& verts,
              const float3& a, const float3& b,
              const BeamColor& c, float alpha)
{
	BeamVertex v0;
	v0.pos[0] = a.x; v0.pos[1] = a.y; v0.pos[2] = a.z;
	v0.color[0] = c.r; v0.color[1] = c.g; v0.color[2] = c.b; v0.color[3] = alpha;
	v0.seg = 0.0f;

	BeamVertex v1;
	v1.pos[0] = b.x; v1.pos[1] = b.y; v1.pos[2] = b.z;
	v1.color[0] = c.r; v1.color[1] = c.g; v1.color[2] = c.b; v1.color[3] = alpha;
	v1.seg = 1.0f;

	verts.push_back(v0);
	verts.push_back(v1);
}

} // namespace


MetalNanoBeams::MetalNanoBeams()
{
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr)
		return;
	auto& backend = *globalRendering->renderBackend;

	bufferCapacity = 256; // 128 beams headroom; grows on demand
	const std::vector<BeamVertex> seed(bufferCapacity);
	vertexBuffer = backend.CreateBuffer(bufferCapacity * sizeof(BeamVertex), seed.data());
	if (!vertexBuffer || !vertexBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalNanoBeams] vertex buffer creation failed");
		return;
	}

	UBOLayout seedUBO{};
	seedUBO.viewProj[0] = 1.0f; seedUBO.viewProj[5] = 1.0f;
	seedUBO.viewProj[10] = 1.0f; seedUBO.viewProj[15] = 1.0f;
	uniformBuffer = backend.CreateBuffer(sizeof(UBOLayout), &seedUBO);
	if (!uniformBuffer || !uniformBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalNanoBeams] uniform buffer creation failed");
		return;
	}

	PipelineDesc pd;
	pd.name           = "metal_nano_beams";
	pd.vertexSource   = kVertexGlsl;
	pd.fragmentSource = kFragmentGlsl;
	pd.vertexAttributes = {
		VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = offsetof(BeamVertex, pos),   .format = VertexFormat::Float3 },
		VertexAttribute{ .location = 1, .bufferSlot = 0, .offset = offsetof(BeamVertex, color), .format = VertexFormat::Float4 },
		VertexAttribute{ .location = 2, .bufferSlot = 0, .offset = offsetof(BeamVertex, seg),   .format = VertexFormat::Float1 },
	};
	pd.vertexBindings = {
		VertexBindingLayout{ .slot = 0, .stride = sizeof(BeamVertex) },
	};
	// Premultiplied additive-leaning blend: the fragment outputs
	// rgb * alpha so cores brighten the world cleanly without
	// double-counting alpha at depth.
	pd.blendState.enabled  = true;
	pd.blendState.srcColor = GL_ONE;
	pd.blendState.dstColor = GL_ONE_MINUS_SRC_ALPHA;
	pd.blendState.srcAlpha = GL_ONE;
	pd.blendState.dstAlpha = GL_ONE_MINUS_SRC_ALPHA;

	pipeline = backend.CreatePipeline(pd);
	if (!pipeline || !pipeline->IsValid()) {
		LOG_L(L_ERROR, "[MetalNanoBeams] pipeline link failed: %s",
			pipeline ? pipeline->GetLog().c_str() : "(null)");
		return;
	}

	valid = true;
}

MetalNanoBeams::~MetalNanoBeams() = default;

void MetalNanoBeams::Draw()
{
	if (!valid || gu == nullptr)
		return;

	std::vector<BeamVertex> verts;
	verts.reserve(64 * 2);

	// Lift the beam start point above the constructor's base so the
	// beam doesn't disappear into the ground; ditto for the target
	// end so it bites the build target's body rather than its feet.
	const float startLift = 8.0f;
	const float endLift   = 4.0f;

	for (const CUnit* u : unitHandler.GetActiveUnits()) {
		if (u == nullptr) continue;
		if (u->beingBuilt) continue;

		const CBuilder* builder = dynamic_cast<const CBuilder*>(u);
		if (builder == nullptr) continue;

		const float3 from = builder->pos + float3(0.0f, startLift + builder->radius * 0.4f, 0.0f);

		// Build / repair: same field, distinguished by whether the
		// target is still beingBuilt. Colour swap reads to the
		// player as different actions without having to peek at
		// progress numbers.
		if (builder->curBuild != nullptr) {
			const float3 to = builder->curBuild->pos + float3(0.0f, endLift, 0.0f);
			const BeamColor& c = builder->curBuild->beingBuilt ? kColBuild : kColRepair;
			PushBeam(verts, from, to, c, 0.85f);
			continue;
		}

		if (builder->curReclaim != nullptr) {
			const float3 to = builder->curReclaim->pos + float3(0.0f, endLift, 0.0f);
			PushBeam(verts, from, to, kColReclaim, 0.80f);
			continue;
		}

		if (builder->curResurrect != nullptr) {
			const float3 to = builder->curResurrect->pos + float3(0.0f, endLift, 0.0f);
			PushBeam(verts, from, to, kColResurrect, 0.85f);
			continue;
		}
	}

	if (verts.empty())
		return;

	auto& backend = *globalRendering->renderBackend;
	if (verts.size() > bufferCapacity) {
		uint32_t newCap = bufferCapacity;
		while (newCap < verts.size())
			newCap *= 2;
		auto bigger = backend.CreateBuffer(newCap * sizeof(BeamVertex), verts.data());
		if (bigger && bigger->IsValid()) {
			vertexBuffer = std::move(bigger);
			bufferCapacity = newCap;
		} else {
			verts.resize(bufferCapacity);
		}
	} else {
		vertexBuffer->UpdateData(verts.data(), verts.size() * sizeof(BeamVertex), 0);
	}

	const CCamera* cam = CCameraHandler::GetActiveCamera();
	if (cam == nullptr)
		return;

	UBOLayout ubo{};
	std::memcpy(ubo.viewProj, cam->GetViewProjectionMatrix().m, sizeof(ubo.viewProj));
	ubo.uTime[0] = std::fmod(static_cast<float>(gu->modGameTime) * 1.5f, 1.0f);
	uniformBuffer->UpdateData(&ubo, sizeof(ubo), 0);

	pipeline->Enable();
	pipeline->BindUniformBuffer(0, *uniformBuffer, 0, sizeof(ubo));
	pipeline->BindVertexBuffer(0, *vertexBuffer);
	pipeline->Draw(PrimitiveTopology::Lines, 0, static_cast<uint32_t>(verts.size()));
	pipeline->Disable();
}

#endif // RENDER_BACKEND_METAL
