/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <cstdint>

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
#include "Rendering/IRenderBackend.h"
#include "Rendering/IBuffer.h"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "Rendering/Textures/ITexture.h"
#include "Rendering/Textures/TextureCreationParams.hpp"
#include "System/Log/ILog.h"
#include "System/type2.h"
#endif

#ifndef HEADLESS
void ShowSplashScreen(
	const std::string& splashScreenFile,
	const std::string& springVersionStr,
	const std::function<bool()>& testDoneFunc
) {
#if defined(RENDER_BACKEND_METAL)
	// Metal splash path: textured full-screen quad via IShaderPipeline +
	// IBuffer + ITexture. Font / version text lands in the next S8-C5
	// sub-slice once CglFont has a Metal backend; for now the splash
	// image alone replaces the all-black clear.
	(void)springVersionStr;

	// --- Load splash bitmap (forced to RGBA8 by the default reqChannel=4).
	// If no splash file was found or Load failed, substitute a small
	// visible placeholder so the draw path still exercises the pipeline +
	// buffer + texture binding end-to-end, matching the GL placeholder
	// behavior in the non-Metal path below.
	CBitmap splashBmp;
	const bool haveSplashImage = !splashScreenFile.empty() && splashBmp.Load(splashScreenFile);
	if (!haveSplashImage)
		splashBmp.AllocDummy({64, 64, 80, 255}); // dim slate, visibly non-black

	// --- Create the splash texture. GL internalFormat 0x8058 == GL_RGBA8 is
	// mapped by MetalTexture::MapGlInternalFormat to MTLPixelFormatRGBA8Unorm.
	std::unique_ptr<ITexture> splashTex;
	if (splashBmp.xsize > 0 && splashBmp.ysize > 0) {
		GL::TextureCreationParams tcp;
		tcp.linearTextureFilter = true;
		tcp.linearMipMapFilter  = false;
		tcp.reqNumLevels = 1;
		constexpr uint32_t kGL_RGBA8 = 0x8058;
		splashTex = globalRendering->renderBackend->CreateTexture2D(
			int2(splashBmp.xsize, splashBmp.ysize), kGL_RGBA8, tcp, /*wantCompress=*/false);
		if (splashTex && splashTex->IsValid())
			splashTex->UploadImage(splashBmp.GetRawMem());
	}

	// --- Triangle list. Two floats of NDC position, two floats of UV;
	// UV origin is top-left so uv.y=0 maps to the top of the texture,
	// matching how CBitmap rows load and how replaceRegion uploaded them.
	// Real splash image fills the viewport; placeholder renders as a small
	// aspect-correct centered tile so the quad is obviously "alive" vs the
	// previous all-black clear.
	struct SplashVertex { float x, y, u, v; };
	SplashVertex quadVerts[6];
	if (haveSplashImage) {
		const SplashVertex fs[6] = {
			{-1.0f, -1.0f, 0.0f, 1.0f},
			{ 1.0f, -1.0f, 1.0f, 1.0f},
			{-1.0f,  1.0f, 0.0f, 0.0f},
			{-1.0f,  1.0f, 0.0f, 0.0f},
			{ 1.0f, -1.0f, 1.0f, 1.0f},
			{ 1.0f,  1.0f, 1.0f, 0.0f},
		};
		memcpy(quadVerts, fs, sizeof(fs));
	} else {
		constexpr float hx = 0.125f;
		const float hy = hx * globalRendering->aspectRatio;
		const SplashVertex tile[6] = {
			{-hx, -hy, 0.0f, 1.0f},
			{ hx, -hy, 1.0f, 1.0f},
			{-hx,  hy, 0.0f, 0.0f},
			{-hx,  hy, 0.0f, 0.0f},
			{ hx, -hy, 1.0f, 1.0f},
			{ hx,  hy, 1.0f, 0.0f},
		};
		memcpy(quadVerts, tile, sizeof(tile));
	}
	auto quadBuf = globalRendering->renderBackend->CreateBuffer(sizeof(quadVerts), quadVerts);

	// --- Pipeline. Minimal Vulkan-semantic GLSL 450 so the shared
	// glslang -> spirv-cross pipeline (Shader::TranslateGlslToMsl) can
	// turn it into MSL for MetalShaderPipeline::Link.
	PipelineDesc pipelineDesc;
	pipelineDesc.name = "splash_quad";
	pipelineDesc.vertexSource = R"(#version 450
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 0) out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";
	pipelineDesc.fragmentSource = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;
layout(set = 0, binding = 0) uniform sampler2D uTex;
void main() {
    fragColor = texture(uTex, vUV);
}
)";
	pipelineDesc.vertexAttributes = {
		VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = 0,                     .format = VertexFormat::Float2 },
		VertexAttribute{ .location = 1, .bufferSlot = 0, .offset = sizeof(float) * 2,     .format = VertexFormat::Float2 },
	};
	pipelineDesc.vertexBindings = {
		VertexBindingLayout{ .slot = 0, .stride = sizeof(SplashVertex) },
	};

	auto pipeline = globalRendering->renderBackend->CreatePipeline(pipelineDesc);

	const bool canDrawSplash =
		pipeline != nullptr && pipeline->IsValid() &&
		quadBuf  != nullptr && quadBuf->IsValid()  &&
		splashTex != nullptr && splashTex->IsValid();

	if (!canDrawSplash) {
		LOG_L(L_INFO, "[ShowSplashScreen] Metal: splash draw disabled (haveImage=%d, pipelineValid=%d); falling back to clear",
		      static_cast<int>(haveSplashImage),
		      static_cast<int>(pipeline && pipeline->IsValid()));
	}

	while (!testDoneFunc()) {
		globalRendering->BeginFrame();

		if (canDrawSplash) {
			pipeline->Enable();
			pipeline->BindVertexBuffer(0, *quadBuf);
			pipeline->BindTexture(0, *splashTex);
			pipeline->Draw(PrimitiveTopology::Triangles, 0, 6);
			pipeline->Disable();
		}

		globalRendering->PresentFrame(true, true);

		SDL_Event event;
		while (SDL_PollEvent(&event)) {}
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

