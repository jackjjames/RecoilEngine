/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalUnitMesh.h"

#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Rendering/Env/ISky.h"
#include "Rendering/Env/SkyLight.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/IBuffer.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/Models/3DModel.hpp"
#include "Rendering/Models/3DModelPiece.hpp"
#include "Rendering/Models/VertexData.hpp"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "Sim/Misc/Team.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitHandler.h"
#include "System/Log/ILog.h"
#include "System/Matrix44f.h"
#include "System/Transform.hpp"
#include "System/float4.h"

#include <cstring>
#include <vector>


namespace {

// Compact per-vertex format. SVertexData carries tangents + UVs + bone
// data we don't need for the first slice; strip to pos + normal so the
// Metal vertex buffer is a third of the size of the GL interleaved one.
struct UnitVertex {
	float px, py, pz;
	float nx, ny, nz;
};

struct alignas(16) UBOLayout {
	float viewProj[16];
	float model[16];
	float sunDir[4];     // xyz + unused w
	float materialRGB[4];// rgb + unused w; per-draw recolour knob
};


constexpr const char* kVertexGlsl = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;

layout(location = 0) out vec3 vNormalWS;

layout(set = 0, binding = 0) uniform UBO {
    mat4 uViewProj;
    mat4 uModel;
    vec4 uSunDir;      // world-space light direction
    vec4 uMaterialRGB; // unused here, sampled in FS
} ubo;

void main() {
    vNormalWS = mat3(ubo.uModel) * aNormal;
    gl_Position = ubo.uViewProj * ubo.uModel * vec4(aPos, 1.0);
}
)";

constexpr const char* kFragmentGlsl = R"(#version 450
layout(location = 0) in vec3 vNormalWS;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    mat4 uViewProj;
    mat4 uModel;
    vec4 uSunDir;
    vec4 uMaterialRGB;
} ubo;

void main() {
    vec3 N = normalize(vNormalWS);
    vec3 L = normalize(ubo.uSunDir.xyz);
    float diffuse = max(dot(N, L), 0.0);
    float ambient = 0.35 + 0.15 * max(N.y, 0.0);
    vec3 col = ubo.uMaterialRGB.rgb * (ambient + diffuse * 0.7);
    fragColor = vec4(col, 1.0);
}
)";


// Flatten every piece's vertices into model-space by applying the piece
// bind-pose transform. Matches what S3DModelVAO::ProcessVertices does on
// the GL path (it keeps the piece-local coords and applies the bind pose
// through a per-vertex bone weight SSBO; we do the same work on the CPU
// here to avoid depending on transformsUploader + SSBO plumbing for this
// first slice).
bool FlattenModelBindPose(const S3DModel* model,
                          std::vector<UnitVertex>& verts,
                          std::vector<uint32_t>&   inds)
{
	if (model == nullptr)
		return false;

	verts.clear();
	inds.clear();

	for (const S3DModelPiece* piece : model->pieceObjects) {
		if (piece == nullptr || !piece->HasGeometryData())
			continue;

		const auto& pieceVerts = piece->GetVerticesVec();
		const auto& pieceInds  = piece->GetIndicesVec();
		if (pieceVerts.empty() || pieceInds.empty())
			continue;

		const CMatrix44f bpose = piece->bposeTransform.ToMatrix();

		const uint32_t baseIdx = static_cast<uint32_t>(verts.size());
		verts.reserve(verts.size() + pieceVerts.size());
		for (const SVertexData& v : pieceVerts) {
			const float3 p = bpose.Mul(v.pos);
			// Transform-as-direction (w=0) ignores translation, so the
			// rotation component of the bind pose matrix is enough for
			// normals. If non-uniform scale ever shows up on a piece
			// bpose this must switch to inverse-transpose 3x3.
			const float3& n0 = v.normal;
			float3 n(
				bpose.m[0] * n0.x + bpose.m[4] * n0.y + bpose.m[ 8] * n0.z,
				bpose.m[1] * n0.x + bpose.m[5] * n0.y + bpose.m[ 9] * n0.z,
				bpose.m[2] * n0.x + bpose.m[6] * n0.y + bpose.m[10] * n0.z
			);
			if (n.SqLength() > 1e-6f)
				n.Normalize();

			verts.push_back(UnitVertex{
				p.x, p.y, p.z,
				n.x, n.y, n.z,
			});
		}

		inds.reserve(inds.size() + pieceInds.size());
		for (uint32_t ix : pieceInds)
			inds.push_back(baseIdx + ix);
	}

	return !verts.empty() && !inds.empty();
}

} // namespace


MetalUnitMesh::MetalUnitMesh()
{
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr)
		return;
	auto& backend = *globalRendering->renderBackend;

	UBOLayout seed{};
	seed.viewProj[0]  = 1.0f; seed.viewProj[5]  = 1.0f;
	seed.viewProj[10] = 1.0f; seed.viewProj[15] = 1.0f;
	seed.model[0]     = 1.0f; seed.model[5]     = 1.0f;
	seed.model[10]    = 1.0f; seed.model[15]    = 1.0f;
	seed.sunDir[1]    = 1.0f;
	seed.materialRGB[0] = 0.75f;
	seed.materialRGB[1] = 0.75f;
	seed.materialRGB[2] = 0.78f;
	uniformBuffer = backend.CreateBuffer(sizeof(UBOLayout), &seed);
	if (!uniformBuffer || !uniformBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalUnitMesh] uniform buffer creation failed");
		return;
	}

	PipelineDesc pd;
	pd.name = "unit_mesh";
	pd.vertexSource   = kVertexGlsl;
	pd.fragmentSource = kFragmentGlsl;
	pd.vertexAttributes = {
		VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = offsetof(UnitVertex, px), .format = VertexFormat::Float3 },
		VertexAttribute{ .location = 1, .bufferSlot = 0, .offset = offsetof(UnitVertex, nx), .format = VertexFormat::Float3 },
	};
	pd.vertexBindings = {
		VertexBindingLayout{ .slot = 0, .stride = sizeof(UnitVertex) },
	};

	pipeline = backend.CreatePipeline(pd);
	if (!pipeline || !pipeline->IsValid()) {
		LOG_L(L_ERROR, "[MetalUnitMesh] pipeline link failed: %s",
			pipeline ? pipeline->GetLog().c_str() : "(null)");
		return;
	}

	valid = true;
}

MetalUnitMesh::~MetalUnitMesh() = default;

const MetalUnitMesh::ModelBuffers* MetalUnitMesh::GetOrUploadModel(const S3DModel* model)
{
	if (model == nullptr)
		return nullptr;

	auto it = modelCache.find(model);
	if (it != modelCache.end())
		return it->second.indexCount > 0 ? &it->second : nullptr;

	std::vector<UnitVertex> verts;
	std::vector<uint32_t>   inds;
	if (!FlattenModelBindPose(model, verts, inds)) {
		// Cache the negative so we don't retry flattening every frame.
		modelCache.emplace(model, ModelBuffers{});
		return nullptr;
	}

	auto& backend = *globalRendering->renderBackend;
	ModelBuffers mb;
	mb.vertexBuffer = backend.CreateBuffer(verts.size() * sizeof(UnitVertex), verts.data());
	mb.indexBuffer  = backend.CreateBuffer(inds.size()  * sizeof(uint32_t),   inds.data());
	if (!mb.vertexBuffer || !mb.vertexBuffer->IsValid() ||
	    !mb.indexBuffer  || !mb.indexBuffer->IsValid())
	{
		LOG_L(L_WARNING, "[MetalUnitMesh] buffer upload failed for model '%s'", model->name.c_str());
		modelCache.emplace(model, ModelBuffers{});
		return nullptr;
	}
	mb.indexCount = static_cast<uint32_t>(inds.size());

	LOG_L(L_INFO, "[MetalUnitMesh] uploaded '%s' verts=%zu indices=%zu pieces=%d",
		model->name.c_str(), verts.size(), inds.size(), model->numPieces);

	auto [emplaced, ok] = modelCache.emplace(model, std::move(mb));
	return &emplaced->second;
}

void MetalUnitMesh::Draw()
{
	if (!valid)
		return;

	const auto& active = unitHandler.GetActiveUnits();
	if (active.empty())
		return;

	const CCamera* cam = CCameraHandler::GetActiveCamera();
	if (cam == nullptr)
		return;

	UBOLayout ubo{};
	std::memcpy(ubo.viewProj, cam->GetViewProjectionMatrix().m, sizeof(ubo.viewProj));

	float4 sunDir = float4(0.3f, 0.85f, 0.4f, 0.0f);
	const auto& skyPtr = ISky::GetSky();
	if (skyPtr != nullptr && skyPtr->GetLight() != nullptr)
		sunDir = skyPtr->GetLight()->GetLightDir();
	ubo.sunDir[0] = sunDir.x; ubo.sunDir[1] = sunDir.y; ubo.sunDir[2] = sunDir.z;

	pipeline->Enable();

	for (const CUnit* u : active) {
		if (u == nullptr || u->model == nullptr)
			continue;
		if (u->noDraw)
			continue;

		const ModelBuffers* mb = GetOrUploadModel(u->model);
		if (mb == nullptr)
			continue;

		// Unit transform: same ComposeMatrix the GL path uses
		// (CUnit::GetTransformMatrix ends up here too). Sample synced
		// `pos` rather than `drawPos` because the Metal build skips
		// CUnitDrawerData::UpdateDrawPos for now, so drawPos is still
		// ZeroVector. The sim ticks on the main thread synchronously
		// with Draw, so pos is already the "current frame" value; no
		// interpolation smoothness to lose yet.
		const CMatrix44f model = u->ComposeMatrix(u->pos);
		std::memcpy(ubo.model, model.m, sizeof(ubo.model));

		// Per-unit team colour. The GL build replaces the alpha=0
		// pixels in the texture with this colour via the team-mask
		// channel; until S3O texture sampling is wired up here we
		// just tint the whole model. CUnit.team -> CTeam.color is
		// the same source of truth the GL path uses.
		float tr = 0.65f, tg = 0.65f, tb = 0.65f;
		if (teamHandler.IsValidTeam(u->team)) {
			const uint8_t* c = teamHandler.Team(u->team)->color;
			tr = c[0] * (1.0f / 255.0f);
			tg = c[1] * (1.0f / 255.0f);
			tb = c[2] * (1.0f / 255.0f);
		}
		ubo.materialRGB[0] = tr;
		ubo.materialRGB[1] = tg;
		ubo.materialRGB[2] = tb;

		uniformBuffer->UpdateData(&ubo, sizeof(ubo), 0);

		pipeline->BindUniformBuffer(0, *uniformBuffer, 0, sizeof(ubo));
		pipeline->BindVertexBuffer(0, *mb->vertexBuffer);
		pipeline->DrawIndexed(PrimitiveTopology::Triangles, mb->indexCount,
			IndexType::Uint32, *mb->indexBuffer);
	}

	pipeline->Disable();
}

#endif // RENDER_BACKEND_METAL
