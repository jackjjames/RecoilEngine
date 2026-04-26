/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalDeathFX.h"

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


namespace {

struct DeathVertex {
	float pos[3];
	float uv [2];
	float age;        // 0..1 where 1 = end of life
	float baseRadius; // elmo
};

struct alignas(16) UBOLayout {
	float viewProj[16];
	float camRight[4];
	float camUp[4];
};

constexpr float kLifetime  = 1.40f; // seconds
constexpr float kCoreScale = 1.4f;  // peak ring scale relative to base radius
constexpr float kBillboardCorners[6][2] = {
	{-1.0f, -1.0f}, { 1.0f, -1.0f}, { 1.0f,  1.0f},
	{-1.0f, -1.0f}, { 1.0f,  1.0f}, {-1.0f,  1.0f},
};

// Camera-billboarded expanding plume. Each particle ships its raw
// world-space center + corner UV (-1..1 across the quad) + age + base
// radius. Vertex shader reconstructs the billboard via camRight /
// camUp from the UBO; fragment shader paints a hot core fading into
// smoke based on radial distance + age.
constexpr const char* kVertexGlsl = R"(#version 450
layout(location = 0) in vec3  aPos;
layout(location = 1) in vec2  aUV;
layout(location = 2) in float aAge;
layout(location = 3) in float aBaseRadius;

layout(location = 0) out vec2  vUV;
layout(location = 1) out float vAge;

layout(set = 0, binding = 0) uniform DeathUBO {
    mat4 uViewProj;
    vec4 uCamRight;
    vec4 uCamUp;
} ubo;

void main() {
    // Particle radius grows fast, then plateaus, mimicking an
    // expanding fireball.
    float t      = clamp(aAge, 0.0, 1.0);
    float scale  = mix(0.55, 1.4, 1.0 - pow(1.0 - t, 2.0));
    float radius = aBaseRadius * scale;
    vec3 right = ubo.uCamRight.xyz * (aUV.x * radius);
    vec3 up    = ubo.uCamUp.xyz    * (aUV.y * radius);
    vec3 wp    = aPos + right + up;
    vUV    = aUV;
    vAge   = t;
    gl_Position = ubo.uViewProj * vec4(wp, 1.0);
}
)";

constexpr const char* kFragmentGlsl = R"(#version 450
layout(location = 0) in vec2  vUV;
layout(location = 1) in float vAge;
layout(location = 0) out vec4 fragColor;

void main() {
    float r2 = dot(vUV, vUV);
    if (r2 > 1.0)
        discard;
    float r  = sqrt(r2);

    // Hot white-yellow core fading to orange smoke through the
    // particle's life. Core shrinks as age advances so the ring of
    // smoke reads as expanding.
    float coreRange = mix(0.45, 0.05, vAge);
    float core = 1.0 - smoothstep(0.0, coreRange, r);

    vec3 hot   = vec3(1.00, 0.95, 0.75);
    vec3 ember = vec3(1.00, 0.45, 0.10);
    vec3 smoke = vec3(0.30, 0.25, 0.22);

    vec3 col = mix(ember, hot, core);
    col      = mix(col, smoke, vAge);

    // Radial alpha falloff + age-driven dimming so the particle fades
    // out instead of popping; additive-friendly RGB premultiplied so
    // even with alpha blend we read as a glow.
    float rim    = 1.0 - smoothstep(0.6, 1.0, r);
    float alpha  = rim * (1.0 - vAge);
    fragColor = vec4(col * alpha, alpha);
}
)";

} // namespace


MetalDeathFX::MetalDeathFX()
{
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr)
		return;
	auto& backend = *globalRendering->renderBackend;

	bufferCapacity = 256; // 42 particles * 6 verts; grows on demand
	const std::vector<DeathVertex> seed(bufferCapacity);
	vertexBuffer = backend.CreateBuffer(bufferCapacity * sizeof(DeathVertex), seed.data());
	if (!vertexBuffer || !vertexBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalDeathFX] vertex buffer creation failed");
		return;
	}

	UBOLayout seedUBO{};
	seedUBO.viewProj[0]  = 1.0f; seedUBO.viewProj[5]  = 1.0f;
	seedUBO.viewProj[10] = 1.0f; seedUBO.viewProj[15] = 1.0f;
	seedUBO.camRight[0]  = 1.0f;
	seedUBO.camUp[1]     = 1.0f;
	uniformBuffer = backend.CreateBuffer(sizeof(UBOLayout), &seedUBO);
	if (!uniformBuffer || !uniformBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalDeathFX] uniform buffer creation failed");
		return;
	}

	PipelineDesc pd;
	pd.name           = "metal_death_fx";
	pd.vertexSource   = kVertexGlsl;
	pd.fragmentSource = kFragmentGlsl;
	pd.vertexAttributes = {
		VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = offsetof(DeathVertex, pos),        .format = VertexFormat::Float3 },
		VertexAttribute{ .location = 1, .bufferSlot = 0, .offset = offsetof(DeathVertex, uv),         .format = VertexFormat::Float2 },
		VertexAttribute{ .location = 2, .bufferSlot = 0, .offset = offsetof(DeathVertex, age),        .format = VertexFormat::Float1 },
		VertexAttribute{ .location = 3, .bufferSlot = 0, .offset = offsetof(DeathVertex, baseRadius), .format = VertexFormat::Float1 },
	};
	pd.vertexBindings = {
		VertexBindingLayout{ .slot = 0, .stride = sizeof(DeathVertex) },
	};
	// Premultiplied additive-leaning blend so cores brighten the
	// world without crushing the alpha of layers below.
	pd.blendState.enabled  = true;
	pd.blendState.srcColor = GL_ONE;
	pd.blendState.dstColor = GL_ONE_MINUS_SRC_ALPHA;
	pd.blendState.srcAlpha = GL_ONE;
	pd.blendState.dstAlpha = GL_ONE_MINUS_SRC_ALPHA;

	pipeline = backend.CreatePipeline(pd);
	if (!pipeline || !pipeline->IsValid()) {
		LOG_L(L_ERROR, "[MetalDeathFX] pipeline link failed: %s",
			pipeline ? pipeline->GetLog().c_str() : "(null)");
		return;
	}

	valid = true;
}

MetalDeathFX::~MetalDeathFX() = default;

void MetalDeathFX::Draw()
{
	if (!valid || gu == nullptr)
		return;

	const float now = static_cast<float>(gu->modGameTime);

	// Snapshot active units this frame and diff against last frame's
	// snapshot to detect deaths. Using ID -> last-known-pos lets the
	// explosion appear at where the unit *was*, not at the origin.
	std::unordered_map<int, float3> currUnits;
	{
		const auto& active = unitHandler.GetActiveUnits();
		currUnits.reserve(active.size());
		for (const CUnit* u : active) {
			if (u == nullptr)
				continue;
			currUnits.emplace(u->id, u->pos);
		}
	}

	for (const auto& kv : prevUnits) {
		if (currUnits.find(kv.first) != currUnits.end())
			continue;
		// Pin the explosion to the terrain so deaths on slopes don't
		// look like they're floating mid-air.
		float3 p = kv.second;
		p.y = std::max(p.y, CGround::GetHeightReal(p.x, p.z, false)) + 8.0f;

		Particle pt;
		pt.pos        = p;
		pt.birthTime  = now;
		pt.baseRadius = 28.0f; // chunky enough to read at the default zoom
		particles.push_back(pt);
	}
	prevUnits.swap(currUnits);

	// Age + compact in place. erase-remove with a lifetime predicate
	// is simplest and cheap at typical death counts.
	particles.erase(
		std::remove_if(particles.begin(), particles.end(),
			[now](const Particle& p) { return (now - p.birthTime) >= kLifetime; }),
		particles.end());

	if (particles.empty())
		return;

	const CCamera* cam = CCameraHandler::GetActiveCamera();
	if (cam == nullptr)
		return;

	const float3 camRight = cam->GetRight();
	const float3 camUp    = cam->GetUp();

	std::vector<DeathVertex> verts;
	verts.reserve(particles.size() * 6);
	for (const Particle& p : particles) {
		const float age = std::clamp((now - p.birthTime) / kLifetime, 0.0f, 1.0f);
		for (int i = 0; i < 6; ++i) {
			DeathVertex v;
			v.pos[0] = p.pos.x;
			v.pos[1] = p.pos.y;
			v.pos[2] = p.pos.z;
			v.uv[0]  = kBillboardCorners[i][0];
			v.uv[1]  = kBillboardCorners[i][1];
			v.age    = age;
			v.baseRadius = p.baseRadius;
			verts.push_back(v);
		}
	}

	auto& backend = *globalRendering->renderBackend;
	if (verts.size() > bufferCapacity) {
		uint32_t newCap = bufferCapacity;
		while (newCap < verts.size())
			newCap *= 2;
		auto bigger = backend.CreateBuffer(newCap * sizeof(DeathVertex), verts.data());
		if (bigger && bigger->IsValid()) {
			vertexBuffer = std::move(bigger);
			bufferCapacity = newCap;
		} else {
			verts.resize(bufferCapacity);
		}
	} else {
		vertexBuffer->UpdateData(verts.data(), verts.size() * sizeof(DeathVertex), 0);
	}

	UBOLayout ubo{};
	std::memcpy(ubo.viewProj, cam->GetViewProjectionMatrix().m, sizeof(ubo.viewProj));
	ubo.camRight[0] = camRight.x; ubo.camRight[1] = camRight.y; ubo.camRight[2] = camRight.z;
	ubo.camUp[0]    = camUp.x;    ubo.camUp[1]    = camUp.y;    ubo.camUp[2]    = camUp.z;
	uniformBuffer->UpdateData(&ubo, sizeof(ubo), 0);

	(void)kCoreScale; // documented constant kept for shader review

	pipeline->Enable();
	pipeline->BindUniformBuffer(0, *uniformBuffer, 0, sizeof(ubo));
	pipeline->BindVertexBuffer(0, *vertexBuffer);
	pipeline->Draw(PrimitiveTopology::Triangles, 0, static_cast<uint32_t>(verts.size()));
	pipeline->Disable();
}

#endif // RENDER_BACKEND_METAL
