#include "Rendering/IRenderBackend.h"

#include "Lua/LuaGLCapabilities.h"
#include "Rendering/IRenderTarget.h"
#include "Rendering/Platform/IRenderContext.h"
#include "Rendering/Platform/IPresenter.h"
#include "Rendering/Textures/ISampler.h"
#include "Rendering/Textures/ITexture.h"
#include "Rendering/Textures/TextureCreationParams.hpp"

#include <memory>

std::unique_ptr<IRenderContext> CreateMetalRenderContext();
std::unique_ptr<IPresenter> CreateMetalPresenter();
std::unique_ptr<IRenderTarget> CreateMetalRenderTarget();
std::unique_ptr<IBuffer> CreateMetalBuffer(size_t size, const void* data = nullptr);
std::unique_ptr<IShaderPipeline> CreateMetalShaderPipeline(const PipelineDesc& desc);
std::unique_ptr<ITexture> CreateMetalTexture2D(const int2& size, uint32_t internalFormat, const GL::TextureCreationParams& params);
std::unique_ptr<ISampler> CreateMetalSampler(const GL::TextureCreationParams& params);

namespace {

class MetalRenderBackend final : public IRenderBackend
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
		return CreateMetalRenderTarget();
	}

	std::unique_ptr<ITexture> CreateTexture2D(const int2& size, uint32_t internalFormat, const GL::TextureCreationParams& params, bool /*wantCompress*/) const override
	{
		return CreateMetalTexture2D(size, internalFormat, params);
	}

	std::unique_ptr<ITexture> CreateTexture2DArray(const int2&, uint32_t, uint32_t, const GL::TextureCreationParams&, bool) const override
	{
		// 2D arrays land when we have a consumer that actually needs them
		// (unit atlas, font atlas array). Loading screen is single-texture.
		return {};
	}

	std::unique_ptr<ITexture> CreateImportedTexture(uint32_t, uint32_t, const int2&, uint32_t, int32_t, uint32_t, bool) const override
	{
		// Importing an externally-owned GL texture id onto the Metal backend
		// is nonsensical; leave null. When engine callers use this path on
		// Metal we'll need to audit whether the caller really needs a
		// backend-owned texture.
		return {};
	}

	std::unique_ptr<ISampler> CreateSampler(const GL::TextureCreationParams& params) const override
	{
		return CreateMetalSampler(params);
	}

	std::unique_ptr<IBuffer> CreateBuffer(size_t size, const void* data) const override
	{
		return CreateMetalBuffer(size, data);
	}

	std::unique_ptr<IShaderPipeline> CreatePipeline(const PipelineDesc& desc) const override
	{
		return CreateMetalShaderPipeline(desc);
	}

	const LuaGLCapabilities& GetLuaCapabilities() const override
	{
		return luaCaps;
	}

private:
	std::unique_ptr<IRenderContext> renderContext = CreateMetalRenderContext();
	std::unique_ptr<IPresenter> presenter = CreateMetalPresenter();
	LuaGLCapabilities luaCaps;
};

} // namespace

std::unique_ptr<IRenderBackend> CreateMetalRenderBackend()
{
	return std::make_unique<MetalRenderBackend>();
}
