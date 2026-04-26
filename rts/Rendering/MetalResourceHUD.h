/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <memory>

class IBuffer;
class IShaderPipeline;
class MetalTextOverlay;

// Top-of-screen metal + energy bars for the local player's team.
// Reads CTeam::res / resStorage / resIncome / resExpense each frame
// and renders two stacked NDC-space rectangles (steel for metal, gold
// for energy) plus numeric labels through the supplied
// MetalTextOverlay. Colour-key matches BAR's standard chobby HUD so
// players coming from the GL build read the bars at a glance.
//
// Pure overlay - read-only on common code (teamHandler, gu->myTeam),
// no game-state mutation. Skipped silently when there is no local
// team (replays without spectator hand-off, headless boot).
//
// Limitations vs. the real chobby HUD:
//   - no separate stored / share thresholds; just current / max
//   - no commander uplink, no buildpower bar
//   - no economy graph / over-time history
// All three follow once the Metal HUD has its own glyph atlas
// (right now it shares MetalTextOverlay's tiny 5x7 ASCII font).
class MetalResourceHUD
{
public:
	MetalResourceHUD();
	~MetalResourceHUD();

	MetalResourceHUD(const MetalResourceHUD&) = delete;
	MetalResourceHUD& operator=(const MetalResourceHUD&) = delete;

	bool IsValid() const { return valid; }

	// `text` is borrowed for the labels; ownership stays with the
	// caller (typically the static MetalTextOverlay in CGame::Draw).
	// Safe to pass nullptr - bars draw without numeric labels.
	void Draw(MetalTextOverlay* text);

private:
	std::unique_ptr<IShaderPipeline> pipeline;
	std::unique_ptr<IBuffer>         vertexBuffer;
	uint32_t                         bufferCapacity = 0;
	bool                             valid = false;
};

#endif // RENDER_BACKEND_METAL
