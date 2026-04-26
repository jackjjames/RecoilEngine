/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalResourceHUD.h"

#include "Game/GlobalUnsynced.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/IBuffer.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/MetalTextOverlay.h"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "Sim/Misc/Resource.h"
#include "Sim/Misc/Team.h"
#include "Sim/Misc/TeamHandler.h"
#include "System/Log/ILog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>


namespace {

// 2D NDC-space vertex; same format used by MetalSelectionMarkers /
// MetalCommandLines. Bar geometry is fully precomputed in NDC so the
// vertex shader is a passthrough.
struct HudVertex {
	float pos[2];
	float color[4];
};

constexpr const char* kVertexGlsl = R"(#version 450
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 0) out vec4 vColor;

void main() {
    vColor = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

constexpr const char* kFragmentGlsl = R"(#version 450
layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = vColor;
}
)";


// Push a quad spanning [x0..x1] x [y0..y1] in NDC, colour `c`.
void PushQuad(std::vector<HudVertex>& v,
              float x0, float y0, float x1, float y1,
              const float c[4])
{
	const HudVertex tl{ {x0, y0}, {c[0], c[1], c[2], c[3]} };
	const HudVertex tr{ {x1, y0}, {c[0], c[1], c[2], c[3]} };
	const HudVertex bl{ {x0, y1}, {c[0], c[1], c[2], c[3]} };
	const HudVertex br{ {x1, y1}, {c[0], c[1], c[2], c[3]} };
	v.push_back(tl); v.push_back(bl); v.push_back(br);
	v.push_back(tl); v.push_back(br); v.push_back(tr);
}

// Format a resource value compactly so the label fits inside the bar:
// values >= 10000 collapse to thousands ("12.3k"), >= 1M collapse to
// megas ("1.2M"). Matches BAR's ingame topbar formatting closely
// enough that values feel familiar.
void FormatRes(char* dst, size_t n, float v)
{
	if (!std::isfinite(v) || v < 0.0f) v = 0.0f;
	if (v >= 1.0e6f)
		std::snprintf(dst, n, "%.1fM", v * 1.0e-6f);
	else if (v >= 1.0e4f)
		std::snprintf(dst, n, "%.1fk", v * 1.0e-3f);
	else
		std::snprintf(dst, n, "%d", static_cast<int>(v + 0.5f));
}

} // namespace


MetalResourceHUD::MetalResourceHUD()
{
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr)
		return;
	auto& backend = *globalRendering->renderBackend;

	bufferCapacity = 256; // 4 quads (M back + M fill + E back + E fill) leaves headroom for spec-mode summary later
	const std::vector<HudVertex> seed(bufferCapacity);
	vertexBuffer = backend.CreateBuffer(bufferCapacity * sizeof(HudVertex), seed.data());
	if (!vertexBuffer || !vertexBuffer->IsValid()) {
		LOG_L(L_ERROR, "[MetalResourceHUD] vertex buffer creation failed");
		return;
	}

	PipelineDesc pd;
	pd.name           = "metal_resource_hud";
	pd.vertexSource   = kVertexGlsl;
	pd.fragmentSource = kFragmentGlsl;
	pd.vertexAttributes = {
		VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = offsetof(HudVertex, pos),   .format = VertexFormat::Float2 },
		VertexAttribute{ .location = 1, .bufferSlot = 0, .offset = offsetof(HudVertex, color), .format = VertexFormat::Float4 },
	};
	pd.vertexBindings = {
		VertexBindingLayout{ .slot = 0, .stride = sizeof(HudVertex) },
	};
	pd.blendState.enabled  = true;
	pd.blendState.srcColor = GL_SRC_ALPHA;
	pd.blendState.dstColor = GL_ONE_MINUS_SRC_ALPHA;
	pd.blendState.srcAlpha = GL_ONE;
	pd.blendState.dstAlpha = GL_ONE_MINUS_SRC_ALPHA;

	pipeline = backend.CreatePipeline(pd);
	if (!pipeline || !pipeline->IsValid()) {
		LOG_L(L_ERROR, "[MetalResourceHUD] pipeline link failed: %s",
			pipeline ? pipeline->GetLog().c_str() : "(null)");
		return;
	}

	valid = true;
}

MetalResourceHUD::~MetalResourceHUD() = default;

void MetalResourceHUD::Draw(MetalTextOverlay* text)
{
	if (!valid)
		return;
	if (gu == nullptr)
		return;

	// Pick the team whose economy to show. In normal play that's
	// gu->myTeam; spectators with full view see team 0 by convention
	// until proper spec-mode HUD lands.
	const int teamID = (gu->spectating && !teamHandler.IsValidTeam(gu->myTeam))
		? 0
		: gu->myTeam;
	if (!teamHandler.IsValidTeam(teamID))
		return;

	const CTeam* team = teamHandler.Team(teamID);
	if (team == nullptr || team->isDead)
		return;

	// MetalTextOverlay uses NDC y-down (top = y=-1). Lay the bars out
	// just under the top edge, centered horizontally, with metal on
	// the left and energy on the right - matches the GL HUD layout
	// most BAR players have already trained their eyes for.
	const float topY    = -0.985f;
	const float botY    = -0.945f;
	const float gap     =  0.020f;
	const float halfW   =  0.45f;
	const float metalX0 = -halfW;
	const float metalX1 = -gap * 0.5f;
	const float energX0 =  gap * 0.5f;
	const float energX1 =  halfW;

	// Bar fill ratios. Empty / mis-loaded storage -> 0 so we don't
	// divide by zero; the empty-fill case still gets a visible
	// backing rect to anchor the player's eye.
	const float metalRatio = (team->resStorage.metal > 1.0e-3f)
		? std::clamp(team->res.metal  / team->resStorage.metal, 0.0f, 1.0f)
		: 0.0f;
	const float energyRatio = (team->resStorage.energy > 1.0e-3f)
		? std::clamp(team->res.energy / team->resStorage.energy, 0.0f, 1.0f)
		: 0.0f;

	const float backColor[4]   = { 0.05f, 0.05f, 0.05f, 0.55f };
	const float metalColor[4]  = { 0.78f, 0.82f, 0.88f, 0.92f };
	const float energyColor[4] = { 1.00f, 0.85f, 0.20f, 0.92f };

	std::vector<HudVertex> verts;
	verts.reserve(4 * 6);
	PushQuad(verts, metalX0, topY, metalX1, botY, backColor);
	PushQuad(verts, metalX0, topY,
		metalX0 + (metalX1 - metalX0) * metalRatio, botY, metalColor);
	PushQuad(verts, energX0, topY, energX1, botY, backColor);
	PushQuad(verts, energX0, topY,
		energX0 + (energX1 - energX0) * energyRatio, botY, energyColor);

	if (verts.size() > bufferCapacity) {
		auto& backend = *globalRendering->renderBackend;
		uint32_t newCap = bufferCapacity;
		while (newCap < verts.size())
			newCap *= 2;
		auto bigger = backend.CreateBuffer(newCap * sizeof(HudVertex), verts.data());
		if (bigger && bigger->IsValid()) {
			vertexBuffer = std::move(bigger);
			bufferCapacity = newCap;
		} else {
			verts.resize(bufferCapacity);
		}
	} else {
		vertexBuffer->UpdateData(verts.data(), verts.size() * sizeof(HudVertex), 0);
	}

	pipeline->Enable();
	pipeline->BindVertexBuffer(0, *vertexBuffer);
	pipeline->Draw(PrimitiveTopology::Triangles, 0, static_cast<uint32_t>(verts.size()));
	pipeline->Disable();

	// Numeric labels - centred just below each bar so values don't
	// occlude the fill. Uses MetalTextOverlay's bitmap font; same
	// glyph cell size we use elsewhere in the overlay HUD.
	if (text == nullptr)
		return;

	char metalBuf [64];
	char metalCur [16];
	char metalMax [16];
	char metalFlow[24];
	FormatRes(metalCur, sizeof(metalCur), team->res.metal);
	FormatRes(metalMax, sizeof(metalMax), team->resStorage.metal);
	const float metalNet = team->resPrevIncome.metal - team->resPrevExpense.metal;
	std::snprintf(metalFlow, sizeof(metalFlow), "%+.1f", metalNet);
	std::snprintf(metalBuf, sizeof(metalBuf), "M %s/%s  %s",
		metalCur, metalMax, metalFlow);

	char energyBuf [64];
	char energyCur [16];
	char energyMax [16];
	char energyFlow[24];
	FormatRes(energyCur, sizeof(energyCur), team->res.energy);
	FormatRes(energyMax, sizeof(energyMax), team->resStorage.energy);
	const float energyNet = team->resPrevIncome.energy - team->resPrevExpense.energy;
	std::snprintf(energyFlow, sizeof(energyFlow), "%+.1f", energyNet);
	std::snprintf(energyBuf, sizeof(energyBuf), "E %s/%s  %s",
		energyCur, energyMax, energyFlow);

	// MetalTextOverlay::DrawLine is NDC y-down. Place the labels just
	// south of the bottom of each bar with a small visual gap.
	const float labelY    = botY + 0.012f;
	const float glyphH    = 0.030f;
	text->DrawLine(metalX0 + 0.010f, labelY, glyphH, metalBuf,
		metalX1 - 0.010f);
	text->DrawLine(energX0 + 0.010f, labelY, glyphH, energyBuf,
		energX1 - 0.010f);
}

#endif // RENDER_BACKEND_METAL
