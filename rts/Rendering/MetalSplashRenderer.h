/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#if defined(RENDER_BACKEND_METAL)

#include <memory>
#include <string>

class IBuffer;
class IShaderPipeline;
class ITexture;

// Small helper that owns a textured-quad pipeline + vertex buffer +
// splash/load texture and draws one frame at a time through the
// IShaderPipeline abstraction. Used by both the initial splash (VFS
// init) and the load screen (CGame::Load) so neither path has to
// repeat the pipeline / vertex descriptor plumbing.
//
// Metal-only. On GL backends this header is empty so the translation
// unit can still be included unconditionally from cross-backend files.
class MetalSplashRenderer
{
public:
	// `bitmapPath` may be empty; a small placeholder tile is rendered
	// instead so even without an asset on disk the quad pipeline is
	// exercised end-to-end and the window shows a visible signal.
	explicit MetalSplashRenderer(const std::string& bitmapPath = "");
	~MetalSplashRenderer();

	MetalSplashRenderer(const MetalSplashRenderer&) = delete;
	MetalSplashRenderer& operator=(const MetalSplashRenderer&) = delete;

	bool HasImage() const { return haveImage; }
	bool IsValid() const { return valid; }

	// Issue the draw against the current Metal frame's encoder. Safe to
	// call when invalid (no-op) so callers can check once at init and
	// then call Draw() every frame.
	void Draw() const;

private:
	std::unique_ptr<IShaderPipeline> pipeline;
	std::unique_ptr<IBuffer>         vertexBuffer;
	std::unique_ptr<ITexture>        texture;

	bool haveImage = false;
	bool valid     = false;
};

#endif // RENDER_BACKEND_METAL
