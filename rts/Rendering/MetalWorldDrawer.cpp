/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalWorldDrawer.h"

#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Map/MapDimensions.h"
#include "Map/ReadMap.h"
#include "Rendering/Env/ISky.h"
#include "Rendering/Env/SkyLight.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/IBuffer.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "Rendering/Textures/ITexture.h"
#include "Rendering/Textures/TextureCreationParams.hpp"
#include "Sim/Misc/GlobalConstants.h"
#include "System/Log/ILog.h"
#include "System/Matrix44f.h"
#include "System/float4.h"
#include "System/type2.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>


namespace {

struct WorldVertex { float x, z; float u, v; };

// Mesh resolution (grid vertices across). Decoupled from the corner
// heightmap density: the vertex shader samples the R32F heightmap at
// (u, v) to get Y. 193x193 = ~37k verts, ~73k triangles - a rounding
// error for Metal on Apple silicon and well below what the GL backend
// rasterises for BAR's SMFGroundDrawer. SMF's real LOD lands with
// C3c; this stays static until then.
constexpr int kGridVertsAcross = 193;

constexpr uint32_t kGL_R32F = 0x822E;

constexpr const char* kVertexGlsl = R"(#version 450
layout(location = 0) in vec2 aPosXZ;
layout(location = 1) in vec2 aUV;
layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec2 vUV;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 uViewProj;
    vec4 uMinHeight_MaxHeight_Unused_Unused;
    vec4 uSunDirXYZ_SquareSize;
    vec4 uCornerSizeXY_InvCornerSizeXY;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D uHeight;

void main() {
    // textureLod keeps the sample deterministic regardless of any
    // implicit LOD the driver would pick for vertex-stage sampling.
    float h = textureLod(uHeight, aUV, 0.0).r;
    vec3 world = vec3(aPosXZ.x, h, aPosXZ.y);
    vWorldPos = world;
    vUV = aUV;
    gl_Position = ubo.uViewProj * vec4(world, 1.0);
}
)";

constexpr const char* kFragmentGlsl = R"(#version 450
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 uViewProj;
    vec4 uMinHeight_MaxHeight_Unused_Unused;
    vec4 uSunDirXYZ_SquareSize;
    vec4 uCornerSizeXY_InvCornerSizeXY;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D uHeight;

// Build a world-space normal from the heightmap by central-differencing
// four neighbour samples in UV space. The world-space step for one
// heightmap texel is SQUARE_SIZE (pxOrigin -> px+1 on the corner grid);
// dy is raw heightmap delta. Cross product gives the upward normal.
vec3 ComputeTerrainNormal(vec2 uv)
{
    vec2 texel = ubo.uCornerSizeXY_InvCornerSizeXY.zw;
    float hL = textureLod(uHeight, uv + vec2(-texel.x,  0.0), 0.0).r;
    float hR = textureLod(uHeight, uv + vec2( texel.x,  0.0), 0.0).r;
    float hD = textureLod(uHeight, uv + vec2( 0.0, -texel.y), 0.0).r;
    float hU = textureLod(uHeight, uv + vec2( 0.0,  texel.y), 0.0).r;

    float sq = ubo.uSunDirXYZ_SquareSize.w;
    vec3 dx = vec3(2.0 * sq, hR - hL, 0.0);
    vec3 dz = vec3(0.0, hU - hD, 2.0 * sq);
    return normalize(cross(dz, dx));
}

void main() {
    float minH = ubo.uMinHeight_MaxHeight_Unused_Unused.x;
    float maxH = ubo.uMinHeight_MaxHeight_Unused_Unused.y;
    float t = clamp((vWorldPos.y - minH) / max(maxH - minH, 1.0), 0.0, 1.0);

    vec3 lo = vec3(0.14, 0.20, 0.30);  // underwater-ish
    vec3 mid = vec3(0.30, 0.40, 0.22); // lowland
    vec3 hi = vec3(0.82, 0.80, 0.72);  // peaks
    vec3 albedo = mix(lo, mid, smoothstep(0.00, 0.30, t));
    albedo = mix(albedo, hi, smoothstep(0.55, 0.95, t));

    // Lambertian + constant ambient, sun direction is world-space "to the
    // light". Half-Lambert blend keeps slopes from pitch-black, which
    // matches BAR's usual look better than pure N.L.
    vec3 N = ComputeTerrainNormal(vUV);
    vec3 L = normalize(ubo.uSunDirXYZ_SquareSize.xyz);
    float ndl = max(dot(N, L), 0.0);
    float half_lambert = 0.5 * ndl + 0.5 * max(dot(N, L), -0.2);
    float diffuse = clamp(half_lambert, 0.0, 1.0);

    vec3 sunColor = vec3(1.00, 0.96, 0.88);
    vec3 ambColor = vec3(0.28, 0.30, 0.38);
    vec3 col = albedo * (sunColor * diffuse + ambColor);

    // Faint 128-unit grid so the mesh structure stays legible until the
    // real SMF tile textures land (C3b).
    vec2 g = floor(vWorldPos.xz / 128.0);
    float checker = mod(g.x + g.y, 2.0);
    col *= mix(0.96, 1.04, checker);

    fragColor = vec4(col, 1.0);
}
)";


struct alignas(16) CameraUBOLayout {
	float viewProj[16];
	float params[4];            // minHeight, maxHeight, 0, 0
	float sunDirAndSquare[4];   // sun.xyz, SQUARE_SIZE
	float cornerSize[4];        // cornersX, cornersZ, 1/cornersX, 1/cornersZ
};


// Build a flat XZ grid mesh that covers the full map footprint. Heights
// are resolved in the vertex shader by sampling uHeight at aUV, so the
// vertex buffer carries XZ + UV only. Returns true on success.
bool BuildGridMesh(std::vector<WorldVertex>& verts, std::vector<uint32_t>& indices)
{
	const int vertsX = kGridVertsAcross;
	const int vertsZ = kGridVertsAcross;
	if (vertsX < 2 || vertsZ < 2)
		return false;

	const float mapWidth  = static_cast<float>(mapDims.mapx) * SQUARE_SIZE;
	const float mapDepth  = static_cast<float>(mapDims.mapy) * SQUARE_SIZE;

	verts.resize(vertsX * vertsZ);
	for (int j = 0; j < vertsZ; ++j) {
		// Parametric [0, 1] across the grid for both world-space XZ and
		// heightmap UV sampling; the texture is clamp-to-edge so the
		// outermost ring snaps to the corner row/column (GL and Metal
		// agree here).
		const float tz = static_cast<float>(j) / static_cast<float>(vertsZ - 1);
		for (int i = 0; i < vertsX; ++i) {
			const float tx = static_cast<float>(i) / static_cast<float>(vertsX - 1);
			verts[j * vertsX + i] = {
				tx * mapWidth,
				tz * mapDepth,
				tx,
				tz,
			};
		}
	}

	indices.clear();
	indices.reserve((vertsX - 1) * (vertsZ - 1) * 6);

	// Two triangles per quad, CCW when viewed from +Y.
	for (uint32_t j = 0; j + 1 < static_cast<uint32_t>(vertsZ); ++j) {
		for (uint32_t i = 0; i + 1 < static_cast<uint32_t>(vertsX); ++i) {
			const uint32_t row0 = j * static_cast<uint32_t>(vertsX);
			const uint32_t row1 = (j + 1) * static_cast<uint32_t>(vertsX);
			const uint32_t v00 = row0 + i;
			const uint32_t v10 = row0 + i + 1;
			const uint32_t v01 = row1 + i;
			const uint32_t v11 = row1 + i + 1;

			indices.push_back(v00);
			indices.push_back(v01);
			indices.push_back(v10);

			indices.push_back(v10);
			indices.push_back(v01);
			indices.push_back(v11);
		}
	}

	return true;
}

// Compute min/max height over the corner heightmap for the UBO so the
// fragment stage can band-colour against true map extents.
bool ComputeHeightRange(const float* hmap, int cornersX, int cornersZ, float& minHOut, float& maxHOut)
{
	if (hmap == nullptr || cornersX <= 0 || cornersZ <= 0)
		return false;

	float minH =  std::numeric_limits<float>::max();
	float maxH = -std::numeric_limits<float>::max();

	const size_t n = static_cast<size_t>(cornersX) * static_cast<size_t>(cornersZ);
	for (size_t k = 0; k < n; ++k) {
		const float h = hmap[k];
		if (h < minH) minH = h;
		if (h > maxH) maxH = h;
	}
	minHOut = minH;
	maxHOut = maxH;
	return true;
}

} // namespace


MetalWorldDrawer::MetalWorldDrawer()
{
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr) {
		LOG_L(L_WARNING, "[MetalWorldDrawer] no render backend");
		return;
	}
	if (readMap == nullptr) {
		LOG_L(L_WARNING, "[MetalWorldDrawer] readMap unavailable; drawer disabled");
		return;
	}
	auto& backend = *globalRendering->renderBackend;

	const float* hmap = readMap->GetCornerHeightMapUnsynced();
	const int cornersX = mapDims.mapxp1;
	const int cornersZ = mapDims.mapyp1;
	if (hmap == nullptr || cornersX <= 1 || cornersZ <= 1) {
		LOG_L(L_WARNING, "[MetalWorldDrawer] heightmap unavailable; drawer disabled");
		return;
	}

	// --- Flat grid mesh (XZ + UV only).
	std::vector<WorldVertex> verts;
	std::vector<uint32_t>    indices;
	if (!BuildGridMesh(verts, indices)) {
		LOG_L(L_WARNING, "[MetalWorldDrawer] mesh build failed");
		return;
	}
	indexCount = static_cast<uint32_t>(indices.size());

	float minH = 0.0f;
	float maxH = 0.0f;
	ComputeHeightRange(hmap, cornersX, cornersZ, minH, maxH);

	LOG("[MetalWorldDrawer] grid verts=%zu indices=%u corners=%dx%d minH=%.1f maxH=%.1f",
		verts.size(), indexCount, cornersX, cornersZ, minH, maxH);

	vertexBuffer = backend.CreateBuffer(verts.size() * sizeof(WorldVertex), verts.data());
	if (!vertexBuffer || !vertexBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalWorldDrawer] vertex buffer creation failed");
		return;
	}

	indexBuffer = backend.CreateBuffer(indices.size() * sizeof(uint32_t), indices.data());
	if (!indexBuffer || !indexBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalWorldDrawer] index buffer creation failed");
		return;
	}

	// --- R32F heightmap texture uploaded at full corner resolution.
	// Vertex shader samples with linear filtering so the displaced mesh
	// interpolates smoothly between corner samples; clamp-to-edge on the
	// outer ring avoids wrap artefacts at the seams.
	GL::TextureCreationParams tcp;
	tcp.linearTextureFilter = true;
	tcp.linearMipMapFilter  = false;
	tcp.reqNumLevels = 1;
	heightmapTexture = backend.CreateTexture2D(int2(cornersX, cornersZ), kGL_R32F, tcp, /*wantCompress=*/false);
	if (!heightmapTexture || !heightmapTexture->IsValid()) {
		LOG_L(L_ERROR, "[MetalWorldDrawer] heightmap texture creation failed");
		return;
	}
	heightmapTexture->UploadImage(hmap);

	// --- Uniform buffer (viewProj + height range + sun / corner info).
	CameraUBOLayout seedUbo{};
	seedUbo.viewProj[0]  = 1.0f;
	seedUbo.viewProj[5]  = 1.0f;
	seedUbo.viewProj[10] = 1.0f;
	seedUbo.viewProj[15] = 1.0f;
	seedUbo.params[0] = minH;
	seedUbo.params[1] = maxH;
	seedUbo.sunDirAndSquare[1] = 1.0f; // overhead sun until Draw() fills from ISkyLight
	seedUbo.sunDirAndSquare[3] = static_cast<float>(SQUARE_SIZE);
	seedUbo.cornerSize[0] = static_cast<float>(cornersX);
	seedUbo.cornerSize[1] = static_cast<float>(cornersZ);
	seedUbo.cornerSize[2] = 1.0f / static_cast<float>(cornersX);
	seedUbo.cornerSize[3] = 1.0f / static_cast<float>(cornersZ);
	uniformBuffer = backend.CreateBuffer(sizeof(CameraUBOLayout), &seedUbo);
	if (!uniformBuffer || !uniformBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalWorldDrawer] uniform buffer creation failed");
		return;
	}

	// --- Pipeline. Interleaved vec2 XZ + vec2 UV.
	PipelineDesc pd;
	pd.name = "world_ground_terrain";
	pd.vertexSource   = kVertexGlsl;
	pd.fragmentSource = kFragmentGlsl;
	pd.vertexAttributes = {
		VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = 0,                 .format = VertexFormat::Float2 },
		VertexAttribute{ .location = 1, .bufferSlot = 0, .offset = sizeof(float) * 2, .format = VertexFormat::Float2 },
	};
	pd.vertexBindings = {
		VertexBindingLayout{ .slot = 0, .stride = sizeof(WorldVertex) },
	};

	pipeline = backend.CreatePipeline(pd);
	if (!pipeline || !pipeline->IsValid()) {
		LOG_L(L_ERROR, "[MetalWorldDrawer] pipeline link failed: %s",
			pipeline ? pipeline->GetLog().c_str() : "(null)");
		return;
	}

	cachedMinHeight = minH;
	cachedMaxHeight = maxH;
	cachedCornersX  = cornersX;
	cachedCornersZ  = cornersZ;
	valid = true;
}

MetalWorldDrawer::~MetalWorldDrawer() = default;

void MetalWorldDrawer::Draw() const
{
	if (!valid)
		return;

	CameraUBOLayout ubo{};

	const CCamera* cam = CCameraHandler::GetActiveCamera();
	if (cam != nullptr) {
		std::memcpy(ubo.viewProj, cam->GetViewProjectionMatrix().m, sizeof(ubo.viewProj));
	} else {
		ubo.viewProj[0]  = 1.0f;
		ubo.viewProj[5]  = 1.0f;
		ubo.viewProj[10] = 1.0f;
		ubo.viewProj[15] = 1.0f;
	}
	ubo.params[0] = cachedMinHeight;
	ubo.params[1] = cachedMaxHeight;

	// Sun direction: take from the live sky (CNullSky wires ISkyLight on
	// init from mapInfo->light.sunDir). Fall back to a reasonable
	// overhead-ish direction if the sky hasn't initialised yet.
	float4 sunDir = float4(0.3f, 0.85f, 0.4f, 1.0f);
	const auto& skyPtr = ISky::GetSky();
	if (skyPtr != nullptr && skyPtr->GetLight() != nullptr)
		sunDir = skyPtr->GetLight()->GetLightDir();
	ubo.sunDirAndSquare[0] = sunDir.x;
	ubo.sunDirAndSquare[1] = sunDir.y;
	ubo.sunDirAndSquare[2] = sunDir.z;
	ubo.sunDirAndSquare[3] = static_cast<float>(SQUARE_SIZE);

	ubo.cornerSize[0] = static_cast<float>(cachedCornersX);
	ubo.cornerSize[1] = static_cast<float>(cachedCornersZ);
	ubo.cornerSize[2] = (cachedCornersX > 0) ? 1.0f / static_cast<float>(cachedCornersX) : 0.0f;
	ubo.cornerSize[3] = (cachedCornersZ > 0) ? 1.0f / static_cast<float>(cachedCornersZ) : 0.0f;

	uniformBuffer->UpdateData(&ubo, sizeof(ubo), 0);

	pipeline->Enable();
	pipeline->BindUniformBuffer(0, *uniformBuffer, 0, sizeof(ubo));
	pipeline->BindTexture(1, *heightmapTexture);
	pipeline->BindVertexBuffer(0, *vertexBuffer);
	pipeline->DrawIndexed(PrimitiveTopology::Triangles, indexCount, IndexType::Uint32, *indexBuffer);
	pipeline->Disable();
}

#endif // RENDER_BACKEND_METAL
