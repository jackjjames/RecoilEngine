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
#include "Sim/Misc/GlobalConstants.h"
#include "System/Log/ILog.h"
#include "System/Matrix44f.h"

#include <algorithm>
#include <cstring>
#include <vector>


namespace {

struct WorldVertex { float x, y, z; };

// Stride in corner samples. The corner heightmap is (mapx+1) x (mapy+1);
// on a 10 km BAR map that's ~1025 x 1025 = ~1M verts, which is overkill
// for a first-pass visual. We subsample to stay in the ~16-64k vertex
// range so the buffer fits comfortably in a single MTLBuffer and the
// draw cost stays a rounding error; SMFGroundDrawer's real LOD system
// lands with C3c.
constexpr int kTargetVertsAcross = 192;

constexpr const char* kVertexGlsl = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 0) out vec3 vWorldPos;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 uViewProj;
    vec4 uMinHeight_MaxHeight_Unused_Unused;
} ubo;

void main() {
    vWorldPos = aPos;
    gl_Position = ubo.uViewProj * vec4(aPos, 1.0);
}
)";

constexpr const char* kFragmentGlsl = R"(#version 450
layout(location = 0) in vec3 vWorldPos;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 uViewProj;
    vec4 uMinHeight_MaxHeight_Unused_Unused;
} ubo;

void main() {
    // Height-banded grayscale + a cheap checkerboard so the mesh
    // structure is legible without lighting. Replaced by the real
    // SMFRenderState::DrawForward fragment in S9-C3b.
    float minH = ubo.uMinHeight_MaxHeight_Unused_Unused.x;
    float maxH = ubo.uMinHeight_MaxHeight_Unused_Unused.y;
    float t = clamp((vWorldPos.y - minH) / max(maxH - minH, 1.0), 0.0, 1.0);

    vec3 lo = vec3(0.18, 0.22, 0.30); // underwater-ish
    vec3 mid = vec3(0.35, 0.45, 0.30); // lowland
    vec3 hi = vec3(0.85, 0.82, 0.75); // peaks
    vec3 col = mix(lo, mid, smoothstep(0.0, 0.35, t));
    col = mix(col, hi, smoothstep(0.45, 0.9, t));

    // 128-unit checker in world XZ to make the grid readable.
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


// Build a subsampled triangle-list heightmap mesh. Returns true on
// success; `verts` is (X, h, Z) in world units, `indices` is a
// uint32 triangle list wound CCW when viewed from above (+Y).
bool BuildMesh(std::vector<WorldVertex>& verts, std::vector<uint32_t>& indices,
               float& minHOut, float& maxHOut)
{
	if (readMap == nullptr)
		return false;

	const float* hmap = readMap->GetCornerHeightMapUnsynced();
	if (hmap == nullptr)
		return false;

	const int cornersX = mapDims.mapxp1;
	const int cornersZ = mapDims.mapyp1;
	if (cornersX <= 1 || cornersZ <= 1)
		return false;

	const int strideX = std::max(1, cornersX / kTargetVertsAcross);
	const int strideZ = std::max(1, cornersZ / kTargetVertsAcross);

	// Sample grid: include every strideX-th corner; force-include the
	// last corner so the mesh covers the full map width / depth rather
	// than snapping to an interior line.
	std::vector<int> sampleX;
	std::vector<int> sampleZ;
	sampleX.reserve(cornersX / strideX + 2);
	sampleZ.reserve(cornersZ / strideZ + 2);
	for (int x = 0; x < cornersX; x += strideX)
		sampleX.push_back(x);
	if (sampleX.back() != cornersX - 1)
		sampleX.push_back(cornersX - 1);
	for (int z = 0; z < cornersZ; z += strideZ)
		sampleZ.push_back(z);
	if (sampleZ.back() != cornersZ - 1)
		sampleZ.push_back(cornersZ - 1);

	const size_t nx = sampleX.size();
	const size_t nz = sampleZ.size();
	verts.resize(nx * nz);

	float minH =  std::numeric_limits<float>::max();
	float maxH = -std::numeric_limits<float>::max();

	for (size_t j = 0; j < nz; ++j) {
		const int sz = sampleZ[j];
		for (size_t i = 0; i < nx; ++i) {
			const int sx = sampleX[i];
			const float h = hmap[sz * cornersX + sx];
			verts[j * nx + i] = {
				static_cast<float>(sx) * SQUARE_SIZE,
				h,
				static_cast<float>(sz) * SQUARE_SIZE,
			};
			minH = std::min(minH, h);
			maxH = std::max(maxH, h);
		}
	}
	minHOut = minH;
	maxHOut = maxH;

	indices.clear();
	indices.reserve((nx - 1) * (nz - 1) * 6);

	// Two triangles per quad. Winding: viewed from +Y (above), BAR/Recoil
	// treats CCW as front-facing in the GL backend. Metal without an
	// explicit cullMode defaults to no culling so this winding choice
	// is cosmetic until S9-C3c wires a depth/cull state.
	for (uint32_t j = 0; j + 1 < nz; ++j) {
		for (uint32_t i = 0; i + 1 < nx; ++i) {
			const uint32_t row0 = j * static_cast<uint32_t>(nx);
			const uint32_t row1 = (j + 1) * static_cast<uint32_t>(nx);
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

} // namespace


MetalWorldDrawer::MetalWorldDrawer()
{
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr) {
		LOG_L(L_WARNING, "[MetalWorldDrawer] no render backend");
		return;
	}
	auto& backend = *globalRendering->renderBackend;

	std::vector<WorldVertex> verts;
	std::vector<uint32_t>    indices;
	float minH = 0.0f;
	float maxH = 0.0f;
	if (!BuildMesh(verts, indices, minH, maxH)) {
		LOG_L(L_WARNING, "[MetalWorldDrawer] heightmap unavailable; drawer disabled");
		return;
	}
	indexCount = static_cast<uint32_t>(indices.size());
	LOG("[MetalWorldDrawer] mesh verts=%zu indices=%u minH=%.1f maxH=%.1f",
		verts.size(), indexCount, minH, maxH);

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

	CameraUBOLayout seedUbo{};
	// Identity ViewProj until Draw() fills this in from the active camera.
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

	PipelineDesc pd;
	pd.name = "world_ground_terrain";
	pd.vertexSource   = kVertexGlsl;
	pd.fragmentSource = kFragmentGlsl;
	pd.vertexAttributes = {
		VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = 0, .format = VertexFormat::Float3 },
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
	pipeline->BindVertexBuffer(0, *vertexBuffer);
	pipeline->DrawIndexed(PrimitiveTopology::Triangles, indexCount, IndexType::Uint32, *indexBuffer);
	pipeline->Disable();
}

#endif // RENDER_BACKEND_METAL
