/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalUnitMesh.h"

#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Rendering/Env/ISky.h"
#include "Rendering/Env/SkyLight.h"
#include "Rendering/Env/SunLighting.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/IBuffer.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/MetalModelData.h"
#include "Rendering/Models/3DModel.hpp"
#include "Rendering/Models/3DModelPiece.hpp"
#include "Rendering/Models/LocalModel.hpp"
#include "Rendering/Models/ModelsMemStorage.h"
#include "Rendering/Models/VertexData.hpp"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "Rendering/Textures/ITexture.h"
#include "Rendering/Textures/S3OTextureHandler.h"
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureHandler.h"
#include "Sim/Misc/Team.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitHandler.h"
#include "System/Log/ILog.h"
#include "System/Matrix44f.h"
#include "System/Transform.hpp"
#include "System/float4.h"

#include <algorithm>
#include <cstring>
#include <vector>


namespace {

// Compact per-vertex format. SVertexData carries tangents + UVs + bone
// data we don't need for this slice; strip to pos + normal + uv0 so
// the Metal vertex buffer is half the size of the GL interleaved one.
struct UnitVertex {
	float px, py, pz;
	float nx, ny, nz;
	float u, v;
	float boneId0, boneId1, boneId2, boneId3;
	float weight0, weight1, weight2, weight3;
};

constexpr size_t kMaxBones = 256;

struct alignas(16) UBOLayout {
	float viewProj[16];
	float object[16];
	float sunDir[4];     // xyz + unused w
	float materialRGB[4];// rgb + unused w; per-draw recolour knob
	float camPos[4];     // xyz + unused w; world-space view origin
	float sunAmbient[4]; // model ambient RGB + unused w
	float sunDiffuse[4]; // model diffuse RGB + unused w
	float sunSpecular[4];// model specular RGB + exponent
	float pieceMats[kMaxBones][16];
	float bindMats[kMaxBones][16];
};


constexpr const char* kVertexGlsl = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV0;
layout(location = 3) in vec4 aBoneIDs;
layout(location = 4) in vec4 aBoneWeights;

layout(location = 0) out vec3 vNormalWS;
layout(location = 1) out vec2 vUV;
layout(location = 2) out vec3 vWorldPos;

layout(set = 0, binding = 0) uniform UBO {
    mat4 uViewProj;
    mat4 uObject;
    vec4 uSunDir;      // world-space light direction
    vec4 uMaterialRGB; // team colour for the alpha-mask channel
    vec4 uCamPos;
    vec4 uSunAmbient;
    vec4 uSunDiffuse;
    vec4 uSunSpecular;
    mat4 uPieceMats[256];
    mat4 uBindMats[256];
} ubo;

int BoneIndex(float v) {
    return clamp(int(v + 0.5), 0, 255);
}

void AddBoneInfluence(inout vec4 posSum, inout vec4 normalSum, vec4 piecePos, vec4 pieceNormal, int baseBone, int bone, float weight) {
    if (weight <= 0.0)
        return;
    mat4 skinMat = ubo.uPieceMats[bone] * inverse(ubo.uBindMats[bone]) * ubo.uBindMats[baseBone];
    posSum += (skinMat * piecePos) * weight;
    normalSum += (skinMat * pieceNormal) * weight;
}

void main() {
    vec4 weights = max(aBoneWeights, vec4(0.0)) / 255.0;
    float weightSum = max(dot(weights, vec4(1.0)), 0.0001);
    weights /= weightSum;

    int b0 = BoneIndex(aBoneIDs.x);
    vec4 piecePos = vec4(aPos, 1.0);
    vec4 pieceNormal = vec4(aNormal, 0.0);

    vec4 modelPos = ubo.uPieceMats[b0] * piecePos * weights.x;
    vec4 modelNormal = ubo.uPieceMats[b0] * pieceNormal * weights.x;
    AddBoneInfluence(modelPos, modelNormal, piecePos, pieceNormal, b0, BoneIndex(aBoneIDs.y), weights.y);
    AddBoneInfluence(modelPos, modelNormal, piecePos, pieceNormal, b0, BoneIndex(aBoneIDs.z), weights.z);
    AddBoneInfluence(modelPos, modelNormal, piecePos, pieceNormal, b0, BoneIndex(aBoneIDs.w), weights.w);

    vNormalWS = mat3(ubo.uObject) * modelNormal.xyz;
    vUV = aUV0;
    vec4 worldH = ubo.uObject * modelPos;
    vWorldPos = worldH.xyz;
    gl_Position = ubo.uViewProj * worldH;
}
)";

constexpr const char* kFragmentGlsl = R"(#version 450
layout(location = 0) in vec3 vNormalWS;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vWorldPos;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform UBO {
    mat4 uViewProj;
    mat4 uObject;
    vec4 uSunDir;
    vec4 uMaterialRGB;
    vec4 uCamPos;
    vec4 uSunAmbient;
    vec4 uSunDiffuse;
    vec4 uSunSpecular;
    mat4 uPieceMats[256];
    mat4 uBindMats[256];
} ubo;

layout(set = 0, binding = 1) uniform sampler2D uDiffuse;
layout(set = 0, binding = 2) uniform sampler2D uExtra;   // S3O tex2: R=emissive, G=spec mask, A=mask

void main() {
    // S3O convention (matches GLSL/ModelFragProg.glsl): tex1.a is the
    // team-mask weight (alpha=0 keeps the diffuse texel, alpha=1
    // overrides with the team colour); tex2 carries R=emissive,
    // G=specular intensity, A=opacity bit. Most BAR commanders /
    // factories texture the body and reserve alpha=1 for trim,
    // accents and logos.
    vec4 diff  = texture(uDiffuse, vUV);
    vec4 extra = texture(uExtra,   vUV);
    vec3 albedo = mix(diff.rgb, ubo.uMaterialRGB.rgb, diff.a);

    vec3 N = normalize(vNormalWS);
    vec3 L = normalize(ubo.uSunDir.xyz);
    float NdotL = max(dot(N, L), 0.0);
    vec3 modelLight = max(ubo.uSunAmbient.rgb, vec3(0.03)) + ubo.uSunDiffuse.rgb * NdotL;

    // Approximate the GL model shader's cubemap reflection term until
    // the Metal path has sky/reflection cubemaps. Tex2.G remains the
    // material's reflectivity/specular knob, so metal panels get the
    // stronger high-contrast read BAR expects without inventing new data.
    float hemi = 0.35 + 0.65 * max(N.y, 0.0);
    vec3 envLight = mix(vec3(0.20, 0.22, 0.24), vec3(0.70, 0.78, 0.86), hemi);
    vec3 mixedLight = mix(modelLight, envLight * (0.65 + 0.35 * NdotL), clamp(extra.g, 0.0, 1.0));

    // Phong specular. Cubemap-based env reflections (GL ModelFragProg
    // uses `specular = textureCube(reflectTex, ...) * sunSpecular`)
    // are deferred until S9-C5b lands a real sky cubemap; until then
    // the sun-specular term gives metallic pieces (turret barrels,
    // canopies) a recognisable highlight.
    vec3 V = normalize(ubo.uCamPos.xyz - vWorldPos);
    vec3 H = normalize(L + V);
    float specExp = max(8.0, ubo.uSunSpecular.a);
    vec3 spec = ubo.uSunSpecular.rgb * pow(max(dot(N, H), 0.0), specExp) * extra.g * 2.5 * NdotL;

    // R channel doubles as a self-illum mask: glow trim stays bright
    // even in shadow, matches the GL path's `reflection += extra.rrr`.
    vec3 emissive = vec3(extra.r) * 1.35;

    fragColor = vec4(albedo * (mixedLight + emissive) + spec, extra.a);
}
)";


// Pack a piece's raw piece-local vertices into the compact UnitVertex
// layout. Bone IDs and weights are preserved so the Metal shader can
// apply the same piece/bind-pose skinning path used by GL4.
bool PackPieceGeometry(const S3DModelPiece* piece,
                       std::vector<UnitVertex>& verts,
                       std::vector<uint32_t>&   inds)
{
	if (piece == nullptr || !piece->HasGeometryData())
		return false;

	const auto& pieceVerts = piece->GetVerticesVec();
	const auto& pieceInds  = piece->GetIndicesVec();
	if (pieceVerts.empty() || pieceInds.empty())
		return false;

	verts.clear();
	verts.reserve(pieceVerts.size());
	for (const SVertexData& v : pieceVerts) {
		const auto boneID = [&v](size_t idx) {
			return static_cast<float>(uint16_t(v.boneIDsLow[idx]) | (uint16_t(v.boneIDsHigh[idx]) << 8u));
		};
		verts.push_back(UnitVertex{
			v.pos.x, v.pos.y, v.pos.z,
			v.normal.x, v.normal.y, v.normal.z,
			v.texCoords[0].x, v.texCoords[0].y,
			boneID(0), boneID(1), boneID(2), boneID(3),
			static_cast<float>(v.boneWeights[0]),
			static_cast<float>(v.boneWeights[1]),
			static_cast<float>(v.boneWeights[2]),
			static_cast<float>(v.boneWeights[3]),
		});
	}

	inds.assign(pieceInds.begin(), pieceInds.end());
	return true;
}

void StoreMatrix(float* dst, const CMatrix44f& mat)
{
	std::memcpy(dst, mat.m, sizeof(float) * 16);
}

void StoreIdentity(float* dst)
{
	const CMatrix44f identity;
	StoreMatrix(dst, identity);
}

void ResetMatrixPalette(UBOLayout& ubo)
{
	for (size_t i = 0; i < kMaxBones; ++i) {
		StoreIdentity(ubo.pieceMats[i]);
		StoreIdentity(ubo.bindMats[i]);
	}
}

template<typename TObject>
bool FillObjectSkinningData(const TObject* object, UBOLayout& ubo)
{
	if (object == nullptr || object->model == nullptr)
		return false;

	const ScopedTransformMemAlloc& transformAlloc = MetalModelData::GetTransformMemAlloc(object);
	if (!transformAlloc.Valid())
		return false;

	const float timeOffset = std::clamp(globalRendering->timeOffset, 0.0f, 1.0f);
	StoreMatrix(ubo.object, Transform::Lerp(transformAlloc[0], transformAlloc[1], timeOffset).ToMatrix());

	ResetMatrixPalette(ubo);

	const size_t numPieces = std::min<size_t>(object->model->numPieces, kMaxBones);
	for (size_t i = 0; i < numPieces; ++i) {
		const size_t transformBase = 2 * (1 + i);
		StoreMatrix(ubo.pieceMats[i], Transform::Lerp(transformAlloc[transformBase + 0], transformAlloc[transformBase + 1], timeOffset).ToMatrix());

		if (i < object->model->pieceObjects.size() && object->model->pieceObjects[i] != nullptr) {
			StoreMatrix(ubo.bindMats[i], object->model->pieceObjects[i]->bposeTransform.ToMatrix());
		}
	}

	return true;
}

} // namespace


MetalUnitMesh::MetalUnitMesh()
{
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr)
		return;
	auto& backend = *globalRendering->renderBackend;

	// 1x1 fallback so models with no resident diffuse read something
	// deterministic instead of an undefined sampler. RGB=grey lets
	// the lambert lighting pick up shape; alpha=1 forces the
	// team-mask path in the FS so the unit gets fully tinted with
	// the team colour - that's a closer approximation than rendering
	// flat grey when texture loading fails.
	GL::TextureCreationParams whiteParams;
	whiteParams.linearMipMapFilter = false;
	whiteParams.linearTextureFilter = false;
	whiteParams.wrapMirror = false;
	whiteParams.reqNumLevels = 1;
	whiteTexture = backend.CreateTexture2D(
		int2(1, 1), 0x8058 /*GL_RGBA8*/, whiteParams, /*wantCompress=*/false);
	if (whiteTexture && whiteTexture->IsValid()) {
		const uint8_t fallback[4] = {200, 200, 200, 255};
		whiteTexture->Bind();
		whiteTexture->UploadImage(fallback);
		whiteTexture->Unbind();
	}

	// Black 1x1 stand-in for tex2 when a model has no extra/spec
	// texture. RGBA=0 means no emissive, no specular response, full
	// (alpha-mask) opacity from the diffuse path: the unit just falls
	// back to pure lambert + ambient. Same TextureCreationParams as
	// `whiteTexture` because the sampler doesn't care about content,
	// only that *something* is bound.
	blackTexture = backend.CreateTexture2D(
		int2(1, 1), 0x8058 /*GL_RGBA8*/, whiteParams, /*wantCompress=*/false);
	if (blackTexture && blackTexture->IsValid()) {
		const uint8_t zero[4] = {0, 0, 0, 255};
		blackTexture->Bind();
		blackTexture->UploadImage(zero);
		blackTexture->Unbind();
	}

	UBOLayout seed{};
	seed.viewProj[0]  = 1.0f; seed.viewProj[5]  = 1.0f;
	seed.viewProj[10] = 1.0f; seed.viewProj[15] = 1.0f;
	seed.object[0]    = 1.0f; seed.object[5]    = 1.0f;
	seed.object[10]   = 1.0f; seed.object[15]   = 1.0f;
	seed.sunDir[1]    = 1.0f;
	seed.materialRGB[0] = 0.75f;
	seed.materialRGB[1] = 0.75f;
	seed.materialRGB[2] = 0.78f;
	seed.sunAmbient[0] = 0.35f;
	seed.sunAmbient[1] = 0.35f;
	seed.sunAmbient[2] = 0.35f;
	seed.sunDiffuse[0] = 0.75f;
	seed.sunDiffuse[1] = 0.75f;
	seed.sunDiffuse[2] = 0.75f;
	seed.sunSpecular[0] = 0.45f;
	seed.sunSpecular[1] = 0.45f;
	seed.sunSpecular[2] = 0.45f;
	seed.sunSpecular[3] = 24.0f;
	for (size_t i = 0; i < kMaxBones; ++i) {
		seed.pieceMats[i][0] = seed.pieceMats[i][5] = seed.pieceMats[i][10] = seed.pieceMats[i][15] = 1.0f;
		seed.bindMats[i][0] = seed.bindMats[i][5] = seed.bindMats[i][10] = seed.bindMats[i][15] = 1.0f;
	}
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
		VertexAttribute{ .location = 2, .bufferSlot = 0, .offset = offsetof(UnitVertex, u),  .format = VertexFormat::Float2 },
		VertexAttribute{ .location = 3, .bufferSlot = 0, .offset = offsetof(UnitVertex, boneId0), .format = VertexFormat::Float4 },
		VertexAttribute{ .location = 4, .bufferSlot = 0, .offset = offsetof(UnitVertex, weight0), .format = VertexFormat::Float4 },
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
		return it->second.anyGeometry ? &it->second : nullptr;

	if (static_cast<size_t>(model->numPieces) > kMaxBones) {
		LOG_L(L_WARNING, "[MetalUnitMesh] skipping model '%s' with %d pieces (Metal palette cap is %zu)",
			model->name.c_str(), model->numPieces, kMaxBones);
		modelCache.emplace(model, ModelBuffers{});
		return nullptr;
	}

	auto& backend = *globalRendering->renderBackend;
	ModelBuffers mb;
	mb.pieces.resize(model->pieceObjects.size());

	size_t totalVerts = 0;
	size_t totalInds  = 0;

	std::vector<UnitVertex> verts;
	std::vector<uint32_t>   inds;
	for (size_t i = 0; i < model->pieceObjects.size(); ++i) {
		const S3DModelPiece* piece = model->pieceObjects[i];
		if (!PackPieceGeometry(piece, verts, inds))
			continue;

		PieceBuffers pb;
		pb.vertexBuffer = backend.CreateBuffer(verts.size() * sizeof(UnitVertex), verts.data());
		pb.indexBuffer  = backend.CreateBuffer(inds.size()  * sizeof(uint32_t),   inds.data());
		if (!pb.vertexBuffer || !pb.vertexBuffer->IsValid() ||
		    !pb.indexBuffer  || !pb.indexBuffer->IsValid())
		{
			LOG_L(L_WARNING, "[MetalUnitMesh] piece buffer upload failed for model '%s' piece %zu",
				model->name.c_str(), i);
			continue;
		}
		pb.indexCount = static_cast<uint32_t>(inds.size());

		mb.pieces[i] = std::move(pb);
		mb.anyGeometry = true;
		totalVerts += verts.size();
		totalInds  += inds.size();
	}

	if (!mb.anyGeometry) {
		// Cache the negative so we don't retry every frame.
		modelCache.emplace(model, ModelBuffers{});
		return nullptr;
	}

	LOG_L(L_INFO, "[MetalUnitMesh] uploaded '%s' verts=%zu indices=%zu pieces=%d",
		model->name.c_str(), totalVerts, totalInds, model->numPieces);

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

	const float3 camPos = cam->GetPos();
	ubo.camPos[0] = camPos.x; ubo.camPos[1] = camPos.y; ubo.camPos[2] = camPos.z;

	float4 sunDir = float4(0.3f, 0.85f, 0.4f, 0.0f);
	const auto& skyPtr = ISky::GetSky();
	if (skyPtr != nullptr && skyPtr->GetLight() != nullptr)
		sunDir = skyPtr->GetLight()->GetLightDir();
	ubo.sunDir[0] = sunDir.x; ubo.sunDir[1] = sunDir.y; ubo.sunDir[2] = sunDir.z;
	ubo.sunAmbient[0] = sunLighting->modelAmbientColor.x;
	ubo.sunAmbient[1] = sunLighting->modelAmbientColor.y;
	ubo.sunAmbient[2] = sunLighting->modelAmbientColor.z;
	ubo.sunDiffuse[0] = sunLighting->modelDiffuseColor.x;
	ubo.sunDiffuse[1] = sunLighting->modelDiffuseColor.y;
	ubo.sunDiffuse[2] = sunLighting->modelDiffuseColor.z;
	ubo.sunSpecular[0] = sunLighting->modelSpecularColor.x;
	ubo.sunSpecular[1] = sunLighting->modelSpecularColor.y;
	ubo.sunSpecular[2] = sunLighting->modelSpecularColor.z;
	ubo.sunSpecular[3] = sunLighting->specularExponent;

	pipeline->Enable();

	auto drawSolid = [&](const auto* so, float matR, float matG, float matB) {
		if (so == nullptr || so->model == nullptr)
			return;
		const ModelBuffers* mb = GetOrUploadModel(so->model);
		if (mb == nullptr)
			return;
		if (!so->localModel.Initialized())
			return;

		// Diffuse + extra (tex2) + team colour are constant across all
		// pieces of one solid; resolve them once before walking the
		// piece tree.
		ITexture* diffuse = nullptr;
		ITexture* extra   = nullptr;
		if (so->model->textureType > 0) {
			const auto* mat = textureHandlerS3O.GetTexture(so->model->textureType);
			if (mat != nullptr) {
				diffuse = mat->tex1;
				extra   = mat->tex2;
			}
		}
		if (diffuse == nullptr || !diffuse->IsValid())
			diffuse = whiteTexture.get();
		if (extra == nullptr || !extra->IsValid())
			extra = blackTexture.get();

		ubo.materialRGB[0] = matR;
		ubo.materialRGB[1] = matG;
		ubo.materialRGB[2] = matB;

		if (!FillObjectSkinningData(so, ubo))
			return;
		uniformBuffer->UpdateData(&ubo, sizeof(ubo), 0);

		const auto& lpieces = so->localModel.pieces;
		const size_t pieceCount = std::min(lpieces.size(), mb->pieces.size());
		for (size_t i = 0; i < pieceCount; ++i) {
			const PieceBuffers& pb = mb->pieces[i];
			if (pb.indexCount == 0)
				continue;

			const LocalModelPiece& lmp = lpieces[i];
			if (!lmp.GetScriptVisible())
				continue;

			pipeline->BindUniformBuffer(0, *uniformBuffer, 0, sizeof(ubo));
			if (diffuse != nullptr)
				pipeline->BindTexture(1, *diffuse);
			if (extra != nullptr)
				pipeline->BindTexture(2, *extra);
			pipeline->BindVertexBuffer(0, *pb.vertexBuffer);
			pipeline->DrawIndexed(PrimitiveTopology::Triangles, pb.indexCount,
				IndexType::Uint32, *pb.indexBuffer);
		}
	};

	for (const CUnit* u : active) {
		if (u == nullptr || u->noDraw || u->drawFlag == 0 || u->GetIsIcon())
			continue;
		// Per-unit team colour - this is the input to the alpha-mask
		// replacement in the FS, not a flat tint. CUnit.team ->
		// CTeam.color is the same source of truth the GL path uses.
		float r = 0.65f, g = 0.65f, b = 0.65f;
		if (teamHandler.IsValidTeam(u->team)) {
			const uint8_t* c = teamHandler.Team(u->team)->color;
			r = c[0] * (1.0f / 255.0f);
			g = c[1] * (1.0f / 255.0f);
			b = c[2] * (1.0f / 255.0f);
		}
		drawSolid(u, r, g, b);
	}

	// Features (trees, rocks, wrecks). Same bind-pose path; team-tint
	// would just paint trees green so we use a neutral earthy shade
	// that's stable across feature classes. S3O texture sampling
	// (S9-C4b part 3) lifts this onto the diffuse texel and team-mask
	// channel for wrecks. Map-placed features come up through Lua's
	// s11n_load_map_features gadget on BAR; this loop is empty until
	// that gadget runs (S9-C6) and harmless in the meantime.
	for (int id : featureHandler.GetActiveFeatureIDs()) {
		const CFeature* f = featureHandler.GetFeature(id);
		if (f == nullptr || f->noDraw || f->drawFlag == 0)
			continue;
		drawSolid(f, 0.45f, 0.40f, 0.32f);
	}

	pipeline->Disable();
}

#endif // RENDER_BACKEND_METAL
