/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <memory>
#include <string>
#include <vector>

class IBuffer;
class IShaderPipeline;
class ITexture;

// Minimal bitmap-font overlay used by the Metal loading screen to show the
// current load-progress message while CglFont still targets GL. Bolts a tiny
// hand-rolled 5x7 ASCII atlas onto the same IShaderPipeline + ITexture path
// used by MetalSplashRenderer; the atlas is R8Unorm and the fragment shader
// discards on coverage so no blend state is required. Lowercase letters are
// folded to uppercase, unsupported glyphs render as blanks. This lets S8-C5
// unblock without porting CFontTexture / CglFont; a real CglFont Metal
// backend replaces it in a later slice.
class MetalTextOverlay
{
public:
	MetalTextOverlay();
	~MetalTextOverlay();

	MetalTextOverlay(const MetalTextOverlay&) = delete;
	MetalTextOverlay& operator=(const MetalTextOverlay&) = delete;

	bool IsValid() const { return valid; }

	// Draw a single line of text starting at NDC (x, y) — top-left origin.
	// ndcGlyphH is the desired glyph cell height in NDC units. If the full
	// string would extend past `ndcMaxX` at that height, glyphs are
	// uniformly scaled down so the line fits; this keeps long load
	// messages on-screen instead of clipping off the right edge. The
	// vertex scratch buffer is reused across calls. Safe to call when
	// invalid (no-op).
	void DrawLine(float ndcX, float ndcY, float ndcGlyphH, const std::string& text,
	              float ndcMaxX = 0.95f);

	// Interleaved vec2 position + vec2 uv; public so the nearby .cpp
	// translation unit can reuse the struct in its scratch buffer without
	// a second definition.
	struct TextVertex { float x, y, u, v; };

private:
	std::unique_ptr<IShaderPipeline> pipeline;
	std::unique_ptr<ITexture>        atlas;
	std::unique_ptr<IBuffer>         vertexBuffer;
	size_t                           vertexBufferCapacityBytes = 0;

	// Per-line scratch. Reused so DrawLine does not allocate per frame.
	std::vector<TextVertex>          vertsScratch;

	bool valid = false;
};

#endif // RENDER_BACKEND_METAL
