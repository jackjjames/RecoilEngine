/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalMinimap.h"

#include "Game/GlobalUnsynced.h"
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
#include <vector>


namespace {

constexpr uint32_t kGL_RGBA8 = 0x8058;
constexpr int      kMinimapMip0 = 1024;

// Bottom-left corner placement in NDC (y-up). The map quad spans
// kPanelSize x kPanelSize, leaving kPanelMargin between it and the
// screen edges. Y is corrected for window aspect at draw time so
// non-square viewports don't squash the map.
constexpr float kPanelHalf   = 0.18f; // half-width of the panel in NDC X
constexpr float kPanelMargin = 0.02f;

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
    // 1-pixel inset border so the minimap reads as a discrete UI
    // element rather than blending into the terrain underneath.
    vec2 d = min(vUV, 1.0 - vUV);
    float edge = min(d.x, d.y);
    if (edge < 0.01) col = vec3(1.0);
    fragColor = vec4(col, 0.92);
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

	// Aspect-correct the panel: we want a square minimap regardless
	// of window aspect, and panelHalfX in NDC corresponds to a
	// different pixel count than panelHalfY at non-square ratios.
	const float aspect = (globalRendering != nullptr && globalRendering->aspectRatio > 0.0f)
		? globalRendering->aspectRatio : 1.0f;
	const float halfX = kPanelHalf;
	const float halfY = kPanelHalf * aspect;

	// Bottom-left anchor in NDC (y-up).
	const float minX = -1.0f + kPanelMargin;
	const float maxX = minX + 2.0f * halfX;
	const float minY = -1.0f + kPanelMargin;
	const float maxY = minY + 2.0f * halfY;

	// SMF UV: tile (0, 0) is the south-west corner of the map; minimap
	// sample (0, 0) = top-left in image space which BAR's map convention
	// treats as the north-west corner (low Z). So map +Z grows downward
	// in V; we keep V un-flipped here so the minimap reads correctly.
	const BgVertex bgVerts[6] = {
		{ {minX, minY}, {0.0f, 1.0f} },
		{ {maxX, minY}, {1.0f, 1.0f} },
		{ {maxX, maxY}, {1.0f, 0.0f} },
		{ {minX, minY}, {0.0f, 1.0f} },
		{ {maxX, maxY}, {1.0f, 0.0f} },
		{ {minX, maxY}, {0.0f, 0.0f} },
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

	// Dot half-extent in NDC: a 4x4-pixel-ish square at typical 1280x720,
	// scaled with the panel size so the dots remain readable on tall
	// vs wide windows.
	const float dotHalfX = halfX * 0.012f;
	const float dotHalfY = halfY * 0.012f;

	// Each unit emits 6 verts (two triangles).
	std::vector<DotVertex> dots;
	dots.reserve(units.size() * 6);

	const float mapW = static_cast<float>(mapDims.mapx) * SQUARE_SIZE;
	const float mapH = static_cast<float>(mapDims.mapy) * SQUARE_SIZE;
	if (mapW <= 0.0f || mapH <= 0.0f)
		return;

	for (const CUnit* u : units) {
		if (u == nullptr)
			continue;

		const float uMap = std::clamp(u->pos.x / mapW, 0.0f, 1.0f);
		const float vMap = std::clamp(u->pos.z / mapH, 0.0f, 1.0f);
		const float ndcX = minX + uMap * (maxX - minX);
		// V grows top-to-bottom on the minimap; NDC Y grows bottom-to-top.
		const float ndcY = maxY - vMap * (maxY - minY);

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
