/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalWorldDrawer.h"

#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Map/MapDimensions.h"
#include "Map/ReadMap.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/IBuffer.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "Rendering/Textures/ITexture.h"
#include "Rendering/Textures/TextureCreationParams.hpp"
#include "Sim/Misc/GlobalConstants.h"
#include "System/Log/ILog.h"
#include "System/Matrix44f.h"
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

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 uViewProj;
    vec4 uMinHeight_MaxHeight_Unused_Unused;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D uHeight;

void main() {
    // textureLod keeps the sample deterministic regardless of any
    // implicit LOD the driver would pick for vertex-stage sampling.
    float h = textureLod(uHeight, aUV, 0.0).r;
    vec3 world = vec3(aPosXZ.x, h, aPosXZ.y);
    vWorldPos = world;
    gl_Position = ubo.uViewProj * vec4(world, 1.0);
}
)";

constexpr const char* kFragmentGlsl = R"(#version 450
layout(location = 0) in vec3 vWorldPos;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 uViewProj;
    vec4 uMinHeight_MaxHeight_Unused_Unused;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D uHeight;

void main() {
    // Height-banded grayscale + a cheap checkerboard so the mesh
    // structure is legible without lighting. Replaced by the real
    // SMFRenderState::DrawForward fragment in S9-C3b.
    float minH = ubo.uMinHeight_MaxHeight_Unused_Unused.x;
    float maxH = ubo.uMinHeight_MaxHeight_Unused_Unused.y;
    float t = clamp((vWorldPos.y - minH) / max(maxH - minH, 1.0), 0.0, 1.0);

    vec3 lo = vec3(0.18, 0.22, 0.30);
    vec3 mid = vec3(0.35, 0.45, 0.30);
    vec3 hi = vec3(0.85, 0.82, 0.75);
    vec3 col = mix(lo, mid, smoothstep(0.0, 0.35, t));
    col = mix(col, hi, smoothstep(0.45, 0.9, t));

    vec2 g = floor(vWorldPos.xz / 128.0);
    float checker = mod(g.x + g.y, 2.0);
    col *= mix(0.9, 1.1, checker);

    fragColor = vec4(col, 1.0);
}
)";


struct alignas(16) CameraUBOLayout {
	float viewProj[16];
	float params[4]; // minHeight, maxHeight, 0, 0
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

	// --- Uniform buffer (viewProj + height range).
	CameraUBOLayout seedUbo{};
	seedUbo.viewProj[0]  = 1.0f;
	seedUbo.viewProj[5]  = 1.0f;
	seedUbo.viewProj[10] = 1.0f;
	seedUbo.viewProj[15] = 1.0f;
	seedUbo.params[0] = minH;
	seedUbo.params[1] = maxH;
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

	uniformBuffer->UpdateData(&ubo, sizeof(ubo), 0);

	pipeline->Enable();
	pipeline->BindUniformBuffer(0, *uniformBuffer, 0, sizeof(ubo));
	pipeline->BindTexture(1, *heightmapTexture);
	pipeline->BindVertexBuffer(0, *vertexBuffer);
	pipeline->DrawIndexed(PrimitiveTopology::Triangles, indexCount, IndexType::Uint32, *indexBuffer);
	pipeline->Disable();
}

#endif // RENDER_BACKEND_METAL
