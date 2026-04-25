/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalWorldDrawer.h"

#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Map/MapDimensions.h"
#include "Map/ReadMap.h"
#include "Map/SMF/SMFFormat.h"
#include "Map/SMF/SMFMapFile.h"
#include "Map/SMF/SMFReadMap.h"
#include "Rendering/Env/ISky.h"
#include "Rendering/Env/SkyLight.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/IBuffer.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/MetalGroundTextures.h"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "Rendering/Textures/ITexture.h"
#include "Rendering/Textures/MetalDXTDecoder.h"
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

constexpr uint32_t kGL_R32F  = 0x822E;
constexpr uint32_t kGL_RGBA8 = 0x8058;

// Minimap mip 0 is 1024x1024 (DXT1). 128x128 blocks, 8 bytes each.
constexpr int      kMinimapMip0Size = 1024;
constexpr size_t   kMinimapMip0Bytes = (kMinimapMip0Size / 4) * (kMinimapMip0Size / 4) * 8;

constexpr const char* kVertexGlsl = R"(#version 450
layout(location = 0) in vec2 aPosXZ;
layout(location = 1) in vec2 aUV;
layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec2 vUV;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 uViewProj;
    vec4 uMinHeight_MaxHeight_HaveDiffuse_HaveNormals;
    vec4 uSunDirXYZ_SquareSize;
    vec4 uCornerSizeXY_InvCornerSizeXY;
    vec4 uCamPos;          // xyz, w = fog density (1 / fogFar)
    vec4 uHorizonColor;    // sky horizon to blend distant terrain into
    vec4 uTileGrid;        // x = tilesPerRow, y = tilesPerCol, z = tileMapSizeX, w = tileMapSizeY
    vec4 uTileFlags;       // x = haveTiles (0/1), yzw unused
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
    vec4 uMinHeight_MaxHeight_HaveDiffuse_HaveNormals;
    vec4 uSunDirXYZ_SquareSize;
    vec4 uCornerSizeXY_InvCornerSizeXY;
    vec4 uCamPos;
    vec4 uHorizonColor;
    vec4 uTileGrid;        // x = tilesPerRow, y = tilesPerCol, z = tileMapSizeX, w = tileMapSizeY
    vec4 uTileFlags;       // x = haveTiles (0/1)
} ubo;

layout(set = 0, binding = 1) uniform sampler2D uHeight;
layout(set = 0, binding = 2) uniform sampler2D uDiffuse;
layout(set = 0, binding = 3) uniform sampler2D uNormals;
layout(set = 0, binding = 4) uniform sampler2D uTileAtlas;
layout(set = 0, binding = 5) uniform sampler2D uTileIndex;

// Sample the SMF tiled-diffuse atlas at world UV. Maps mapUV to a
// per-cell index via uTileIndex (NEAREST), then reconstructs the
// per-tile sub-UV inside the atlas. A 1-pixel inset prevents the
// atlas's bilinear filter from bleeding across tile borders.
vec3 SampleTiledDiffuse(vec2 mapUV)
{
    vec2 tileMapSize = ubo.uTileGrid.zw;
    vec2 cellPos     = mapUV * tileMapSize;
    vec2 cellUV      = (floor(cellPos) + 0.5) / tileMapSize;
    float idxF       = textureLod(uTileIndex, cellUV, 0.0).r;
    float idx        = floor(idxF + 0.5);

    float tilesPerRow = ubo.uTileGrid.x;
    float tilesPerCol = ubo.uTileGrid.y;
    float tx = mod(idx, tilesPerRow);
    float ty = floor(idx / tilesPerRow);

    vec2 subUV = fract(cellPos);
    // 1 / 32 pixel inset per side keeps bilinear filtering inside
    // the tile - drops the seam artefact when zoomed in.
    subUV = clamp(subUV, vec2(1.0 / 64.0), vec2(63.0 / 64.0));

    vec2 atlasUV;
    atlasUV.x = (tx + subUV.x) / tilesPerRow;
    atlasUV.y = (ty + subUV.y) / tilesPerCol;
    return texture(uTileAtlas, atlasUV).rgb;
}

// Cheap value-noise hash. Two-octave sum gives a recognisable
// crease/dust pattern at the texel scale without sampling any extra
// textures - keeps the terrain looking like ground rather than an
// aggressively-upscaled minimap.
float hash21(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float ValueNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Fallback: build a world-space normal from the heightmap by
// central-differencing four neighbour samples in UV space. Used only
// when the pre-baked map normals aren't available (haveNormals == 0).
vec3 ComputeTerrainNormalFromHeight(vec2 uv)
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
    // Albedo source priority:
    //   1) SMF tiled diffuse from the .smt file (real per-cell
    //      ground textures, S9-C3c)
    //   2) SMF minimap top mip as full-map diffuse stand-in
    //   3) Hypsometric height-band ramp (synthetic test maps)
    float haveTiles   = ubo.uTileFlags.x;
    float haveDiffuse = ubo.uMinHeight_MaxHeight_HaveDiffuse_HaveNormals.z;
    vec3 albedo;
    if (haveTiles > 0.5) {
        albedo = SampleTiledDiffuse(vUV);
    } else if (haveDiffuse > 0.5) {
        albedo = texture(uDiffuse, vUV).rgb;
    } else {
        float minH = ubo.uMinHeight_MaxHeight_HaveDiffuse_HaveNormals.x;
        float maxH = ubo.uMinHeight_MaxHeight_HaveDiffuse_HaveNormals.y;
        float t = clamp((vWorldPos.y - minH) / max(maxH - minH, 1.0), 0.0, 1.0);
        vec3 lo = vec3(0.14, 0.20, 0.30);
        vec3 mid = vec3(0.30, 0.40, 0.22);
        vec3 hi = vec3(0.82, 0.80, 0.72);
        albedo = mix(lo, mid, smoothstep(0.00, 0.30, t));
        albedo = mix(albedo, hi, smoothstep(0.55, 0.95, t));
    }

    // Normal: prefer the pre-baked per-texel center normals uploaded
    // from CReadMap::GetCenterNormalsUnsynced() (one normal per
    // heightmap square, packed in RGBA8 with xyz*0.5+0.5). This is
    // higher frequency than central-differencing the coarse vertex
    // mesh's heightmap sampler. Fall back to heightmap differencing if
    // the normals texture failed to upload.
    float haveNormals = ubo.uMinHeight_MaxHeight_HaveDiffuse_HaveNormals.w;
    vec3 N;
    if (haveNormals > 0.5) {
        vec3 n = texture(uNormals, vUV).xyz * 2.0 - 1.0;
        N = normalize(n);
    } else {
        N = ComputeTerrainNormalFromHeight(vUV);
    }

    // Lambertian + constant ambient, sun direction is world-space "to
    // the light". Half-Lambert keeps slopes from pitch-black, matching
    // BAR's usual shaded-terrain look better than pure N.L.
    vec3 L = normalize(ubo.uSunDirXYZ_SquareSize.xyz);
    float ndl = max(dot(N, L), 0.0);
    float half_lambert = 0.5 * ndl + 0.5 * max(dot(N, L), -0.2);
    float diffuse = clamp(half_lambert, 0.0, 1.0);

    // Two-octave detail noise sampled in world XZ space. Skipped
    // when SMF tiled diffuse is active because the per-cell ground
    // tiles already carry real surface detail; only the minimap /
    // hypsometric paths need this to mask their low-frequency
    // smoothness. Modulates brightness only (no chroma shift) so a
    // desert reads as dusty desert and a green map as patchy grass.
    vec3 detailedAlbedo = albedo;
    if (haveTiles < 0.5) {
        float n0 = ValueNoise(vWorldPos.xz * 0.07);
        float n1 = ValueNoise(vWorldPos.xz * 0.31 + 17.0);
        float detail = n0 * 0.65 + n1 * 0.35;
        detail = mix(0.82, 1.18, detail);
        detailedAlbedo = albedo * detail;
    }

    vec3 sunColor = vec3(1.00, 0.96, 0.88);
    vec3 ambColor = vec3(0.35, 0.38, 0.45);
    vec3 col = detailedAlbedo * (sunColor * diffuse + ambColor);

    // Distance-fade into the horizon colour. The fog factor is an
    // exponential of the squared world-space distance: hides the
    // mesh-edge clip line at the back of the map, ties the terrain
    // visually to MetalSkyPass's atmospheric horizon stop, and
    // matches the look BAR's GL fog stanza produces with the
    // mapInfo->atmosphere fogStart/fogEnd defaults. fogDensity is
    // pre-baked into uCamPos.w on the CPU so the per-fragment math
    // stays a single mul + exp.
    float fogDensity = ubo.uCamPos.w;
    float dist = length(vWorldPos - ubo.uCamPos.xyz);
    float fog  = 1.0 - exp(-dist * dist * (fogDensity * fogDensity) * 1.4);
    fog = clamp(fog, 0.0, 0.85); // never let the foreground fully wash out
    col = mix(col, ubo.uHorizonColor.rgb, fog);

    fragColor = vec4(col, 1.0);
}
)";


struct alignas(16) CameraUBOLayout {
	float viewProj[16];
	float params[4];            // minHeight, maxHeight, haveDiffuse(0/1), haveNormals(0/1)
	float sunDirAndSquare[4];   // sun.xyz, SQUARE_SIZE
	float cornerSize[4];        // cornersX, cornersZ, 1/cornersX, 1/cornersZ
	float camPos[4];            // xyz, w = fog density (1 / fogFar)
	float horizonColor[4];      // ties terrain fade to MetalSkyPass horizon
	float tileGrid[4];          // tilesPerRow, tilesPerCol, tileMapSizeX, tileMapSizeY
	float tileFlags[4];         // x = haveTiles (0 / 1)
};


// SMF-minimap DXT1 -> RGBA8 lives on MetalDXT::DecompressBC1Image now,
// shared with the S3O / feature texture path.

// Pack world-space float3 normals (-1..1) into RGBA8 (0..255, xyz*0.5
// + 0.5, alpha=255). The fragment shader unpacks symmetrically. Avoids
// requiring RGBA16F support on every Metal device - 8-bit normals are
// fine for diffuse-only lighting at the map resolution.
std::vector<uint8_t> PackCenterNormalsRGBA8(const float3* src, int w, int h)
{
	const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
	std::vector<uint8_t> out(n * 4, 0);
	for (size_t i = 0; i < n; ++i) {
		const float3 nrm = src[i];
		const uint8_t r = static_cast<uint8_t>(std::clamp(nrm.x * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
		const uint8_t g = static_cast<uint8_t>(std::clamp(nrm.y * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
		const uint8_t b = static_cast<uint8_t>(std::clamp(nrm.z * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
		out[i * 4 + 0] = r;
		out[i * 4 + 1] = g;
		out[i * 4 + 2] = b;
		out[i * 4 + 3] = 255;
	}
	return out;
}

// Read the SMF-embedded minimap (top mip, 1024x1024 DXT1) and
// decompress it to a 1024x1024 RGBA8 buffer. Returns empty on
// failure - the caller falls back to the hypsometric colour ramp.
std::vector<uint8_t> LoadMinimapRGBA8(CReadMap* rm)
{
	auto* smf = dynamic_cast<CSMFReadMap*>(rm);
	if (smf == nullptr)
		return {};

	std::vector<uint8_t> dxt1(MINIMAP_SIZE, 0);
	smf->GetMapFile().ReadMinimap(dxt1.data());

	std::vector<uint8_t> rgba(static_cast<size_t>(kMinimapMip0Size) * kMinimapMip0Size * 4, 0);
	MetalDXT::DecompressBC1Image(dxt1.data(), rgba.data(), kMinimapMip0Size, kMinimapMip0Size);
	return rgba;
}


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

	// --- Diffuse (SMF minimap as full-map texture). Stays optional:
	// if the SMF has no minimap (only true for synthetic test maps) the
	// fragment shader falls back to the hypsometric band. Linear filter
	// + clamp-to-edge is the same setup GL uses for the minimap's
	// coarser mips, and we only upload mip 0 since at this resolution
	// the main draw view is always zoomed out.
	{
		std::vector<uint8_t> rgba = LoadMinimapRGBA8(readMap);
		if (!rgba.empty()) {
			GL::TextureCreationParams mmParams;
			mmParams.linearTextureFilter = true;
			mmParams.linearMipMapFilter  = false;
			mmParams.reqNumLevels = 1;
			diffuseTexture = backend.CreateTexture2D(
				int2(kMinimapMip0Size, kMinimapMip0Size), kGL_RGBA8, mmParams, /*wantCompress=*/false);
			if (diffuseTexture && diffuseTexture->IsValid()) {
				diffuseTexture->UploadImage(rgba.data());
				LOG("[MetalWorldDrawer] minimap diffuse loaded (%dx%d RGBA8)",
					kMinimapMip0Size, kMinimapMip0Size);
			} else {
				LOG_L(L_WARNING, "[MetalWorldDrawer] minimap texture creation failed");
				diffuseTexture.reset();
			}
		} else {
			LOG_L(L_WARNING, "[MetalWorldDrawer] minimap unavailable; falling back to hypsometric colours");
		}
	}

	// --- SMF tiled diffuse. Optional: when the .smt files load
	// successfully MetalGroundTextures owns an atlas + per-cell
	// index lookup that replaces the minimap-as-diffuse stand-in.
	// Falls back silently on non-SMF maps or when the atlas would
	// exceed the texture-dimension cap; the fragment shader keys
	// off uTileFlags.x to pick the right path.
	groundTextures = std::make_unique<MetalGroundTextures>();
	if (!groundTextures->IsValid()) {
		groundTextures.reset();
	}

	// --- Pre-baked per-texel center normals. One RGB8 per heightmap
	// square (mapx x mapy). Sampled at aUV with linear filtering so
	// the shading between texels blends smoothly. Caller must fall
	// back to the vertex-stage heightmap derivative if this fails.
	{
		const float3* centerNormals = readMap->GetCenterNormalsUnsynced();
		if (centerNormals != nullptr && mapDims.mapx > 0 && mapDims.mapy > 0) {
			std::vector<uint8_t> packed = PackCenterNormalsRGBA8(centerNormals, mapDims.mapx, mapDims.mapy);

			GL::TextureCreationParams nmParams;
			nmParams.linearTextureFilter = true;
			nmParams.linearMipMapFilter  = false;
			nmParams.reqNumLevels = 1;
			normalsTexture = backend.CreateTexture2D(
				int2(mapDims.mapx, mapDims.mapy), kGL_RGBA8, nmParams, /*wantCompress=*/false);
			if (normalsTexture && normalsTexture->IsValid()) {
				normalsTexture->UploadImage(packed.data());
				LOG("[MetalWorldDrawer] center normals loaded (%dx%d RGBA8)", mapDims.mapx, mapDims.mapy);
			} else {
				LOG_L(L_WARNING, "[MetalWorldDrawer] normals texture creation failed");
				normalsTexture.reset();
			}
		}
	}

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
		const float3& p = cam->GetPos();
		ubo.camPos[0] = p.x; ubo.camPos[1] = p.y; ubo.camPos[2] = p.z;
	} else {
		ubo.viewProj[0]  = 1.0f;
		ubo.viewProj[5]  = 1.0f;
		ubo.viewProj[10] = 1.0f;
		ubo.viewProj[15] = 1.0f;
	}
	ubo.params[0] = cachedMinHeight;
	ubo.params[1] = cachedMaxHeight;
	ubo.params[2] = (diffuseTexture && diffuseTexture->IsValid()) ? 1.0f : 0.0f;
	ubo.params[3] = (normalsTexture && normalsTexture->IsValid()) ? 1.0f : 0.0f;

	// Sun direction: take from the live sky (CNullSky wires ISkyLight on
	// init from mapInfo->light.sunDir). Fall back to a reasonable
	// overhead-ish direction if the sky hasn't initialised yet.
	float4 sunDir = float4(0.3f, 0.85f, 0.4f, 1.0f);
	float3 mapSkyCol = float3(0.62f, 0.72f, 0.80f);
	float3 mapSunCol = float3(1.00f, 0.92f, 0.78f);
	const auto& skyPtr = ISky::GetSky();
	if (skyPtr != nullptr) {
		if (skyPtr->GetLight() != nullptr)
			sunDir = skyPtr->GetLight()->GetLightDir();
		mapSkyCol = skyPtr->skyColor;
		mapSunCol = skyPtr->sunColor;
	}
	ubo.sunDirAndSquare[0] = sunDir.x;
	ubo.sunDirAndSquare[1] = sunDir.y;
	ubo.sunDirAndSquare[2] = sunDir.z;
	ubo.sunDirAndSquare[3] = static_cast<float>(SQUARE_SIZE);

	ubo.cornerSize[0] = static_cast<float>(cachedCornersX);
	ubo.cornerSize[1] = static_cast<float>(cachedCornersZ);
	ubo.cornerSize[2] = (cachedCornersX > 0) ? 1.0f / static_cast<float>(cachedCornersX) : 0.0f;
	ubo.cornerSize[3] = (cachedCornersZ > 0) ? 1.0f / static_cast<float>(cachedCornersZ) : 0.0f;

	// Fog density: derive from the larger map axis so the falloff
	// reaches saturation roughly where the terrain mesh ends. Matches
	// BAR's GL atmosphere fogEnd defaults closely enough for a coarse
	// pass; a future slice can route mapInfo->atmosphere{} fogStart /
	// fogEnd through here once we audit it isn't common-code-coupled.
	const float mapDiag = std::sqrt(
		static_cast<float>(mapDims.mapx * mapDims.mapx + mapDims.mapy * mapDims.mapy)
	) * static_cast<float>(SQUARE_SIZE);
	const float fogFar  = std::max(mapDiag * 0.7f, 1024.0f);
	ubo.camPos[3] = 1.0f / fogFar;

	const float3 horizon = mapSkyCol * 0.35f + mapSunCol * 0.55f + float3(0.05f, 0.05f, 0.04f);
	ubo.horizonColor[0] = horizon.x;
	ubo.horizonColor[1] = horizon.y;
	ubo.horizonColor[2] = horizon.z;
	ubo.horizonColor[3] = 1.0f;

	const bool haveTiles = (groundTextures != nullptr) && groundTextures->IsValid();
	ubo.tileGrid[0] = haveTiles ? static_cast<float>(groundTextures->GetTilesPerRow())   : 1.0f;
	ubo.tileGrid[1] = haveTiles ? static_cast<float>(groundTextures->GetTilesPerCol())   : 1.0f;
	ubo.tileGrid[2] = haveTiles ? static_cast<float>(groundTextures->GetTileMapSizeX())  : 1.0f;
	ubo.tileGrid[3] = haveTiles ? static_cast<float>(groundTextures->GetTileMapSizeY())  : 1.0f;
	ubo.tileFlags[0] = haveTiles ? 1.0f : 0.0f;

	uniformBuffer->UpdateData(&ubo, sizeof(ubo), 0);

	pipeline->Enable();
	pipeline->BindUniformBuffer(0, *uniformBuffer, 0, sizeof(ubo));
	pipeline->BindTexture(1, *heightmapTexture);
	if (diffuseTexture && diffuseTexture->IsValid())
		pipeline->BindTexture(2, *diffuseTexture);
	else
		pipeline->BindTexture(2, *heightmapTexture);   // placeholder - fragment won't sample it (haveDiffuse=0)
	if (normalsTexture && normalsTexture->IsValid())
		pipeline->BindTexture(3, *normalsTexture);
	else
		pipeline->BindTexture(3, *heightmapTexture);   // placeholder - fragment won't sample it (haveNormals=0)
	if (haveTiles && groundTextures->GetAtlasTexture() != nullptr) {
		pipeline->BindTexture(4, *groundTextures->GetAtlasTexture());
		pipeline->BindTexture(5, *groundTextures->GetTileIndexTexture());
	} else {
		// Placeholders so Metal's argument-buffer slot validation
		// stays happy; the fragment shader gates the actual sample
		// on uTileFlags.x.
		pipeline->BindTexture(4, *heightmapTexture);
		pipeline->BindTexture(5, *heightmapTexture);
	}
	pipeline->BindVertexBuffer(0, *vertexBuffer);
	pipeline->DrawIndexed(PrimitiveTopology::Triangles, indexCount, IndexType::Uint32, *indexBuffer);
	pipeline->Disable();
}

#endif // RENDER_BACKEND_METAL
