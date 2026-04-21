/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "GLRenderBackend.h"

#include "Rendering/IRenderBackend.h"
#include "Rendering/IRenderTarget.h"
#include "Rendering/GL/GLRenderTarget.h"
#include "Rendering/Platform/GLPresenter.h"
#include "Rendering/Platform/SDLGLRenderContext.h"
#include "Rendering/Textures/GL/GLTexture.h"

#include <memory>

namespace {

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

	std::unique_ptr<ISampler> CreateSampler(const GL::TextureCreationParams& params) const override
	{
		return CreateGLSampler(params);
	}

private:
	std::unique_ptr<IRenderContext> renderContext = CreateSDLGLRenderContext();
	std::unique_ptr<IPresenter> presenter = CreateGLPresenter();
};

} // namespace

std::unique_ptr<IRenderBackend> CreateGLRenderBackend()
{
	return std::make_unique<GLRenderBackend>();
}
