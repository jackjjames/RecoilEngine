/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalSelectionMarkers.h"

#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Game/GlobalUnsynced.h"
#include "Map/Ground.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/IBuffer.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/IRenderTarget.h"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitHandler.h"
#include "System/Log/ILog.h"
#include "System/Matrix44f.h"
#include "System/float3.h"
#include "System/float4.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>


namespace {

// Vertex format for world-space selection markers.
struct MarkerVertex {
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


// Selection ring: 32 segment quads forming an annulus on the ground.
// Each segment is a thin radial strip - inner radius slightly inside
// the unit's collision radius, outer radius slightly outside. Six
// verts per segment.
constexpr int kRingSegments = 32;

// Sample the ground height at (x, z) so the ring sits flush on
// terrain even when the unit straddles a slope. CGround::GetHeightReal
// is the synced API and is safe to call from the render thread (it
// reads the heightmap snapshot the renderer already pinned).
float GroundY(float x, float z)
{
	return CGround::GetHeightReal(x, z, false);
}

void EmitSelectionRing(std::vector<MarkerVertex>& out, const CUnit& u)
{
	const float r = std::max(u.radius, 8.0f);
	const float rIn  = r * 0.92f;
	const float rOut = r * 1.04f;

	const float yLift = 1.5f; // floats just above the terrain to dodge z-fighting

	for (int i = 0; i < kRingSegments; ++i) {
		const float a0 = (static_cast<float>(i)     / kRingSegments) * 2.0f * 3.14159265f;
		const float a1 = (static_cast<float>(i + 1) / kRingSegments) * 2.0f * 3.14159265f;

		const float c0 = std::cos(a0), s0 = std::sin(a0);
		const float c1 = std::cos(a1), s1 = std::sin(a1);

		const float x0 = u.pos.x + c0 * rIn;  const float z0 = u.pos.z + s0 * rIn;
		const float x1 = u.pos.x + c1 * rIn;  const float z1 = u.pos.z + s1 * rIn;
		const float x2 = u.pos.x + c1 * rOut; const float z2 = u.pos.z + s1 * rOut;
		const float x3 = u.pos.x + c0 * rOut; const float z3 = u.pos.z + s0 * rOut;

		const float y0 = GroundY(x0, z0) + yLift;
		const float y1 = GroundY(x1, z1) + yLift;
		const float y2 = GroundY(x2, z2) + yLift;
		const float y3 = GroundY(x3, z3) + yLift;

		const float col[4] = { 1.0f, 1.0f, 1.0f, 0.65f };

		MarkerVertex va{ {x0, y0, z0}, {col[0], col[1], col[2], col[3]} };
		MarkerVertex vb{ {x1, y1, z1}, {col[0], col[1], col[2], col[3]} };
		MarkerVertex vc{ {x2, y2, z2}, {col[0], col[1], col[2], col[3]} };
		MarkerVertex vd{ {x3, y3, z3}, {col[0], col[1], col[2], col[3]} };

		out.push_back(va); out.push_back(vb); out.push_back(vc);
		out.push_back(va); out.push_back(vc); out.push_back(vd);
	}
}

} // namespace


MetalSelectionMarkers::MetalSelectionMarkers()
{
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr)
		return;
	auto& backend = *globalRendering->renderBackend;

	// Initial buffer sized for ~256 selected-unit rings. Grows
	// power-of-two when active unit count climbs past it.
	bufferCapacity = 256u * 6u * kRingSegments;
	vertexBuffer = backend.CreateBuffer(bufferCapacity * sizeof(MarkerVertex), nullptr);
	if (!vertexBuffer || !vertexBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalSelectionMarkers] vertex buffer creation failed");
		return;
	}

	UBOLayout seed{};
	seed.viewProj[0]  = 1.0f;
	seed.viewProj[5]  = 1.0f;
	seed.viewProj[10] = 1.0f;
	seed.viewProj[15] = 1.0f;
	uniformBuffer = backend.CreateBuffer(sizeof(UBOLayout), &seed);
	if (!uniformBuffer || !uniformBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalSelectionMarkers] uniform buffer creation failed");
		return;
	}

	PipelineDesc pd;
	pd.name = "selection_markers";
	pd.vertexSource   = kVertexGlsl;
	pd.fragmentSource = kFragmentGlsl;
	pd.vertexAttributes = {
		VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = 0,                     .format = VertexFormat::Float3 },
		VertexAttribute{ .location = 1, .bufferSlot = 0, .offset = sizeof(float) * 3,     .format = VertexFormat::Float4 },
	};
	pd.vertexBindings = { VertexBindingLayout{ .slot = 0, .stride = sizeof(MarkerVertex) } };
	pd.blendState.enabled  = true;
	pd.blendState.srcColor = GL_SRC_ALPHA;
	pd.blendState.dstColor = GL_ONE_MINUS_SRC_ALPHA;
	pd.blendState.srcAlpha = GL_ONE;
	pd.blendState.dstAlpha = GL_ONE_MINUS_SRC_ALPHA;

	pipeline = backend.CreatePipeline(pd);
	if (!pipeline || !pipeline->IsValid()) {
		LOG_L(L_ERROR, "[MetalSelectionMarkers] pipeline link failed: %s",
			pipeline ? pipeline->GetLog().c_str() : "(null)");
		return;
	}

	valid = true;
}

MetalSelectionMarkers::~MetalSelectionMarkers() = default;


void MetalSelectionMarkers::Draw()
{
	if (!valid)
		return;

	const CCamera* cam = CCameraHandler::GetActiveCamera();
	if (cam == nullptr)
		return;

	const auto& units = unitHandler.GetActiveUnits();
	if (units.empty())
		return;

	std::vector<MarkerVertex> verts;
	verts.reserve(units.size() * 6 * kRingSegments);

	for (const CUnit* u : units) {
		if (u == nullptr)
			continue;

		if (u->isSelected)
			EmitSelectionRing(verts, *u);
	}

	if (verts.empty())
		return;

	if (verts.size() > bufferCapacity) {
		uint32_t newCap = bufferCapacity > 0 ? bufferCapacity : 1024u;
		while (newCap < verts.size())
			newCap *= 2u;
		vertexBuffer = globalRendering->renderBackend->CreateBuffer(
			newCap * sizeof(MarkerVertex), nullptr);
		bufferCapacity = newCap;
	}

	UBOLayout ubo{};
	std::memcpy(ubo.viewProj, cam->GetViewProjectionMatrix().m, sizeof(ubo.viewProj));
	uniformBuffer->UpdateData(&ubo, sizeof(ubo), 0);

	vertexBuffer->UpdateData(verts.data(), verts.size() * sizeof(MarkerVertex), 0);

	pipeline->Enable();
	pipeline->BindUniformBuffer(0, *uniformBuffer, 0, sizeof(ubo));
	pipeline->BindVertexBuffer(0, *vertexBuffer);
	pipeline->Draw(PrimitiveTopology::Triangles, 0, static_cast<uint32_t>(verts.size()));
	pipeline->Disable();
}

#endif // RENDER_BACKEND_METAL
