/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalMinimap.h"

#include "Game/GlobalUnsynced.h"
#include "Game/UI/MiniMap.h"
#include "Map/MapDimensions.h"
#include "Map/ReadMap.h"
#include "Map/SMF/SMFFormat.h"
#include "Map/SMF/SMFMapFile.h"
#include "Map/SMF/SMFReadMap.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/IBuffer.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/IRenderTarget.h"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "Rendering/Textures/ITexture.h"
#include "Rendering/Textures/MetalDXTDecoder.h"
#include "Rendering/Textures/TextureCreationParams.hpp"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitHandler.h"
#include "System/Log/ILog.h"
#include "System/type2.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>


namespace {

constexpr uint32_t kGL_RGBA8 = 0x8058;
constexpr int      kMinimapMip0 = 1024;

// Background quad is two triangles, vec2 pos + vec2 uv. Six verts.
struct BgVertex {
	float pos[2];
	float uv[2];
};

// Coloured dot vertices: 6 per dot. Pre-baked in NDC by the CPU
// each frame (no per-vertex transform needed).
struct DotVertex {
	float pos[2];
	float color[4];
};

constexpr const char* kBgVertexGlsl = R"(#version 450
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 0) out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

constexpr const char* kBgFragmentGlsl = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;
layout(set = 0, binding = 1) uniform sampler2D uMinimap;

void main() {
    vec3 col = texture(uMinimap, vUV).rgb;
    // Framed viewport treatment: dark translucent outer lip, bright
    // inner bevel, then the map texture. This keeps the Metal
    // minimap visually separated from the world like BAR's PIP/minimap
    // widgets instead of reading as raw map texture pasted on top.
    vec2 d = min(vUV, 1.0 - vUV);
    float edge = min(d.x, d.y);
    float alpha = 0.94;
    if (edge < 0.018) {
        col = vec3(0.015, 0.020, 0.025);
        alpha = 0.88;
    } else if (edge < 0.028) {
        col = vec3(0.82, 0.88, 0.95);
        alpha = 0.98;
    } else {
        // Mild contrast lift to make the embedded SMF minimap read
        // crisper, closer to BAR's SSAA/minimap clarity target.
        col = clamp((col - vec3(0.5)) * 1.08 + vec3(0.5), 0.0, 1.0);
    }
    fragColor = vec4(col, alpha);
}
)";

constexpr const char* kDotVertexGlsl = R"(#version 450
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 0) out vec4 vColor;
void main() {
    vColor = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

constexpr const char* kDotFragmentGlsl = R"(#version 450
layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 fragColor;
void main() {
    fragColor = vColor;
}
)";


// Read the SMF embedded minimap and decompress mip 0 to RGBA8. Same
// payload MetalWorldDrawer pulls; duplicating the DXT1 pass here
// keeps MetalMinimap self-contained at the cost of ~4 MB temp during
// init. Returns empty on non-SMF readMap.
std::vector<uint8_t> LoadMinimapRGBA8()
{
	auto* smf = dynamic_cast<CSMFReadMap*>(readMap);
	if (smf == nullptr)
		return {};

	std::vector<uint8_t> dxt1(MINIMAP_SIZE, 0);
	smf->GetMapFile().ReadMinimap(dxt1.data());

	std::vector<uint8_t> rgba(static_cast<size_t>(kMinimapMip0) * kMinimapMip0 * 4, 0);
	MetalDXT::DecompressBC1Image(dxt1.data(), rgba.data(), kMinimapMip0, kMinimapMip0);
	return rgba;
}

std::pair<float, float> ScreenToMapUV(float sx, float sy, CMiniMap::RotationOptions rotation)
{
	switch (rotation) {
		case CMiniMap::ROTATION_90:  return {1.0f - sy, sx};
		case CMiniMap::ROTATION_180: return {1.0f - sx, 1.0f - sy};
		case CMiniMap::ROTATION_270: return {sy, 1.0f - sx};
		case CMiniMap::ROTATION_0:
		default:                     return {sx, sy};
	}
}

std::pair<float, float> MapUVToScreen(float u, float v, CMiniMap::RotationOptions rotation)
{
	switch (rotation) {
		case CMiniMap::ROTATION_90:  return {v, 1.0f - u};
		case CMiniMap::ROTATION_180: return {1.0f - u, 1.0f - v};
		case CMiniMap::ROTATION_270: return {1.0f - v, u};
		case CMiniMap::ROTATION_0:
		default:                     return {u, v};
	}
}

} // namespace


MetalMinimap::MetalMinimap()
{
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr)
		return;
	auto& backend = *globalRendering->renderBackend;

	std::vector<uint8_t> rgba = LoadMinimapRGBA8();
	if (rgba.empty()) {
		LOG_L(L_INFO, "[MetalMinimap] no SMF minimap; minimap disabled");
		return;
	}

	GL::TextureCreationParams tp;
	tp.linearTextureFilter = true;
	tp.linearMipMapFilter  = false;
	tp.reqNumLevels = 1;
	minimapTexture = backend.CreateTexture2D(int2(kMinimapMip0, kMinimapMip0), kGL_RGBA8, tp, /*wantCompress=*/false);
	if (!minimapTexture || !minimapTexture->IsValid()) {
		LOG_L(L_ERROR, "[MetalMinimap] minimap texture creation failed");
		return;
	}
	minimapTexture->UploadImage(rgba.data());

	// Background quad - immutable, six verts in NDC. The actual
	// rectangle is rebuilt each Draw() because the panel's vertical
	// extent depends on aspect ratio (which can change at runtime if
	// the user resizes the window). We seed with a placeholder and
	// UpdateData() per-frame.
	BgVertex seed[6] = {};
	bgVertexBuffer = backend.CreateBuffer(sizeof(seed), seed);
	if (!bgVertexBuffer || !bgVertexBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalMinimap] bg vertex buffer creation failed");
		return;
	}

	// Dot vertex buffer - dynamic, sized for an initial 256 dots.
	dotBufferCapacity = 256u * 6u;
	dotVertexBuffer = backend.CreateBuffer(dotBufferCapacity * sizeof(DotVertex), nullptr);
	if (!dotVertexBuffer || !dotVertexBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalMinimap] dot vertex buffer creation failed");
		return;
	}

	{
		PipelineDesc pd;
		pd.name = "minimap_bg";
		pd.vertexSource   = kBgVertexGlsl;
		pd.fragmentSource = kBgFragmentGlsl;
		pd.vertexAttributes = {
			VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = 0,                 .format = VertexFormat::Float2 },
			VertexAttribute{ .location = 1, .bufferSlot = 0, .offset = sizeof(float) * 2, .format = VertexFormat::Float2 },
		};
		pd.vertexBindings = { VertexBindingLayout{ .slot = 0, .stride = sizeof(BgVertex) } };
		pd.blendState.enabled  = true;
		pd.blendState.srcColor = GL_SRC_ALPHA;
		pd.blendState.dstColor = GL_ONE_MINUS_SRC_ALPHA;
		pd.blendState.srcAlpha = GL_ONE;
		pd.blendState.dstAlpha = GL_ONE_MINUS_SRC_ALPHA;
		bgPipeline = backend.CreatePipeline(pd);
		if (!bgPipeline || !bgPipeline->IsValid()) {
			LOG_L(L_ERROR, "[MetalMinimap] bg pipeline link failed: %s",
				bgPipeline ? bgPipeline->GetLog().c_str() : "(null)");
			return;
		}
	}

	{
		PipelineDesc pd;
		pd.name = "minimap_dots";
		pd.vertexSource   = kDotVertexGlsl;
		pd.fragmentSource = kDotFragmentGlsl;
		pd.vertexAttributes = {
			VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = 0,                 .format = VertexFormat::Float2 },
			VertexAttribute{ .location = 1, .bufferSlot = 0, .offset = sizeof(float) * 2, .format = VertexFormat::Float4 },
		};
		pd.vertexBindings = { VertexBindingLayout{ .slot = 0, .stride = sizeof(DotVertex) } };
		pd.blendState.enabled  = true;
		pd.blendState.srcColor = GL_SRC_ALPHA;
		pd.blendState.dstColor = GL_ONE_MINUS_SRC_ALPHA;
		pd.blendState.srcAlpha = GL_ONE;
		pd.blendState.dstAlpha = GL_ONE_MINUS_SRC_ALPHA;
		dotPipeline = backend.CreatePipeline(pd);
		if (!dotPipeline || !dotPipeline->IsValid()) {
			LOG_L(L_ERROR, "[MetalMinimap] dot pipeline link failed: %s",
				dotPipeline ? dotPipeline->GetLog().c_str() : "(null)");
			return;
		}
	}

	LOG("[MetalMinimap] minimap loaded (%dx%d RGBA8)", kMinimapMip0, kMinimapMip0);
	valid = true;
}

MetalMinimap::~MetalMinimap() = default;


void MetalMinimap::Draw()
{
	if (!valid)
		return;
	if (minimap == nullptr)
		return;
	if (globalRendering == nullptr || globalRendering->viewSizeX <= 1 || globalRendering->viewSizeY <= 1)
		return;

	if (!restoredCommonGeometry && (minimap->GetMinimized() || minimap->GetSizeX() <= 1 || minimap->GetSizeY() <= 1)) {
		minimap->SetMinimized(false);
		minimap->ReloadGeometryFromConfig();
		restoredCommonGeometry = true;
	}

	if (minimap->GetMinimized() || minimap->GetMaximized())
		return;

	// CMiniMap owns geometry/config. LuaUI's gui_minimap/gui_pip can
	// move/resize the engine minimap through Spring.SendCommands
	// ("minimap geometry ..."), and the engine applies constraints in
	// CMiniMap::UpdateGeometry. Metal consumes that final state here
	// instead of maintaining any parallel layout constants.
	const int px = minimap->GetPosX();
	const int py = minimap->GetPosY();
	const int sx = minimap->GetSizeX();
	const int sy = minimap->GetSizeY();
	if (sx <= 0 || sy <= 0)
		return;
	// If BAR/LuaUI drives CMiniMap into a large placement viewport,
	// do not let the engine minimap cover the Metal world view.
	if (sx > (globalRendering->viewSizeX * 3) / 5 || sy > (globalRendering->viewSizeY * 3) / 5)
		return;

	const float invViewX = 1.0f / static_cast<float>(globalRendering->viewSizeX);
	const float invViewY = 1.0f / static_cast<float>(globalRendering->viewSizeY);
	const float minX = -1.0f + 2.0f * static_cast<float>(px)      * invViewX;
	const float maxX = -1.0f + 2.0f * static_cast<float>(px + sx) * invViewX;
	const float minY = -1.0f + 2.0f * static_cast<float>(py)      * invViewY;
	const float maxY = -1.0f + 2.0f * static_cast<float>(py + sy) * invViewY;
	const CMiniMap::RotationOptions rotation = minimap->GetRotationOption();

	const auto [uvBLx, uvBLy] = ScreenToMapUV(0.0f, 1.0f, rotation);
	const auto [uvBRx, uvBRy] = ScreenToMapUV(1.0f, 1.0f, rotation);
	const auto [uvTRx, uvTRy] = ScreenToMapUV(1.0f, 0.0f, rotation);
	const auto [uvTLx, uvTLy] = ScreenToMapUV(0.0f, 0.0f, rotation);

	// SMF UV: tile (0, 0) is the south-west corner of the map; minimap
	// sample (0, 0) = top-left in image space which BAR's map convention
	// treats as the north-west corner (low Z). So map +Z grows downward
	// in V. Rotation follows CMiniMap so mouse picking and Metal dots
	// stay aligned with the rendered image.
	const BgVertex bgVerts[6] = {
		{ {minX, minY}, {uvBLx, uvBLy} },
		{ {maxX, minY}, {uvBRx, uvBRy} },
		{ {maxX, maxY}, {uvTRx, uvTRy} },
		{ {minX, minY}, {uvBLx, uvBLy} },
		{ {maxX, maxY}, {uvTRx, uvTRy} },
		{ {minX, maxY}, {uvTLx, uvTLy} },
	};
	bgVertexBuffer->UpdateData(bgVerts, sizeof(bgVerts), 0);

	bgPipeline->Enable();
	bgPipeline->BindTexture(1, *minimapTexture);
	bgPipeline->BindVertexBuffer(0, *bgVertexBuffer);
	bgPipeline->Draw(PrimitiveTopology::Triangles, 0, 6);
	bgPipeline->Disable();

	// --- Unit dots. Skip if simulation isn't up yet (start of load
	// when activeUnits is empty is fine, just nothing to draw).
	const auto& units = unitHandler.GetActiveUnits();
	if (units.empty())
		return;

	// Each unit emits 6 verts (two triangles).
	std::vector<DotVertex> dots;
	dots.reserve(units.size() * 6);

	const float mapW = static_cast<float>(mapDims.mapx) * SQUARE_SIZE;
	const float mapH = static_cast<float>(mapDims.mapy) * SQUARE_SIZE;
	if (mapW <= 0.0f || mapH <= 0.0f)
		return;

	// Dot size follows CMiniMap's configured unit scaling instead of
	// a hardcoded panel fraction. Convert world-space minimap unit size
	// back through the current map rectangle into NDC extents.
	const float dotHalfX = std::clamp((minimap->GetUnitSizeX() / mapW) * (maxX - minX), 2.0f * invViewX, 7.0f * invViewX);
	const float dotHalfY = std::clamp((minimap->GetUnitSizeY() / mapH) * (maxY - minY), 2.0f * invViewY, 7.0f * invViewY);

	for (const CUnit* u : units) {
		if (u == nullptr)
			continue;

		const float uMap = std::clamp(u->pos.x / mapW, 0.0f, 1.0f);
		const float vMap = std::clamp(u->pos.z / mapH, 0.0f, 1.0f);
		const auto [screenU, screenV] = MapUVToScreen(uMap, vMap, rotation);
		const float ndcX = minX + screenU * (maxX - minX);
		// screenV grows top-to-bottom on the minimap; NDC Y grows bottom-to-top.
		const float ndcY = maxY - screenV * (maxY - minY);

		float color[4] = { 0.7f, 0.7f, 0.7f, 1.0f };
		if (u->team >= 0 && teamHandler.IsValidTeam(u->team)) {
			const auto* team = teamHandler.Team(u->team);
			if (team != nullptr) {
				color[0] = team->color[0] / 255.0f;
				color[1] = team->color[1] / 255.0f;
				color[2] = team->color[2] / 255.0f;
				color[3] = 1.0f;
			}
		}
		// Boost own-team dots so the player sees their own forces
		// reading clearly against the textured backdrop.
		if (u->allyteam == gu->myAllyTeam) {
			color[0] = std::min(color[0] + 0.15f, 1.0f);
			color[1] = std::min(color[1] + 0.15f, 1.0f);
			color[2] = std::min(color[2] + 0.15f, 1.0f);
		}

		const float x0 = ndcX - dotHalfX, x1 = ndcX + dotHalfX;
		const float y0 = ndcY - dotHalfY, y1 = ndcY + dotHalfY;

		dots.push_back({ {x0, y0}, {color[0], color[1], color[2], color[3]} });
		dots.push_back({ {x1, y0}, {color[0], color[1], color[2], color[3]} });
		dots.push_back({ {x1, y1}, {color[0], color[1], color[2], color[3]} });
		dots.push_back({ {x0, y0}, {color[0], color[1], color[2], color[3]} });
		dots.push_back({ {x1, y1}, {color[0], color[1], color[2], color[3]} });
		dots.push_back({ {x0, y1}, {color[0], color[1], color[2], color[3]} });
	}

	if (dots.empty())
		return;

	// Grow the dot buffer when active-unit counts spike past current
	// capacity. Power-of-two growth so we don't realloc every frame
	// during early game when units rapidly spawn.
	if (dots.size() > dotBufferCapacity) {
		uint32_t newCap = dotBufferCapacity > 0 ? dotBufferCapacity : 256u;
		while (newCap < dots.size())
			newCap *= 2u;
		dotVertexBuffer = globalRendering->renderBackend->CreateBuffer(
			newCap * sizeof(DotVertex), nullptr);
		dotBufferCapacity = newCap;
	}

	dotVertexBuffer->UpdateData(dots.data(), dots.size() * sizeof(DotVertex), 0);

	dotPipeline->Enable();
	dotPipeline->BindVertexBuffer(0, *dotVertexBuffer);
	dotPipeline->Draw(PrimitiveTopology::Triangles, 0, static_cast<uint32_t>(dots.size()));
	dotPipeline->Disable();
}

#endif // RENDER_BACKEND_METAL
