/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <cstdint>
#include <cstdlib>

#include <SDL.h>

#include "SplashScreen.hpp"
#include "Rendering/GlobalRendering.h"
#include "Rendering/GL/myGL.h"
#include "Rendering/GL/RenderBuffers.h"
#include "Rendering/Fonts/glFont.h"
#include "Rendering/Textures/Bitmap.h"
#include "System/float4.h"
#include "System/Matrix44f.h"
#include "System/FileSystem/ArchiveScanner.h"
#include "System/Platform/Watchdog.h"

#if defined(RENDER_BACKEND_METAL)
#include "Rendering/MetalSplashRenderer.h"
#include "Rendering/MetalTextOverlay.h"
#include "System/Misc/SpringTime.h"
#endif

#ifndef HEADLESS
void ShowSplashScreen(
	const std::string& splashScreenFile,
	const std::string& springVersionStr,
	const std::function<bool()>& testDoneFunc
) {
#if defined(RENDER_BACKEND_METAL)
	// Metal splash path: textured full-screen quad via IShaderPipeline +
	// IBuffer + ITexture, plus a tiny 5x7 bitmap-font overlay for the
	// version / license text. CglFont still targets GL and lands in a
	// later S8-C5 sub-slice; MetalTextOverlay bridges the gap without
	// touching the full font stack so the splash shows something readable
	// instead of just a tile. The same renderers are reused by
	// CLoadScreen::Draw so the load phase also shows progress.
	MetalSplashRenderer splash(splashScreenFile);
	MetalTextOverlay    textOverlay;

	// Upper-case the version string in the overlay since the bitmap font
	// folds lowercase to blanks today.
	const std::string recoilLine = "RECOIL " + springVersionStr;
	static constexpr const char* kLicenseLine =
		"GNU GENERAL PUBLIC LICENSE - SEE DOC/LICENSE";

	// Throttle the present loop. Unlike the GL SwapBuffers path (which
	// blocks on vsync via the GL context), the Metal Begin/PresentFrame
	// path currently spins as fast as the CPU feeds it and can saturate
	// the GPU / command queue during the multi-second VFS scan. Cap at
	// ~60fps until we wire proper displaySync handling through the
	// Metal presenter.
	constexpr unsigned minFrameMs = 16;
	spring_time lastDraw = spring_gettime();

	while (!testDoneFunc()) {
		const spring_time now = spring_gettime();
		const unsigned elapsed = spring_tomsecs(now - lastDraw);
		if (elapsed < minFrameMs)
			spring_sleep(spring_msecs(minFrameMs - elapsed));
		lastDraw = spring_gettime();

		globalRendering->BeginFrame();
		splash.Draw();
		// Bottom-left stack: version over license, roughly 2.5% of viewport
		// height per glyph. Coordinates are in NDC with y-up origin.
		constexpr float glyphH = 0.025f;
		textOverlay.DrawLine(-0.95f, -0.90f + glyphH * 1.4f, glyphH, recoilLine);
		textOverlay.DrawLine(-0.95f, -0.90f,                 glyphH, kLicenseLine);
		globalRendering->PresentFrame(true, true);

		// Pump SDL events so the OS does not mark the window as hung.
		// Honor explicit close/quit now: the splash phase runs before
		// the engine's main event loop is installed, so dropping these
		// would leave the traffic-light buttons feeling dead.
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_QUIT ||
			    (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) {
				std::exit(0);
			}
		}
		Watchdog::ClearTimer(WDT_MAIN);
	}
	return;
#else
	CBitmap bmp;

	VA_TYPE_2DT quadElems[] = {
		{0.0f, 1.0f,  0.0f, 0.0f},
		{0.0f, 0.0f,  0.0f, 1.0f},
		{1.0f, 0.0f,  1.0f, 1.0f},
		{1.0f, 1.0f,  1.0f, 0.0f},
	};

	// passing an empty name would cause bmp FileHandler to also
	// search inside the VFS since its default mode is RAW_FIRST
	if (splashScreenFile.empty() || !bmp.Load(splashScreenFile)) {
		bmp.AllocDummy({0, 0, 0, 255});
		quadElems[0].x = 0.5f - 0.125f * 0.5f; quadElems[0].y = 0.5f + 0.125f * 0.5f * globalRendering->aspectRatio;
		quadElems[1].x = 0.5f - 0.125f * 0.5f; quadElems[1].y = 0.5f - 0.125f * 0.5f * globalRendering->aspectRatio;
		quadElems[2].x = 0.5f + 0.125f * 0.5f; quadElems[2].y = 0.5f - 0.125f * 0.5f * globalRendering->aspectRatio;
		quadElems[3].x = 0.5f + 0.125f * 0.5f; quadElems[3].y = 0.5f + 0.125f * 0.5f * globalRendering->aspectRatio;
	}

	static constexpr const char* fmtStrs[] = {
		"[Initializing Virtual File System]",
		"* archives scanned: %u",
		"* scantime elapsed: %.1fms",
		"Recoil %s",
		"This program is distributed under the GNU General Public License, see doc/LICENSE for more information.",
	};

	char versionStrBuf[512];

	memset(versionStrBuf, 0, sizeof(versionStrBuf));
	snprintf(versionStrBuf, sizeof(versionStrBuf), fmtStrs[3], springVersionStr.c_str());

	const unsigned int splashTex = bmp.CreateTexture();
	const unsigned int fontFlags = FONT_NORM | FONT_SCALE;

	const float4 color = {1.0f, 1.0f, 1.0f, 1.0f};
	const float4 coors = {0.5f, 0.175f, 0.8f, 0.04f}; // x, y, scale, space

	const float textWidth[3] = {font->GetTextWidth(fmtStrs[0]), font->GetTextWidth(fmtStrs[4]), font->GetTextWidth(versionStrBuf)};
	const float normWidth[3] = {
		textWidth[0] * globalRendering->pixelX * font->GetSize() * coors.z,
		textWidth[1] * globalRendering->pixelX * font->GetSize() * coors.z,
		textWidth[2] * globalRendering->pixelX * font->GetSize() * coors.z,
	};

	auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_2DT>();
	rb.AssertSubmission();
	auto& sh = rb.GetShader();

	glPushAttrib(GL_ENABLE_BIT);
	glEnable(GL_TEXTURE_2D);

	for (spring_time t0 = spring_now(), t1 = t0; !testDoneFunc(); t1 = spring_now()) {
		glClear(GL_COLOR_BUFFER_BIT);

		glBindTexture(GL_TEXTURE_2D, splashTex);

		rb.AddQuadTriangles(
			{ quadElems[0].x, quadElems[0].y, quadElems[0].s, quadElems[0].t },
			{ quadElems[1].x, quadElems[1].y, quadElems[1].s, quadElems[1].t },
			{ quadElems[2].x, quadElems[2].y, quadElems[2].s, quadElems[2].t },
			{ quadElems[3].x, quadElems[3].y, quadElems[3].s, quadElems[3].t }
		);

		sh.Enable();
		rb.DrawElements(GL_TRIANGLES);
		sh.Disable();

		font->Begin();
		font->SetTextColor(color.x, color.y, color.z, color.w);
		font->glFormat(coors.x - (normWidth[0] * 0.500f), coors.y                             , coors.z, fontFlags, fmtStrs[0]);
		font->glFormat(coors.x - (normWidth[0] * 0.475f), coors.y - (coors.w * coors.z * 1.0f), coors.z, fontFlags, fmtStrs[1], CArchiveScanner::GetNumScannedArchives());
		font->glFormat(coors.x - (normWidth[0] * 0.475f), coors.y - (coors.w * coors.z * 2.0f), coors.z, fontFlags, fmtStrs[2], (t1 - t0).toMilliSecsf());
		font->End();

		// always render Spring's license notice
		font->Begin();
		font->SetOutlineColor(0.0f, 0.0f, 0.0f, 0.65f);
		font->SetTextColor(color.x, color.y, color.z, color.w);
		font->glFormat(coors.x - (normWidth[2] * 0.5f), coors.y * 0.5f - (coors.w * coors.z * 1.0f), coors.z, fontFlags | FONT_OUTLINE, versionStrBuf);
		font->glFormat(coors.x - (normWidth[1] * 0.5f), coors.y * 0.5f - (coors.w * coors.z * 2.0f), coors.z, fontFlags | FONT_OUTLINE, fmtStrs[4]);
		font->End();

		globalRendering->SwapBuffers(true, true);

		// prevent WM's from assuming the window is unresponsive and
		// generating a kill-request
		SDL_Event event;
		while (SDL_PollEvent(&event)) {}
		Watchdog::ClearTimer(WDT_MAIN);
	}

	glPopAttrib();
	glDeleteTextures(1, &splashTex);
#endif // !RENDER_BACKEND_METAL
}
#endif

