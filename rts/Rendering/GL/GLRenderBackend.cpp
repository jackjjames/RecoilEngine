/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "GLRenderBackend.h"

#include "Lua/LuaGLCapabilities.h"
#include "Rendering/IRenderBackend.h"
#include "Rendering/IRenderTarget.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/GL/FBO.h"
#include "Rendering/GL/GLBuffer.h"
#include "Rendering/GL/GLRenderTarget.h"
#include "Rendering/Platform/GLPresenter.h"
#include "Rendering/Platform/SDLGLRenderContext.h"
#include "Rendering/Textures/GL/GLTexture.h"

#include <memory>

namespace {

LuaGLCapabilities BuildLuaGLCapabilities()
{
	LuaGLCapabilities caps;
	caps.depthClamp = GLAD_GL_ARB_depth_clamp;
	caps.alphaToCoverage = GLAD_GL_ARB_multisample;
	caps.blendEquationSeparate = GLAD_GL_EXT_blend_equation_separate;
	caps.blendFuncSeparate = GLAD_GL_EXT_blend_func_separate;
	caps.stencilTwoSide = GLAD_GL_EXT_stencil_two_side;
	caps.framebuffer = FBO::IsSupported();
	caps.generateMipmap = IS_GL_FUNCTION_AVAILABLE(glGenerateMipmapEXT);
	caps.occlusionQuery = GLAD_GL_ARB_occlusion_query;
	caps.khrDebug = GLAD_GL_KHR_debug;
	caps.shaders = globalRendering != nullptr && globalRendering->haveGL4;
	caps.legacyImmediate = true;
	caps.legacyMatrix = true;
	caps.legacyLighting = true;
	caps.displayLists = true;
	caps.computeShader = GLAD_GL_ARB_compute_shader || GLAD_GL_VERSION_4_3;
	return caps;
}

class GLRenderBackend final : public IRenderBackend
{
public:
	IRenderContext& GetRenderContext() override
	{
		return *renderContext;
	}

	const IRenderContext& GetRenderContext() const override
	{
		return *renderContext;
	}

	IPresenter& GetPresenter() override
	{
		return *presenter;
	}

	const IPresenter& GetPresenter() const override
	{
		return *presenter;
	}

	std::unique_ptr<IRenderTarget> CreateRenderTarget() const override
	{
		return CreateGLRenderTarget();
	}

	std::unique_ptr<ITexture> CreateTexture2D(const int2& size, uint32_t internalFormat, const GL::TextureCreationParams& params, bool wantCompress) const override
	{
		return CreateGLTexture2D(size, internalFormat, params, wantCompress);
	}

	std::unique_ptr<ITexture> CreateTexture2DArray(const int2& size, uint32_t numPages, uint32_t internalFormat, const GL::TextureCreationParams& params, bool wantCompress) const override
	{
		return CreateGLTexture2DArray(size, numPages, internalFormat, params, wantCompress);
	}

	std::unique_ptr<ITexture> CreateImportedTexture(uint32_t texTarget, uint32_t textureId, const int2& size, uint32_t internalFormat, int32_t numLevels, uint32_t numPages, bool takeOwnership) const override
	{
		return CreateGLImportedTexture(texTarget, textureId, size, internalFormat, numLevels, numPages, takeOwnership);
	}

	std::unique_ptr<ISampler> CreateSampler(const GL::TextureCreationParams& params) const override
	{
		return CreateGLSampler(params);
	}

	std::unique_ptr<IBuffer> CreateBuffer(size_t size, const void* data) const override
	{
		return CreateGLBuffer(size, data);
	}

	const LuaGLCapabilities& GetLuaCapabilities() const override
	{
		luaCaps = BuildLuaGLCapabilities();
		return luaCaps;
	}

private:
	std::unique_ptr<IRenderContext> renderContext = CreateSDLGLRenderContext();
	std::unique_ptr<IPresenter> presenter = CreateGLPresenter();
	mutable LuaGLCapabilities luaCaps;
};

} // namespace

std::unique_ptr<IRenderBackend> CreateGLRenderBackend()
{
	return std::make_unique<GLRenderBackend>();
}
