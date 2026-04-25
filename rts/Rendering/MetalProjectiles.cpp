/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalProjectiles.h"

#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/IBuffer.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Projectiles/Projectile.h"
#include "Sim/Projectiles/ProjectileHandler.h"
#include "System/Log/ILog.h"
#include "System/float3.h"
#include "System/Matrix44f.h"

#include <algorithm>
#include <cstring>
#include <vector>


namespace {

// One vertex per quad corner. The vertex shader treats `corner` as
// the offset within the streak's local frame: corner.x runs along
// the velocity (-1 = tail, +1 = head), corner.y runs perpendicular
// to it in screen space.
struct ProjVertex {
	float px, py, pz;       // streak centre in world space
	float dx, dy, dz;       // streak length vector (velocity * dt scaled)
	float cornerX, cornerY; // unit-square corner in streak-local space
	float halfWidth;        // billboard half-width in world units
	float r, g, b, a;
};

struct alignas(16) UBOLayout {
	float viewProj[16];
	float camRight[4];      // xyz, unused
	float camUp[4];
};

constexpr const char* kVertexGlsl = R"(#version 450
layout(location = 0) in vec3 aCenter;
layout(location = 1) in vec3 aLengthVec;
layout(location = 2) in vec2 aCorner;
layout(location = 3) in float aHalfWidth;
layout(location = 4) in vec4 aColor;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vCorner;

layout(set = 0, binding = 0) uniform UBO {
    mat4 uViewProj;
    vec4 uCamRight;
    vec4 uCamUp;
} ubo;

void main() {
    // The "right" axis of the streak follows the velocity vector
    // projected onto screen space, so a fast missile reads as a
    // long bright streak while a slow plasma blob reads as a near-
    // square dot. The "up" axis is the camera-right cross with the
    // streak axis - a stable perpendicular that tracks the view.
    vec3 along  = aLengthVec;
    vec3 viewUp = normalize(ubo.uCamUp.xyz);
    vec3 perp   = normalize(cross(along, viewUp));
    if (length(perp) < 0.001) {
        // Velocity parallel to the view-up axis: fall back to camera-
        // right so the quad still has finite area.
        perp = normalize(ubo.uCamRight.xyz);
    }

    vec3 world = aCenter
               + along * aCorner.x
               + perp  * (aCorner.y * aHalfWidth);
    vColor  = aColor;
    vCorner = aCorner;
    gl_Position = ubo.uViewProj * vec4(world, 1.0);
}
)";

constexpr const char* kFragmentGlsl = R"(#version 450
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vCorner;
layout(location = 0) out vec4 fragColor;

void main() {
    // Soft elongated falloff: x radius = 1 (matches streak length),
    // y radius = 1 (matches half-width). Squared distance with a
    // small bias keeps the core hot and the edges near-zero alpha
    // so additive blend doesn't smear into a uniform glow.
    float d = vCorner.x * vCorner.x + vCorner.y * vCorner.y;
    float core = exp(-3.5 * d);
    float halo = exp(-1.2 * d) * 0.35;
    float a = core + halo;
    if (a <= 0.01)
        discard;
    vec3 hot = mix(vColor.rgb, vec3(1.0), core);
    fragColor = vec4(hot * a, a * vColor.a);
}
)";

// Heuristic streak length scale: BAR projectile speeds are in world-
// units-per-frame, so a "1 frame ahead, 0.5 frames behind" streak
// gives a recognisable tracer without depending on framerate.
constexpr float kStreakLengthScale = 1.5f;
// Minimum streak so non-moving (recently spawned / muzzle flash)
// projectiles are still visible as a small dot rather than zero-area.
constexpr float kMinStreakLength = 4.0f;
constexpr float kBillboardHalfWidth = 1.6f;


void EmitProjectile(std::vector<ProjVertex>& verts,
                    std::vector<uint32_t>&   inds,
                    const CProjectile* p,
                    float r, float g, float b, float a)
{
	if (p == nullptr)
		return;

	const float3 vel = static_cast<float3>(p->speed);
	const float  len = std::max(kMinStreakLength,
	                           p->speed.w * kStreakLengthScale);
	float3 along = vel;
	const float speedMag = p->speed.w;
	if (speedMag > 1e-3f) {
		along *= (len / speedMag);
	} else {
		along = float3(0.0f, 0.0f, len);
	}

	const float3 centre(p->pos.x, p->pos.y, p->pos.z);

	const uint32_t base = static_cast<uint32_t>(verts.size());
	const float halfW = kBillboardHalfWidth;

	verts.push_back(ProjVertex{centre.x, centre.y, centre.z,
		along.x, along.y, along.z, -1.0f, -1.0f, halfW, r, g, b, a});
	verts.push_back(ProjVertex{centre.x, centre.y, centre.z,
		along.x, along.y, along.z,  1.0f, -1.0f, halfW, r, g, b, a});
	verts.push_back(ProjVertex{centre.x, centre.y, centre.z,
		along.x, along.y, along.z,  1.0f,  1.0f, halfW, r, g, b, a});
	verts.push_back(ProjVertex{centre.x, centre.y, centre.z,
		along.x, along.y, along.z, -1.0f,  1.0f, halfW, r, g, b, a});

	inds.push_back(base + 0);
	inds.push_back(base + 1);
	inds.push_back(base + 2);
	inds.push_back(base + 0);
	inds.push_back(base + 2);
	inds.push_back(base + 3);
}

void GetTeamRGB(int team, float& r, float& g, float& b)
{
	r = 1.00f; g = 0.85f; b = 0.45f; // warm tracer fallback
	if (teamHandler.IsValidTeam(team)) {
		const uint8_t* c = teamHandler.Team(team)->color;
		r = c[0] * (1.0f / 255.0f);
		g = c[1] * (1.0f / 255.0f);
		b = c[2] * (1.0f / 255.0f);
	}
}

} // namespace


MetalProjectiles::MetalProjectiles()
{
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr)
		return;
	auto& backend = *globalRendering->renderBackend;

	UBOLayout seed{};
	seed.viewProj[0]  = 1.0f; seed.viewProj[5]  = 1.0f;
	seed.viewProj[10] = 1.0f; seed.viewProj[15] = 1.0f;
	uniformBuffer = backend.CreateBuffer(sizeof(UBOLayout), &seed);
	if (!uniformBuffer || !uniformBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalProjectiles] uniform buffer creation failed");
		return;
	}

	PipelineDesc pd;
	pd.name = "projectile_tracers";
	pd.vertexSource   = kVertexGlsl;
	pd.fragmentSource = kFragmentGlsl;
	pd.vertexAttributes = {
		VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = offsetof(ProjVertex, px),         .format = VertexFormat::Float3 },
		VertexAttribute{ .location = 1, .bufferSlot = 0, .offset = offsetof(ProjVertex, dx),         .format = VertexFormat::Float3 },
		VertexAttribute{ .location = 2, .bufferSlot = 0, .offset = offsetof(ProjVertex, cornerX),    .format = VertexFormat::Float2 },
		VertexAttribute{ .location = 3, .bufferSlot = 0, .offset = offsetof(ProjVertex, halfWidth),  .format = VertexFormat::Float1 },
		VertexAttribute{ .location = 4, .bufferSlot = 0, .offset = offsetof(ProjVertex, r),          .format = VertexFormat::Float4 },
	};
	pd.vertexBindings = {
		VertexBindingLayout{ .slot = 0, .stride = sizeof(ProjVertex) },
	};
	// Additive blend: tracers brighten the underlying scene rather
	// than darkening it. Fragment writes premultiplied colour, so
	// (src * 1) + (dst * 1) reproduces the soft additive glow the
	// GL build uses for the same primitive.
	pd.blendState.enabled  = true;
	pd.blendState.srcColor = GL_ONE;
	pd.blendState.dstColor = GL_ONE;
	pd.blendState.srcAlpha = GL_ONE;
	pd.blendState.dstAlpha = GL_ONE;

	pipeline = backend.CreatePipeline(pd);
	if (!pipeline || !pipeline->IsValid()) {
		LOG_L(L_ERROR, "[MetalProjectiles] pipeline link failed: %s",
			pipeline ? pipeline->GetLog().c_str() : "(null)");
		return;
	}

	valid = true;
}

MetalProjectiles::~MetalProjectiles() = default;

void MetalProjectiles::Draw()
{
	if (!valid)
		return;

	const CCamera* cam = CCameraHandler::GetActiveCamera();
	if (cam == nullptr)
		return;

	std::vector<ProjVertex> verts;
	std::vector<uint32_t>   inds;

	auto emitList = [&](const auto& list) {
		for (CProjectile* p : list) {
			if (p == nullptr || p->deleteMe || p->createMe)
				continue;
			float r, g, b;
			GetTeamRGB(static_cast<int>(p->GetTeamID()), r, g, b);
			// Slight warm tint pull so even a "blue" team's lasers
			// read as energetic projectiles instead of cold dots.
			r = std::min(1.0f, r * 0.85f + 0.25f);
			g = std::min(1.0f, g * 0.85f + 0.20f);
			b = std::min(1.0f, b * 0.85f + 0.10f);
			EmitProjectile(verts, inds, p, r, g, b, 0.85f);
		}
	};

	emitList(projectileHandler.GetActiveProjectiles(true));
	emitList(projectileHandler.GetActiveProjectiles(false));

	if (verts.empty())
		return;

	// Grow buffers in 2x slack so battles with thousands of plasma
	// rounds don't realloc per frame. Each projectile is 4 verts /
	// 6 indices.
	const uint32_t neededProj = static_cast<uint32_t>(verts.size() / 4);
	const size_t   vbSize = verts.size() * sizeof(ProjVertex);
	const size_t   ibSize = inds.size()  * sizeof(uint32_t);
	auto& backend = *globalRendering->renderBackend;
	if (neededProj > bufferCapacity || !vertexBuffer || !indexBuffer) {
		bufferCapacity = std::max<uint32_t>(neededProj * 2, 256);
		vertexBuffer = backend.CreateBuffer(bufferCapacity * 4 * sizeof(ProjVertex), nullptr);
		indexBuffer  = backend.CreateBuffer(bufferCapacity * 6 * sizeof(uint32_t),    nullptr);
		if (!vertexBuffer || !indexBuffer || !vertexBuffer->IsValid() || !indexBuffer->IsValid()) {
			LOG_L(L_ERROR, "[MetalProjectiles] dynamic buffer growth failed");
			valid = false;
			return;
		}
	}
	vertexBuffer->UpdateData(verts.data(), vbSize, 0);
	indexBuffer ->UpdateData(inds.data(),  ibSize, 0);

	UBOLayout ubo{};
	std::memcpy(ubo.viewProj, cam->GetViewProjectionMatrix().m, sizeof(ubo.viewProj));
	const float3& camRight = cam->GetRight();
	const float3& camUp    = cam->GetUp();
	ubo.camRight[0] = camRight.x; ubo.camRight[1] = camRight.y; ubo.camRight[2] = camRight.z;
	ubo.camUp[0]    = camUp.x;    ubo.camUp[1]    = camUp.y;    ubo.camUp[2]    = camUp.z;

	uniformBuffer->UpdateData(&ubo, sizeof(ubo), 0);

	pipeline->Enable();
	pipeline->BindUniformBuffer(0, *uniformBuffer, 0, sizeof(ubo));
	pipeline->BindVertexBuffer(0, *vertexBuffer);
	pipeline->DrawIndexed(PrimitiveTopology::Triangles, static_cast<uint32_t>(inds.size()),
		IndexType::Uint32, *indexBuffer);
	pipeline->Disable();
}

#endif // RENDER_BACKEND_METAL
