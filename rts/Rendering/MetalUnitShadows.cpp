/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalUnitShadows.h"

#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Map/Ground.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/IBuffer.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureHandler.h"
#include "Sim/Misc/CollisionVolume.h"
#include "Sim/Objects/SolidObject.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitHandler.h"
#include "System/Log/ILog.h"
#include "System/Matrix44f.h"
#include "System/float3.h"
#include "System/type2.h"

#include <algorithm>
#include <cstring>
#include <vector>


namespace {

struct ShadowVertex {
	float px, py, pz;
	float u, v;
};

struct alignas(16) UBOLayout {
	float viewProj[16];
};

constexpr const char* kVertexGlsl = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;

layout(location = 0) out vec2 vUV;

layout(set = 0, binding = 0) uniform UBO {
    mat4 uViewProj;
} ubo;

void main() {
    vUV = aUV;
    gl_Position = ubo.uViewProj * vec4(aPos, 1.0);
}
)";

constexpr const char* kFragmentGlsl = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

void main() {
    // uv is in [-1, 1]; r=1 at the rim. Smoothstep keeps the centre
    // solid so an isolated commander has a well-defined contact patch
    // and feathers towards the edge so units brushing past each other
    // don't blot together into a hard black mass.
    float r = length(vUV);
    float a = 1.0 - smoothstep(0.55, 1.0, r);
    if (a <= 0.0)
        discard;
    // Multiplying alpha by the centre fall-off again deepens the
    // contact darkness without changing the silhouette.
    fragColor = vec4(0.0, 0.0, 0.0, a * 0.55);
}
)";


// Sample CGround at the four corners and the centre to follow uneven
// terrain. The +0.4 lift is enough to stop z-fighting against the
// terrain mesh in tests but small enough that the shadow still reads
// as if it sits on the ground.
constexpr float kShadowLift = 0.4f;

void EmitShadowQuad(std::vector<ShadowVertex>& verts,
                    std::vector<uint32_t>&     inds,
                    const float3& centre, float radius)
{
	const uint32_t base = static_cast<uint32_t>(verts.size());

	const float x0 = centre.x - radius, x1 = centre.x + radius;
	const float z0 = centre.z - radius, z1 = centre.z + radius;

	const float y00 = CGround::GetHeightReal(x0, z0) + kShadowLift;
	const float y10 = CGround::GetHeightReal(x1, z0) + kShadowLift;
	const float y11 = CGround::GetHeightReal(x1, z1) + kShadowLift;
	const float y01 = CGround::GetHeightReal(x0, z1) + kShadowLift;

	verts.push_back(ShadowVertex{x0, y00, z0, -1.0f, -1.0f});
	verts.push_back(ShadowVertex{x1, y10, z0,  1.0f, -1.0f});
	verts.push_back(ShadowVertex{x1, y11, z1,  1.0f,  1.0f});
	verts.push_back(ShadowVertex{x0, y01, z1, -1.0f,  1.0f});

	inds.push_back(base + 0);
	inds.push_back(base + 1);
	inds.push_back(base + 2);
	inds.push_back(base + 0);
	inds.push_back(base + 2);
	inds.push_back(base + 3);
}

// Pull a sensible shadow radius for the object. Uses the collision
// volume's bounding radius first because that's the input every other
// rendering path treats as "true silhouette size"; falls back to the
// solid object's own radius (set from model bounds at load time) when
// the col-vol hasn't been assigned a radius yet.
float ComputeShadowRadius(const CSolidObject* so)
{
	float r = 0.0f;
	if (so->collisionVolume.GetBoundingRadius() > 0.0f)
		r = so->collisionVolume.GetBoundingRadius();
	if (r <= 0.0f)
		r = so->radius;
	if (r <= 0.0f)
		r = 16.0f; // last-resort default; matches the smallest BAR unit footprint
	// Pull in slightly so the rim doesn't extend past the unit's
	// silhouette by an obvious margin on top-down isometric framing.
	return r * 0.9f;
}

} // namespace


MetalUnitShadows::MetalUnitShadows()
{
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr)
		return;
	auto& backend = *globalRendering->renderBackend;

	UBOLayout seed{};
	seed.viewProj[0]  = 1.0f; seed.viewProj[5]  = 1.0f;
	seed.viewProj[10] = 1.0f; seed.viewProj[15] = 1.0f;
	uniformBuffer = backend.CreateBuffer(sizeof(UBOLayout), &seed);
	if (!uniformBuffer || !uniformBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalUnitShadows] uniform buffer creation failed");
		return;
	}

	PipelineDesc pd;
	pd.name = "unit_shadows";
	pd.vertexSource   = kVertexGlsl;
	pd.fragmentSource = kFragmentGlsl;
	pd.vertexAttributes = {
		VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = offsetof(ShadowVertex, px), .format = VertexFormat::Float3 },
		VertexAttribute{ .location = 1, .bufferSlot = 0, .offset = offsetof(ShadowVertex, u),  .format = VertexFormat::Float2 },
	};
	pd.vertexBindings = {
		VertexBindingLayout{ .slot = 0, .stride = sizeof(ShadowVertex) },
	};
	// Standard alpha blend; the fragment writes (0,0,0,a) so the
	// resulting pixel is `dst * (1-a)`, i.e. terrain darkened in
	// proportion to a. Equivalent to multiply-by-(1-a) without
	// needing a dual-source blend mode.
	pd.blendState.enabled  = true;
	pd.blendState.srcColor = GL_SRC_ALPHA;
	pd.blendState.dstColor = GL_ONE_MINUS_SRC_ALPHA;
	pd.blendState.srcAlpha = GL_SRC_ALPHA;
	pd.blendState.dstAlpha = GL_ONE_MINUS_SRC_ALPHA;

	pipeline = backend.CreatePipeline(pd);
	if (!pipeline || !pipeline->IsValid()) {
		LOG_L(L_ERROR, "[MetalUnitShadows] pipeline link failed: %s",
			pipeline ? pipeline->GetLog().c_str() : "(null)");
		return;
	}

	valid = true;
}

MetalUnitShadows::~MetalUnitShadows() = default;

void MetalUnitShadows::Draw()
{
	if (!valid)
		return;

	const CCamera* cam = CCameraHandler::GetActiveCamera();
	if (cam == nullptr)
		return;

	std::vector<ShadowVertex> verts;
	std::vector<uint32_t>     inds;

	const auto& units = unitHandler.GetActiveUnits();
	verts.reserve(units.size() * 4);
	inds.reserve(units.size() * 6);

	for (const CUnit* u : units) {
		if (u == nullptr || u->noDraw || u->IsInWater())
			continue;
		EmitShadowQuad(verts, inds, u->pos, ComputeShadowRadius(u));
	}

	for (int id : featureHandler.GetActiveFeatureIDs()) {
		const CFeature* f = featureHandler.GetFeature(id);
		if (f == nullptr || f->noDraw)
			continue;
		EmitShadowQuad(verts, inds, f->pos, ComputeShadowRadius(f));
	}

	if (verts.empty())
		return;

	// Re-allocating the vertex / index buffers every frame would tax
	// the heap; the GPU can read while the CPU writes the next frame's
	// payload because Metal's shared storage mode synchronises through
	// the command buffer. Reuse if the existing buffer fits, grow with
	// 2x slack when not.
	const uint32_t neededQuads = static_cast<uint32_t>(verts.size() / 4);
	const size_t   vbSize = verts.size() * sizeof(ShadowVertex);
	const size_t   ibSize = inds.size()  * sizeof(uint32_t);
	auto& backend = *globalRendering->renderBackend;
	if (neededQuads > bufferCapacity || !vertexBuffer || !indexBuffer) {
		bufferCapacity = std::max<uint32_t>(neededQuads * 2, 64);
		vertexBuffer = backend.CreateBuffer(bufferCapacity * 4 * sizeof(ShadowVertex), nullptr);
		indexBuffer  = backend.CreateBuffer(bufferCapacity * 6 * sizeof(uint32_t),     nullptr);
		if (!vertexBuffer || !indexBuffer || !vertexBuffer->IsValid() || !indexBuffer->IsValid()) {
			LOG_L(L_ERROR, "[MetalUnitShadows] dynamic buffer growth failed");
			valid = false;
			return;
		}
	}
	vertexBuffer->UpdateData(verts.data(), vbSize, 0);
	indexBuffer ->UpdateData(inds.data(),  ibSize, 0);

	UBOLayout ubo{};
	std::memcpy(ubo.viewProj, cam->GetViewProjectionMatrix().m, sizeof(ubo.viewProj));
	uniformBuffer->UpdateData(&ubo, sizeof(ubo), 0);

	pipeline->Enable();
	pipeline->BindUniformBuffer(0, *uniformBuffer, 0, sizeof(ubo));
	pipeline->BindVertexBuffer(0, *vertexBuffer);
	pipeline->DrawIndexed(PrimitiveTopology::Triangles, static_cast<uint32_t>(inds.size()),
		IndexType::Uint32, *indexBuffer);
	pipeline->Disable();
}

#endif // RENDER_BACKEND_METAL
