/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalUnitMarkers.h"

#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/IBuffer.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureHandler.h"
#include "Sim/Misc/Team.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitHandler.h"
#include "System/Log/ILog.h"

#include <algorithm>
#include <cstring>
#include <vector>


namespace {

// Each unit is a camera-facing billboard quad (6 verts, 2 triangles).
// Per-vertex: center world pos + per-corner offset in [-1, 1] + color.
// Vertex shader combines them with the camera basis in-stage.
struct MarkerVertex {
	float cx, cy, cz;    // unit world-space center
	float ox, oy;        // corner offset in quad-local space (-1..1)
	float radius;        // padding / scale, lets VS apply radius per-vertex
	float r, g, b, a;    // team colour (0..1)
};

constexpr size_t kVertsPerUnit = 6;
constexpr size_t kInitialUnitCapacity = 1024;

// Keep the cap low enough to not stall large games; BAR late-game can
// push ~4-8k units per side. Real CUnitDrawer streams through SSBOs /
// indirect; this array is just the visualisation fallback.
constexpr size_t kMaxUnits = 16384;


struct alignas(16) UBOLayout {
	float viewProj[16];
	float cameraRight[4];
	float cameraUp[4];
};


constexpr const char* kVertexGlsl = R"(#version 450
layout(location = 0) in vec3 aCenter;
layout(location = 1) in vec3 aCornerAndRadius;  // xy = corner (-1..1), z = radius
layout(location = 2) in vec4 aColor;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vLocal;

layout(set = 0, binding = 0) uniform UBO {
    mat4 uViewProj;
    vec4 uCameraRight;
    vec4 uCameraUp;
} ubo;

void main() {
    vec3 right = ubo.uCameraRight.xyz;
    vec3 up    = ubo.uCameraUp.xyz;
    float r    = aCornerAndRadius.z;
    vec3 world = aCenter + (right * aCornerAndRadius.x + up * aCornerAndRadius.y) * r;

    vColor = aColor;
    vLocal = aCornerAndRadius.xy;
    gl_Position = ubo.uViewProj * vec4(world, 1.0);
}
)";

constexpr const char* kFragmentGlsl = R"(#version 450
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vLocal;
layout(location = 0) out vec4 fragColor;

void main() {
    // Soft round marker: fall off toward the quad's corners so the
    // unit reads as a disc rather than a square tile. Discard well
    // outside the disc so adjacent markers don't merge visually.
    float d = length(vLocal);
    if (d > 1.05) discard;

    float rim  = smoothstep(1.00, 0.85, d);    // soft edge
    float core = smoothstep(0.55, 0.00, d);    // hot center

    vec3 col = mix(vColor.rgb * 0.55, vColor.rgb, core);
    fragColor = vec4(col, rim * vColor.a);
}
)";


// Build the six vertices for one unit's billboard quad. Corner offsets
// in XY are the local quad space (-1..1); the VS offsets by corners *
// radius * camera basis. CCW when viewed from +Z, matching the world
// drawer so face-culling settings (if added later) line up.
void EmitUnit(std::vector<MarkerVertex>& out, const float3& pos, float radius, uint8_t r, uint8_t g, uint8_t b)
{
	const float rf = static_cast<float>(r) / 255.0f;
	const float gf = static_cast<float>(g) / 255.0f;
	const float bf = static_cast<float>(b) / 255.0f;
	const float af = 0.85f;
	const MarkerVertex base[6] = {
		{ pos.x, pos.y, pos.z, -1.0f, -1.0f, radius, rf, gf, bf, af },
		{ pos.x, pos.y, pos.z,  1.0f, -1.0f, radius, rf, gf, bf, af },
		{ pos.x, pos.y, pos.z, -1.0f,  1.0f, radius, rf, gf, bf, af },
		{ pos.x, pos.y, pos.z, -1.0f,  1.0f, radius, rf, gf, bf, af },
		{ pos.x, pos.y, pos.z,  1.0f, -1.0f, radius, rf, gf, bf, af },
		{ pos.x, pos.y, pos.z,  1.0f,  1.0f, radius, rf, gf, bf, af },
	};
	out.insert(out.end(), std::begin(base), std::end(base));
}

} // namespace


MetalUnitMarkers::MetalUnitMarkers()
{
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr)
		return;
	auto& backend = *globalRendering->renderBackend;

	// --- Vertex buffer: pre-allocate at kInitialUnitCapacity, grown on
	// demand inside Draw() when the active-units count spikes over cap.
	vertexBufferCapacity = kInitialUnitCapacity * kVertsPerUnit * sizeof(MarkerVertex);
	vertexBuffer = backend.CreateBuffer(vertexBufferCapacity, nullptr);
	if (!vertexBuffer || !vertexBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalUnitMarkers] vertex buffer creation failed");
		return;
	}

	UBOLayout seed{};
	seed.viewProj[0]  = 1.0f;
	seed.viewProj[5]  = 1.0f;
	seed.viewProj[10] = 1.0f;
	seed.viewProj[15] = 1.0f;
	seed.cameraRight[0] = 1.0f;
	seed.cameraUp[1]    = 1.0f;
	uniformBuffer = backend.CreateBuffer(sizeof(UBOLayout), &seed);
	if (!uniformBuffer || !uniformBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalUnitMarkers] uniform buffer creation failed");
		return;
	}

	PipelineDesc pd;
	pd.name = "unit_markers";
	pd.vertexSource   = kVertexGlsl;
	pd.fragmentSource = kFragmentGlsl;
	pd.vertexAttributes = {
		VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = offsetof(MarkerVertex, cx), .format = VertexFormat::Float3 },
		VertexAttribute{ .location = 1, .bufferSlot = 0, .offset = offsetof(MarkerVertex, ox), .format = VertexFormat::Float3 },
		VertexAttribute{ .location = 2, .bufferSlot = 0, .offset = offsetof(MarkerVertex, r),  .format = VertexFormat::Float4 },
	};
	pd.vertexBindings = {
		VertexBindingLayout{ .slot = 0, .stride = sizeof(MarkerVertex) },
	};

	pipeline = backend.CreatePipeline(pd);
	if (!pipeline || !pipeline->IsValid()) {
		LOG_L(L_ERROR, "[MetalUnitMarkers] pipeline link failed: %s",
			pipeline ? pipeline->GetLog().c_str() : "(null)");
		return;
	}

	valid = true;
}

MetalUnitMarkers::~MetalUnitMarkers() = default;

void MetalUnitMarkers::Draw()
{
	if (!valid)
		return;

	const auto& active = unitHandler.GetActiveUnits();
	if (active.empty())
		return;

	// --- Build per-frame vertex stream. Units with no radius (defaults
	// to SolidObject::radius which is set post-construction) still get
	// a sensible fallback so commanders + rez-pending units don't
	// disappear.
	std::vector<MarkerVertex> verts;
	const size_t cap = std::min<size_t>(active.size(), kMaxUnits);
	verts.reserve(cap * kVertsPerUnit);

	for (size_t i = 0; i < cap; ++i) {
		const CUnit* u = active[i];
		if (u == nullptr)
			continue;
		const float3& p = u->pos;
		const float r   = (u->radius > 0.0f) ? u->radius : 24.0f;

		uint8_t rC = 200, gC = 200, bC = 200;
		if (teamHandler.IsValidTeam(u->team)) {
			const auto* t = teamHandler.Team(u->team);
			if (t != nullptr) {
				rC = t->color[0];
				gC = t->color[1];
				bC = t->color[2];
			}
		}
		EmitUnit(verts, p, r, rC, gC, bC);
	}

	// Features (trees / rocks / wrecks). Draw these after units so a
	// unit parked on top of a wreck stays legible. Use a desaturated
	// grey-brown so they read as static scenery versus the bright
	// team-coloured unit markers.
	const auto& featureIDs = featureHandler.GetActiveFeatureIDs();
	size_t featRemaining = (verts.size() / kVertsPerUnit >= kMaxUnits)
		? 0
		: (kMaxUnits - verts.size() / kVertsPerUnit);
	size_t featEmitted = 0;
	for (int id : featureIDs) {
		if (featEmitted >= featRemaining)
			break;
		const CFeature* f = featureHandler.GetFeature(id);
		if (f == nullptr)
			continue;
		const float3& p = f->pos;
		const float r   = (f->radius > 0.0f) ? f->radius * 0.6f : 12.0f;
		EmitUnit(verts, p, r, 150, 130, 100);
		++featEmitted;
	}

	if (verts.empty())
		return;

	// Grow the GPU buffer if the active-units count outgrew the pre-alloc.
	const size_t needBytes = verts.size() * sizeof(MarkerVertex);
	if (needBytes > vertexBufferCapacity) {
		auto& backend = *globalRendering->renderBackend;
		vertexBufferCapacity = needBytes * 2; // amortise
		vertexBuffer = backend.CreateBuffer(vertexBufferCapacity, nullptr);
		if (!vertexBuffer || !vertexBuffer->IsValid()) {
			LOG_L(L_ERROR, "[MetalUnitMarkers] vertex buffer regrow failed");
			valid = false;
			return;
		}
	}
	vertexBuffer->UpdateData(verts.data(), needBytes, 0);

	// --- Camera basis for the billboard expansion in the vertex shader.
	UBOLayout ubo{};
	const CCamera* cam = CCameraHandler::GetActiveCamera();
	if (cam != nullptr) {
		std::memcpy(ubo.viewProj, cam->GetViewProjectionMatrix().m, sizeof(ubo.viewProj));
		// Camera exposes right / up as public members so we read them
		// straight. Billboards align to this plane so they always face
		// the eye regardless of camera tilt or rotation.
		ubo.cameraRight[0] = cam->right.x; ubo.cameraRight[1] = cam->right.y; ubo.cameraRight[2] = cam->right.z;
		ubo.cameraUp[0]    = cam->up.x;    ubo.cameraUp[1]    = cam->up.y;    ubo.cameraUp[2]    = cam->up.z;
	} else {
		ubo.viewProj[0]  = 1.0f; ubo.viewProj[5]  = 1.0f;
		ubo.viewProj[10] = 1.0f; ubo.viewProj[15] = 1.0f;
		ubo.cameraRight[0] = 1.0f;
		ubo.cameraUp[1]    = 1.0f;
	}
	uniformBuffer->UpdateData(&ubo, sizeof(ubo), 0);

	pipeline->Enable();
	pipeline->BindUniformBuffer(0, *uniformBuffer, 0, sizeof(ubo));
	pipeline->BindVertexBuffer(0, *vertexBuffer);
	pipeline->Draw(PrimitiveTopology::Triangles, 0, static_cast<uint32_t>(verts.size()));
	pipeline->Disable();
}

#endif // RENDER_BACKEND_METAL
