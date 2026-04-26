/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalCraters.h"

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

#include <cmath>
#include <cstring>


namespace {

struct CraterVertex {
	float pos[3];
	float uv [2]; // -1..1 across the crater quad
};

struct alignas(16) UBOLayout {
	float viewProj[16];
};

constexpr int   kGridSegs    = 6;            // per side -> 6x6 verts, 5x5 quads, 50 tris per crater
constexpr int   kVertsPerCrater = kGridSegs * kGridSegs * 6; // triangle list, no index buffer
constexpr int   kMaxCraters  = 1024;
constexpr float kYLift       = 0.6f;         // float just above the ground to dodge z-fighting

// Tessellate one crater into `out`. Vertices are world-space, with
// y sampled from CGround::GetHeightReal at each grid corner so the
// quad conforms to terrain. UV is -1..1 across the disk so the
// fragment shader can do radial falloff via length(uv).
void Tessellate(CraterVertex* out, const float3& center, float radius)
{
	for (int gz = 0; gz < kGridSegs; ++gz) {
		for (int gx = 0; gx < kGridSegs; ++gx) {
			const float u0 = -1.0f + (static_cast<float>(gx)     / kGridSegs) * 2.0f;
			const float u1 = -1.0f + (static_cast<float>(gx + 1) / kGridSegs) * 2.0f;
			const float v0 = -1.0f + (static_cast<float>(gz)     / kGridSegs) * 2.0f;
			const float v1 = -1.0f + (static_cast<float>(gz + 1) / kGridSegs) * 2.0f;

			const float wx0 = center.x + u0 * radius, wz0 = center.z + v0 * radius;
			const float wx1 = center.x + u1 * radius, wz1 = center.z + v0 * radius;
			const float wx2 = center.x + u1 * radius, wz2 = center.z + v1 * radius;
			const float wx3 = center.x + u0 * radius, wz3 = center.z + v1 * radius;

			const float y0 = CGround::GetHeightReal(wx0, wz0, false) + kYLift;
			const float y1 = CGround::GetHeightReal(wx1, wz1, false) + kYLift;
			const float y2 = CGround::GetHeightReal(wx2, wz2, false) + kYLift;
			const float y3 = CGround::GetHeightReal(wx3, wz3, false) + kYLift;

			const CraterVertex v00{ {wx0, y0, wz0}, {u0, v0} };
			const CraterVertex v10{ {wx1, y1, wz1}, {u1, v0} };
			const CraterVertex v11{ {wx2, y2, wz2}, {u1, v1} };
			const CraterVertex v01{ {wx3, y3, wz3}, {u0, v1} };
			*out++ = v00; *out++ = v10; *out++ = v11;
			*out++ = v00; *out++ = v11; *out++ = v01;
		}
	}
}

constexpr const char* kVertexGlsl = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 0) out vec2 vUV;

layout(set = 0, binding = 0) uniform CraterUBO {
    mat4 uViewProj;
} ubo;

void main() {
    vUV = aUV;
    gl_Position = ubo.uViewProj * vec4(aPos, 1.0);
}
)";

// Dark brown splotch with smooth radial falloff. Output rgb is the
// scorch colour and alpha is the falloff mask, so standard alpha
// blending gives "ground darkened toward scorch colour by mask".
constexpr const char* kFragmentGlsl = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

void main() {
    float r = length(vUV);
    if (r > 1.0)
        discard;
    float core = 1.0 - smoothstep(0.0, 0.55, r);
    float ring = 1.0 - smoothstep(0.55, 1.0, r);
    float mask = core * 0.85 + ring * 0.45;

    vec3 scorch = vec3(0.05, 0.04, 0.03);
    fragColor = vec4(scorch, mask);
}
)";

} // namespace


MetalCraters::MetalCraters()
{
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr)
		return;
	auto& backend = *globalRendering->renderBackend;

	vertexBufferCapacity = static_cast<uint32_t>(kMaxCraters * kVertsPerCrater);
	const std::vector<CraterVertex> seed(vertexBufferCapacity);
	vertexBuffer = backend.CreateBuffer(vertexBufferCapacity * sizeof(CraterVertex), seed.data());
	if (!vertexBuffer || !vertexBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalCraters] vertex buffer creation failed");
		return;
	}

	UBOLayout seedUBO{};
	seedUBO.viewProj[0] = 1.0f; seedUBO.viewProj[5] = 1.0f;
	seedUBO.viewProj[10] = 1.0f; seedUBO.viewProj[15] = 1.0f;
	uniformBuffer = backend.CreateBuffer(sizeof(UBOLayout), &seedUBO);
	if (!uniformBuffer || !uniformBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalCraters] uniform buffer creation failed");
		return;
	}

	PipelineDesc pd;
	pd.name           = "metal_craters";
	pd.vertexSource   = kVertexGlsl;
	pd.fragmentSource = kFragmentGlsl;
	pd.vertexAttributes = {
		VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = offsetof(CraterVertex, pos), .format = VertexFormat::Float3 },
		VertexAttribute{ .location = 1, .bufferSlot = 0, .offset = offsetof(CraterVertex, uv),  .format = VertexFormat::Float2 },
	};
	pd.vertexBindings = {
		VertexBindingLayout{ .slot = 0, .stride = sizeof(CraterVertex) },
	};
	pd.blendState.enabled  = true;
	pd.blendState.srcColor = GL_SRC_ALPHA;
	pd.blendState.dstColor = GL_ONE_MINUS_SRC_ALPHA;
	pd.blendState.srcAlpha = GL_ONE;
	pd.blendState.dstAlpha = GL_ONE_MINUS_SRC_ALPHA;

	pipeline = backend.CreatePipeline(pd);
	if (!pipeline || !pipeline->IsValid()) {
		LOG_L(L_ERROR, "[MetalCraters] pipeline link failed: %s",
			pipeline ? pipeline->GetLog().c_str() : "(null)");
		return;
	}

	craters.reserve(64);
	valid = true;
}

MetalCraters::~MetalCraters() = default;

void MetalCraters::Draw()
{
	if (!valid)
		return;

	// Snapshot active units; diff against last frame's snapshot.
	std::unordered_map<int, float3> currUnits;
	{
		const auto& active = unitHandler.GetActiveUnits();
		currUnits.reserve(active.size());
		for (const CUnit* u : active) {
			if (u != nullptr)
				currUnits.emplace(u->id, u->pos);
		}
	}

	auto& backend = *globalRendering->renderBackend;
	for (const auto& kv : prevUnits) {
		if (currUnits.find(kv.first) != currUnits.end())
			continue;

		Crater c;
		c.center = kv.second;
		c.radius = 26.0f;

		// FIFO drop oldest if we'd exceed the cap. Keeping a vector
		// + erase(begin) is O(n) but n is bounded and this only
		// fires after kMaxCraters deaths so it's amortised cheap.
		if (static_cast<int>(craters.size()) >= kMaxCraters)
			craters.erase(craters.begin());
		craters.push_back(c);

		// Tessellate the new crater straight into the buffer slot
		// that matches its index. We rewrite the whole used range
		// each draw call below, so this in-memory cache is what
		// actually matters.
	}
	prevUnits.swap(currUnits);

	if (craters.empty())
		return;

	const CCamera* cam = CCameraHandler::GetActiveCamera();
	if (cam == nullptr)
		return;

	// Tessellate every crater each frame so terrain edits via Lua /
	// gadgets propagate. Keeps memory traffic predictable; the disc
	// grid is tiny.
	std::vector<CraterVertex> verts(craters.size() * kVertsPerCrater);
	for (size_t i = 0; i < craters.size(); ++i)
		Tessellate(verts.data() + i * kVertsPerCrater, craters[i].center, craters[i].radius);

	if (verts.size() > vertexBufferCapacity) {
		uint32_t newCap = vertexBufferCapacity;
		while (newCap < verts.size())
			newCap *= 2;
		auto bigger = backend.CreateBuffer(newCap * sizeof(CraterVertex), verts.data());
		if (bigger && bigger->IsValid()) {
			vertexBuffer = std::move(bigger);
			vertexBufferCapacity = newCap;
		} else {
			verts.resize(vertexBufferCapacity);
		}
	} else {
		vertexBuffer->UpdateData(verts.data(), verts.size() * sizeof(CraterVertex), 0);
	}

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
