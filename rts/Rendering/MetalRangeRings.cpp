/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalRangeRings.h"

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
#include "Sim/Weapons/Weapon.h"
#include "System/Log/ILog.h"
#include "System/float3.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>


namespace {

struct RingVertex {
	float pos[3];
	float color[4];
};

struct alignas(16) UBOLayout {
	float viewProj[16];
};

constexpr float kInnerScale = 0.985f; // ring is intentionally thin so big arty
constexpr float kOuterScale = 1.000f; // circles don't smother the terrain
constexpr float kYLift      = 1.4f;

// Adaptive segment count: rings scale with radius so commanders'
// tiny rings stay smooth and arty's huge rings stay smooth too,
// without paying for many segments on the small case.
int SegmentCount(float radius)
{
	const float perElmo = radius / 12.0f;
	int n = static_cast<int>(std::clamp(perElmo, 48.0f, 192.0f));
	// Round up to even so the ring closes cleanly.
	if (n & 1) ++n;
	return n;
}

// Push a thin ground-conforming ring at `center` with `radius`,
// colour `color`. Y is sampled per-vertex from CGround so big rings
// follow terrain elevation along their circumference.
void PushRing(std::vector<RingVertex>& out,
              const float3& center, float radius,
              const float color[4])
{
	const int segs = SegmentCount(radius);
	for (int i = 0; i < segs; ++i) {
		const float a0 = (static_cast<float>(i)     / segs) * 6.28318530718f;
		const float a1 = (static_cast<float>(i + 1) / segs) * 6.28318530718f;
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

		const RingVertex i0{ {ix0, iy0, iz0}, {color[0], color[1], color[2], color[3]} };
		const RingVertex o0{ {ox0, oy0, oz0}, {color[0], color[1], color[2], color[3]} };
		const RingVertex i1{ {ix1, iy1, iz1}, {color[0], color[1], color[2], color[3]} };
		const RingVertex o1{ {ox1, oy1, oz1}, {color[0], color[1], color[2], color[3]} };
		out.push_back(i0); out.push_back(o0); out.push_back(o1);
		out.push_back(i0); out.push_back(o1); out.push_back(i1);
	}
}

constexpr const char* kVertexGlsl = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 0) out vec4 vColor;

layout(set = 0, binding = 0) uniform RingUBO {
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


// Pick the longest-range weapon (skipping zero-range ones, which are
// usually utility scripts or shield emitters that wouldn't make
// sense as a "firing circle").
float PrimaryRange(const CUnit* u)
{
	float best = 0.0f;
	for (const CWeapon* w : u->weapons) {
		if (w == nullptr) continue;
		if (w->range <= 1.0f) continue; // skip zero / dummy
		if (w->range > best)
			best = w->range;
	}
	return best;
}

} // namespace


MetalRangeRings::MetalRangeRings()
{
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr)
		return;
	auto& backend = *globalRendering->renderBackend;

	// Headroom for ~8 selected units * 96 segments * 6 = 4608 verts.
	bufferCapacity = 8192;
	const std::vector<RingVertex> seed(bufferCapacity);
	vertexBuffer = backend.CreateBuffer(bufferCapacity * sizeof(RingVertex), seed.data());
	if (!vertexBuffer || !vertexBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalRangeRings] vertex buffer creation failed");
		return;
	}

	UBOLayout seedUBO{};
	seedUBO.viewProj[0] = 1.0f; seedUBO.viewProj[5] = 1.0f;
	seedUBO.viewProj[10] = 1.0f; seedUBO.viewProj[15] = 1.0f;
	uniformBuffer = backend.CreateBuffer(sizeof(UBOLayout), &seedUBO);
	if (!uniformBuffer || !uniformBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalRangeRings] uniform buffer creation failed");
		return;
	}

	PipelineDesc pd;
	pd.name           = "metal_range_rings";
	pd.vertexSource   = kVertexGlsl;
	pd.fragmentSource = kFragmentGlsl;
	pd.vertexAttributes = {
		VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = offsetof(RingVertex, pos),   .format = VertexFormat::Float3 },
		VertexAttribute{ .location = 1, .bufferSlot = 0, .offset = offsetof(RingVertex, color), .format = VertexFormat::Float4 },
	};
	pd.vertexBindings = {
		VertexBindingLayout{ .slot = 0, .stride = sizeof(RingVertex) },
	};
	pd.blendState.enabled  = true;
	pd.blendState.srcColor = GL_SRC_ALPHA;
	pd.blendState.dstColor = GL_ONE_MINUS_SRC_ALPHA;
	pd.blendState.srcAlpha = GL_ONE;
	pd.blendState.dstAlpha = GL_ONE_MINUS_SRC_ALPHA;

	pipeline = backend.CreatePipeline(pd);
	if (!pipeline || !pipeline->IsValid()) {
		LOG_L(L_ERROR, "[MetalRangeRings] pipeline link failed: %s",
			pipeline ? pipeline->GetLog().c_str() : "(null)");
		return;
	}

	valid = true;
}

MetalRangeRings::~MetalRangeRings() = default;

void MetalRangeRings::Draw()
{
	if (!valid || gu == nullptr)
		return;

	std::vector<RingVertex> verts;
	verts.reserve(8 * 96 * 6);

	// Soft pulse so the ring reads against bright snow / sand without
	// strobing - lower amplitude than the build halo since the player
	// already has a strong selection ring at the unit's foot.
	const float t     = static_cast<float>(gu->modGameTime);
	const float pulse = 0.55f + 0.10f * std::sin(t * 3.0f);

	const float color[4] = { 1.00f, 0.45f, 0.30f, pulse };

	for (const CUnit* u : unitHandler.GetActiveUnits()) {
		if (u == nullptr) continue;
		if (!u->isSelected) continue;
		const float range = PrimaryRange(u);
		if (range <= 1.0f) continue;
		PushRing(verts, u->pos, range, color);
	}

	if (verts.empty())
		return;

	auto& backend = *globalRendering->renderBackend;
	if (verts.size() > bufferCapacity) {
		uint32_t newCap = bufferCapacity;
		while (newCap < verts.size())
			newCap *= 2;
		auto bigger = backend.CreateBuffer(newCap * sizeof(RingVertex), verts.data());
		if (bigger && bigger->IsValid()) {
			vertexBuffer = std::move(bigger);
			bufferCapacity = newCap;
		} else {
			verts.resize(bufferCapacity);
		}
	} else {
		vertexBuffer->UpdateData(verts.data(), verts.size() * sizeof(RingVertex), 0);
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
