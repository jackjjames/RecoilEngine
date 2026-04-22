#include "Rendering/IRenderBackend.h"

#include "Lua/LuaGLCapabilities.h"
#include "Rendering/IRenderTarget.h"
#include "Rendering/Platform/IRenderContext.h"
#include "Rendering/Platform/IPresenter.h"

#include <memory>

std::unique_ptr<IRenderContext> CreateMetalRenderContext();
std::unique_ptr<IPresenter> CreateMetalPresenter();
std::unique_ptr<IRenderTarget> CreateMetalRenderTarget();

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

	std::unique_ptr<ITexture> CreateTexture2D(const int2&, uint32_t, const GL::TextureCreationParams&, bool) const override
	{
		return {};
	}

	std::unique_ptr<ITexture> CreateTexture2DArray(const int2&, uint32_t, uint32_t, const GL::TextureCreationParams&, bool) const override
	{
		return {};
	}

	std::unique_ptr<ITexture> CreateImportedTexture(uint32_t, uint32_t, const int2&, uint32_t, int32_t, uint32_t, bool) const override
	{
		return {};
	}

	std::unique_ptr<ISampler> CreateSampler(const GL::TextureCreationParams&) const override
	{
		return {};
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
