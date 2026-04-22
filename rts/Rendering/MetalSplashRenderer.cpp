/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#if defined(RENDER_BACKEND_METAL)

#include "Rendering/MetalSplashRenderer.h"

#include "Rendering/GlobalRendering.h"
#include "Rendering/IBuffer.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/Shaders/IShaderPipeline.h"
#include "Rendering/Textures/Bitmap.h"
#include "Rendering/Textures/ITexture.h"
#include "Rendering/Textures/TextureCreationParams.hpp"
#include "System/Log/ILog.h"
#include "System/type2.h"

#include <cstring>

namespace {

struct SplashVertex { float x, y, u, v; };

constexpr uint32_t kGL_RGBA8 = 0x8058;

// Vulkan-semantic GLSL 450 so the shared glslang -> spirv-cross
// pipeline in Shader::TranslateGlslToMsl can turn it into MSL for
// MetalShaderPipeline::Link. Matches the fragment-stage sampler
// slot (binding=0) that MetalShaderPipeline::ApplyBindings routes
// to [[texture(0)]]/[[sampler(0)]].
constexpr const char* kVertexGlsl = R"(#version 450
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 0) out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

constexpr const char* kFragmentGlsl = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;
layout(set = 0, binding = 0) uniform sampler2D uTex;
void main() {
    fragColor = texture(uTex, vUV);
}
)";

} // namespace


MetalSplashRenderer::MetalSplashRenderer(const std::string& bitmapPath)
{
	if (globalRendering == nullptr || globalRendering->renderBackend == nullptr)
		return;

	auto& backend = *globalRendering->renderBackend;

	// --- Bitmap. CBitmap::Load forces RGBA8 by default (reqChannel=4).
	// When no splash asset is found, AllocDummy a dim slate tile so the
	// quad still exercises the pipeline and the window shows a visible
	// non-black signal.
	CBitmap bmp;
	haveImage = !bitmapPath.empty() && bmp.Load(bitmapPath);
	if (!haveImage)
		bmp.AllocDummy({64, 64, 80, 255});

	if (bmp.xsize <= 0 || bmp.ysize <= 0)
		return;

	// --- Texture. 0x8058 == GL_RGBA8; MetalTexture maps this to
	// MTLPixelFormatRGBA8Unorm + MTLStorageModeShared and uses
	// replaceRegion for the upload.
	GL::TextureCreationParams tcp;
	tcp.linearTextureFilter = true;
	tcp.linearMipMapFilter  = false;
	tcp.reqNumLevels = 1;
	texture = backend.CreateTexture2D(int2(bmp.xsize, bmp.ysize), kGL_RGBA8, tcp, /*wantCompress=*/false);
	if (!texture || !texture->IsValid())
		return;
	texture->UploadImage(bmp.GetRawMem());

	// --- Geometry. Triangle list (6 verts, interleaved vec2 pos + vec2
	// uv). UV origin is top-left, matching CBitmap row order. Full
	// viewport when a real image was loaded; small aspect-correct
	// centered tile for the placeholder.
	SplashVertex quadVerts[6];
	if (haveImage) {
		const SplashVertex fs[6] = {
			{-1.0f, -1.0f, 0.0f, 1.0f},
			{ 1.0f, -1.0f, 1.0f, 1.0f},
			{-1.0f,  1.0f, 0.0f, 0.0f},
			{-1.0f,  1.0f, 0.0f, 0.0f},
			{ 1.0f, -1.0f, 1.0f, 1.0f},
			{ 1.0f,  1.0f, 1.0f, 0.0f},
		};
		std::memcpy(quadVerts, fs, sizeof(fs));
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
		std::memcpy(quadVerts, tile, sizeof(tile));
	}
	vertexBuffer = backend.CreateBuffer(sizeof(quadVerts), quadVerts);
	if (!vertexBuffer || !vertexBuffer->IsValid())
		return;

	// --- Pipeline.
	PipelineDesc pd;
	pd.name = "splash_quad";
	pd.vertexSource   = kVertexGlsl;
	pd.fragmentSource = kFragmentGlsl;
	pd.vertexAttributes = {
		VertexAttribute{ .location = 0, .bufferSlot = 0, .offset = 0,                 .format = VertexFormat::Float2 },
		VertexAttribute{ .location = 1, .bufferSlot = 0, .offset = sizeof(float) * 2, .format = VertexFormat::Float2 },
	};
	pd.vertexBindings = {
		VertexBindingLayout{ .slot = 0, .stride = sizeof(SplashVertex) },
	};

	pipeline = backend.CreatePipeline(pd);
	if (!pipeline || !pipeline->IsValid()) {
		LOG_L(L_ERROR, "[MetalSplashRenderer] pipeline did not link");
		return;
	}

	valid = true;
}

MetalSplashRenderer::~MetalSplashRenderer() = default;

void MetalSplashRenderer::Draw() const
{
	if (!valid)
		return;

	pipeline->Enable();
	pipeline->BindVertexBuffer(0, *vertexBuffer);
	pipeline->BindTexture(0, *texture);
	pipeline->Draw(PrimitiveTopology::Triangles, 0, 6);
	pipeline->Disable();
}

#endif // RENDER_BACKEND_METAL
