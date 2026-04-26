/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalBuildHalo.h"

#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Game/GlobalUnsynced.h"
#include "Map/Ground.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/IBuffer.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitHandler.h"
#include "System/Log/ILog.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>


namespace {

struct HaloVertex {
	float pos[3];
	float color[4];
};

struct alignas(16) UBOLayout {
	float viewProj[16];
};

constexpr int   kRingSegments = 36;
constexpr float kInnerScale   = 0.92f;
constexpr float kOuterScale   = 1.00f;
constexpr float kYLift        = 1.2f;

constexpr const char* kVertexGlsl = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 0) out vec4 vColor;

layout(set = 0, binding = 0) uniform HaloUBO {
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

// Push a thin ring on the XZ plane around `center` with terrain-snapped
// y at each segment so the ring conforms to the ground around the
// unit's footprint.
void PushRing(std::vector<HaloVertex>& out, const float3& center, float radius, const float color[4])
{
	for (int i = 0; i < kRingSegments; ++i) {
		const float a0 = (static_cast<float>(i)     / kRingSegments) * 6.28318530718f;
		const float a1 = (static_cast<float>(i + 1) / kRingSegments) * 6.28318530718f;
		const float c0 = std::cos(a0), s0 = std::sin(a0);
		const float c1 = std::cos(a1), s1 = std::sin(a1);

		const float ix0 = center.x + c0 * radius * kInnerScale;
		const float iz0 = center.z + s0 * radius * kInnerScale;
		const float ox0 = center.x + c0 * radius * kOuterScale;
		const float oz0 = center.z + s0 * radius * kOuterScale;
		const float ix1 = center.x + c1 * radius * kInnerScale;
		const float iz1 = center.z + s1 * radius * kInnerScale;
		const float ox1 = center.x + c1 * radius * kOuterScale;
		const float oz1 = center.z + s1 * radius * kOuterScale;

		const float iy0 = CGround::GetHeightReal(ix0, iz0, false) + kYLift;
		const float oy0 = CGround::GetHeightReal(ox0, oz0, false) + kYLift;
		const float iy1 = CGround::GetHeightReal(ix1, iz1, false) + kYLift;
		const float oy1 = CGround::GetHeightReal(ox1, oz1, false) + kYLift;

		const HaloVertex i0{ {ix0, iy0, iz0}, {color[0], color[1], color[2], color[3]} };
		const HaloVertex o0{ {ox0, oy0, oz0}, {color[0], color[1], color[2], color[3]} };
		const HaloVertex i1{ {ix1, iy1, iz1}, {color[0], color[1], color[2], color[3]} };
		const HaloVertex o1{ {ox1, oy1, oz1}, {color[0], color[1], color[2], color[3]} };
		out.push_back(i0); out.push_back(o0); out.push_back(o1);
		out.push_back(i0); out.push_back(o1); out.push_back(i1);
	}
}

} // namespace


MetalBuildHalo::MetalBuildHalo()
{
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr)
		return;
	auto& backend = *globalRendering->renderBackend;

	bufferCapacity = 64 * kRingSegments * 6; // headroom for ~64 active build sites
	const std::vector<HaloVertex> seed(bufferCapacity);
	vertexBuffer = backend.CreateBuffer(bufferCapacity * sizeof(HaloVertex), seed.data());
	if (!vertexBuffer || !vertexBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalBuildHalo] vertex buffer creation failed");
		return;
	}

	UBOLayout seedUBO{};
	seedUBO.viewProj[0] = 1.0f; seedUBO.viewProj[5] = 1.0f;
	seedUBO.viewProj[10] = 1.0f; seedUBO.viewProj[15] = 1.0f;
	uniformBuffer = backend.CreateBuffer(sizeof(UBOLayout), &seedUBO);
	if (!uniformBuffer || !uniformBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalBuildHalo] uniform buffer creation failed");
		return;
	}

	PipelineDesc pd;
	pd.name           = "metal_build_halo";
	pd.vertexSource   = kVertexGlsl;
	pd.fragmentSource = kFragmentGlsl;
	pd.vertexAttributes = {
		VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = offsetof(HaloVertex, pos),   .format = VertexFormat::Float3 },
		VertexAttribute{ .location = 1, .bufferSlot = 0, .offset = offsetof(HaloVertex, color), .format = VertexFormat::Float4 },
	};
	pd.vertexBindings = {
		VertexBindingLayout{ .slot = 0, .stride = sizeof(HaloVertex) },
	};
	pd.blendState.enabled  = true;
	pd.blendState.srcColor = GL_SRC_ALPHA;
	pd.blendState.dstColor = GL_ONE_MINUS_SRC_ALPHA;
	pd.blendState.srcAlpha = GL_ONE;
	pd.blendState.dstAlpha = GL_ONE_MINUS_SRC_ALPHA;

	pipeline = backend.CreatePipeline(pd);
	if (!pipeline || !pipeline->IsValid()) {
		LOG_L(L_ERROR, "[MetalBuildHalo] pipeline link failed: %s",
			pipeline ? pipeline->GetLog().c_str() : "(null)");
		return;
	}

	valid = true;
}

MetalBuildHalo::~MetalBuildHalo() = default;

void MetalBuildHalo::Draw()
{
	if (!valid || gu == nullptr)
		return;

	const float t     = static_cast<float>(gu->modGameTime);
	const float pulse = 0.55f + 0.35f * std::sin(t * 5.0f);

	std::vector<HaloVertex> verts;
	verts.reserve(64 * kRingSegments * 6);
	for (const CUnit* u : unitHandler.GetActiveUnits()) {
		if (u == nullptr) continue;
		if (!u->beingBuilt) continue;

		// Lerp colour from amber (low progress) to bright green
		// (near completion) so the player can read the build state
		// at a glance even from distance.
		const float p = std::clamp(u->buildProgress, 0.0f, 1.0f);
		float color[4];
		color[0] = 1.0f - p * 0.8f;          // 1.0 -> 0.2 (drop red as we go green)
		color[1] = 0.55f + p * 0.45f;        // 0.55 -> 1.0
		color[2] = 0.10f;                    // stay low so it doesn't read as cyan / blue
		color[3] = pulse;

		const float radius = std::max(8.0f, u->radius * 1.10f);
		PushRing(verts, u->pos, radius, color);
	}

	if (verts.empty())
		return;

	auto& backend = *globalRendering->renderBackend;
	if (verts.size() > bufferCapacity) {
		uint32_t newCap = bufferCapacity;
		while (newCap < verts.size())
			newCap *= 2;
		auto bigger = backend.CreateBuffer(newCap * sizeof(HaloVertex), verts.data());
		if (bigger && bigger->IsValid()) {
			vertexBuffer = std::move(bigger);
			bufferCapacity = newCap;
		} else {
			verts.resize(bufferCapacity);
		}
	} else {
		vertexBuffer->UpdateData(verts.data(), verts.size() * sizeof(HaloVertex), 0);
	}

	const CCamera* cam = CCameraHandler::GetActiveCamera();
	if (cam == nullptr)
		return;

	UBOLayout ubo{};
	std::memcpy(ubo.viewProj, cam->GetViewProjectionMatrix().m, sizeof(ubo.viewProj));
	uniformBuffer->UpdateData(&ubo, sizeof(ubo), 0);

	pipeline->Enable();
	pipeline->BindUniformBuffer(0, *uniformBuffer, 0, sizeof(ubo));
	pipeline->BindVertexBuffer(0, *vertexBuffer);
	pipeline->Draw(PrimitiveTopology::Triangles, 0, static_cast<uint32_t>(verts.size()));
	pipeline->Disable();
}

#endif // RENDER_BACKEND_METAL
