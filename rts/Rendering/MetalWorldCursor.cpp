/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalWorldCursor.h"

#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Game/GlobalUnsynced.h"
#include "Game/UI/MouseHandler.h"
#include "Map/Ground.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/IBuffer.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "System/EventHandler.h"
#include "System/Log/ILog.h"
#include "System/float3.h"

#include <cmath>
#include <cstring>
#include <vector>


namespace {

struct CursorVertex {
	float pos[3];
};

// Per-draw state: viewProj for the world-space ring + a packed
// (cx, cy, cz, radius) anchor so the vertex shader can rebuild the
// ring around the live cursor without reuploading vertex geometry
// each frame. Pulse alpha goes through `colorA.w`.
struct alignas(16) UBOLayout {
	float viewProj[16];
	float anchor[4];   // cx, cy, cz, radius
	float colorA[4];   // rgba (alpha pulses each frame)
};

constexpr int kRingSegments = 48;

// VS positions sit on the unit circle (XZ plane); the anchor UBO
// scales + translates them into world space. y is wired live to the
// terrain height in the vertex shader to keep the ring flush on
// uneven ground.
constexpr const char* kVertexGlsl = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 0) out vec4 vColor;

layout(set = 0, binding = 0) uniform CursorUBO {
    mat4 uViewProj;
    vec4 uAnchor; // cx, cy, cz, radius
    vec4 uColor;
} ubo;

void main() {
    vec3 wp;
    wp.x = ubo.uAnchor.x + aPos.x * ubo.uAnchor.w;
    wp.y = ubo.uAnchor.y + aPos.y;          // ring vertically anchored to ground
    wp.z = ubo.uAnchor.z + aPos.z * ubo.uAnchor.w;
    vColor = ubo.uColor;
    gl_Position = ubo.uViewProj * vec4(wp, 1.0);
}
)";

constexpr const char* kFragmentGlsl = R"(#version 450
layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 fragColor;
void main() {
    fragColor = vColor;
}
)";

} // namespace


MetalWorldCursor::MetalWorldCursor()
{
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr)
		return;
	auto& backend = *globalRendering->renderBackend;

	// Build a closed ring on the XZ unit circle as a triangle strip
	// of two-segment quads (inner + outer radius), giving the ring
	// thickness without LineWidth (which the abstract pipeline doesn't
	// expose). 2 verts per segment, +2 to close the loop.
	std::vector<CursorVertex> verts;
	verts.reserve((kRingSegments + 1) * 6);
	const float lift = 1.5f;
	for (int i = 0; i < kRingSegments; ++i) {
		const float a0 = (static_cast<float>(i)     / kRingSegments) * 6.28318530718f;
		const float a1 = (static_cast<float>(i + 1) / kRingSegments) * 6.28318530718f;
		const float c0 = std::cos(a0), s0 = std::sin(a0);
		const float c1 = std::cos(a1), s1 = std::sin(a1);
		const float inner = 0.92f;
		const float outer = 1.00f;
		const CursorVertex i0{ {c0 * inner, lift, s0 * inner} };
		const CursorVertex o0{ {c0 * outer, lift, s0 * outer} };
		const CursorVertex i1{ {c1 * inner, lift, s1 * inner} };
		const CursorVertex o1{ {c1 * outer, lift, s1 * outer} };
		verts.push_back(i0); verts.push_back(o0); verts.push_back(o1);
		verts.push_back(i0); verts.push_back(o1); verts.push_back(i1);
	}
	vertCount = static_cast<uint32_t>(verts.size());

	vertexBuffer = backend.CreateBuffer(verts.size() * sizeof(CursorVertex), verts.data());
	if (!vertexBuffer || !vertexBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalWorldCursor] vertex buffer creation failed");
		return;
	}

	UBOLayout seed{};
	seed.viewProj[0] = 1.0f; seed.viewProj[5] = 1.0f;
	seed.viewProj[10] = 1.0f; seed.viewProj[15] = 1.0f;
	seed.colorA[0] = seed.colorA[1] = seed.colorA[2] = seed.colorA[3] = 1.0f;
	uniformBuffer = backend.CreateBuffer(sizeof(UBOLayout), &seed);
	if (!uniformBuffer || !uniformBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalWorldCursor] uniform buffer creation failed");
		return;
	}

	PipelineDesc pd;
	pd.name           = "metal_world_cursor";
	pd.vertexSource   = kVertexGlsl;
	pd.fragmentSource = kFragmentGlsl;
	pd.vertexAttributes = {
		VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = offsetof(CursorVertex, pos), .format = VertexFormat::Float3 },
	};
	pd.vertexBindings = {
		VertexBindingLayout{ .slot = 0, .stride = sizeof(CursorVertex) },
	};
	pd.blendState.enabled  = true;
	pd.blendState.srcColor = GL_SRC_ALPHA;
	pd.blendState.dstColor = GL_ONE_MINUS_SRC_ALPHA;
	pd.blendState.srcAlpha = GL_ONE;
	pd.blendState.dstAlpha = GL_ONE_MINUS_SRC_ALPHA;

	pipeline = backend.CreatePipeline(pd);
	if (!pipeline || !pipeline->IsValid()) {
		LOG_L(L_ERROR, "[MetalWorldCursor] pipeline link failed: %s",
			pipeline ? pipeline->GetLog().c_str() : "(null)");
		return;
	}

	valid = true;
}

MetalWorldCursor::~MetalWorldCursor() = default;

void MetalWorldCursor::Draw()
{
	if (!valid)
		return;
	if (mouse == nullptr)
		return;

	// GetWorldMapPos returns -OnesVector when the cursor ray misses
	// the ground (over sky / off-map); skip silently in that case.
	const float3 wpos = mouse->GetWorldMapPos();
	if (wpos.x < 0.0f && wpos.y < 0.0f && wpos.z < 0.0f)
		return;

	const CCamera* cam = CCameraHandler::GetActiveCamera();
	if (cam == nullptr)
		return;

	// Ring radius scales gently with camera distance so the cursor
	// stays visually consistent in size whether the player is zoomed
	// out over the whole map or zoomed all the way in. Clamp to sane
	// elmo bounds.
	const float camDist = std::max(1.0f, (cam->GetPos() - wpos).Length());
	const float radius  = std::clamp(camDist * 0.012f, 6.0f, 80.0f);

	// Soft pulse so the cursor remains visible against bright snow /
	// dark forest tiles without looking like it's flashing erratically.
	const float t = static_cast<float>(gu != nullptr ? gu->modGameTime : 0.0);
	const float pulse = 0.6f + 0.4f * std::sin(t * 4.0f);

	UBOLayout ubo{};
	std::memcpy(ubo.viewProj, cam->GetViewProjectionMatrix().m, sizeof(ubo.viewProj));
	const float groundY = CGround::GetHeightReal(wpos.x, wpos.z, false);
	ubo.anchor[0] = wpos.x;
	ubo.anchor[1] = groundY;
	ubo.anchor[2] = wpos.z;
	ubo.anchor[3] = radius;
	ubo.colorA[0] = 1.00f;
	ubo.colorA[1] = 0.95f;
	ubo.colorA[2] = 0.40f;
	ubo.colorA[3] = pulse;

	uniformBuffer->UpdateData(&ubo, sizeof(ubo), 0);

	pipeline->Enable();
	pipeline->BindUniformBuffer(0, *uniformBuffer, 0, sizeof(ubo));
	pipeline->BindVertexBuffer(0, *vertexBuffer);
	pipeline->Draw(PrimitiveTopology::Triangles, 0, vertCount);
	pipeline->Disable();
}

#endif // RENDER_BACKEND_METAL
